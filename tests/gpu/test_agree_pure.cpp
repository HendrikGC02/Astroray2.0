// SPDX-License-Identifier: Apache-2.0
// The agreement harness and its first users: planck_radiance and sigmoid_eval(fetch_coeffs(rgb))
// on 10^6 fixed inputs, step (ii) no-FMA GPU vs CPU float and step (iii) production GPU vs CPU
// double (0009 d17), and the detector check of Verification row 17 (a no-FMA module built with
// FMA on trips step (ii) on a cancellation-sensitive function).
// Expected values are typed from the setups below, never read back from the code under test:
// the unit() pins and input rows come from a pure-Python model of pcg4d and of the generator
// rules; the metric and threshold pins are hand arithmetic.
// Ref: docs/decisions/0002-kernel-architecture.md D12, C1, D4; 0009 d17, Verification row 17;
// Planck: 0003 d25 (domain; B >= 1e-30 of the peak as in tests/cpu/test_planck.cpp);
// Jakob-Hanika lookup: 0003 d16, ar::fetch_coeffs.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agree.h"
#include "color/rgb2spec.h"
#include "host/device.h"
#include "spectral/planck.h"

using namespace astroray;
using namespace astroray::agree;
using astroray::host::DeviceBuffer;
using astroray::host::Ledger;
using astroray::host::LedgerCategory;
using astroray::host::Owner;

