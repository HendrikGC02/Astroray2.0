// SPDX-License-Identifier: Apache-2.0
// GRATING extension kind: a 1D periodic grating with delta diffraction orders (0005 items 21-23).
// It replaces the whole OpenPBR lobe stack (item 15); sample, eval and pdf follow item 17.
//
// Frame: frame_from_normal_tangent(N, T) with T the record's geometry_tangent, so local x runs along
// the grooves, local y is the grating vector g = N x T and z the normal. Directions point away from
// the surface (wo towards the previous vertex, wi towards the next), so a unit direction's
// tangential projection is its (x, y) and item 21 reads, for order m:
//   reflection    t_o = -t_i + m (lambda / d) g
//   transmission  n_o t_o = -n_i t_i + m (lambda / d) g
// with t_i the projection of wo and t_o that of wi; |t_o| > 1 is evanescent. In the plane normal to
// the grooves this is Palmer's m lambda = d (sin alpha + sin beta) (eq. 2-1); out of it, the conical
// form (eq. 2-3). The ambient medium has index 1 and the substrate is on the back (-z) side.
//
// Efficiency: the table is indexed by |cos| of wo. On a light path wo is the incident direction; on
// a camera path the light arrives along wi, and the reciprocity theorem (Palmer section 9.10: the
// efficiency in order m is unchanged when incidence and diffraction directions are exchanged) makes
// eta at wo the same number.
//
// Sampling (item 22): p_m = mean over lanes of eta_m (0 where order m is evanescent at that lane's
// wavelength) over its sum across orders, a permutation-invariant reduction (0003 d6). Order 0 in
// reflection, and in transmission over a substrate of constant index, has one direction for every
// lane: all lanes continue. Any other order (and order 0 into a dispersive substrate) has a
// wavelength-dependent direction: the sample carries LOBE_WL_DEPENDENT_DIR, its direction is lane
// 0's and only lane 0's value is non-zero; the caller applies terminate_secondaries, which keeps
// lane 0 (0003 d5, ruling 24; pbrt-v4 SampledWavelengths::TerminateSecondary). The estimate stays
// unbiased since p_m >= eta_m(lambda_0) / (4 sum) > 0 wherever eta_m(lambda_0) > 0.
//
// Delta convention (as ggx.h and stack.h): pdf = p_m and f = eta / |cos wi|, so f |cos| / pdf =
// eta / p_m. Transmission in Radiance mode carries (n_i / n_o)^2, n_i on wo's side (pbrt-v4
// bxdfs.cpp DielectricBxDF::Sample_f, ft /= Sqr(etap); Veach 1997 5.2). grating_eval and grating_pdf are 0 (delta; no NEE).
//
// Ref: docs/decisions/0005-material-model.md items 15, 17, 21-23; Harvey and Vernold 1998, Applied
// Optics 37(34) (grating equation in direction cosines, as item 21 states it); C. Palmer,
// Diffraction Grating Handbook, 7th ed. (Newport 2014), eqs. 2-1 and 2-3, section 9.10;
// third_party/_cache/refs/pbrt-v4-092b23a47ad4: src/pbrt/util/spectrum.h line 313
// (TerminateSecondary), src/pbrt/bxdfs.cpp (DielectricBxDF::Sample_f).
#pragma once
#include <cstdint>

#include "core/frame.h"
#include "core/real.h"
#include "core/spectrum.h"
#include "core/vec.h"
#include "material/ext/grating_table.h"
#include "material/ext_params.h"
#include "material/lobes/lobe.h"
#include "material/slot_resolve.h"
#include "material/spectrum_ref.h"
#include "portability.h"

AR_NS_BEGIN

// Extension sample flag (0005 item 17): the direction depends on wavelength, so the caller must
// terminate the secondary lanes. lobes/lobe.h owns the LobeSample flag bits; this one sits here
// until a stage consumes it, on the first bit lobe.h leaves free.
inline constexpr uint32_t LOBE_WL_DEPENDENT_DIR = 1u << 5;
static_assert((LOBE_WL_DEPENDENT_DIR & (LOBE_DELTA | LOBE_REFLECTION | LOBE_TRANSMISSION | LOBE_DIFFUSE | LOBE_GLOSSY)) == 0u,
              "LOBE_WL_DEPENDENT_DIR must not share a bit with lobe.h's flags");

