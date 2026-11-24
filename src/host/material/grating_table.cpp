// SPDX-License-Identifier: Apache-2.0
// Sinusoidal grating efficiencies by scalar thin-phase theory (0005 item 24).
//
// A thin sinusoidal phase grating with peak-to-peak phase modulation phi diffracts a fraction
// J_m^2(phi / 2) of the incident power into order m (Jacobi-Anger expansion of exp(i (phi / 2)
// sin(2 pi x / d)); J. C. Wyant, Optics 505 notes "Sinusoidal Phase Grating"; Gaylord and Moharam
// 1985, Proc. IEEE 73(5), eqs. 73 and 84; Goodman 2005).
//
// Phase depth from the groove depth h, PROVISIONAL (lead ruling of task 169):
//   reflection    phi = 4 pi h cos(theta_i) / lambda: the path difference 2 h sin(alpha) of two
//                 rays reflected at heights h apart, alpha the grazing angle (= 90 deg - theta_i),
//                 P. Zhao, "X-ray Scattering from Random Rough Surfaces", arXiv:1704.03783,
//                 section 2, eqs. 1-2 (the Rayleigh roughness phase). At normal incidence it gives
//                 Palmer's scalar-domain peaks: first order at lambda = 3.41 h, second at 2.05 h
//                 (Diffraction Grating Handbook, 7th ed., sections 9.3 and 9.5).
//   transmission phi = 2 pi h |n_s - 1| / lambda, independent of angle: the thin-element phase
//                 t = exp[i k (n - 1) h] of MIT 2.71 (Spring 2014) lecture 14 notes, eq. 9
//                 (Goodman 2005, section 5.1); the ambient index is 1 (grating.h).
// Scalar theory holds for small groove modulation: Palmer 7th ed. section 9.4 (after Loewen,
// Neviere and Maystre 1978) calls sinusoidal gratings with h / d < 0.10 scalar-domain, so a
// profile at or above that is refused, PROVISIONAL. It is a depth-over-pitch rule, not d / lambda.
// The efficiencies are times the coating reflectance or the substrate transmittance; every cell
// whose sum over the table's orders exceeds 1 is scaled to 1 and reported. The sum over all table
// orders bounds the sum over the propagating ones at every azimuth and, the lookup being a convex
// combination of nodes, between nodes too.
// Bessel J_m: std::cyl_bessel_j in double (C++17 special functions).
#include "grating_table.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

#include "../../kernel/material/ext/grating_table.h"
#include "astroray/astroray.h"

namespace astroray::host {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Palmer, Diffraction Grating Handbook 7th ed., section 9.4 (Loewen, Neviere, Maystre 1978).
constexpr double kScalarModulationLimit = 0.10;
// Rounding allowance of a sum of at most 17 terms in [0, 1]: 17 ulps of 1 is below 32 epsilon.
constexpr double kSumRounding = 32.0 * std::numeric_limits<double>::epsilon();

[[noreturn]] void refuse(const std::string& why) { throw std::runtime_error("grating table: " + why); }

std::string num(double v) {
  char b[64];
  std::snprintf(b, sizeof(b), "%.6g", v);
  return b;
}

}  // namespace

double grating_phase_pp(bool transmit, double depth_nm, double lambda_nm, double cos_i, double n_sub) {
  if (transmit) return 2.0 * kPi * depth_nm * std::fabs(n_sub - 1.0) / lambda_nm;
  return 4.0 * kPi * depth_nm * cos_i / lambda_nm;
}

GratingTableResult build_grating_table(const GratingTableSpec& s) {
  if (!(std::isfinite(s.pitch_nm) && s.pitch_nm > 0.0)) refuse("pitch_nm " + num(s.pitch_nm) + " is not finite and positive");
  if (!(std::isfinite(s.depth_nm) && s.depth_nm >= 0.0)) refuse("depth_nm " + num(s.depth_nm) + " is not finite and >= 0");
  if (s.m_min < -8 || s.m_max > 8 || s.m_min > s.m_max)
    refuse("orders [" + std::to_string(s.m_min) + ", " + std::to_string(s.m_max) + "] need -8 <= m_min <= m_max <= 8");
  if (!s.coating) refuse("no coating reflectance or substrate transmittance");
  if (s.transmit && !s.substrate_ior) refuse("a transmission table needs the substrate index");
  const double modulation = s.depth_nm / s.pitch_nm;
  if (!(modulation < kScalarModulationLimit))
    refuse("scalar theory out of range: groove modulation h/d = " + num(modulation) + " is not below " +
           num(kScalarModulationLimit) + " (Palmer, Diffraction Grating Handbook 7th ed. 9.4)");

  GratingTableResult r;
  r.m_min = s.m_min;
  r.order_count = s.m_max - s.m_min + 1;
  r.eta.assign(size_t(r.order_count) * ar::kGratingCosBins * ar::kGratingWavelengths, 0.0f);
  double worst = 0.0;
  for (int w = 0; w < ar::kGratingWavelengths; ++w) {
    const double lambda = double(ar::kGratingLambdaMin) + double(ar::kGratingLambdaStep) * w;
    double n_sub = 1.0;
    if (s.transmit) {
      n_sub = s.substrate_ior(lambda);
      if (!(std::isfinite(n_sub) && n_sub > 0.0)) refuse("substrate index " + num(n_sub) + " at " + num(lambda) + " nm");
    }
    for (int c = 0; c < ar::kGratingCosBins; ++c) {
      const double cos_i = double(c) / double(ar::kGratingCosBins - 1);
      const double t = s.coating(lambda, cos_i);
      if (!(std::isfinite(t) && t >= 0.0)) refuse("coating value " + num(t) + " at " + num(lambda) + " nm, cos " + num(cos_i));
      const double half = 0.5 * grating_phase_pp(s.transmit, s.depth_nm, lambda, cos_i, n_sub);
      double eta[ar::kGratingMaxOrders];
      double sum = 0.0;
      for (int o = 0; o < r.order_count; ++o) {
        // J_{-m}^2 = J_m^2: J_{-m} = (-1)^m J_m.
        const double j = std::cyl_bessel_j(double(std::abs(s.m_min + o)), half);
        eta[o] = t * j * j;
        sum += eta[o];
      }
      // A lossless coating gives sum_m J_m^2 <= 1 exactly; its double sum may round a few ulps
      // above 1, which is not a clip.
      const bool clip = sum > 1.0 + kSumRounding;
      const double scale = clip ? 1.0 / sum : 1.0;
      if (clip) {
        ++r.clipped_cells;
        worst = std::fmax(worst, sum);
      }
      for (int o = 0; o < r.order_count; ++o) r.eta[ar::grating_table_index(o, c, w)] = float(eta[o] * scale);
    }
  }
  if (r.clipped_cells > 0) {
    const size_t cells = size_t(ar::kGratingCosBins) * ar::kGratingWavelengths;
    r.report_code = AR_REPORT_GRATING_TABLE_LIMIT;
    r.report_detail = std::to_string(r.clipped_cells) + " of " + std::to_string(cells) +
                      " cells clipped to sum eta <= 1 (largest sum " + num(worst) + ")";
  }
  return r;
}

}  // namespace astroray::host
