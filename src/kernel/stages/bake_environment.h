// SPDX-License-Identifier: Apache-2.0
// bake_environment: the environment importance grid of 0004 D11. One thread per cell of an n x n
// equal-area grid; the cell holds the world radiance at the cell's centre direction, summed over
// the observer grid with w(lambda) (the CMF sum over its total, D10 "Importance weighting"), so a
// constant world's cell equals the host's display_energy of the world. The host reads the grid
// back, normalises it by its maximum and builds the sampling table (host/light/env_bake.h).
// Cell index iv n + iu, the layout host/light/env_table.h consumes. The excitation term of
// w(lambda) is absent until FLUORESCENT exists (D10).
//
// The 471 observer samples are visited kLanes at a time (one Spec evaluation per visit) and
// reduced with Neumaier's compensated sum in Real: a kernel holds no double (the production
// modules are built with ptxas --warn-on-double-precision-use, cmake/kernels.cmake), so the
// accumulation cannot simply be widened. The compensation keeps the reduction's own error near one
// rounding instead of the (n - 1) u of a plain float sum (n = 118 visits of 4 lanes), so the bound
// below is dominated by the world callable. Bound against an exact (double) evaluation of the same
// sum, with u = 2^-24 and e_s the world callable's own relative error (a positive spectrum, so the
// terms never cancel):
//   numerator terms s c        e_s + 2 u (c = (x + y) + z in float) + u (product)
//   the compensated sums       u each, plus gamma_{n-1}^2 sum |x| (Neumaier 1974), with n = 471
//                              terms: gamma_{n-1} = (n - 1) u / (1 - (n - 1) u), so the second
//                              order part is at most 7.9e-10 of the sum
//   the weight total W         2 u (the float c) + u
//   the division               u
//   total                      e_s + 8 u + 2 x 7.9e-10
// The host reads the f32 grid, which is the Real result itself when Real is float. The tests state
// the instance of this bound for their callable.
// Ref: docs/decisions/0004-integrator.md D10, D11; Neumaier 1974, "Rundungsfehleranalyse einiger
// Verfahren zur Summation endlicher Summen", ZAMM 54 (the compensated sum); pbrt-v4
// src/pbrt/lights.cpp ImageInfiniteLight (the grid evaluated per cell) and
// third_party/_cache/refs/pbrt-v4-092b23a47ad4/src/pbrt/util/math.cpp EqualAreaSquareToSphere.
#pragma once
#include <cstdint>

#include "../color/observer.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../kernel_globals.h"
#include "../light/equal_area.h"
#include "../light/light_record.h"
#include "../light/light_sampler.h"
#include "../portability.h"
#include "../vm/context.h"

AR_NS_BEGIN

// Neumaier's improved Kahan-Babuska sum: the lost low part of every addition is collected in `c`
// whichever operand is larger. No fast-math reassociation exists in the build (0002 D12), so the
// compiler keeps the order written here.
struct NeumaierSum {
  Real s = Real(0), c = Real(0);
  AR_FN void add(Real x) {
    const Real t = s + x;
    c += ar::abs(s) >= ar::abs(x) ? (s - t) + x : (x - t) + s;
    s = t;
  }
  AR_FN Real value() const { return s + c; }
};

// The direction of the centre of cell `iv n + iu`: the equal-area square point
// ((iu + 0.5) / n, (iv + 0.5) / n). The one place it is computed; the host names a bad cell by it.
AR_FN Real3 bake_environment_direction(uint32_t n, uint32_t cell) {
  const uint32_t iv = cell / n, iu = cell - iv * n;
  return equal_area_square_to_sphere(Real2{(Real(iu) + Real(0.5)) / Real(n), (Real(iv) + Real(0.5)) / Real(n)});
}

// The cell's value for a world given as `world(direction, lambdas) -> Spec`: radiance per
// steradian at the lanes' wavelengths. A cell of a world that is black at every wavelength, or an
// observer whose weights sum to 0, is 0; a non-finite radiance gives a non-finite cell, which the
// host refuses (rule 5).
template <class WorldFn>
AR_FN Real bake_environment_cell(const WorldFn& world, const ObserverView& obs, uint32_t n, uint32_t cell) {
  const Real3 dir = bake_environment_direction(n, cell);
  NeumaierSum weighted, total;
  for (int i = 0; i < kObsSamples; i += kLanes) {
    Lambda lam;
    Real c[kLanes];
    for (int k = 0; k < kLanes; ++k) {
      // The lanes of a short final visit repeat the last sample's wavelength and are skipped
      // below, as display_energy does.
      const int j = i + k < kObsSamples ? i + k : kObsSamples - 1;
      lam.nm[k] = Real(kObsLambdaMin + j);
      c[k] = load_ro(obs.cmf + j) + load_ro(obs.cmf + kObsSamples + j) + load_ro(obs.cmf + 2 * kObsSamples + j);
    }
    const Spec s = world(dir, lam);
    for (int k = 0; k < kLanes && i + k < kObsSamples; ++k) {
      weighted.add(s.v[k] * c[k]);
      total.add(c[k]);
    }
  }
  const Real w = total.value();
  return w > Real(0) ? weighted.value() / w : Real(0);
}

// M1b's world is a constant radiance (the world light record); 172 replaces this callable with the
// world program.
struct ConstantWorld {
  const LightRecord* record;
  const ObserverView* observer;
  AR_FN Spec operator()(const Real3&, const Lambda& lam) const { return light_emission(*record, lam, *observer); }
};

// The launch size, read from the device-resident KernelGlobals (the host's `count` only sizes a
// GPU grid, 0002 D4).
AR_FN uint32_t bake_environment_count(const KernelGlobals& kg) { return kg.env_bake.n * kg.env_bake.n; }

// A scene without a world is black: its cells are 0 and the host reports no table.
AR_FN void bake_environment_write(const KernelGlobals& kg, uint32_t cell) {
  AR_ASSERT(kg.env_bake.grid != nullptr && cell < bake_environment_count(kg));
  Real v = Real(0);
  if (kg.world_light != kNoLight) {
    AR_ASSERT(kg.lights != nullptr && kg.tables != nullptr);
    const ObserverView& obs = kg.tables->observer;
    v = bake_environment_cell(ConstantWorld{&kg.lights[kg.world_light], &obs}, obs, kg.env_bake.n, cell);
  }
  kg.env_bake.grid[cell] = float(v);
}

AR_NS_END

// CPU chunk function: cells [first, last). Defined in kernel_cpu/bake_environment.cpp.
void cpu_bake_environment(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
