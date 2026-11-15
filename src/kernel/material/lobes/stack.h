// SPDX-License-Identifier: Apache-2.0
// M1a lobe stack: the dielectric base of OpenPBR, GGX specular layered over EON diffuse by
// albedo scaling. One selection function (stack_select) gives the lobe probabilities that
// eval, pdf and sample all use. Directions at the entry points are in world space and are
// moved into the shading frame here; f excludes the cosine.
//
// Ref: docs/decisions/0005-material-model.md item 4 (layering tree, one lobe-selection
// function, result times closure_weight), item 5 (albedo scaling
// f = f_top + (1 - E_top) f_bottom; no inter-layer bounces; non-reciprocal as the spec says),
// item 6 (E from E_diel); OpenPBR Surface 1.1.2 layering by albedo scaling; 0003 d6 (surface
// decisions use lane reductions that do not depend on lane order: avg() sums all lanes).
// The coat's squared transmittance and the other layers of item 4 are later tasks.
//
// Interpretation of the task text "(1 - specular_weight E_spec)": specular_weight enters the
// lobe through the modulated index (fresnel.h specular_weighted_eta), so the albedo the base
// must be scaled by is the table at that index; at weight 1 the two readings coincide.
// specular_color tints the specular lobe and so scales the albedo per lane.
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
#include "material/lobes/tables.h"
#include "portability.h"

AR_NS_BEGIN

// The fields of the resolved record (0005 item 2c) that the M1a stack reads, under the record's
// names. The full record replaces this struct and keeps these members.
struct OpenPBRParams {
  Real base_weight;
  Real base_diffuse_roughness;
  Real specular_weight;
  Real specular_roughness;
  Real closure_weight;
  Spec base_color;
  Spec specular_color;
  Spec specular_ior;
};

struct StackSelect {
  Real p_spec;  // probability of choosing the specular lobe
  Real p_diff;  // 1 - p_spec
  Spec e_spec;  // albedo the specular layer takes from the base, per lane
};

