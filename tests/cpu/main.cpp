// SPDX-License-Identifier: Apache-2.0
// --tier=fast|gate|full selects the doctest suite of that name; --tier=all runs
// every suite (land's canary check uses it).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "host/build_info.h"

int main(int argc, char** argv) {
  // First line: check.ps1 compares the sha with HEAD and refuses loose kernels (0009 d5).
  std::printf("build: sha=%s dirty=%d kernels=%s\n",astroray::kBuildInfo.git_sha,
              astroray::kBuildInfo.dirty, astroray::kernel_source().c_str());
  std::string tier = "fast";
  std::vector<char*> rest;
  for (int i = 0; i < argc; ++i) {
    if (std::strncmp(argv[i], "--tier=", 7) == 0) tier = argv[i] + 7;
    else rest.push_back(argv[i]);
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
  return ctx.run();
}
