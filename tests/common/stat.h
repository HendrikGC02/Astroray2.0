// SPDX-License-Identifier: Apache-2.0
// Statistics shared by every lobe, light and sampler test (0009 d15): per-test
// thresholds from the suite's declared family size, hashed seeds, the energy
// test and the pooled chi-squared test with its 2x-resolution re-run.
// Ref: Sidak 1967 (per-test alpha); Wichura 1988, AS241 (inverse normal);
// Press et al., Numerical Recipes 3rd ed. 6.2 (incomplete gamma, own code);
// harness pattern of pbrt-v4 bsdfs_test.cpp and Mitsuba 3 chi2.py.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace astroray::stat {

// Family alpha of 0009 d15 for every suite.
constexpr double kFamilyAlpha = 0.01;

// A suite's declared family: every statistical check executed counts against m.
// Families register themselves; check_families() at program exit fails the run
// for any family whose count exceeds m. A Family destroyed earlier is not checked.
class Family {
 public:
  Family(const char* name, int m);
  ~Family();
  Family(const Family&) = delete;
  Family& operator=(const Family&) = delete;
  const char* name() const { return name_; }
  int m() const { return m_; }
  int count() const;
  void note_check();
  double alpha() const;  // Sidak per-test alpha for this m
  double z() const;      // two-sided normal critical value at alpha()
 private:
  const char* name_;
  int m_;
  int count_;
};

// The three suites of tools/budgets.toml [family].
Family& gate_family();
Family& full_family();
Family& analytic_family();

// Prints "family <name> ran <n> checks, m = <m>; ..." to stderr for each family
// over its m; returns how many were. Called by the test main after the run.
int check_families();

// Per-test Sidak alpha: 1 - (1 - family_alpha)^(1/m).
double sidak_alpha(double family_alpha, int m);
// Inverse of the standard normal CDF (AS241, PPND16).
double inverse_normal_cdf(double p);
// Regularised upper incomplete gamma Q(a, x).
double gamma_q(double a, double x);
// Upper-tail p-value of a chi-squared statistic.
double chi2_pvalue(double chi2, int dof);

// Seeds. seed_for(name) is FNV-1a of the name mixed with the repo constant;
// when ASTRORAY_SEED_SHA is set (full runs) the commit sha is mixed in too.
// --seed=<n> on the test main overrides it so a failure can be reproduced.
uint64_t mix_bits(uint64_t v);
uint64_t seed_for(const std::string& test_name);
void set_seed_override(uint64_t seed);
void set_program(const std::string& path);  // for the reproduce line

struct Run {
  std::string name;
  uint64_t seed;
  Family* family;
};
Run begin(const std::string& test_name, Family& family = gate_family());
// `out/bin/<exe> -tc="<name>" --seed=<s>`
std::string reproduce(const Run& run);

struct EnergyResult {
  double mean, se, z;
  bool pass;
  std::string message;  // empty on pass
};
// Mean of samples (f cos / pdf) against truth; two-sided at the family's alpha.
// Non-finite samples fail.
EnergyResult energy_test(const Run& run, const std::vector<double>& samples, double truth);

struct Chi2Result {
  double chi2, p;
  int dof, cells;
  bool pass, retried;
  std::string message;  // empty on pass
};
// Observed and expected are counts (expected already scaled to the sample count).
// Cells with expected < 5 are pooled into one; dof = pooled cells - 1.
Chi2Result chi2_test(const Run& run, const std::vector<double>& observed,
                     const std::vector<double>& expected);
struct Histogram {
  std::vector<double> observed, expected;
};
// Runs gen(ires); on failure runs it once more at 2 * ires and reports that one.
Chi2Result chi2_with_retry(const Run& run, const std::function<Histogram(int)>& gen, int ires);

}  // namespace astroray::stat