// Substrate index at the lanes. Only CONSTANT and CAUCHY_ABBE IOR refs pass the compiler's block
// check; anything else gives 0, which grating_sample turns into "no sample".
AR_FN Spec grating_substrate_ior(const GratingParams& p, const Lambda& lam) {
  const SpectrumRef& r = p.substrate_ior;
  if (r.kind == uint8_t(SpectrumKind::CONSTANT)) return spec_fill(Real(r.p[0]));
  if (r.kind == uint8_t(SpectrumKind::CAUCHY_ABBE)) return cauchy_abbe_ior(Real(r.p[0]), Real(r.p[1]), Real(r.p[2]), lam);
  AR_ASSERT(false);
  return spec_fill(Real(0));
}

// A constant index refracts every wavelength alike.
AR_FN bool grating_substrate_dispersive(const GratingParams& p) {
  return p.substrate_ior.kind != uint8_t(SpectrumKind::CONSTANT);
}

// Indices on wo's side (n_i) and on the far side (n_o) for a wo with local z = wo_z.
AR_FN void grating_sides(bool transmit, Real wo_z, Real n_sub, Real* n_i, Real* n_o) {
  *n_i = Real(1);
  *n_o = Real(1);
  if (!transmit) return;
  if (wo_z > Real(0)) *n_o = n_sub;
  else *n_i = n_sub;
}

// Item 21 in the local frame. False when order m is evanescent (|t_o| > 1) or the inputs give no
// direction; otherwise *wi is the unit direction of order m, on wo's side in reflection and on the
// other side in transmission.
AR_FN bool grating_order_direction(Real3 wo, int m, Real lambda_nm, Real pitch_nm, bool transmit, Real n_i, Real n_o,
                                   Real3* wi) {
  if (!(pitch_nm > Real(0)) || !(n_o > Real(0)) || wo.z == Real(0)) return false;
  const Real k = Real(m) * lambda_nm / pitch_nm;
  const Real tx = -n_i * wo.x / n_o;
  const Real ty = (-n_i * wo.y + k) / n_o;
  const Real t2 = tx * tx + ty * ty;
  if (!(t2 <= Real(1))) return false;
  const Real z = ar::sqrt(ar::max(Real(1) - t2, Real(0)));
  const bool up = (wo.z > Real(0)) != transmit;
  *wi = Real3{tx, ty, up ? z : -z};
  return true;
}

// eta_m at lane j, 0 where order m is evanescent at that lane's wavelength.
AR_FN Real grating_lane_eta(const GratingParams& p, const GratingTable& tab, Real3 wo, int m, Real lambda_nm, Real n_i,
                            Real n_o, Real cos_i) {
  Real3 wi{};
  if (!grating_order_direction(wo, m, lambda_nm, Real(p.pitch_nm), p.mode == uint32_t(GratingMode::Transmit), n_i, n_o, &wi))
    return Real(0);
  return grating_table_eta(tab, m, lambda_nm, cos_i);
}

// eta-bar_m: the lane mean of item 22.
AR_FN Real grating_order_mean(const GratingParams& p, const GratingTable& tab, Real3 wo, int m, const Lambda& lam,
                              const Spec& n_i, const Spec& n_o, Real cos_i) {
  Real s = Real(0);
  for (int j = 0; j < kLanes; ++j) s += grating_lane_eta(p, tab, wo, m, lam.nm[j], n_i.v[j], n_o.v[j], cos_i);
  return s / Real(kLanes);
}

