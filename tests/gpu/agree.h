// SPDX-License-Identifier: Apache-2.0
// The CPU/GPU agreement harness (0009 d17 ladder steps ii and iii). A function or stage is run
// on 10^6 fixed host-generated inputs through the no-FMA GPU module, the production GPU module,
// CPU float and CPU double; the per-input errors are reduced to max and p99.9, and 0002 D12's
// thresholds and outlier rule decide. Each stage task adds its own fixture with this harness.
// Ref: docs/decisions/0002-kernel-architecture.md D12 (presets, outlier rule, raw f never
// gated), C1 (why p99.9), D4 (per-stage fixture); 0009 d17 and Verification row 17;
// docs/spikes/A-kernel-budget/RESULTS.md section 6 (protocol: shared host inputs, 4-lane
// relative L2 error, angle in radians for directions).
#pragma once
#include <cstdint>

#include "core/real.h"
#include "portability.h"

AR_NS_BEGIN

// a*b - c*d with near-equal products: exactly the cancellation an FMA contraction changes
// (fma(a, b, -(c*d)) keeps the rounding error of c*d that the two separate roundings drop).
// It is the detector's function (0009 Verification row 17).
AR_FN Real agree_cancel(Real a, Real b, Real c, Real d) { return a * b - c * d; }

AR_NS_END

#if !defined(__CUDACC__)
#include <string>
#include <vector>

namespace astroray::host {
class ModuleDevice;  // the f64 translation unit needs no device header
}

namespace astroray::agree {

inline constexpr uint32_t kInputs = 1000000;  // 0002 D12: 10^6 inputs per stage
// One fixed seed for every function's inputs; the draws are the sampler's, never a rand().
inline constexpr uint32_t kSeed = 0x2f0b1a5du;

// The input-th value of dimension `dim` in [0, 1): sample_independent(input, 0, 0, dim, kSeed),
// an exact multiple of 2^-24. Defined in agree.cpp.
double unit(uint32_t input, uint32_t dim);

// Row-major: `stride` floats per input.
struct Inputs {
  uint32_t n = 0, stride = 0;
  std::vector<float> v;
};
// What an implementation returned for each input: `stride` values per input, widened to double
// (a float widens exactly), so one comparison serves every build.
struct Results {
  uint32_t n = 0, stride = 0;
  std::vector<double> v;
};

enum class Metric {
  REL_L2,  // ||a - b|| / ||b|| over the stride values (4 lanes for a spectrum); b is the reference
  ANGLE,   // radians between two 3-vectors (stride 3); needs no normalised input
  EQUAL,   // 0 when the stride-1 values are equal, else 1 (lobe and light choices)
};

// Per-input errors reduced. A NaN anywhere in an input gives infinity: a non-finite result
// is never skipped (0004 D17). p999 is the nearest-rank value, sorted[ceil(0.999 n) - 1], so
// the largest n/1000 errors are the ones it ignores.
struct Stats {
  uint32_t n = 0;
  double max = 0.0, p999 = 0.0;
  std::vector<double> sorted;  // ascending
  uint32_t count_above(double x) const;
  double fraction_above(double x) const { return n ? double(count_above(x)) / double(n) : 0.0; }
};

// From the per-input errors themselves.
Stats stats_from(std::vector<double> errors);
// Throws std::invalid_argument when the shapes differ or the stride does not fit the metric.
Stats compare(const Results& a, const Results& b, Metric metric);

enum class Cls { EVAL, THROUGHPUT, DIRECTION, CHOICE };
inline constexpr double kNoLimit = -1.0;  // the preset states no threshold for this entry

// 0002 D12, exactly. eval_max and eval_p999 apply to Cls::EVAL, and so on; choice_frac is the
// largest allowed fraction of inputs whose choice differs.
struct Preset {
  const char* name;
  double eval_max, eval_p999, throughput_p999, direction_p999, choice_frac;
};
inline constexpr Preset NOFMA_VS_CPU_FLOAT{"nofma vs cpu float", 1e-5, kNoLimit, 1e-4, 1e-5, 1e-5};
inline constexpr Preset PROD_VS_CPU_F64{"prod vs cpu f64", kNoLimit, 1e-4, 5e-4, 1e-5, kNoLimit};
// 0002 D12 outliers: fewer than 0.01% of inputs may exceed 10x a p99.9 threshold.
inline constexpr double kOutlierFactor = 10.0;
inline constexpr uint64_t kOutlierDenominator = 10000;  // 0.01% = 1 / 10000

struct Verdict {
  bool pass = true;
  std::string why;  // every failed rule, empty on pass
};
// Applies the class's thresholds and the outlier rule. Throws std::logic_error when the preset
// states no threshold for the class (nothing is silently unchecked).
Verdict judge(const Stats& s, Cls cls, const Preset& preset);
// Prints one line (function, step, max, p99.9, inputs above 10x the p99.9 threshold) and
// returns judge()'s verdict.
Verdict report(const char* function, const char* step, const Stats& s, Cls cls, const Preset& preset);

// Runs `kernel` of the module file `module_path` on `in` and returns out_stride floats per input.
// The kernel takes (const float* in, float* out, uint32_t n) followed by `extra`, one pointer to
// each further parameter's value. Defined in agree.cpp.
Results run_gpu(host::ModuleDevice& dev, const std::string& module_path, const char* kernel, const Inputs& in,
                uint32_t out_stride, const std::vector<void*>& extra = {});

// The Jakob-Hanika table as the CPU references see it: the f32 and f64 translation units each
// build their own kernel-side view from these three fields.
struct TableRef {
  int32_t res = 0;
  const float* z_nodes = nullptr;
  const float* coeffs = nullptr;
};

// CPU double references, defined in test_agree_pure_f64.cpp (compiled with AR_REAL_DOUBLE);
// the float ones are in test_agree_pure.cpp. Out stride is 4 lanes.
Results cpu_planck_f64(const Inputs& in);
Results cpu_sigmoid_f64(const Inputs& in, const TableRef& table);

}  // namespace astroray::agree
#endif
