// SPDX-License-Identifier: Apache-2.0
// ops.def: dense enum in table order, interval rule on every volume-reachable op.
#include <doctest.h>

#include <cstring>

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

}  // TEST_SUITE
