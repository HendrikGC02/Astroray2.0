// SPDX-License-Identifier: Apache-2.0
// Scene edits, generations, the staging arena (0007 decisions 6, 9, 24, 25, 41).
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <string>

#include "host/scene/edit.h"

using namespace astroray;
using namespace astroray::host;

namespace {

std::shared_ptr<EditItem> item(ar_key key, std::function<void()> job) {
  auto it = std::make_shared<EditItem>();
  it->key = key;
  it->job = std::move(job);
  it->needs_device = false;
  return it;
}

uint64_t commit_ok(Edit& e) {
  uint64_t g = 0;
  REQUIRE(e.commit(&g) == AR_OK);
  return g;
}

ar_astro_desc good_astro() {
  ar_astro_desc a{};
  a.astro_kind = AR_ASTRO_BLACK_HOLE;
  a.xf.m[0] = a.xf.m[5] = a.xf.m[10] = 1.0f;
  a.mass = 4.0e6;
  a.spin = 0.5;
  a.meters_per_rg = 1.0e3;
  return a;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("scene/generation numbering") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  for (uint64_t want = 1; want <= 3; ++want) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    CHECK(commit_ok(*e) == want);
    e.reset();
    scene.generations().wait_idle();
    CHECK(scene.generations().active() == want);
  }
  CHECK(scene.generations().failed() == 0);
}

TEST_CASE("scene/failed job keeps previous generation") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  std::atomic<int> dropped{0}, activated{0};
  {
    auto e = scene.begin_edit();
    auto a = item(10, [] {});
    a->on_active = [&] { ++activated; };
    REQUIRE(e->stage(a) == AR_OK);
    REQUIRE(e->item_commit(10) == AR_OK);
    commit_ok(*e);
  }
  scene.generations().wait_idle();
  REQUIRE(scene.generations().active() == 1);
  uint64_t g2 = 0;
  {
    auto e = scene.begin_edit();
    auto ok = item(11, [] {});
    ok->on_dropped = [&] { ++dropped; };
    auto bad = item(12, [] { throw std::runtime_error("corner_vert out of range"); });
    bad->on_dropped = [&] { ++dropped; };
    REQUIRE(e->stage(ok) == AR_OK);
    REQUIRE(e->stage(bad) == AR_OK);
    REQUIRE(e->item_commit(11) == AR_OK);
    REQUIRE(e->item_commit(12) == AR_OK);
    g2 = commit_ok(*e);
  }
  scene.generations().wait_idle();
  CHECK(g2 == 2);
  CHECK(scene.generations().active() == 1);
  CHECK(scene.generations().failed() == 2);
  CHECK(dropped == 2);  // the new objects of both items are freed
  CHECK(activated == 1);
  scene.report().snapshot();
  ar_report_info info{};
  REQUIRE(scene.report().info(&info) == AR_OK);
  REQUIRE(info.entry_count == 1);
  ar_report_entry r{};
  REQUIRE(scene.report().entry(0, &r) == AR_OK);
  CHECK(r.code == AR_REPORT_GENERATION_REJECTED);
  CHECK(r.subject == 12);
  CHECK(r.value[0] == 2.0);
  CHECK(std::string(reinterpret_cast<const char*>(r.detail)) == "corner_vert out of range");
}

TEST_CASE("scene/one open edit") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  auto e1 = scene.begin_edit();
  REQUIRE(e1);
  CHECK(scene.begin_edit() == nullptr);
  CHECK(e1->abort() == AR_OK);
  e1.reset();
  auto e2 = scene.begin_edit();
  CHECK(e2 != nullptr);
}

TEST_CASE("scene/two edits pipeline while workers run") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  std::promise<void> release;
  std::shared_future<void> gate = release.get_future().share();
  std::atomic<bool> started{false};
  {
    auto e = scene.begin_edit();
    REQUIRE(e->stage(item(1, [&started, gate] {
      started = true;
      gate.wait();
    })) == AR_OK);
    REQUIRE(e->item_commit(1) == AR_OK);
    CHECK(commit_ok(*e) == 1);
  }
  // The first edit's job is still running: a second edit opens, commits and queues behind it.
  uint64_t g2 = 0;
  {
    auto e = scene.begin_edit();
    REQUIRE(e);
    REQUIRE(e->stage(item(2, [] {})) == AR_OK);
    REQUIRE(e->item_commit(2) == AR_OK);
    g2 = commit_ok(*e);
  }
  CHECK(g2 == 2);
  CHECK(scene.generations().active() == 0);  // nothing activates while generation 1 runs
  release.set_value();
  scene.generations().wait_idle();
  CHECK(scene.generations().active() == 2);
  CHECK(started.load());
}

TEST_CASE("scene/abort discards staged data") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  Arena::Block blk = scene.arena().alloc(1024);
  std::atomic<int> released{0}, ran{0}, activated{0};
  {
    auto e = scene.begin_edit();
    auto it = item(5, [&] { ++ran; });
    it->release = [&] {
      scene.arena().release(blk);
      ++released;
    };
    it->on_active = [&] { ++activated; };
    REQUIRE(e->stage(it) == AR_OK);
    REQUIRE(e->set_astro(7, nullptr) == AR_ERR_INVALID_ARGUMENT);
    ar_astro_desc a = good_astro();
    REQUIRE(e->set_astro(7, &a) == AR_OK);
    CHECK(e->abort() == AR_OK);
  }
  scene.generations().wait_idle();
  CHECK(released == 1);
  CHECK(ran == 0);
  CHECK(activated == 0);
  CHECK(scene.generations().pending() == 0);
  ar_astro_desc out{};
  CHECK_FALSE(scene.astro(7, &out));
  // The edit slot is free again and numbering has no gap.
  auto e = scene.begin_edit();
  REQUIRE(e);
  CHECK(commit_ok(*e) == 1);
}

