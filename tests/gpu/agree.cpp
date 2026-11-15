// SPDX-License-Identifier: Apache-2.0
// The agreement harness: sampler-hash inputs, error metrics, the D12 thresholds and outlier
// rule, and the module launcher. Ref: docs/decisions/0002-kernel-architecture.md D12, C1;
// docs/spikes/A-kernel-budget/RESULTS.md section 6 for the metrics.
#include "agree.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

#include "host/device.h"
#include "sample/sobol.h"

namespace astroray::agree {

double unit(uint32_t input, uint32_t dim) {
  // The sampler's own mapping: a 24-bit fraction, exact in double.
  return static_cast<double>(ar::sample_independent(input, 0u, 0u, dim, kSeed));
}

uint32_t Stats::count_above(double x) const {
  return static_cast<uint32_t>(sorted.end() - std::upper_bound(sorted.begin(), sorted.end(), x));
}

Stats stats_from(std::vector<double> errors) {
  Stats s;
  s.n = static_cast<uint32_t>(errors.size());
  std::sort(errors.begin(), errors.end());
  s.sorted = std::move(errors);
  if (s.n == 0) return s;
  s.max = s.sorted.back();
  // ceil(0.999 n) in integers, so no rounding of 0.999 can move the rank.
  const uint64_t rank = (uint64_t(s.n) * 999u + 999u) / 1000u;
  s.p999 = s.sorted[rank - 1];
  return s;
}

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

double rel_l2(const double* a, const double* b, uint32_t k) {
  double num = 0.0, den = 0.0;
  for (uint32_t i = 0; i < k; ++i) {
    const double d = a[i] - b[i];
    num += d * d;
    den += b[i] * b[i];
  }
  if (std::isnan(num) || std::isnan(den)) return kInf;
  if (den == 0.0) return num == 0.0 ? 0.0 : kInf;
  const double r = std::sqrt(num / den);
  return std::isnan(r) ? kInf : r;  // inf / inf
}

// atan2(|a x b|, a . b): accurate for tiny angles, where acos of a dot product is not.
double angle(const double* a, const double* b) {
  const double cx = a[1] * b[2] - a[2] * b[1];
  const double cy = a[2] * b[0] - a[0] * b[2];
  const double cz = a[0] * b[1] - a[1] * b[0];
  const double r = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
  return std::isnan(r) ? kInf : r;
}

}  // namespace

Stats compare(const Results& a, const Results& b, Metric metric) {
  if (a.n != b.n || a.stride != b.stride || a.v.size() != size_t(a.n) * a.stride || b.v.size() != size_t(b.n) * b.stride)
    throw std::invalid_argument("agree::compare: the two results differ in shape");
  const uint32_t want = metric == Metric::ANGLE ? 3u : (metric == Metric::EQUAL ? 1u : 0u);
  if (want != 0u && a.stride != want)
    throw std::invalid_argument("agree::compare: this metric needs " + std::to_string(want) + " values per input");
  if (a.stride == 0) throw std::invalid_argument("agree::compare: zero values per input");
  std::vector<double> err(a.n);
  for (uint32_t i = 0; i < a.n; ++i) {
    const double* pa = a.v.data() + size_t(i) * a.stride;
    const double* pb = b.v.data() + size_t(i) * b.stride;
    switch (metric) {
      case Metric::REL_L2: err[i] = rel_l2(pa, pb, a.stride); break;
      case Metric::ANGLE: err[i] = angle(pa, pb); break;
      case Metric::EQUAL: err[i] = pa[0] == pb[0] ? 0.0 : 1.0; break;
    }
  }
  return stats_from(std::move(err));
}

namespace {
void fail(Verdict& v, const char* fmt, double a, double b) {
  char buf[200];
  std::snprintf(buf, sizeof buf, fmt, a, b);
  v.pass = false;
  if (!v.why.empty()) v.why += "; ";
  v.why += buf;
}
}  // namespace

