// SPDX-License-Identifier: Apache-2.0
// 0001 V15, the parts that need no rendering.
#include <doctest.h>

#include <cstring>
#include <string>

#include "../../src/host/report.h"

using astroray::host::Report;

namespace {
const double kV[2] = {0, 0};
ar_report_info info_of(Report& r) {
  r.snapshot();
  ar_report_info i{};
  REQUIRE(r.info(&i) == AR_OK);
  return i;
}
}  // namespace
TEST_SUITE("fast") {

TEST_CASE("report/two raises of one key give one entry") {
  Report r;
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 7, "TEX_X", 1, kV, 1);
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 7, "TEX_X", 1, kV, 2);
  CHECK(info_of(r).entry_count == 1);
  ar_report_entry e{};
  REQUIRE(r.entry(0, &e) == AR_OK);
  CHECK(e.count == 2);
  CHECK(e.first_generation == 1);
  CHECK(e.last_generation == 2);
  CHECK(e.level == AR_LEVEL_WARNING);
}

TEST_CASE("report/two node types for one material give two entries") {
  Report r;
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 7, "A", 1, kV, 1);
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 7, "A", 1, kV, 1);
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 7, "B", 1, kV, 1);
  CHECK(info_of(r).entry_count == 2);
}

TEST_CASE("report/revision only on change") {
  Report r;
  CHECK(r.revision() == 0);
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 7, "A", 1, kV, 1);
  uint64_t a = r.revision();
  CHECK(a != 0);
  r.snapshot();
  r.on_film_reset();                        // removes nothing
  r.on_generation_active(2, {99}, false);   // removes nothing
  CHECK(r.revision() == a);
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 7, "A", 1, kV, 2);
  CHECK(r.revision() != a);
  uint64_t b = r.revision();
  r.on_generation_active(3, {7}, false);
  CHECK(r.revision() != b);
}

TEST_CASE("report/snapshots of an unchanged table are equal") {
  Report r;
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 7, "A", 1, kV, 1);
  r.raise(AR_REPORT_SESSION_FAILED, 0, "boom", 1, kV, 1);
  r.raise(AR_REPORT_POOL_CAPACITY, 0, "", 1, kV, 1);
  ar_report_info i1 = info_of(r);
  ar_report_entry e1[3], e2[3];
  for (uint32_t k = 0; k < 3; ++k) REQUIRE(r.entry(k, &e1[k]) == AR_OK);
  ar_report_info i2 = info_of(r);
  for (uint32_t k = 0; k < 3; ++k) REQUIRE(r.entry(k, &e2[k]) == AR_OK);
  CHECK(std::memcmp(&i1, &i2, sizeof i1) == 0);
  CHECK(std::memcmp(e1, e2, sizeof e1) == 0);
  CHECK(e1[0].level == AR_LEVEL_ERROR);  // level descending
  CHECK(i1.highest_level == AR_LEVEL_ERROR);
  ar_report_entry e{};
  CHECK(r.entry(3, &e) == AR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("report/generation replacing a subject removes its SCENE entries") {
  Report r;
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 1, "A", 1, kV, 1);
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 2, "A", 1, kV, 1);
  r.raise(AR_REPORT_GENERATION_REJECTED, 1, "x", 1, kV, 1);
  r.raise(AR_REPORT_SESSION_FAILED, 0, "keep", 1, kV, 1);
  r.on_generation_active(2, {1}, false);
  CHECK(info_of(r).entry_count == 2);  // subject 2 and SESSION_FAILED
  r.on_generation_active(3, {}, true);
  CHECK(info_of(r).entry_count == 1);
}

TEST_CASE("report/film reset removes FILM entries") {
  Report r;
  r.raise(AR_REPORT_INVALID_SAMPLE, 0, "", 5, kV, 1);
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 1, "A", 1, kV, 1);
  r.on_film_reset();
  CHECK(info_of(r).entry_count == 1);
}

TEST_CASE("report/overflow drops the 513th key") {
  Report r;
  for (uint64_t s = 1; s <= 513; ++s) r.raise(AR_REPORT_NODE_UNSUPPORTED, s, "A", 1, kV, 1);
  ar_report_info i = info_of(r);
  CHECK(i.entry_count == 512);
  CHECK(i.dropped == 1);
  r.raise(AR_REPORT_NODE_UNSUPPORTED, 1, "A", 1, kV, 2);  // existing keys keep counting
  ar_report_entry e{};
  r.snapshot();
  REQUIRE(r.entry(0, &e) == AR_OK);
  CHECK(e.count == 2);
}

TEST_CASE("report/unknown code is refused") {
  Report r;
  CHECK(r.raise(0, 0, "", 1, kV, 1) == AR_ERR_INVALID_ARGUMENT);
  CHECK(r.raise(9999, 0, "", 1, kV, 1) == AR_ERR_INVALID_ARGUMENT);
}
}  // TEST_SUITE
