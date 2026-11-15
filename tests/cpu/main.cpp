// SPDX-License-Identifier: Apache-2.0
// --tier=fast|gate|full selects the doctest suite of that name; --tier=all runs
// every suite (land's canary check uses it). --seed=<n> replaces every hashed
// test seed (the reproduce line of a statistical failure prints it).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/stat.h"
#include "host/build_info.h"

int main(int argc, char** argv) {
  // First line: check.ps1 compares the sha with HEAD and refuses loose kernels (0009 d5).
  std::printf("build: sha=%s dirty=%d kernels=%s\n",astroray::kBuildInfo.git_sha,
              astroray::kBuildInfo.dirty, astroray::kernel_source().c_str());
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
  size_t n = 0;
  for (const auto& t : doctest::detail::getRegisteredTests())
    if (tier == "all" || tier == t.m_test_suite) ++n;
  std::printf("tier=%s tests=%zu\n", tier.c_str(), n);
  std::fflush(stdout);

  doctest::Context ctx;
  ctx.applyCommandLine(static_cast<int>(rest.size()), rest.data());
  if (tier != "all") ctx.addFilter("test-suite", tier.c_str());
  const int rc = ctx.run();
  // A family over its declared m fails the run even when every test passed.
  const int over = astroray::stat::check_families();
  return rc != 0 ? rc : (over != 0 ? 1 : 0);
}
