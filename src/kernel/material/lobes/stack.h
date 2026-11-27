// SPDX-License-Identifier: Apache-2.0
// Lobe stack: OpenPBR's base substrate, mix(dielectric base, metal, base_metalness), with the
// dielectric base a GGX specular layer over EON diffuse by albedo scaling and the metal the
// Turquin-compensated F82 lobe. One selection function (stack_select) gives the lobe
// probabilities that eval, pdf and sample all use. Directions at the entry points are in world
// space and are moved into the shading frame here; f excludes the cosine.
//
// Ref: docs/decisions/0005-material-model.md item 4 (layering tree, substrate = mix(metal,
// dielectric base), one lobe-selection function, result times closure_weight), item 5 (albedo
// scaling f = f_top + (1 - E_top) f_bottom; no inter-layer bounces; non-reciprocal as the spec
// says; Turquin compensation of the conductor), item 6 (E from E_diel and E_spec); OpenPBR
// Surface 1.1.2 layering by albedo scaling and M_base-substrate = mix(M_dielectric-base, S_metal,
// base_metalness), third_party/_cache/refs/openpbr-781573a80cb2/index.html line 541 and
// reference/open_pbr_surface.mtlx lines 433-437 (<mix name="base_substrate"> fg = metal,
// bg = dielectric_base, mix = base_metalness); 0003 d6 (surface decisions use lane reductions
// that do not depend on lane order: avg() sums all lanes). The coat's squared transmittance and
// the other layers of item 4 are later tasks.
//
// Interpretation of "(1 - specular_weight E_spec)" for the dielectric layer: specular_weight
// enters the lobe through the modulated index (fresnel.h specular_weighted_eta), so the albedo
// the base must be scaled by is the table at that index; at weight 1 the two readings coincide.
// specular_color tints the specular lobe and so scales the albedo per lane.
//
// The mix: f = closure_weight ((1 - M) (f_spec + (1 - E_diel) f_diff) + M c f_metal), where M =
// base_metalness and c = metal_compensation per lane (metal.h). The metal reads the same record
// inputs as the reference wires them (metal.h: F0 = base_weight base_color, tint = specular_color,
// main roughness); F82 mode only, since metal_ior is not in the record yet (0005 item 12).
#pragma once
#include <cstdint>

#include "core/frame.h"
#include "core/real.h"
#include "core/spectrum.h"
#include "core/vec.h"
#include "material/lobes/eon.h"
#include "material/lobes/fresnel.h"
#include "material/lobes/ggx.h"
#include "material/lobes/lobe.h"
#include "material/lobes/metal.h"
#include "material/lobes/tables.h"
#include "material/openpbr_params.h"
#include "portability.h"

AR_NS_BEGIN

struct StackSelect {
  Real p_spec;   // probability of choosing the dielectric specular lobe
  Real p_diff;   // the EON lobe: 1 - p_spec - p_metal
  Real p_metal;  // the metal lobe; 0 when base_metalness is 0
  Real metal;    // M = base_metalness clamped to OpenPBR's [0, 1]; the mix weight f uses
  Spec e_spec;   // albedo the dielectric specular layer takes from the diffuse base, per lane
  Spec eta;      // specular_weighted_eta clamped to the table's range; the lobe and the table both use it
  Real e_metal;      // E_spec(mu_o, r), the metal's single-scattering albedo at F = 1; 1 when unused
  bool eta_clamped;  // some lane's index was outside [kEDielEtaMin, kEDielEtaMax]; the host reports it
};

// Smallest metal albedo estimate used for selection when the lobe can reflect at all
// (base_metalness > 0, specular_weight > 0). F82 can vanish at the view angle on every lane
// (base_color 0 with specular_color 0 gives F = 0 for 1/7 <= mu <= 6/7) while it reflects at other
// half-vectors, and a lobe with a non-zero f must keep a non-zero probability, or BSDF sampling
// misses it and MIS loses its share. The value only moves variance (0005 item 4).
inline constexpr Real kMetalSelectFloor = Real(1e-3);

