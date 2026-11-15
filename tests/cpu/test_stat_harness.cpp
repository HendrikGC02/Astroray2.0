// SPDX-License-Identifier: Apache-2.0
// Deterministic checks of the statistical harness itself (no p-values of its own,
// no random seeds: 0009 d10). Reference values are tabulated: scipy.stats
// norm.ppf / chi2.sf, cross-checked against the standard critical-value tables.
#include <doctest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "common/stat.h"

namespace st = astroray::stat;

namespace {

bool near_rel(double got, double want, double tol) {
  return std::fabs(got - want) <= tol * std::fmax(std::fabs(want), 1.0);
}

constexpr char kChildEnv[] = "ASTRORAY_STAT_CHILD";

}  // namespace

TEST_SUITE("fast") {
  TEST_CASE("stat_harness/inverse normal quantiles") {
    struct Row { double p, z; };
    // Central region, both tails of AS241's two outer branches, and symmetry.
    const Row rows[] = {{0.5, 0.0}, {0.9, 1.2815515655446004}, {0.975, 1.959963984540054},
                        {0.99, 2.3263478740408408}, {0.995, 2.5758293035489004},
                        {0.999, 3.090232306167813}, {1e-3, -3.090232306167813},
                        {1e-6, -4.753424308822899}, {2.5e-6, -4.564787730280885},
                        {1e-10, -6.361340902404056}, {1e-20, -9.262340089798409},
                        {0.3, -0.5244005127080409}, {0.0625, -1.5341205443525463}};
    for (const Row& r : rows) CHECK(near_rel(st::inverse_normal_cdf(r.p), r.z, 1e-9));
    CHECK_THROWS(st::inverse_normal_cdf(0.0));
  }

  TEST_CASE("stat_harness/incomplete gamma against chi-squared p-values") {
    struct Row { int dof; double x, p; };
    const Row rows[] = {{1, 3.841458820694124, 0.05}, {1, 6.634896601021214, 0.01},
                        {2, 9.21034037197618, 0.01}, {3, 7.814727903251179, 0.05},
                        {5, 11.070497693516351, 0.05}, {10, 18.30703805327515, 0.05},
                        {20, 31.410432844230918, 0.05}, {100, 124.34211340400407, 0.05},
                        {4, 0.5, 0.9735009788392561}, {7, 40.0, 1.2587903873713078e-06}};
    for (const Row& r : rows) CHECK(std::fabs(st::chi2_pvalue(r.x, r.dof) - r.p) <= 1e-8 * r.p);
    CHECK(st::gamma_q(2.5, 0.0) == 1.0);
  }

  TEST_CASE("stat_harness/chi2 pools cells below expected 5") {
    st::Family fam("pooling", 100);
    const st::Run run = st::begin("stat_harness/chi2 pools cells below expected 5", fam);
    // Cells 3, 2, 4, 1 pool into (observed 9, expected 10): 4 cells, dof 3,
    // chi2 = 4/10 + 1/8 + 4/20 + 1/10 = 0.825.
    const st::Chi2Result r = st::chi2_test(run, {12, 2, 4, 7, 3, 22, 0}, {10, 3, 2, 8, 4, 20, 1});
    CHECK(r.cells == 4);
    CHECK(r.dof == 3);
    CHECK(std::fabs(r.chi2 - 0.825) < 1e-12);
    CHECK(near_rel(r.p, 0.8434784898694276, 1e-8));
    CHECK(r.pass);
    // A sample in a cell of zero expected count fails whatever the pooling does.
    const st::Chi2Result z = st::chi2_test(run, {50, 1, 49}, {50, 0, 50});
    CHECK_FALSE(z.pass);
    CHECK(z.message.find("zero expected") != std::string::npos);
    CHECK(fam.count() == 2);
  }

  TEST_CASE("stat_harness/sidak alpha and z") {
    const double a = st::sidak_alpha(0.01, 2000);
    CHECK(near_rel(a / 5.025155300615524e-06, 1.0, 1e-10));
    CHECK(std::fabs(a - 5.0e-6) < 0.05e-6);
    CHECK(near_rel(st::sidak_alpha(0.01, 1), 0.01, 1e-14));
    st::Family fam("sidak", 2000);
    CHECK(near_rel(fam.z(), 4.5637347874563305, 1e-9));
  }

  TEST_CASE("stat_harness/seeds are hashed per name and reproduce line names them") {
    const std::string a = "stat_harness/a", b = "stat_harness/b";
    CHECK(st::seed_for(a) == st::seed_for(a));
    CHECK(st::seed_for(a) != st::seed_for(b));
    CHECK(st::seed_for(a) != 0);
    // Pinned values from an independent Python FNV-1a (64-bit) + pbrt-v4 MixBits script:
    // mix_bits(fnv1a(name) ^ 0x415354524159).
    CHECK(st::seed_for(a) == 4808046070232382604ull);
    CHECK(st::seed_for(b) == 15873143595697496289ull);
    const st::Run run = st::begin(a);
    const std::string line = st::reproduce(run);
    CHECK(line.find("seed " + std::to_string(run.seed)) != std::string::npos);
    CHECK(line.find("-tc=\"" + a + "\" --seed=" + std::to_string(run.seed)) != std::string::npos);
  }

  TEST_CASE("stat_harness/energy test threshold") {
    st::Family fam("energy", 2000);
    const st::Run run = st::begin("stat_harness/energy test threshold", fam);
    // Ten-value cycle with mean exactly 0.5 over 1000 samples.
    std::vector<double> s;
    for (int i = 0; i < 1000; ++i) s.push_back(0.5 + 0.1 * ((i % 10) - 4.5) / 4.5);
    const st::EnergyResult ok = st::energy_test(run, s, 0.5);
    CHECK(ok.pass);
    CHECK(std::fabs(ok.mean - 0.5) < 1e-12);
    // numpy: s.std(ddof=1) / sqrt(1000) for this exact cycle; closed form is
    // sqrt(8.25 * (0.1 / 4.5)^2 * 1000 / 999 / 1000).
    CHECK(near_rel(ok.se, 0.0020194435437269277, 1e-12));
    const double zc = fam.z();
    CHECK(st::energy_test(run, s, 0.5 + 0.9 * zc * ok.se).pass);
    const st::EnergyResult bad = st::energy_test(run, s, 0.5 + 1.1 * zc * ok.se);
    CHECK_FALSE(bad.pass);
    CHECK(bad.message.find("--seed=") != std::string::npos);
    s[3] = std::nan("");
    CHECK_FALSE(st::energy_test(run, s, 0.5).pass);
  }

  TEST_CASE("stat_harness/chi2 retry runs once at twice ires") {
    st::Family fam("retry", 100);
    const st::Run run = st::begin("stat_harness/chi2 retry runs once at twice ires", fam);
    const std::vector<double> expected(4, 100.0);
    std::vector<int> calls;
    auto fails_at = [&](int bad_ires) {
      return [&, bad_ires](int ires) {
        calls.push_back(ires);
        const bool bad = ires <= bad_ires;
        return st::Histogram{bad ? std::vector<double>{150, 50, 100, 100}
                                   : std::vector<double>{98, 103, 100, 99},
                               expected};
      };
    };
    st::Chi2Result r = st::chi2_with_retry(run, fails_at(4), 4);
    CHECK(r.pass);
    CHECK(r.retried);
    CHECK(calls == std::vector<int>{4, 8});
    calls.clear();
    r = st::chi2_with_retry(run, fails_at(0), 4);
    CHECK(r.pass);
    CHECK_FALSE(r.retried);
    CHECK(calls == std::vector<int>{4});
    calls.clear();
    r = st::chi2_with_retry(run, fails_at(100), 4);
    CHECK_FALSE(r.pass);
    CHECK(calls == std::vector<int>{4, 8});
    CHECK(r.message.find("reproduce:") != std::string::npos);
    CHECK(fam.count() == 3);  // one check per call, retry included
  }

  // Runs only inside the child process of the next case.
  TEST_CASE("stat_harness/child family over m") {
    if (GetEnvironmentVariableA(kChildEnv, nullptr, 0) == 0) return;
    // Leaked on purpose: the exit-time check of the main must still see it.
    st::Family* fam = new st::Family("child", 3);
    const st::Run run = st::begin("stat_harness/child family over m", *fam);
    for (int i = 0; i < 4; ++i) st::energy_test(run, {1.0, 2.0, 3.0}, 2.0);
  }

  TEST_CASE("stat_harness/family over m fails") {
    char exe[MAX_PATH];
    REQUIRE(GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0);
    REQUIRE(_putenv_s(kChildEnv, "1") == 0);
    // The extra outer quotes are cmd.exe's rule for a quoted program path.
    const std::string cmd = std::string("\"\"") + exe +
                            "\" --tier=fast -tc=\"stat_harness/child family over m\" 2>&1\"";
    FILE* p = _popen(cmd.c_str(), "r");
    REQUIRE(p != nullptr);
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    const int code = _pclose(p);
    _putenv_s(kChildEnv, "");
    const std::string want = "family child ran 4 checks, m = 3; raising m is a reviewed budget change";
    const size_t at = out.find("family child");
    std::printf("child exit %d: %s", code, at == std::string::npos ? out.c_str() : out.c_str() + at);
    CHECK(code != 0);
    CHECK(out.find(want) != std::string::npos);
  }
}
