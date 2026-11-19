// SPDX-License-Identifier: Apache-2.0
// The VRAM ledger: totals per scene, session and category after allocate and free
// sequences, the refusal text, the category list. Expected numbers are typed from
// the sequences below, not computed from the code under test.
// Ref: docs/decisions/0007-blender-integration.md d14 (ledger, budget table rows,
// "needs X GiB for <category>, Y available").
#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include "astroray/astroray.h"
#include "host/ledger.h"
#include "host/thread.h"

using namespace astroray;
using namespace astroray::host;

namespace {
constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kGiB = 1ull << 30;
}  // namespace

TEST_SUITE("fast") {

TEST_CASE("ledger/totals per category") {
  Ledger l;
  CHECK(l.total() == 0);
  // scene 1 session 1 / scene 1 session 2 / scene 2 session 1 (the F12 and viewport mix).
  const uint64_t a = l.add({1, 1}, LedgerCategory::GEOMETRY, 100 * kMiB);
  const uint64_t b = l.add({1, 2}, LedgerCategory::TEXTURES, 50 * kMiB);
  const uint64_t c = l.add({2, 1}, LedgerCategory::GEOMETRY, 25 * kMiB);
  const uint64_t d = l.add({2, 1}, LedgerCategory::MAIN_PATH_POOL, 512 * kMiB);
  CHECK(a != 0);
  CHECK(a != b);
  CHECK(l.total() == 687 * kMiB);
  CHECK(l.category_total(LedgerCategory::GEOMETRY) == 125 * kMiB);
  CHECK(l.category_total(LedgerCategory::TEXTURES) == 50 * kMiB);
  CHECK(l.category_total(LedgerCategory::MAIN_PATH_POOL) == 512 * kMiB);
  CHECK(l.category_total(LedgerCategory::VOLUMES) == 0);
  CHECK(l.scene_total(1) == 150 * kMiB);
  CHECK(l.scene_total(2) == 537 * kMiB);
  CHECK(l.scene_total(3) == 0);
  CHECK(l.session_total(1) == 637 * kMiB);
  CHECK(l.session_total(2) == 50 * kMiB);

  l.remove(c);
  CHECK(l.total() == 662 * kMiB);
  CHECK(l.category_total(LedgerCategory::GEOMETRY) == 100 * kMiB);
  CHECK(l.scene_total(2) == 512 * kMiB);
  CHECK(l.session_total(1) == 612 * kMiB);

  l.remove(d);
  l.remove(a);
  CHECK(l.total() == 50 * kMiB);
  CHECK(l.category_total(LedgerCategory::GEOMETRY) == 0);
  CHECK(l.scene_total(1) == 50 * kMiB);
  CHECK(l.session_total(1) == 0);
  l.remove(b);
  CHECK(l.total() == 0);
  CHECK(l.records().empty());
}

TEST_CASE("ledger/records carry scene, session, category and bytes") {
  Ledger l;
  l.add({7, 9}, LedgerCategory::ACCELERATION, 4096);
  const std::vector<LedgerRecord> r = l.records();
  REQUIRE(r.size() == 1);
  CHECK(r[0].owner.scene == 7);
  CHECK(r[0].owner.session == 9);
  CHECK(r[0].category == LedgerCategory::ACCELERATION);
  CHECK(r[0].bytes == 4096);
}

TEST_CASE("ledger/invalid use throws") {
  Ledger l;
  CHECK_THROWS_WITH_AS(l.add({}, LedgerCategory::FILM, 0), "ledger: an allocation of zero bytes", std::invalid_argument);
  CHECK_THROWS_WITH_AS(l.add({}, static_cast<LedgerCategory>(16), 8), "ledger: unknown category 16", std::invalid_argument);
  CHECK(l.total() == 0);
  const uint64_t id = l.add({}, LedgerCategory::FILM, 8);
  l.remove(id);
  const std::string again = "ledger: free of allocation " + std::to_string(id) + " that is not live";
  CHECK_THROWS_WITH_AS(l.remove(id), again.c_str(), std::invalid_argument);
}

TEST_CASE("ledger/categories are the budget table rows then the scene categories") {
  // 0007 d14 budget table row names in table order, then the scene categories, then the device
  // context (driver and OptiX memory outside our allocations).
  const char* want[] = {"main path pool", "shadow pool", "queue and sort indices", "shadow hit records",
                        "shadow-pass split", "geodesic record", "random-walk record", "film", "tables",
                        "display frame", "denoiser", "DLSS", "geometry", "acceleration", "textures", "volumes",
                        "device context"};
  REQUIRE(kLedgerCategoryCount == 17);
  for (uint32_t i = 0; i < kLedgerCategoryCount; ++i) {
    INFO("category " << i);
    CHECK(std::strcmp(ledger_category_name(static_cast<LedgerCategory>(i)), want[i]) == 0);
  }
  CHECK_THROWS_AS(ledger_category_name(static_cast<LedgerCategory>(17)), std::invalid_argument);
}

TEST_CASE("ledger/refused allocation text") {
  const AllocationRefused e(LedgerCategory::GEOMETRY, 3 * kGiB, kGiB + kGiB / 2);
  CHECK(std::string(e.what()) == "needs 3.00 GiB for geometry, 1.50 GiB available");
  CHECK(e.status() == AR_ERR_OUT_OF_MEMORY);
  CHECK(e.category() == LedgerCategory::GEOMETRY);
  CHECK(e.need() == 3 * kGiB);
  CHECK(e.available() == kGiB + kGiB / 2);
  const AllocationRefused f(LedgerCategory::MAIN_PATH_POOL, 512 * kMiB, 0);
  CHECK(std::string(f.what()) == "needs 0.50 GiB for main path pool, 0.00 GiB available");
}

TEST_CASE("ledger/concurrent adds and removes balance") {
  Ledger l;
  ContextState ctx;
  std::vector<std::thread> threads;
  for (uint64_t t = 0; t < 8; ++t)
    threads.push_back(start_engine_thread(ctx, [&l, t] {
      for (int i = 0; i < 500; ++i) l.remove(l.add({t, t}, LedgerCategory::TEXTURES, 64));
    }));
  const uint64_t keep = l.add({100, 100}, LedgerCategory::FILM, 1000);
  for (std::thread& t : threads) t.join();
  CHECK_FALSE(ctx.poisoned.load());
  CHECK(l.total() == 1000);
  CHECK(l.records().size() == 1);
  l.remove(keep);
}

}  // TEST_SUITE