// The lobe takes the clamped, already weight-modulated index with weight 1: at weight 1
// specular_weighted_eta returns its argument, so the lobe's F is the one E_spec was built from.
AR_FN GgxParams stack_ggx_params(const OpenPBRParams& p, const StackSelect& s) {
  GgxParams g;
  g.specular_color = p.specular_color;
  g.specular_ior = s.eta;
  g.specular_weight = Real(1);
  g.roughness = p.specular_roughness;
  return g;
}

// OpenPBR: base_weight multiplies the base colour (diffuse reflectivity of the base).
AR_FN EonParams stack_eon_params(const OpenPBRParams& p) {
  EonParams e;
  e.base_color = p.base_color * p.base_weight;
  e.roughness = p.base_diffuse_roughness;
  return e;
}

// The metal lobe on the record's inputs (metal.h; open_pbr_surface.mtlx lines 407-418).
AR_FN MetalParams stack_metal_params(const OpenPBRParams& p) {
  MetalParams m;
  m.base_color = p.base_color;
  m.specular_color = p.specular_color;
  m.base_weight = p.base_weight;
  m.specular_weight = p.specular_weight;
  m.roughness = p.specular_roughness;
  return m;
}

// The one lobe-selection function (0005 item 4). Each lobe is chosen in proportion to its share
// of the mix times the lane-mean of its albedo estimate: the dielectric specular one from E_diel,
// the diffuse one after the layering scale (1 - E_spec), both times 1 - base_metalness; the metal
// one F_metal(mu_o) E_spec(mu_o, r) c per lane (F-weighted single-scattering albedo, compensated),
// times base_metalness. mu_o is wo.z in the shading frame.
// The E_diel table covers eta in [1, 3]. Each lane's modulated index is clamped to that range once
// here and the same value feeds the table and the lobe, so a stack with an index outside the
// range stays energy-conserving (a table clamped alone would leave the lobe reflecting more or
// less than the layer takes from the base). A clamp sets eta_clamped; the host turns it into the
// SPECTRUM_OUT_OF_RANGE report (src/host/report_codes.def). Ref: 0005 item 6.
// base_metalness 0 skips the metal terms, so the result is the dielectric stack's to the bit.
// M is clamped to [0, 1], the input's range (open_pbr_surface.mtlx line 14, uimin 0, uimax 1): outside
// it the mix would give the dielectric a negative weight, so negative f and probabilities.
AR_FN StackSelect stack_select(const EDielTable& tab, const ESpecTable& tab_m, const OpenPBRParams& p, Real mu_o) {
  const Real alpha = p.specular_roughness * p.specular_roughness;
  const Real r = e_roughness_axis(alpha, alpha);
  StackSelect s;
  s.eta_clamped = false;
  for (int i = 0; i < kLanes; ++i) {
    const Real eta_raw = specular_weighted_eta(p.specular_ior.v[i], p.specular_weight);
    const Real eta = min(max(eta_raw, kEDielEtaMin), kEDielEtaMax);
    s.eta.v[i] = eta;
    if (eta != eta_raw) s.eta_clamped = true;
    const Real e = p.specular_color.v[i] * e_diel_lookup(tab, mu_o, r, eta);
    s.e_spec.v[i] = min(max(e, Real(0)), Real(1));
  }
  const Spec e_diff = eon_albedo(stack_eon_params(p), mu_o);
  Real a_diff = Real(0);
  for (int i = 0; i < kLanes; ++i) a_diff += (Real(1) - s.e_spec.v[i]) * e_diff.v[i];
  a_diff /= Real(kLanes);
  const Real a_spec = avg(s.e_spec);

  const Real m = min(max(p.base_metalness, Real(0)), Real(1));
  s.metal = m;
  s.e_metal = Real(1);
  Real a_metal = Real(0);
  if (m > Real(0)) {
    const MetalParams mp = stack_metal_params(p);
    const Real e_m = e_spec_lookup(tab_m, mu_o, r);
    s.e_metal = e_m;
    const Spec comp = metal_compensation(mp, e_m);
    const Spec f_o = metal_fresnel(mp, mu_o);
    for (int i = 0; i < kLanes; ++i) a_metal += min(max(f_o.v[i] * e_m * comp.v[i], Real(0)), Real(1));
    a_metal /= Real(kLanes);
    if (p.specular_weight > Real(0)) a_metal = max(a_metal, kMetalSelectFloor);
  }
  const Real w_diel = Real(1) - m;
  const Real w_spec = w_diel * a_spec, w_diff = w_diel * a_diff, w_metal = m * a_metal;
  const Real total = w_spec + w_diff + w_metal;
  // All albedos zero: the stack reflects nothing, any choice gives f = 0.
  s.p_spec = total > Real(0) ? w_spec / total : Real(1);
  s.p_metal = total > Real(0) ? w_metal / total : Real(0);
  s.p_diff = max(Real(1) - s.p_spec - s.p_metal, Real(0));
  return s;
}