Verdict judge(const Stats& s, Cls cls, const Preset& preset) {
  Verdict v;
  // A threshold of the class that the preset leaves out is a harness bug, not a pass.
  double max_limit = kNoLimit, p999_limit = kNoLimit;
  switch (cls) {
    case Cls::EVAL: max_limit = preset.eval_max; p999_limit = preset.eval_p999; break;
    case Cls::THROUGHPUT: p999_limit = preset.throughput_p999; break;
    case Cls::DIRECTION: p999_limit = preset.direction_p999; break;
    case Cls::CHOICE: break;
  }
  if (cls == Cls::CHOICE) {
    if (preset.choice_frac == kNoLimit)
      throw std::logic_error(std::string("agree: preset '") + preset.name + "' has no choice threshold");
    // Disagreement is a fraction of inputs; errors are 0 or 1 under Metric::EQUAL.
    const double frac = s.fraction_above(0.0);
    if (!(frac <= preset.choice_frac)) fail(v, "choice disagreement %.3g of inputs > limit %.3g", frac, preset.choice_frac);
    return v;
  }
  if (max_limit == kNoLimit && p999_limit == kNoLimit)
    throw std::logic_error(std::string("agree: preset '") + preset.name + "' has no threshold for this class");
  if (max_limit != kNoLimit && !(s.max <= max_limit)) fail(v, "max %.3g > limit %.3g", s.max, max_limit);
  if (p999_limit != kNoLimit) {
    if (!(s.p999 <= p999_limit)) fail(v, "p99.9 %.3g > limit %.3g", s.p999, p999_limit);
    const uint32_t over = s.count_above(kOutlierFactor * p999_limit);
    if (!(uint64_t(over) * kOutlierDenominator < uint64_t(s.n)))
      fail(v, "%.0f inputs above 10x the p99.9 limit (%.3g), need fewer than 0.01%%", double(over), kOutlierFactor * p999_limit);
  }
  return v;
}

Verdict report(const char* function, const char* step, const Stats& s, Cls cls, const Preset& preset) {
  const Verdict v = judge(s, cls, preset);
  const double p999_limit = cls == Cls::EVAL ? preset.eval_p999 : (cls == Cls::THROUGHPUT ? preset.throughput_p999 : preset.direction_p999);
  const uint32_t over = (cls != Cls::CHOICE && p999_limit != kNoLimit) ? s.count_above(kOutlierFactor * p999_limit) : 0u;
  std::printf("agree %s %s: n=%u max=%.3e p99.9=%.3e above10x=%u %s\n", function, step, s.n, s.max, s.p999, over,
              v.pass ? "pass" : ("FAIL: " + v.why).c_str());
  std::fflush(stdout);
  return v;
}

Results run_gpu(host::ModuleDevice& dev, const std::string& module_path, const char* kernel, const Inputs& in,
                uint32_t out_stride, const std::vector<void*>& extra) {
  using host::LedgerCategory;
  using host::Owner;
  const uint64_t in_bytes = sizeof(float) * uint64_t(in.n) * in.stride;
  const uint64_t out_bytes = sizeof(float) * uint64_t(in.n) * out_stride;
  host::DeviceBuffer b_in = dev.allocate(LedgerCategory::TABLES, in_bytes, Owner{});
  host::DeviceBuffer b_out = dev.allocate(LedgerCategory::TABLES, out_bytes, Owner{});
  dev.upload(b_in, 0, in.v.data(), in_bytes);
  dev.memset(b_out, 0, 0xFF, out_bytes);  // an unwritten output is a NaN, which compare() reports
  const uint32_t module = dev.load_module_file(module_path);
  uint64_t in_p = reinterpret_cast<uint64_t>(b_in.ptr), out_p = reinterpret_cast<uint64_t>(b_out.ptr);
  uint32_t n = in.n;
  std::vector<void*> args = {&in_p, &out_p, &n};
  args.insert(args.end(), extra.begin(), extra.end());
  dev.launch_module_kernel(module, kernel, (in.n + 255u) / 256u, 256u, args.data());
  dev.synchronize();
  std::vector<float> raw(size_t(in.n) * out_stride);
  dev.download(raw.data(), b_out, 0, out_bytes);
  dev.free(b_in);
  dev.free(b_out);
  Results r;
  r.n = in.n;
  r.stride = out_stride;
  r.v.assign(raw.begin(), raw.end());
  return r;
}

}  // namespace astroray::agree
