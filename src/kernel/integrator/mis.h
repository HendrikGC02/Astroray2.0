// SPDX-License-Identifier: Apache-2.0
// The one MIS form of the engine (ruling 5): Veach's balance heuristic over the two strategies of
// a non-delta vertex, light sampling (NEE) and BSDF sampling, in pbrt-v4 VolPathIntegrator's
// rescaled-probability form. beta, r_u and r_l are 4-lane spectra and the weights average over
// lanes, so they also combine wavelengths (0004 D1, 0003 d6-d7).
//
// The path carries r_u (unidirectional rescaled pdf, 1 at the camera; changed only by hero
// decisions, 0003 d6) and, after a BSDF sample of pdf p_b, r_l = r_u / p_b (shade_surface).
//  - NEE from a vertex, light chosen with pmf and sampled with solid-angle pdf p_li, p_l = pmf
//    p_li, BSDF pdf p_b toward the light direction (0 for delta lobes):
//      L += beta f |cos| Le / avg(r_u (p_l + p_b))   (non-delta light)
//      L += beta f |cos| Le / avg(r_u p_l)           (delta light: BSDF pdf 0, weight 1)
//    pbrt SampleLd: r_l = r_p lightPDF, r_u = r_p scatterPDF, divided by (r_l + r_u).Average()
//    or r_l.Average() for a delta light.
//  - An emitter reached by a BSDF ray, with p_l = pmf(ctx_prev, light) pdf_li(ctx_prev, wi) from
//    the previous vertex's context (the same selection pmf and solid-angle measure as NEE, 0004
//    D1):
//      L += beta Le / avg(r_u + r_l p_l)   after a non-delta bounce
//      L += beta Le / avg(r_u)             at the first vertex or after a delta bounce
//    pbrt Li: r_l *= lightPDF; L += beta Le / (r_u + r_l).Average(), else / r_u.Average().
// With r_u = 1 both reduce to the balance heuristic p_l / (p_l + p_b) and p_b / (p_l + p_b) on
// the estimates f Le / p_l and f Le / p_b, which sum to one.
//
// The previous vertex's context point is stored as ray_o - prev_dist ray_d (0003 d19 row 17), the
// offset continuation origin, while NEE is evaluated from offset_ray_origin(P, Ng, wo) as pbrt
// SampleLd does for a reflection-only BSDF. For a reflected continuation the two points are the
// same float point (both are pushed along the wo-facing Ng), so the weights of the two strategies
// sum to 1 exactly. Where they differ (a transmitted continuation, M1b on) the offset is a few
// float ulps of the coordinates (geom/ray_offset.h), so the light pdf ratio d^2 / cos moves by a
// relative amount of order offset / distance: about 1e-5 at distance 1e-2 for coordinates of 1 m,
// far below every harness threshold.
// Ref: Veach and Guibas 1995, "Optimally Combining Sampling Techniques for Monte Carlo
// Rendering" (balance heuristic); pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::Li
// (emitter hits, r_u / r_l update) and VolPathIntegrator::SampleLd (Apache-2.0); pbr-book 4e
// section 14.2; docs/decisions/0004-integrator.md D1; RULINGS.md 5.
#pragma once

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../portability.h"

AR_NS_BEGIN

// The factor NEE multiplies beta f |cos| Le by: 1 / avg(r_u (p_l + p_b)), or 1 / avg(r_u p_l) for
// a delta light. p_light is pmf times the solid-angle pdf; 0 when p_light is 0 (no sample).
AR_FN Real mis_nee_scale(const Spec& r_u, Real p_light, Real p_bsdf, bool delta_light) {
  if (!(p_light > Real(0))) return Real(0);
  const Real denom = avg(r_u * (delta_light ? p_light : p_light + p_bsdf));
  return denom > Real(0) ? Real(1) / denom : Real(0);
}

// The factor an emitter hit multiplies beta Le by. `weighted` is false at the first vertex and
// after a delta bounce; p_light is pmf(ctx_prev, light) pdf_li(ctx_prev, wi), 0 for a light the
// sampler can never pick (0004 D9: pmf 0, weight 1).
AR_FN Real mis_emitter_scale(const Spec& r_u, const Spec& r_l, Real p_light, bool weighted) {
  const Real denom = weighted ? avg(r_u + r_l * p_light) : avg(r_u);
  return Real(1) / denom;
}

AR_NS_END