// A sample is a result when f is finite and pdf is a finite number; a pdf of 0 (or below) is "no sample"
// and carries f = 0. A NaN or infinite f or pdf is left exactly as the lobes produced it, for the stage to
// end the sample INVALID: zeroing it would render a black sample with no AR Invalid count (rule 5, 0004 D17).
AR_FN void stack_sample_settle(LobeSample& out) {
  if (!is_finite(out.f) || !(out.pdf - out.pdf == Real(0))) return;
  if (!(out.pdf > Real(0))) {
    out.f = spec_fill(Real(0));
    out.pdf = Real(0);
  }
}

AR_FN LobeEval stack_zero_eval() {
  LobeEval e;
  e.f = spec_fill(Real(0));
  e.pdf = Real(0);
  return e;
}

// The per-lane compensation c of the metal lobe at this selection (metal.h metal_compensation; 1 at
// E = 1). It is recomputed from StackSelect::e_metal rather than carried per lane, which keeps three
// registers out of the selection state that stays live across the lobe evaluations in shade_surface.
AR_FN Spec stack_metal_comp(const OpenPBRParams& p, const StackSelect& s) {
  return s.e_metal < Real(1) ? metal_compensation(stack_metal_params(p), s.e_metal) : spec_fill(Real(1));
}

// f of the substrate, closure_weight ((1 - M) (f_spec + (1 - E_spec) f_diff) + M c f_metal), in two
// steps: the dielectric part, then the metal added when the selection gives it a share (with no
// share M or F is 0, so the term is 0). Two steps let the dielectric lobes' values die before the
// metal is evaluated, which keeps shade_surface's NEE inside its register budget.
AR_FN Spec stack_dielectric_f(const OpenPBRParams& p, const StackSelect& s, const Spec& f_spec, const Spec& f_diff) {
  const Real m = s.metal;
  Spec f;
  for (int i = 0; i < kLanes; ++i)
    f.v[i] = p.closure_weight * ((Real(1) - m) * (f_spec.v[i] + (Real(1) - s.e_spec.v[i]) * f_diff.v[i]));
  return f;
}
AR_FN void stack_add_metal_f(const OpenPBRParams& p, const StackSelect& s, const Spec& f_metal, Spec& f) {
  const Spec comp = stack_metal_comp(p, s);
  for (int i = 0; i < kLanes; ++i) f.v[i] += p.closure_weight * (s.metal * comp.v[i] * f_metal.v[i]);
}
AR_FN Spec stack_combine_f(const OpenPBRParams& p, const StackSelect& s, const Spec& f_spec, const Spec& f_diff,
                           const Spec& f_metal) {
  Spec f = stack_dielectric_f(p, s, f_spec, f_diff);
  if (s.p_metal > Real(0)) stack_add_metal_f(p, s, f_metal, f);
  return f;
}

// The metal lobe's eval, or zero when the selection gives it no share.
AR_FN LobeEval stack_metal_eval(const OpenPBRParams& p, const StackSelect& s, Real3 wo, Real3 wi, TransportMode mode) {
  if (!(s.p_metal > Real(0))) return stack_zero_eval();
  return metal_eval(stack_metal_params(p), wo, wi, mode);
}