AR_FN GgxParams stack_ggx_params(const OpenPBRParams& p) {
  GgxParams g;
  g.specular_color = p.specular_color;
  g.specular_ior = p.specular_ior;
  g.specular_weight = p.specular_weight;
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

// The one lobe-selection function (0005 item 4). Each lobe is chosen in proportion to the
// lane-mean of its albedo estimate, the specular one from the table, the diffuse one after the
// layering scale (1 - E_spec). mu_o is wo.z in the shading frame.
AR_FN StackSelect stack_select(const EDielTable& tab, const OpenPBRParams& p, Real mu_o) {
  const Real alpha = p.specular_roughness * p.specular_roughness;
  const Real r = e_roughness_axis(alpha, alpha);
  StackSelect s;
  for (int i = 0; i < kLanes; ++i) {
    const Real eta = specular_weighted_eta(p.specular_ior.v[i], p.specular_weight);
    const Real e = p.specular_color.v[i] * e_diel_lookup(tab, mu_o, r, eta);
    s.e_spec.v[i] = min(max(e, Real(0)), Real(1));
  }
  const Spec e_diff = eon_albedo(stack_eon_params(p), mu_o);
  Real a_diff = Real(0);
  for (int i = 0; i < kLanes; ++i) a_diff += (Real(1) - s.e_spec.v[i]) * e_diff.v[i];
  a_diff /= Real(kLanes);
  const Real a_spec = avg(s.e_spec);
  const Real total = a_spec + a_diff;
  // Both albedos zero: the stack reflects nothing, any choice gives f = 0.
  s.p_spec = total > Real(0) ? a_spec / total : Real(1);
  s.p_diff = Real(1) - s.p_spec;
  return s;
}

AR_FN LobeEval stack_zero_eval() {
  LobeEval e;
  e.f = spec_fill(Real(0));
  e.pdf = Real(0);
  return e;
}

// f of the layered base from the two lobes' values: f_spec + (1 - E_spec) f_diff, times closure_weight.
AR_FN Spec stack_combine_f(const OpenPBRParams& p, const StackSelect& s, const Spec& f_spec, const Spec& f_diff) {
  Spec f;
  for (int i = 0; i < kLanes; ++i) f.v[i] = p.closure_weight * (f_spec.v[i] + (Real(1) - s.e_spec.v[i]) * f_diff.v[i]);
  return f;
}

AR_FN LobeEval stack_eval(const EDielTable& tab, const OpenPBRParams& p, const Frame& frame, Real3 wo_world,
                          Real3 wi_world, TransportMode mode) {
  const Real3 wo = to_local(frame, wo_world), wi = to_local(frame, wi_world);
  if (wo.z <= Real(0) || wi.z <= Real(0)) return stack_zero_eval();
  const StackSelect s = stack_select(tab, p, wo.z);
  const LobeEval es = ggx_eval(stack_ggx_params(p), wo, wi, mode);
  const LobeEval ed = eon_eval(stack_eon_params(p), wo, wi, mode);
  LobeEval e;
  e.f = stack_combine_f(p, s, es.f, ed.f);
  e.pdf = s.p_spec * es.pdf + s.p_diff * ed.pdf;
  if (!is_finite(e.f) || !(e.pdf >= Real(0))) return stack_zero_eval();
  return e;
}

AR_FN Real stack_pdf(const EDielTable& tab, const OpenPBRParams& p, const Frame& frame, Real3 wo_world, Real3 wi_world,
                     TransportMode mode) {
  const Real3 wo = to_local(frame, wo_world), wi = to_local(frame, wi_world);
  if (wo.z <= Real(0) || wi.z <= Real(0)) return Real(0);
  const StackSelect s = stack_select(tab, p, wo.z);
  return s.p_spec * ggx_pdf(stack_ggx_params(p), wo, wi, mode) + s.p_diff * eon_pdf(stack_eon_params(p), wo, wi, mode);
}

// A uniform in [0, 1) stays below 1 after the lobe-selection rescale.
AR_FN Real stack_below_one(Real u) {
  const Real top = sizeof(Real) == 4 ? Real(0.99999994) : Real(1) - Real(1.1102230246251565e-16);
  return min(u, top);
}

// u.x picks the lobe and is rescaled for the chosen lobe's own sampling, as in eon_sample.
// A delta specular sample carries LOBE_DELTA with pdf = p_spec (the choice probability times
// the lobe's unit pdf), so f cos / pdf = F / p_spec; every other sample reports the mixture
// density and the layered f of its direction. pdf = 0 means "no sample".
AR_FN LobeSample stack_sample(const EDielTable& tab, const OpenPBRParams& p, const Frame& frame, Real3 wo_world, Real2 u,
                              TransportMode mode) {
  LobeSample out;
  out.wi = Real3{Real(0), Real(0), Real(1)};
  out.f = spec_fill(Real(0));
  out.pdf = Real(0);
  out.flags = LOBE_REFLECTION;
  const Real3 wo = to_local(frame, wo_world);
  if (wo.z <= Real(0)) return out;
  const StackSelect s = stack_select(tab, p, wo.z);
  const GgxParams gp = stack_ggx_params(p);
  const EonParams ep = stack_eon_params(p);

  LobeSample ls, ds;
  LobeEval es = stack_zero_eval(), ed = stack_zero_eval();
  Real3 wi_local;
  if (u.x < s.p_spec) {
    ls = ggx_sample(gp, wo, Real2{stack_below_one(u.x / s.p_spec), u.y}, mode);
    if (!(ls.pdf > Real(0))) return out;
    if ((ls.flags & LOBE_DELTA) != 0) {
      // No diffuse part at the mirror direction (a measure-zero set for the diffuse lobe).
      out.wi = to_world(frame, ls.wi);
      out.f = stack_combine_f(p, s, ls.f, spec_fill(Real(0)));
      out.pdf = s.p_spec * ls.pdf;
      out.flags = ls.flags;
      if (!is_finite(out.f) || !(out.pdf > Real(0))) {
        out.f = spec_fill(Real(0));
        out.pdf = Real(0);
      }
      return out;
    }
    wi_local = ls.wi;
    es.f = ls.f;
    es.pdf = ls.pdf;
    ed = eon_eval(ep, wo, wi_local, mode);
  } else {
    ds = eon_sample(ep, wo, Real2{stack_below_one((u.x - s.p_spec) / s.p_diff), u.y}, mode);
    if (!(ds.pdf > Real(0))) return out;
    wi_local = ds.wi;
    ed.f = ds.f;
    ed.pdf = ds.pdf;
    es = ggx_eval(gp, wo, wi_local, mode);
  }
  out.wi = to_world(frame, wi_local);
  out.f = stack_combine_f(p, s, es.f, ed.f);
  out.pdf = s.p_spec * es.pdf + s.p_diff * ed.pdf;
  if (!is_finite(out.f) || !(out.pdf > Real(0))) {
    out.f = spec_fill(Real(0));
    out.pdf = Real(0);
  }
  return out;
}

AR_NS_END