TEST_CASE("scene/astro validation") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  auto e = scene.begin_edit();
  ar_astro_desc a = good_astro();
  CHECK(e->set_astro(1, &a) == AR_OK);
  auto expect_bad = [&](auto mutate) {
    ar_astro_desc b = good_astro();
    mutate(b);
    CHECK(e->set_astro(2, &b) == AR_ERR_INVALID_ARGUMENT);
    CHECK_FALSE(e->last_error().empty());
  };
  expect_bad([](ar_astro_desc& b) { b.xf.m[3] = std::nanf(""); });
  expect_bad([](ar_astro_desc& b) { b.mass = 0; });
  expect_bad([](ar_astro_desc& b) { b.mass = -1; });
  expect_bad([](ar_astro_desc& b) { b.spin = 1.0; });
  expect_bad([](ar_astro_desc& b) { b.spin = -1.0; });
  expect_bad([](ar_astro_desc& b) { b.meters_per_rg = 0; });
  expect_bad([](ar_astro_desc& b) { b.meters_per_rg = INFINITY; });
  commit_ok(*e);
  e.reset();
  scene.generations().wait_idle();
  ar_astro_desc out{};
  REQUIRE(scene.astro(1, &out));  // stored when the generation became active
  CHECK(out.mass == 4.0e6);
  CHECK_FALSE(scene.astro(2, &out));
}

TEST_CASE("scene/edit rules") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  auto e = scene.begin_edit();
  CHECK(e->item_commit(99) == AR_ERR_INVALID_ARGUMENT);  // never staged
  CHECK(e->remove(AR_KIND_GEOMETRY, 99) == AR_ERR_INVALID_ARGUMENT);
  CHECK(e->reset() == AR_ERR_INVALID_ARGUMENT);  // not the first call
  REQUIRE(e->stage(item(3, [] {})) == AR_OK);
  uint64_t g = 0;
  CHECK(e->commit(&g) == AR_ERR_INVALID_ARGUMENT);  // staged item not committed
  REQUIRE(e->item_commit(3) == AR_OK);
  commit_ok(*e);
  e.reset();
  scene.generations().wait_idle();
  CHECK(scene.known(AR_KIND_GEOMETRY, 3));
  auto e2 = scene.begin_edit();
  CHECK(e2->reset() == AR_OK);
  commit_ok(*e2);
  e2.reset();
  CHECK_FALSE(scene.known(AR_KIND_GEOMETRY, 3));
}

TEST_CASE("scene/rejected generation does not change the known keys") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  {
    auto e = scene.begin_edit();
    REQUIRE(e->stage(item(20, [] {})) == AR_OK);
    REQUIRE(e->item_commit(20) == AR_OK);
    commit_ok(*e);
  }
  scene.generations().wait_idle();
  REQUIRE(scene.generations().active() == 1);
  {
    auto e = scene.begin_edit();
    REQUIRE(e->remove(AR_KIND_GEOMETRY, 20) == AR_OK);
    REQUIRE(e->stage(item(21, [] { throw std::runtime_error("bad"); })) == AR_OK);
    REQUIRE(e->item_commit(21) == AR_OK);
    commit_ok(*e);
  }
  scene.generations().wait_idle();
  REQUIRE(scene.generations().failed() == 2);
  auto e = scene.begin_edit();
  CHECK(e->remove(AR_KIND_GEOMETRY, 20) == AR_OK);                     // still live
  CHECK(e->remove(AR_KIND_GEOMETRY, 21) == AR_ERR_INVALID_ARGUMENT);  // only the rejected one had it
}

TEST_CASE("scene/init failure is re-raised at the first job") {
  ContextState ctx;
  SceneConfig cfg;
  cfg.workers = 1;
  cfg.init = []() -> std::shared_ptr<void> { throw std::runtime_error("no device"); };
  Scene scene(ctx, std::move(cfg));
  auto e = scene.begin_edit();
  auto it = item(4, [] {});
  it->needs_device = true;
  REQUIRE(e->stage(it) == AR_OK);
  REQUIRE(e->item_commit(4) == AR_OK);
  commit_ok(*e);
  e.reset();
  scene.generations().wait_idle();
  CHECK(scene.generations().active() == 0);
  CHECK(scene.generations().failed() == 1);
  CHECK_THROWS_WITH_AS(scene.wait_init(), "no device", std::runtime_error);
}

TEST_CASE("scene/arena chunks recycle after release") {
  Arena arena(1 << 16);
  Arena::Block a = arena.alloc(40000);
  Arena::Block b = arena.alloc(40000);  // does not fit beside a: second chunk
  CHECK(a.chunk != b.chunk);
  CHECK(arena.chunks_allocated() == 2);
  arena.release(a);
  Arena::Block c = arena.alloc(40000);  // retires b's chunk, takes the recycled one
  CHECK(arena.chunks_allocated() == 2);
  CHECK(c.chunk == a.chunk);
  Arena::Block big = arena.alloc(1 << 20);  // larger than a chunk: its own, freed on release
  CHECK(arena.chunks_allocated() == 3);
  arena.release(big);
  CHECK(arena.chunks_allocated() == 2);
  CHECK_THROWS_AS(arena.release(big), std::invalid_argument);
  arena.release(b);
  arena.release(c);
}

}  // TEST_SUITE