AR_FN LobeEval stack_eval(const EDielTable& tab, const ESpecTable& tab_m, const OpenPBRParams& p, const Frame& frame,
                          Real3 wo_world, Real3 wi_world, TransportMode mode, bool* eta_clamped = nullptr) {
  const Real3 wo = to_local(frame, wo_world), wi = to_local(frame, wi_world);
  if (wo.z <= Real(0) || wi.z <= Real(0)) return stack_zero_eval();
  const StackSelect s = stack_select(tab, tab_m, p, wo.z);
  if (eta_clamped != nullptr && s.eta_clamped) *eta_clamped = true;
  LobeEval e;
  {
    const LobeEval es = ggx_eval(stack_ggx_params(p, s), wo, wi, mode);
    const LobeEval ed = eon_eval(stack_eon_params(p), wo, wi, mode);
    e.f = stack_dielectric_f(p, s, es.f, ed.f);
    e.pdf = s.p_spec * es.pdf + s.p_diff * ed.pdf;
  }
  if (s.p_metal > Real(0)) {
    const LobeEval em = metal_eval(stack_metal_params(p), wo, wi, mode);
    stack_add_metal_f(p, s, em.f, e.f);
    e.pdf += s.p_metal * em.pdf;
  }
  // A non-finite f or pdf is handed to the stage as it is, which ends the sample INVALID (rule 5, 0004 D17).
  if (e.pdf < Real(0)) return stack_zero_eval();
  return e;
}

AR_FN Real stack_pdf(const EDielTable& tab, const ESpecTable& tab_m, const OpenPBRParams& p, const Frame& frame,
                     Real3 wo_world, Real3 wi_world, TransportMode mode, bool* eta_clamped = nullptr) {
  const Real3 wo = to_local(frame, wo_world), wi = to_local(frame, wi_world);
  if (wo.z <= Real(0) || wi.z <= Real(0)) return Real(0);
  const StackSelect s = stack_select(tab, tab_m, p, wo.z);
  if (eta_clamped != nullptr && s.eta_clamped) *eta_clamped = true;
  const Real pm = s.p_metal > Real(0) ? metal_pdf(stack_metal_params(p), wo, wi, mode) : Real(0);
  return s.p_spec * ggx_pdf(stack_ggx_params(p, s), wo, wi, mode) + s.p_diff * eon_pdf(stack_eon_params(p), wo, wi, mode) +
         s.p_metal * pm;
}

// A uniform in [0, 1) stays below 1 after the lobe-selection rescale.
AR_FN Real stack_below_one(Real u) {
  const Real top = sizeof(Real) == 4 ? Real(0.99999994) : Real(1) - Real(1.1102230246251565e-16);
  return min(u, top);
}

