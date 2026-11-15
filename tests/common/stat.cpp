// SPDX-License-Identifier: Apache-2.0
#include "common/stat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>

#include "families.h"  // AR_FAMILY_GATE, AR_FAMILY_FULL, AR_FAMILY_ANALYTIC (from tools/budgets.toml)

namespace astroray::stat {
namespace {

// Test-process bookkeeping: the registry of families, the seed override and the
// program name. Tests are single-process and the engine never links this file.
struct State {
  std::mutex mu;
  std::vector<Family*> families;
  bool has_override = false;
  uint64_t seed_override = 0;
  std::string program = "astroray_tests_cpu.exe";
};
State& state() {
  static State s;
  return s;
}

constexpr uint64_t kRepoConstant = 0x415354524159ull;  // "ASTRAY"

// Ref: Fowler-Noll-Vo FNV-1a, 64-bit offset basis 14695981039346656037 and prime
// 1099511628211 (IETF draft-eastlake-fnv; isthe.com/chongo/tech/comp/fnv).
uint64_t fnv1a(const std::string& s) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string fmt(const char* f, double a, double b, double c, double d) {
  char buf[512];
  std::snprintf(buf, sizeof buf, f, a, b, c, d);
  return buf;
}

}  // namespace

Family::Family(const char* name, int m) : name_(name), m_(m), count_(0) {
  std::lock_guard<std::mutex> g(state().mu);
  state().families.push_back(this);
}
Family::~Family() {
  std::lock_guard<std::mutex> g(state().mu);
  auto& v = state().families;
  v.erase(std::remove(v.begin(), v.end(), this), v.end());
}
int Family::count() const {
  std::lock_guard<std::mutex> g(state().mu);
  return count_;
}
void Family::note_check() {
  std::lock_guard<std::mutex> g(state().mu);
  ++count_;
}
double Family::alpha() const { return sidak_alpha(kFamilyAlpha, m_); }
double Family::z() const { return -inverse_normal_cdf(alpha() / 2.0); }

Family& gate_family() {
  static Family f("gate_statistical", AR_FAMILY_GATE);
  return f;
}
Family& full_family() {
  static Family f("full_statistical", AR_FAMILY_FULL);
  return f;
}
Family& analytic_family() {
  static Family f("analytic_scenes", AR_FAMILY_ANALYTIC);
  return f;
}

int check_families() {
  std::vector<Family*> all;
  {
    std::lock_guard<std::mutex> g(state().mu);
    all = state().families;
  }
  int bad = 0;
  for (const Family* f : all) {
    if (f->count() > f->m()) {
      std::fprintf(stderr,
                   "family %s ran %d checks, m = %d; raising m is a reviewed budget change\n",
                   f->name(), f->count(), f->m());
      ++bad;
    }
  }
  return bad;
}

double sidak_alpha(double family_alpha, int m) {
  // expm1/log1p keep full precision for the tiny result at large m.
  return -std::expm1(std::log1p(-family_alpha) / m);
}

double inverse_normal_cdf(double p) {
  // Ref: Wichura 1988, Applied Statistics 37(3), algorithm AS241, PPND16.
  if (!(p > 0.0 && p < 1.0)) throw std::domain_error("inverse_normal_cdf: p outside (0,1)");
  const double q = p - 0.5;
  double r, val;
  if (std::fabs(q) <= 0.425) {
    r = 0.180625 - q * q;
    val = q *
          (((((((2.5090809287301226727e+3 * r + 3.3430575583588128105e+4) * r +
                6.7265770927008700853e+4) * r + 4.5921953931549871457e+4) * r +
              1.3731693765509461125e+4) * r + 1.9715909503065514427e+3) * r +
            1.3314166789178437745e+2) * r + 3.3871328727963666080e+0) /
          (((((((5.2264952788528545610e+3 * r + 2.8729085735721942674e+4) * r +
                3.9307895800092710610e+4) * r + 2.1213794301586595867e+4) * r +
              5.3941960214247511077e+3) * r + 6.8718700749205790830e+2) * r +
            4.2313330701600911252e+1) * r + 1.0);
    return val;
  }
  r = q < 0.0 ? p : 1.0 - p;
  r = std::sqrt(-std::log(r));
  if (r <= 5.0) {
    r -= 1.6;
    val = (((((((7.74545014278341407640e-4 * r + 2.27238449892691845833e-2) * r +
                2.41780725177450611770e-1) * r + 1.27045825245236838258e+0) * r +
              3.64784832476320460504e+0) * r + 5.76949722146069140550e+0) * r +
            4.63033784615654529590e+0) * r + 1.42343711074968357734e+0) /
          (((((((1.05075007164441684324e-9 * r + 5.47593808499534494600e-4) * r +
                1.51986665636164571966e-2) * r + 1.48103976427480074590e-1) * r +
              6.89767334985100004550e-1) * r + 1.67638483018380384940e+0) * r +
            2.05319162663775882187e+0) * r + 1.0);
  } else {
    r -= 5.0;
    val = (((((((2.01033439929228813265e-7 * r + 2.71155556874348757815e-5) * r +
                1.24266094738807843860e-3) * r + 2.65321895265761230930e-2) * r +
              2.96560571828504891230e-1) * r + 1.78482653991729133580e+0) * r +
            5.46378491116411436990e+0) * r + 6.65790464350110377720e+0) /
          (((((((2.04426310338993978564e-15 * r + 1.42151175831644588870e-7) * r +
                1.84631831751005468180e-5) * r + 7.86869131145613259100e-4) * r +
              1.48753612908506148525e-2) * r + 1.36929880922735805310e-1) * r +
            5.99832206555887937690e-1) * r + 1.0);
  }
  return q < 0.0 ? -val : val;
}

double gamma_q(double a, double x) {
  // Ref: Numerical Recipes 3rd ed. 6.2: series for x < a + 1, else the continued
  // fraction evaluated with the modified Lentz method.
  if (!(a > 0.0) || x < 0.0) throw std::domain_error("gamma_q: needs a > 0 and x >= 0");
  if (x == 0.0) return 1.0;
  constexpr double eps = 1e-16, tiny = 1e-300;
  const double lead = std::exp(-x + a * std::log(x) - std::lgamma(a));
  if (x < a + 1.0) {
    double ap = a, del = 1.0 / a, sum = del;
    for (int n = 0; n < 100000; ++n) {
      ap += 1.0;
      del *= x / ap;
      sum += del;
      if (std::fabs(del) < std::fabs(sum) * eps) return 1.0 - sum * lead;
    }
  } else {
    double b = x + 1.0 - a, c = 1.0 / tiny, d = 1.0 / b, h = d;
    for (int i = 1; i < 100000; ++i) {
      const double an = -i * (i - a);
      b += 2.0;
      d = an * d + b;
      if (std::fabs(d) < tiny) d = tiny;
      c = b + an / c;
      if (std::fabs(c) < tiny) c = tiny;
      d = 1.0 / d;
      const double del = d * c;
      h *= del;
      if (std::fabs(del - 1.0) < eps) return lead * h;
    }
  }
  throw std::runtime_error("gamma_q: no convergence");
}

double chi2_pvalue(double chi2, int dof) { return gamma_q(0.5 * dof, 0.5 * chi2); }

uint64_t mix_bits(uint64_t v) {
  // Ref: pbrt-v4 src/pbrt/util/hash.h MixBits.
  v ^= (v >> 31);
  v *= 0x7fb5d329728ea185ull;
  v ^= (v >> 27);
  v *= 0x81dadef4bc2dd44dull;
  v ^= (v >> 33);
  return v;
}

uint64_t seed_for(const std::string& test_name) {
  {
    std::lock_guard<std::mutex> g(state().mu);
    if (state().has_override) return state().seed_override;
  }
  uint64_t h = mix_bits(fnv1a(test_name) ^ kRepoConstant);
  char* sha = nullptr;
  size_t len = 0;
  if (_dupenv_s(&sha, &len, "ASTRORAY_SEED_SHA") == 0 && sha) {
    if (*sha) h = mix_bits(h ^ fnv1a(sha));
    std::free(sha);
  }
  return h;
}
void set_seed_override(uint64_t seed) {
  std::lock_guard<std::mutex> g(state().mu);
  state().has_override = true;
  state().seed_override = seed;
}
void set_program(const std::string& path) {
  const size_t cut = path.find_last_of("/\\");
  std::lock_guard<std::mutex> g(state().mu);
  state().program = cut == std::string::npos ? path : path.substr(cut + 1);
}

Run begin(const std::string& test_name, Family& family) {
  return Run{test_name, seed_for(test_name), &family};
}

std::string reproduce(const Run& run) {
  std::string prog;
  {
    std::lock_guard<std::mutex> g(state().mu);
    prog = state().program;
  }
  return "seed " + std::to_string(run.seed) + "; reproduce: out/bin/" + prog + " -tc=\"" +
         run.name + "\" --seed=" + std::to_string(run.seed);
}

EnergyResult energy_test(const Run& run, const std::vector<double>& samples, double truth) {
  run.family->note_check();
  EnergyResult r{0.0, 0.0, 0.0, false, {}};
  const double n = static_cast<double>(samples.size());
  double sum = 0.0;
  bool finite = true;
  for (double s : samples) {
    finite = finite && std::isfinite(s);
    sum += s;
  }
  if (samples.size() < 2 || !finite) {
    r.message = std::string("energy test: ") + (finite ? "needs at least 2 samples" : "non-finite sample") +
                "; " + reproduce(run);
    return r;
  }
  r.mean = sum / n;
  double ss = 0.0;
  for (double s : samples) ss += (s - r.mean) * (s - r.mean);
  r.se = std::sqrt(ss / (n - 1.0) / n);
  const double d = r.mean - truth;
  r.z = r.se > 0.0 ? d / r.se : (d == 0.0 ? 0.0 : std::copysign(INFINITY, d));
  r.pass = std::fabs(r.z) <= run.family->z();
  if (!r.pass)
    r.message = fmt("energy test: mean %.9g, truth %.9g, se %.3g, |z| %.4g", r.mean, truth, r.se,
                    std::fabs(r.z)) +
                " > " + std::to_string(run.family->z()) + "; " + reproduce(run);
  return r;
}

namespace {

Chi2Result chi2_core(const Run& run, const std::vector<double>& observed,
                     const std::vector<double>& expected) {
  Chi2Result r{0.0, 0.0, 0, 0, false, false, {}};
  if (observed.size() != expected.size() || observed.empty()) {
    r.message = "chi2: observed and expected differ in size or are empty; " + reproduce(run);
    return r;
  }
  // Ref: pbrt-v4 bsdfs_test.cpp and Mitsuba chi2.py: a sample in a cell of zero
  // expected count is a failure by itself, whatever the pooling would say.
  double pool_o = 0.0, pool_e = 0.0;
  bool pooled = false;
  for (size_t i = 0; i < observed.size(); ++i) {
    if (expected[i] == 0.0 && observed[i] > 0.0) {
      r.message = "chi2: samples in cell " + std::to_string(i) + " of zero expected count; " +
                  reproduce(run);
      return r;
    }
    if (expected[i] < 5.0) {
      pool_o += observed[i];
      pool_e += expected[i];
      pooled = true;
    } else {
      const double d = observed[i] - expected[i];
      r.chi2 += d * d / expected[i];
      ++r.cells;
    }
  }
  if (pooled && pool_e > 0.0) {
    const double d = pool_o - pool_e;
    r.chi2 += d * d / pool_e;
    ++r.cells;
  }
  r.dof = r.cells - 1;
  if (r.dof < 1) {
    r.message = "chi2: fewer than 2 cells after pooling; " + reproduce(run);
    return r;
  }
  r.p = chi2_pvalue(r.chi2, r.dof);
  r.pass = r.p >= run.family->alpha();
  if (!r.pass)
    r.message = fmt("chi2: statistic %.6g, dof %.0f, p %.3g < alpha %.3g", r.chi2, r.dof, r.p,
                    run.family->alpha()) + "; " + reproduce(run);
  return r;
}

}  // namespace

Chi2Result chi2_test(const Run& run, const std::vector<double>& observed,
                     const std::vector<double>& expected) {
  run.family->note_check();
  return chi2_core(run, observed, expected);
}

Chi2Result chi2_with_retry(const Run& run, const std::function<Histogram(int)>& gen, int ires) {
  run.family->note_check();
  Histogram h = gen(ires);
  Chi2Result r = chi2_core(run, h.observed, h.expected);
  if (r.pass) return r;
  // Ref: 0009 d15: a chi-squared failure re-runs at 2x ires before it is reported.
  h = gen(2 * ires);
  r = chi2_core(run, h.observed, h.expected);
  r.retried = true;
  if (!r.pass) r.message += " (after re-run at ires " + std::to_string(2 * ires) + ")";
  return r;
}

}  // namespace astroray::stat
