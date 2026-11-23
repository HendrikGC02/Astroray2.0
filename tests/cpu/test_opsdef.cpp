// SPDX-License-Identifier: Apache-2.0
// ops.def: dense enum in table order, interval rule on every volume-reachable op, the foldable flag.
#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

#include "../../src/host/material/op_meta.h"

using astroray::host::op_meta_count;
using astroray::host::op_meta_table;

TEST_SUITE("fast") {

TEST_CASE("opsdef/dense enum") {
  const int n = op_meta_count();
  REQUIRE(n > 0);
  CHECK(static_cast<int>(ar::Op::COUNT_) == n);
  CHECK(static_cast<int>(ar::Op::END) == 0);
  for (int i = 0; i < n; ++i) {
    CHECK(static_cast<int>(op_meta_table()[i].op) == i);
  }
  CHECK(static_cast<int>(ar::Op::UNSUPPORTED) == 14);
}

TEST_CASE("opsdef/volume reachable ops have interval rules") {
  int reachable = 0;
  for (int i = 0; i < op_meta_count(); ++i) {
    const auto& m = op_meta_table()[i];
    if (m.volume_reachable) {
      ++reachable;
      INFO(m.name);
      REQUIRE(m.interval_rule != nullptr);
      CHECK(std::strncmp(m.interval_rule, "interval_", 9) == 0);
    }
  }
  CHECK(reachable > 0);
}

TEST_CASE("opsdef/foldable flag is parsed") {
  // Written out here, not read from the table: adding a foldable op is a deliberate edit of this list
  // and of compiler.cpp fold().
  const std::set<std::string> want = {"CONST_F32", "CONST_F32X3", "MUL_F32", "MUL_F32X3", "MUL_F32X3_F32", "ADD_F32X3"};
  REQUIRE(op_meta_count() == 18);
  for (int i = 0; i < op_meta_count(); ++i) {
    const auto& m = op_meta_table()[i];
    INFO(m.name);
    CHECK(m.foldable == (want.count(m.name) == 1));
  }
}

}  // TEST_SUITE

TEST_SUITE("gate") {

// Starting pwsh and Python costs about 0.8 s, over the fast tier's per-test limit (B-12), so the
// opsgen self-test runs here. It must refuse a foldable row with a reg+lift operand list and one
// with an empty result.
TEST_CASE("opsdef/foldable self-test refuses ineligible rows") {
  std::string here = __FILE__;  // <root>/tests/cpu/test_opsdef.cpp
  const std::filesystem::path root = std::filesystem::path(here).parent_path().parent_path().parent_path();
  const std::string cmd = "pwsh -NoProfile -Command \". '" + (root / "tools/lib/python.ps1").generic_string() +
                          "'; & (Get-ArPython) -I '" + (root / "tools/opsgen.py").generic_string() +
                          "' --self-test 2>&1; exit $LASTEXITCODE\"";
  std::string output;
  FILE* pipe = _popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buf[512];
  while (std::fgets(buf, sizeof buf, pipe) != nullptr) output += buf;
  const int rc = _pclose(pipe);
  INFO(output);
  CHECK(rc == 0);
  CHECK(output.find("refused a foldable row with a lift operand") != std::string::npos);
  CHECK(output.find("refused a foldable row with an empty result") != std::string::npos);
}

}  // TEST_SUITE