// uc picks the lobe on [0, p_spec) specular, [p_spec, p_spec + p_diff) diffuse, the rest metal;
// u2 drives the chosen lobe's own sampling. With rescale_uc, u2.x is replaced by uc rescaled to
// the chosen lobe's interval (one uniform serves both, as in eon_sample).
// A delta sample (the specular or the metal lobe below lobe_is_delta's threshold) carries
// LOBE_DELTA with pdf = its choice probability (times the lobe's unit pdf), so f cos / pdf is
// that lobe's weighted F over its probability; every other sample reports the mixture density
// and the mixed f of its direction. pdf = 0 means "no sample". The three entry points OR
// StackSelect::eta_clamped into *eta_clamped when it is given (never clear it).
// A sample's flags name the chosen lobe (LOBE_GLOSSY for GGX and metal, LOBE_DIFFUSE for EON).
AR_FN LobeSample stack_sample_core(const EDielTable& tab, const ESpecTable& tab_m, const OpenPBRParams& p, const Frame& frame,
                                   Real3 wo_world, Real uc, Real2 u2, bool rescale_uc, TransportMode mode, bool* eta_clamped) {
  LobeSample out;
  out.wi = Real3{Real(0), Real(0), Real(1)};
  out.f = spec_fill(Real(0));
  out.pdf = Real(0);
  out.flags = LOBE_REFLECTION;
  const Real3 wo = to_local(frame, wo_world);
  if (wo.z <= Real(0)) return out;
  const StackSelect s = stack_select(tab, tab_m, p, wo.z);
  if (eta_clamped != nullptr && s.eta_clamped) *eta_clamped = true;
  const GgxParams gp = stack_ggx_params(p, s);
  const EonParams ep = stack_eon_params(p);
  const Real p_sd = s.p_spec + s.p_diff;
  const bool pick_spec = uc < s.p_spec;
  const bool pick_metal = !pick_spec && s.p_metal > Real(0) && uc >= p_sd;

  LobeSample ls;
  LobeEval es = stack_zero_eval(), ed = stack_zero_eval(), em = stack_zero_eval();
  // The metal and the dielectric specular lobe share one GGX at the main roughness (metal.h), so
  // one visible-normal sampler serves both: ggx_sample's direction and density are metal_sample's,
  // and its value is the specular lobe's at that direction.
  if (pick_spec || pick_metal) {
    const Real ug = pick_metal ? (uc - p_sd) / s.p_metal : uc / s.p_spec;
    ls = ggx_sample(gp, wo, rescale_uc ? Real2{stack_below_one(ug), u2.y} : u2, mode);
  } else {
    ls = eon_sample(ep, wo, rescale_uc ? Real2{stack_below_one((uc - s.p_spec) / s.p_diff), u2.y} : u2, mode);
  }
  if (!(ls.pdf > Real(0))) return out;
  if ((ls.flags & LOBE_DELTA) != 0) {
    // Below lobe_is_delta's threshold the specular and the metal lobe are both delta at the mirror
    // direction (one roughness); each is chosen with its own probability and a delta sample carries
    // only the chosen lobe's weight, so their sum over the choice is unbiased. The diffuse lobe has no
    // value on that measure-zero set. The delta metal's f is metal_sample's: F(wo.z) / wo.z.
    out.wi = to_world(frame, ls.wi);
    const Spec zero = spec_fill(Real(0));
    if (pick_metal) {
      Spec fm = metal_fresnel(stack_metal_params(p), wo.z);
      for (int i = 0; i < kLanes; ++i) fm.v[i] /= wo.z;
      out.f = stack_combine_f(p, s, zero, zero, fm);
    } else {
      out.f = stack_combine_f(p, s, ls.f, zero, zero);
    }
    out.pdf = (pick_metal ? s.p_metal : s.p_spec) * ls.pdf;
    out.flags = ls.flags | LOBE_GLOSSY;
    stack_sample_settle(out);
    return out;
  }
  const Real3 wi_local = ls.wi;
  if (pick_spec || pick_metal) {
    es.f = ls.f, es.pdf = ls.pdf;
    ed = eon_eval(ep, wo, wi_local, mode);
    em = stack_metal_eval(p, s, wo, wi_local, mode);
  } else {
    ed.f = ls.f, ed.pdf = ls.pdf;
    es = ggx_eval(gp, wo, wi_local, mode);
    em = stack_metal_eval(p, s, wo, wi_local, mode);
  }
  out.wi = to_world(frame, wi_local);
  out.flags = LOBE_REFLECTION | (pick_spec || pick_metal ? LOBE_GLOSSY : LOBE_DIFFUSE);
  out.f = stack_combine_f(p, s, es.f, ed.f, em.f);
  out.pdf = s.p_spec * es.pdf + s.p_diff * ed.pdf + s.p_metal * em.pdf;
  stack_sample_settle(out);
  return out;
}

// One uniform: u.x picks the lobe and is rescaled for the lobe's sampling.
AR_FN LobeSample stack_sample(const EDielTable& tab, const ESpecTable& tab_m, const OpenPBRParams& p, const Frame& frame,
                              Real3 wo_world, Real2 u, TransportMode mode, bool* eta_clamped = nullptr) {
  return stack_sample_core(tab, tab_m, p, frame, wo_world, u.x, u, true, mode, eta_clamped);
}

// A separate lobe-choice uniform uc and a 2D direction u2, as pbrt-v4 BSDF::Sample_f(wo, uc, u2)
// and the per-event block of 0003 d23 (lobe choice; direction u, v) give them.
AR_FN LobeSample stack_sample(const EDielTable& tab, const ESpecTable& tab_m, const OpenPBRParams& p, const Frame& frame,
                              Real3 wo_world, Real uc, Real2 u2, TransportMode mode, bool* eta_clamped = nullptr) {
  return stack_sample_core(tab, tab_m, p, frame, wo_world, uc, u2, false, mode, eta_clamped);
}

AR_NS_END
