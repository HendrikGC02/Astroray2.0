// SPDX-License-Identifier: Apache-2.0
// The GPU test executable. Same switches as the CPU one: --tier=fast|gate|full|all selects
// the doctest suite, --seed=<n> replaces every hashed test seed. Tests run serially in
// this one process (0009 d11). After every test case the process waits for the device and
// reads the CUDA error state; a latent error fails the run, naming the case, because it
// would otherwise surface in an unrelated test. Every test creates its device itself and
// never falls back to the CPU device.
// Ref: docs/decisions/0009-build-test-process.md d11.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "common/stat.h"
#include "host/build_info.h"
#include "host/device.h"

namespace {

// Case name and error text of every test case that left a latent error, for main to print.
std::vector<std::string> g_latent_errors;

// Every case announces its start, flushed, with the wall clock and the time since the run began:
// a hung run then names its case in the output, and the wall clock lines it up with what else
// the machine was doing (check.ps1's step timeout prints the last of these lines).
struct LatentErrorListener : doctest::IReporter {
  explicit LatentErrorListener(const doctest::ContextOptions&) {}
  void report_query(const doctest::QueryData&) override {}
  void test_run_start() override {}
  void test_run_end(const doctest::TestRunStats&) override {}
  void test_case_start(const doctest::TestCaseData& tc) override {
    current_ = tc.m_name;
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &t);
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    std::printf("case start %02d:%02d:%02d.%03lld +%.3f s '%s'\n", local.tm_hour, local.tm_min, local.tm_sec, ms, since,
                tc.m_name);
    std::fflush(stdout);
  }
  void test_case_reenter(const doctest::TestCaseData&) override {}
  void test_case_end(const doctest::CurrentTestCaseStats&) override {
    const std::string e = astroray::host::cuda_latent_error();
    if (!e.empty()) g_latent_errors.push_back("'" + current_ + "': " + e);
  }
  void test_case_exception(const doctest::TestCaseException&) override {}
  void subcase_start(const doctest::SubcaseSignature&) override {}
  void subcase_end() override {}
  void log_assert(const doctest::AssertData&) override {}
  void log_message(const doctest::MessageData&) override {}
  void test_case_skipped(const doctest::TestCaseData&) override {}

 private:
  std::string current_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};
REGISTER_LISTENER("latent_cuda_error", 1, LatentErrorListener);

}  // namespace

int main(int argc, char** argv) {
  std::printf("build: sha=%s dirty=%d kernels=%s\n", astroray::kBuildInfo.git_sha, astroray::kBuildInfo.dirty,
              astroray::kernel_source().c_str());
  std::string tier = "fast";
  std::vector<char*> rest;
  astroray::stat::set_program(argv[0]);
  for (int i = 0; i < argc; ++i) {
    if (std::strncmp(argv[i], "--tier=", 7) == 0) tier = argv[i] + 7;
    else if (std::strncmp(argv[i], "--seed=", 7) == 0) {
      char* end = nullptr;
      const unsigned long long v = std::strtoull(argv[i] + 7, &end, 0);
      if (end == argv[i] + 7 || *end != 0) {
        std::fprintf(stderr, "bad --seed '%s' (decimal or 0x hex)\n", argv[i] + 7);
        return 2;
      }
      astroray::stat::set_seed_override(v);
    } else rest.push_back(argv[i]);
  }
  if (tier != "fast" && tier != "gate" && tier != "full" && tier != "all") {
    std::fprintf(stderr, "unknown tier '%s' (fast|gate|full|all)\n", tier.c_str());
    return 2;
  }
  // Every case must sit in a tier suite: a case outside fast/gate/full would
  // never run in any tier, so its absence would pass silently (rule 5).
  size_t n = 0, untiered = 0;
  for (const auto& t : doctest::detail::getRegisteredTests()) {
    const std::string s = t.m_test_suite;
    if (s != "fast" && s != "gate" && s != "full") {
      std::fprintf(stderr, "test case '%s' is in suite '%s', not fast|gate|full\n", t.m_name, t.m_test_suite);
      ++untiered;
    }
    if (tier == "all" || tier == t.m_test_suite) ++n;
  }
  if (untiered != 0) return 2;
  std::printf("tier=%s tests=%zu\n", tier.c_str(), n);
  std::fflush(stdout);

  doctest::Context ctx;
  ctx.applyCommandLine(static_cast<int>(rest.size()), rest.data());
  if (tier != "all") ctx.addFilter("test-suite", tier.c_str());
  const int rc = ctx.run();
  for (const std::string& e : g_latent_errors) std::fprintf(stderr, "latent CUDA error after a test case: %s\n", e.c_str());
  const int over = astroray::stat::check_families();
  return rc != 0 ? rc : ((over != 0 || !g_latent_errors.empty()) ? 1 : 0);
}