// wo in world space, frame from (N, T); u in [0, 1) is the extension-choice dimension (0003). The
// returned wi is in world space. pdf = 0 means no sample: wo in the surface plane, every order
// evanescent or of zero efficiency, or the chosen order evanescent at lane 0.
AR_FN LobeSample grating_sample(const GratingParams& p, const GratingTable& tab, const Frame& frame, Real3 wo_world,
                                TransportMode mode, const Lambda& lam, Real u) {
  LobeSample out;
  out.wi = Real3{Real(0), Real(0), Real(0)};
  out.f = spec_fill(Real(0));
  out.pdf = Real(0);
  out.flags = 0u;
  const Real3 wo = to_local(frame, wo_world);
  if (!(wo.z != Real(0))) return out;
  const bool transmit = p.mode == uint32_t(GratingMode::Transmit);
  const Real cos_i = ar::min(ar::abs(wo.z), Real(1));
  Spec n_i = spec_fill(Real(1));
  Spec n_o = spec_fill(Real(1));
  if (transmit) {
    const Spec n_sub = grating_substrate_ior(p, lam);
    for (int j = 0; j < kLanes; ++j) grating_sides(true, wo.z, n_sub.v[j], &n_i.v[j], &n_o.v[j]);
  }
  // Two passes over the orders (sum, then the walk) instead of an array of 17 means: the
  // recomputation is identical, and the kernel keeps no per-order state.
  Real sum = Real(0);
  for (int m = p.m_min; m <= p.m_max; ++m) sum += grating_order_mean(p, tab, wo, m, lam, n_i, n_o, cos_i);
  if (!(sum > Real(0))) return out;
  const Real target = u * sum;
  Real acc = Real(0);
  int chosen = p.m_max + 1;
  int last = p.m_max + 1;
  Real chosen_mean = Real(0);
  Real last_mean = Real(0);
  for (int m = p.m_min; m <= p.m_max; ++m) {
    const Real e = grating_order_mean(p, tab, wo, m, lam, n_i, n_o, cos_i);
    if (!(e > Real(0))) continue;
    last = m;
    last_mean = e;
    acc += e;
    if (target < acc) {
      chosen = m;
      chosen_mean = e;
      break;
    }
  }
  // Rounding can leave target at or above the running sum: the last order with efficiency takes it.
  if (chosen > p.m_max) {
    chosen = last;
    chosen_mean = last_mean;
  }
  if (chosen > p.m_max) return out;
  const Real pm = chosen_mean / sum;
  const bool all_lanes = chosen == 0 && (!transmit || !grating_substrate_dispersive(p));
  Real3 wi{};
  if (!grating_order_direction(wo, chosen, lam.nm[0], Real(p.pitch_nm), transmit, n_i.v[0], n_o.v[0], &wi)) return out;
  const Real abs_cos = ar::abs(wi.z);
  if (!(abs_cos > Real(0))) return out;
  for (int j = 0; j < kLanes; ++j) {
    if (!all_lanes && j != 0) continue;
    Real e = grating_lane_eta(p, tab, wo, chosen, lam.nm[j], n_i.v[j], n_o.v[j], cos_i);
    if (transmit && mode == TransportMode::Radiance) e *= (n_i.v[j] / n_o.v[j]) * (n_i.v[j] / n_o.v[j]);
    out.f.v[j] = e / abs_cos;
  }
  out.wi = to_world(frame, wi);
  out.pdf = pm;
  out.flags = LOBE_DELTA | (transmit ? LOBE_TRANSMISSION : LOBE_REFLECTION) | (all_lanes ? 0u : LOBE_WL_DEPENDENT_DIR);
  return out;
}

// Delta orders have no density in solid angle (item 22): eval and pdf are 0 for every pair.
AR_FN LobeEval grating_eval(const GratingParams&, const GratingTable&, const Frame&, Real3, Real3, TransportMode,
                            const Lambda&) {
  LobeEval e;
  e.f = spec_fill(Real(0));
  e.pdf = Real(0);
  return e;
}

AR_FN Real grating_pdf(const GratingParams&, const GratingTable&, const Frame&, Real3, Real3, TransportMode,
                       const Lambda&) {
  return Real(0);
}

AR_NS_END