namespace {

constexpr double kInfD = std::numeric_limits<double>::infinity();

std::string prod_module(const char* name) { return std::string(AR_TEST_KERNEL_DIR) + "/" + name + ".cubin"; }
// The no-FMA build; with AR_AGREE_CANARY_FMA it is the same module built with --fmad=true.
std::string nofma_module(const char* name) { return std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/" + name + ".cubin"; }

// ---- inputs ---------------------------------------------------------------------------------

// B(lambda, T) in double from the production formula. The peak is where x = c2 / (lambda T)
// solves x = 5 (1 - e^-x) (Wien), found by iterating that map, so no constant beyond the
// record's c2 enters. Ref: 0003 d25.
double planck_double(double lambda_nm, double T) { return ar::planck_radiance_t<double>(lambda_nm, T); }

double planck_peak(double T) {
  double x = 5.0;
  for (int i = 0; i < 30; ++i) x = 5.0 * (1.0 - std::exp(-x));
  return planck_double(ar::kPlanckC2 * ar::kNmPerUm / (x * T), T);
}

// Row: lambda nm x4, T K. T is log-uniform over [3 K, 1e9 K] and each lambda log-uniform over
// [0.1 nm, 1e9 nm] (0003 d25's domain), redrawn from the next sampler dimensions until
// B >= 1e-30 of the peak at T (the range of the record's check 10) and B is a normal float: a
// subnormal has fewer than 24 significand bits, so no relative bound applies to it.
Inputs make_planck_inputs(uint32_t n) {
  Inputs in;
  in.n = n;
  in.stride = 5;
  in.v.resize(size_t(n) * 5u);
  for (uint32_t i = 0; i < n; ++i) {
    float* row = in.v.data() + size_t(i) * 5u;
    row[4] = static_cast<float>(3.0 * std::pow(10.0, std::log10(1e9 / 3.0) * unit(i, 0)));
    const double bpeak = planck_peak(double(row[4]));
    for (uint32_t j = 0; j < 4; ++j) {
      bool found = false;
      for (uint32_t attempt = 0; attempt < 64 && !found; ++attempt) {
        const float lambda = static_cast<float>(std::pow(10.0, -1.0 + 10.0 * unit(i, 1 + 4 * attempt + j)));
        const double b = planck_double(double(lambda), double(row[4]));
        if (b >= 1e-30 * bpeak && b >= double(std::numeric_limits<float>::min())) {
          row[j] = lambda;
          found = true;
        }
      }
      if (!found) throw std::runtime_error("agree: no valid planck lambda in 64 draws for input " + std::to_string(i));
    }
  }
  return in;
}

// Row: rgb in [0, 1) x3, lambda nm x4 uniform over the observer's [360, 830] (0003 d16).
Inputs make_sigmoid_inputs(uint32_t n) {
  Inputs in;
  in.n = n;
  in.stride = 7;
  in.v.resize(size_t(n) * 7u);
  for (uint32_t i = 0; i < n; ++i) {
    float* row = in.v.data() + size_t(i) * 7u;
    for (uint32_t d = 0; d < 3; ++d) row[d] = static_cast<float>(unit(i, d));
    for (uint32_t j = 0; j < 4; ++j) row[3 + j] = static_cast<float>(360.0 + 470.0 * unit(i, 3 + j));
  }
  return in;
}

// Row: a, b, c, d x4 each with a, b, d in [1, 2) and c = a b / d rounded to float, so a*b and
// c*d agree to a few ulps and the difference is a rounding residue.
Inputs make_cancel_inputs(uint32_t n) {
  Inputs in;
  in.n = n;
  in.stride = 16;
  in.v.resize(size_t(n) * 16u);
  for (uint32_t i = 0; i < n; ++i) {
    float* row = in.v.data() + size_t(i) * 16u;
    for (uint32_t j = 0; j < 4; ++j) {
      row[j] = static_cast<float>(1.0 + unit(i, j));
      row[4 + j] = static_cast<float>(1.0 + unit(i, 4 + j));
      row[12 + j] = static_cast<float>(1.0 + unit(i, 12 + j));
      row[8 + j] = static_cast<float>(double(row[j]) * double(row[4 + j]) / double(row[12 + j]));
    }
  }
  return in;
}

// ---- CPU float references (the double ones are in test_agree_pure_f64.cpp) ----------------------

Results cpu_planck_f32(const Inputs& in) {
  Results r;
  r.n = in.n;
  r.stride = 4;
  r.v.resize(size_t(in.n) * 4u);
  for (uint32_t i = 0; i < in.n; ++i) {
    const float* p = in.v.data() + size_t(i) * in.stride;
    for (int j = 0; j < 4; ++j) r.v[size_t(i) * 4u + j] = ar::planck_radiance(p[j], p[4]);
  }
  return r;
}

ar::Rgb2SpecTable table_of(const TableRef& ref) {
  ar::Rgb2SpecTable t = {};
  t.res = ref.res;
  t.z_nodes = ref.z_nodes;
  t.coeffs = ref.coeffs;
  return t;
}

Results cpu_sigmoid_f32(const Inputs& in, const TableRef& ref) {
  const ar::Rgb2SpecTable t = table_of(ref);
  Results r;
  r.n = in.n;
  r.stride = 4;
  r.v.resize(size_t(in.n) * 4u);
  for (uint32_t i = 0; i < in.n; ++i) {
    const float* p = in.v.data() + size_t(i) * in.stride;
    const ar::SigmoidCoeffs c = ar::fetch_coeffs(t, ar::Rgb{{p[0], p[1], p[2]}});
    for (int j = 0; j < 4; ++j) r.v[size_t(i) * 4u + j] = ar::sigmoid_eval(c, p[3 + j]);
  }
  return r;
}

Results cpu_cancel_f32(const Inputs& in) {
  Results r;
  r.n = in.n;
  r.stride = 4;
  r.v.resize(size_t(in.n) * 4u);
  for (uint32_t i = 0; i < in.n; ++i) {
    const float* p = in.v.data() + size_t(i) * in.stride;
    for (int j = 0; j < 4; ++j) r.v[size_t(i) * 4u + j] = ar::agree_cancel(p[j], p[4 + j], p[8 + j], p[12 + j]);
  }
  return r;
}

// ---- fixtures: inputs and CPU references, built once per process --------------------------------

struct PlanckSet {
  Inputs in;
  Results f32, f64;
  PlanckSet() : in(make_planck_inputs(kInputs)), f32(cpu_planck_f32(in)), f64(cpu_planck_f64(in)) {}
};
const PlanckSet& planck_set() {
  static const PlanckSet s;
  return s;
}

std::vector<uint8_t> read_jh() {
  const std::string here = __FILE__;
  const std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/spectral/rec709.jh", std::ios::binary);
  if (!f) f.open("data/spectral/rec709.jh", std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/spectral/rec709.jh not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct SigmoidSet {
  std::vector<uint8_t> jh;
  ar::Rgb2SpecTable table = {};
  TableRef ref;
  Inputs in;
  Results f32, f64;
  SigmoidSet() : jh(read_jh()), in(make_sigmoid_inputs(kInputs)) {
    const char* err = ar::jh_view(jh.data(), jh.size(), &table);
    REQUIRE_MESSAGE(err == nullptr, (err ? err : ""));
    ref = TableRef{table.res, table.z_nodes, table.coeffs};
    f32 = cpu_sigmoid_f32(in, ref);
    f64 = cpu_sigmoid_f64(in, ref);
  }
};
const SigmoidSet& sigmoid_set() {
  static const SigmoidSet s;
  return s;
}

struct CancelSet {
  Inputs in;
  Results f32;
  CancelSet() : in(make_cancel_inputs(kInputs)), f32(cpu_cancel_f32(in)) {}
};
const CancelSet& cancel_set() {
  static const CancelSet s;
  return s;
}

// The table on the device for one launch of ar_agree_sigmoid.
Results run_sigmoid(host::ModuleDevice& dev, const std::string& module, const SigmoidSet& s) {
  const uint64_t z_bytes = sizeof(float) * uint64_t(s.table.res);
  const uint64_t c_bytes = sizeof(float) * 9ull * uint64_t(s.table.res) * s.table.res * s.table.res;
  DeviceBuffer z = dev.allocate(LedgerCategory::TABLES, z_bytes, Owner{});
  DeviceBuffer c = dev.allocate(LedgerCategory::TABLES, c_bytes, Owner{});
  dev.upload(z, 0, s.table.z_nodes, z_bytes);
  dev.upload(c, 0, s.table.coeffs, c_bytes);
  uint64_t z_p = reinterpret_cast<uint64_t>(z.ptr), c_p = reinterpret_cast<uint64_t>(c.ptr);
  int32_t res = s.table.res;
  Results r = run_gpu(dev, module, "ar_agree_sigmoid", s.in, 4, {&z_p, &c_p, &res});
  dev.free(z);
  dev.free(c);
  return r;
}

Results stride_results(uint32_t stride, std::vector<double> v) {
  Results r;
  r.stride = stride;
  r.n = static_cast<uint32_t>(v.size() / stride);
  r.v = std::move(v);
  return r;
}

// n errors of 0 with `count` of them replaced by `value` (the largest ones).
Stats stats_with(uint32_t n, uint32_t count, double value) {
  std::vector<double> e(n, 0.0);
  for (uint32_t i = 0; i < count; ++i) e[i] = value;
  return stats_from(std::move(e));
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("agree/inputs fixed") {
  // unit() is the sampler's pcg4d: sample_independent(input, 0, 0, dim, 0x2f0b1a5d), from a
  // pure-Python model of pcg4d (Jarzynski and Olano 2020).
  CHECK(unit(0, 0) == 0.8126653432846069);
  CHECK(unit(1, 3) == 0.39199143648147583);
  CHECK(unit(999999, 7) == 0.7884563207626343);
  CHECK(unit(12345, 100) == 0.2875267267227173);
  CHECK(kInputs == 1000000u);

  // Planck rows from the Python model of the generator (T, then each lambda redrawn until
  // B >= 1e-30 of the peak): input 0 needed a second draw for lane 2.
  const PlanckSet& p = planck_set();
  REQUIRE(p.in.n == 1000000u);
  REQUIRE(p.in.stride == 5u);
  const float* r0 = p.in.v.data();
  CHECK(r0[0] == 2.8678386211395264f);
  CHECK(r0[1] == 0.26085540652275085f);
  CHECK(r0[2] == 0.3648763597011566f);
  CHECK(r0[3] == 2.944918632507324f);
  CHECK(r0[4] == 25314504.0f);
  const float* r1 = p.in.v.data() + 5;
  CHECK(r1[0] == 20754644.0f);
  CHECK(r1[1] == 501633.34375f);
  CHECK(r1[2] == 10207976.0f);
  CHECK(r1[3] == 62283708.0f);
  CHECK(r1[4] == 183.41940307617188f);
  const float* rl = p.in.v.data() + 5ull * 999999u;
  CHECK(rl[0] == 15.938660621643066f);
  CHECK(rl[1] == 2281.704833984375f);
  CHECK(rl[2] == 3622.32666015625f);
  CHECK(rl[3] == 30.624303817749023f);
  CHECK(rl[4] == 6821998.5f);

  // Every row is in the domain and in the 1e-30 range, judged with the record's Wien constant
  // b = 2.897771955e-3 m K (CODATA 2018, as tests/cpu/test_planck.cpp), a different route to the
  // peak than the generator's; the tolerance is the 1e-9 flatness of the peak.
  uint32_t outside = 0;
  for (uint32_t i = 0; i < p.in.n; ++i) {
    const float* r = p.in.v.data() + size_t(i) * 5u;
    const double T = r[4];
    const double peak = planck_double(2897771.955 / T, T);
    if (!(T >= 3.0 && T <= 1e9)) ++outside;
    for (int j = 0; j < 4; ++j) {
      const double b = planck_double(double(r[j]), T);
      if (!(r[j] >= 0.1f && r[j] <= 1e9f && b >= 1e-30 * peak * (1.0 - 1e-6))) ++outside;
    }
  }
  CHECK(outside == 0u);

  const SigmoidSet& s = sigmoid_set();
  REQUIRE(s.in.n == 1000000u);
  REQUIRE(s.in.stride == 7u);
  const float* s0 = s.in.v.data();
  CHECK(s0[0] == 0.8126653432846069f);
  CHECK(s0[1] == 0.14575546979904175f);
  CHECK(s0[2] == 0.04163998365402222f);
  CHECK(s0[3] == 827.3960571289062f);
  CHECK(s0[4] == 429.04644775390625f);
  CHECK(s0[5] == 426.6958923339844f);
  CHECK(s0[6] == 670.5704345703125f);
  const float* sl = s.in.v.data() + 7ull * 999999u;
  CHECK(sl[0] == 0.745850145816803f);
  CHECK(sl[1] == 0.22024518251419067f);
  CHECK(sl[2] == 0.4358259439468384f);
  CHECK(sl[3] == 574.2723999023438f);
  CHECK(sl[6] == 657.1422119140625f);
}

TEST_CASE("agree/compare metrics") {
  // Relative L2 over the 4 lanes, reference second: |(0.3, 0.4)| / |(6, 8)| = 0.5 / 10.
  {
    const Results a = stride_results(4, {6.3, 8.4, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0});
    const Results b = stride_results(4, {6, 8, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0});
    const Stats s = compare(a, b, Metric::REL_L2);
    REQUIRE(s.n == 3u);
    CHECK(std::fabs(s.sorted[2] - 0.05) < 1e-15);
    CHECK(s.sorted[1] == 0.0);  // identical
    CHECK(s.sorted[0] == 0.0);  // zero against zero
    CHECK(s.max == s.sorted[2]);
    // The second argument is the reference: swapping gives 0.5 / |(6.3, 8.4)| = 0.5 / 10.5.
    const Stats sw = compare(b, a, Metric::REL_L2);
    CHECK(std::fabs(sw.max - 0.5 / 10.5) < 1e-15);
  }
  // Lanes add in quadrature: a - b = (3, 0, 4, 0) against |b| = |(0, 0, 0, 10)| is 0.5.
  {
    const Results a = stride_results(4, {3, 0, 4, 10});
    const Results b = stride_results(4, {0, 0, 0, 10});
    CHECK(std::fabs(compare(a, b, Metric::REL_L2).max - 0.5) < 1e-15);
  }
  // Non-finite and zero-reference inputs are errors, never skipped.
  {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const Results a = stride_results(4, {nan, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, kInfD, 1, 1, 1});
    const Results b = stride_results(4, {1, 1, 1, 1, kInfD, 1, 1, 1, 0, 0, 0, 0, kInfD, 1, 1, 1});
    const Stats s = compare(a, b, Metric::REL_L2);
    REQUIRE(s.n == 4u);
    // A NaN lane, an infinite reference, a nonzero result against a zero reference, inf against inf.
    CHECK(s.count_above(1e300) == 4u);
  }
  // Directions: the angle in radians, from unnormalised vectors.
  {
    const double t = 1e-6;
    const Results a = stride_results(3, {1, 0, 0, 2, 0, 0, 5, 0, 0, 1, 0, 0});
    const Results b = stride_results(3, {0, 1, 0, 3, 0, 0, 7 * std::cos(t), 7 * std::sin(t), 0, -1, 0, 0});
    const Stats s = compare(a, b, Metric::ANGLE);
    REQUIRE(s.n == 4u);
    CHECK(std::fabs(s.sorted[3] - 3.14159265358979323846) < 1e-15);   // opposite
    CHECK(std::fabs(s.sorted[2] - 1.57079632679489661923) < 1e-15);   // orthogonal
    CHECK(std::fabs(s.sorted[1] - t) < 1e-15);                        // a tiny angle stays accurate
    CHECK(s.sorted[0] == 0.0);                                        // parallel, different lengths
  }
  // Choices: equal is 0, anything else 1, whatever the difference.
  {
    const Results a = stride_results(1, {3, 3, 7});
    const Results b = stride_results(1, {3, 4, 0});
    const Stats s = compare(a, b, Metric::EQUAL);
    CHECK(s.sorted == std::vector<double>({0.0, 1.0, 1.0}));
  }
  // Shape errors are refused loudly.
  CHECK_THROWS_AS(compare(stride_results(4, {1, 2, 3, 4}), stride_results(4, {1, 2, 3, 4, 5, 6, 7, 8}), Metric::REL_L2),
                  std::invalid_argument);
  CHECK_THROWS_AS(compare(stride_results(4, {1, 2, 3, 4}), stride_results(2, {1, 2, 3, 4}), Metric::REL_L2),
                  std::invalid_argument);
  CHECK_THROWS_AS(compare(stride_results(4, {1, 2, 3, 4}), stride_results(4, {1, 2, 3, 4}), Metric::ANGLE),
                  std::invalid_argument);
  CHECK_THROWS_AS(compare(stride_results(3, {1, 2, 3}), stride_results(3, {1, 2, 3}), Metric::EQUAL),
                  std::invalid_argument);

  // max, p99.9 (nearest rank, ceil(0.999 n)) and the strict count above a value.
  {
    std::vector<double> e(1000);
    for (int i = 0; i < 1000; ++i) e[size_t(i)] = double(999 - i);  // unsorted on purpose
    const Stats s = stats_from(e);
    CHECK(s.n == 1000u);
    CHECK(s.max == 999.0);
    CHECK(s.p999 == 998.0);  // rank 999: the single largest error is ignored
    CHECK(s.count_above(997.0) == 2u);
    CHECK(s.count_above(998.0) == 1u);  // strictly above
    CHECK(s.count_above(999.0) == 0u);
    CHECK(s.fraction_above(998.0) == 0.001);
    CHECK(stats_from(std::vector<double>(1001, 0.0)).p999 == 0.0);
    std::vector<double> e1(1001);
    for (int i = 0; i < 1001; ++i) e1[size_t(i)] = double(i);
    CHECK(stats_from(e1).p999 == 999.0);  // rank ceil(999.999) = 1000
    CHECK(stats_from({5.0}).p999 == 5.0);  // rank 1
    CHECK(stats_from({}).n == 0u);
  }
}

TEST_CASE("agree/judge rules") {
  // The presets are 0002 D12 exactly.
  CHECK(NOFMA_VS_CPU_FLOAT.eval_max == 1e-5);
  CHECK(NOFMA_VS_CPU_FLOAT.eval_p999 == kNoLimit);
  CHECK(NOFMA_VS_CPU_FLOAT.throughput_p999 == 1e-4);
  CHECK(NOFMA_VS_CPU_FLOAT.direction_p999 == 1e-5);
  CHECK(NOFMA_VS_CPU_FLOAT.choice_frac == 1e-5);
  CHECK(PROD_VS_CPU_F64.eval_max == kNoLimit);
  CHECK(PROD_VS_CPU_F64.eval_p999 == 1e-4);
  CHECK(PROD_VS_CPU_F64.throughput_p999 == 5e-4);
  CHECK(PROD_VS_CPU_F64.direction_p999 == 1e-5);
  CHECK(PROD_VS_CPU_F64.choice_frac == kNoLimit);
  CHECK(kOutlierFactor == 10.0);
  CHECK(kOutlierDenominator == 10000u);

  constexpr uint32_t N = 100000;  // 0.01% is 10 inputs; the p99.9 rank is 99900
  // No-FMA eval: max <= 1e-5, inclusive.
  CHECK(judge(stats_with(N, 1, 1e-5), Cls::EVAL, NOFMA_VS_CPU_FLOAT).pass);
  CHECK(!judge(stats_with(N, 1, std::nextafter(1e-5, 1.0)), Cls::EVAL, NOFMA_VS_CPU_FLOAT).pass);
  CHECK(judge(stats_with(N, 1, 1e-5), Cls::EVAL, NOFMA_VS_CPU_FLOAT).why.empty());
  // The max is the gate there: even one input over fails, however few (no p99.9 rule for eval).
  CHECK(judge(stats_with(N, 5000, 9e-6), Cls::EVAL, NOFMA_VS_CPU_FLOAT).pass);
  // Throughput p99.9 <= 1e-4: 100 errors above the limit are the 0.1% the rank ignores, 101 are not.
  CHECK(judge(stats_with(N, 100, 2e-4), Cls::THROUGHPUT, NOFMA_VS_CPU_FLOAT).pass);
  CHECK(!judge(stats_with(N, 101, 2e-4), Cls::THROUGHPUT, NOFMA_VS_CPU_FLOAT).pass);
  CHECK(judge(stats_with(N, 5000, 1e-4), Cls::THROUGHPUT, NOFMA_VS_CPU_FLOAT).pass);  // at the limit
  CHECK(!judge(stats_with(N, 5000, std::nextafter(1e-4, 1.0)), Cls::THROUGHPUT, NOFMA_VS_CPU_FLOAT).pass);
  // Outliers: fewer than 0.01% (10 of 1e5) above 10x the p99.9 limit (1e-3 here).
  CHECK(judge(stats_with(N, 9, 1.1e-3), Cls::THROUGHPUT, NOFMA_VS_CPU_FLOAT).pass);
  {
    const Verdict v = judge(stats_with(N, 10, 1.1e-3), Cls::THROUGHPUT, NOFMA_VS_CPU_FLOAT);
    CHECK(!v.pass);
    CHECK(v.why.find("10 inputs above 10x") != std::string::npos);
  }
  CHECK(judge(stats_with(N, 50, kOutlierFactor * 1e-4), Cls::THROUGHPUT, NOFMA_VS_CPU_FLOAT).pass);  // exactly 10x: not above
  CHECK(!judge(stats_with(N, 10, std::nextafter(kOutlierFactor * 1e-4, 1.0)), Cls::THROUGHPUT, NOFMA_VS_CPU_FLOAT).pass);
  // Directions: p99.9 <= 1e-5 rad and the same outlier rule at 1e-4 rad.
  CHECK(judge(stats_with(N, 100, 5e-5), Cls::DIRECTION, NOFMA_VS_CPU_FLOAT).pass);
  CHECK(!judge(stats_with(N, 101, 5e-5), Cls::DIRECTION, NOFMA_VS_CPU_FLOAT).pass);
  CHECK(judge(stats_with(N, 9, 2e-4), Cls::DIRECTION, NOFMA_VS_CPU_FLOAT).pass);
  CHECK(!judge(stats_with(N, 10, 2e-4), Cls::DIRECTION, NOFMA_VS_CPU_FLOAT).pass);
  // Production vs f64: throughput 5e-4 (outliers above 5e-3), eval p99.9 1e-4 (outliers above
  // 1e-3) and no eval max, direction 1e-5.
  CHECK(judge(stats_with(N, 100, 6e-4), Cls::THROUGHPUT, PROD_VS_CPU_F64).pass);
  CHECK(!judge(stats_with(N, 101, 6e-4), Cls::THROUGHPUT, PROD_VS_CPU_F64).pass);
  CHECK(judge(stats_with(N, 9, 6e-3), Cls::THROUGHPUT, PROD_VS_CPU_F64).pass);
  CHECK(!judge(stats_with(N, 10, 6e-3), Cls::THROUGHPUT, PROD_VS_CPU_F64).pass);
  CHECK(judge(stats_with(N, 100, 2e-4), Cls::EVAL, PROD_VS_CPU_F64).pass);
  CHECK(!judge(stats_with(N, 101, 2e-4), Cls::EVAL, PROD_VS_CPU_F64).pass);
  CHECK(judge(stats_with(N, 9, 1e3), Cls::EVAL, PROD_VS_CPU_F64).pass);  // the raw max of eval is not gated
  CHECK(!judge(stats_with(N, 10, 1e3), Cls::EVAL, PROD_VS_CPU_F64).pass);
  CHECK(judge(stats_with(N, 100, 5e-5), Cls::DIRECTION, PROD_VS_CPU_F64).pass);
  CHECK(!judge(stats_with(N, 101, 5e-5), Cls::DIRECTION, PROD_VS_CPU_F64).pass);
  // A non-finite error is above everything: it fails an eval max and counts as an outlier.
  CHECK(!judge(stats_with(N, 1, kInfD), Cls::EVAL, NOFMA_VS_CPU_FLOAT).pass);
  CHECK(!judge(stats_with(N, 10, kInfD), Cls::EVAL, PROD_VS_CPU_F64).pass);
  // Both failures are reported in one verdict.
  {
    const Verdict v = judge(stats_with(N, 200, 1.0), Cls::THROUGHPUT, NOFMA_VS_CPU_FLOAT);
    CHECK(!v.pass);
    CHECK(v.why.find("p99.9") != std::string::npos);
    CHECK(v.why.find("above 10x") != std::string::npos);
  }
  // Choices: at most 1e-5 of inputs differ (10 of 1e6), inclusive.
  {
    constexpr uint32_t M = 1000000;
    CHECK(judge(stats_with(M, 10, 1.0), Cls::CHOICE, NOFMA_VS_CPU_FLOAT).pass);
    CHECK(!judge(stats_with(M, 11, 1.0), Cls::CHOICE, NOFMA_VS_CPU_FLOAT).pass);
    CHECK(judge(stats_with(M, 0, 1.0), Cls::CHOICE, NOFMA_VS_CPU_FLOAT).pass);
  }
  // A class the preset has no threshold for is refused, never passed unchecked.
  CHECK_THROWS_AS(judge(stats_with(N, 0, 0.0), Cls::CHOICE, PROD_VS_CPU_F64), std::logic_error);
  const Preset empty{"empty", kNoLimit, kNoLimit, kNoLimit, kNoLimit, kNoLimit};
  CHECK_THROWS_AS(judge(stats_with(N, 0, 0.0), Cls::EVAL, empty), std::logic_error);
  CHECK_THROWS_AS(judge(stats_with(N, 0, 0.0), Cls::THROUGHPUT, empty), std::logic_error);
  CHECK_THROWS_AS(judge(stats_with(N, 0, 0.0), Cls::DIRECTION, empty), std::logic_error);
}

TEST_CASE("agree/planck nofma vs cpu float") {
  const PlanckSet& s = planck_set();
  Ledger ledger;
  auto dev = host::make_cuda_device(ledger, 0);
  const Results gpu = run_gpu(*dev, nofma_module("agree_pure"), "ar_agree_planck", s.in, 4);
  const Verdict v = report("planck", "(ii) nofma vs cpu float", compare(gpu, s.f32, Metric::REL_L2), Cls::EVAL,
                           NOFMA_VS_CPU_FLOAT);
  CHECK_MESSAGE(v.pass, v.why);
  CHECK(ledger.total() == 0);
}

TEST_CASE("agree/planck prod vs cpu f64") {
  const PlanckSet& s = planck_set();
  Ledger ledger;
  auto dev = host::make_cuda_device(ledger, 0);
  const Results gpu = run_gpu(*dev, prod_module("agree_pure"), "ar_agree_planck", s.in, 4);
  const Verdict v =
      report("planck", "(iii) prod vs cpu f64", compare(gpu, s.f64, Metric::REL_L2), Cls::EVAL, PROD_VS_CPU_F64);
  CHECK_MESSAGE(v.pass, v.why);
}

TEST_CASE("agree/sigmoid nofma vs cpu float") {
  const SigmoidSet& s = sigmoid_set();
  Ledger ledger;
  auto dev = host::make_cuda_device(ledger, 0);
  const Results gpu = run_sigmoid(*dev, nofma_module("agree_pure"), s);
  const Verdict v = report("sigmoid", "(ii) nofma vs cpu float", compare(gpu, s.f32, Metric::REL_L2), Cls::EVAL,
                           NOFMA_VS_CPU_FLOAT);
  CHECK_MESSAGE(v.pass, v.why);
  CHECK(ledger.total() == 0);
}

TEST_CASE("agree/sigmoid prod vs cpu f64") {
  const SigmoidSet& s = sigmoid_set();
  Ledger ledger;
  auto dev = host::make_cuda_device(ledger, 0);
  const Results gpu = run_sigmoid(*dev, prod_module("agree_pure"), s);
  const Verdict v =
      report("sigmoid", "(iii) prod vs cpu f64", compare(gpu, s.f64, Metric::REL_L2), Cls::EVAL, PROD_VS_CPU_F64);
  CHECK_MESSAGE(v.pass, v.why);
}

TEST_CASE("agree/fma detector") {
  // 0009 Verification row 17: the no-FMA module must agree with CPU float on a function whose
  // result an FMA contraction changes, and the production module must not. With
  // AR_AGREE_CANARY_FMA the "no-FMA" module is built with --fmad=true and the first check fails.
#if defined(AR_AGREE_CANARY_FMA)
  std::printf("agree: the nofma module of agree_pure was built with --fmad=true (AR_AGREE_CANARY_FMA)\n");
#endif
  const CancelSet& s = cancel_set();
  Ledger ledger;
  auto dev = host::make_cuda_device(ledger, 0);
  const Stats nofma = compare(run_gpu(*dev, nofma_module("agree_pure"), "ar_agree_cancel", s.in, 4), s.f32, Metric::REL_L2);
  const Verdict ii = report("cancel", "(ii) nofma vs cpu float", nofma, Cls::EVAL, NOFMA_VS_CPU_FLOAT);
  CHECK_MESSAGE(ii.pass, "step (ii) tripped: the no-FMA module disagrees with CPU float: " << ii.why);
  CHECK(nofma.max == 0.0);  // two correctly rounded products and a subtraction: bit-identical

  // The detector has power: the production module on the same inputs is far outside the limit.
  const Stats prod = compare(run_gpu(*dev, prod_module("agree_pure"), "ar_agree_cancel", s.in, 4), s.f32, Metric::REL_L2);
  const Verdict power = judge(prod, Cls::EVAL, NOFMA_VS_CPU_FLOAT);
  std::printf("agree cancel detector power: prod vs cpu float max=%.3e, above the limit on %.1f%% of inputs (%s)%c", prod.max,
              100.0 * prod.fraction_above(NOFMA_VS_CPU_FLOAT.eval_max), power.pass ? "NOT TRIPPED" : "trips, as required", '\n');
  CHECK(!power.pass);
  CHECK(prod.max > 100.0 * NOFMA_VS_CPU_FLOAT.eval_max);
  CHECK(prod.fraction_above(NOFMA_VS_CPU_FLOAT.eval_max) > 0.1);
}

}  // TEST_SUITE
