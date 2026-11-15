// SPDX-License-Identifier: Apache-2.0
// Instances: the visibility mask, transform patches, generation rejection (task 036).
#include <doctest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "host/scene/edit.h"
#include "host/scene/instances.h"

using namespace astroray;
using namespace astroray::host;

namespace {

// N instances of geometry key 100, instance i translated by (i, 0, 0).
struct Desc {
  uint32_t n;
  std::vector<ar_key> geom, object, materials = {7, 8};
  std::vector<ar_xform> xf;
  std::vector<uint32_t> zeros, vis, flags, moff, mcount;
  std::vector<ar_rgba> colors;
  std::vector<uint8_t> lg;
  ar_instances_desc d{};
  explicit Desc(uint32_t count) : n(count) {
    for (uint32_t i = 0; i < n; ++i) {
      geom.push_back(100);
      object.push_back(1000 + i);
      ar_xform x{};
      x.m[0] = x.m[5] = x.m[10] = 1.0f;
      x.m[3] = float(i);
      xf.push_back(x);
      zeros.push_back(0);
      vis.push_back(AR_VISIBILITY_CAMERA | AR_VISIBILITY_SHADOW);
      flags.push_back(0);
      moff.push_back(0);
      mcount.push_back(i % 3);
      colors.push_back({{1, 1, 1, 1}});
      lg.push_back(0);
    }
  }
  const ar_instances_desc* get() {
    d.instance_count = n;
    d.motion_steps = 1;
    d.transform_count = uint32_t(xf.size());
    d.material_key_count = uint32_t(materials.size());
    d.geom_keys = geom.data();
    d.xf = xf.data();
    d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data();
    d.visibility = vis.data();
    d.flags = flags.data();
    d.light_groups = lg.data();
    d.material_offsets = moff.data();
    d.material_counts = mcount.data();
    d.material_keys = materials.data();
    return &d;
  }
};

std::shared_ptr<EditItem> geometry(ar_key key) {
  auto it = std::make_shared<EditItem>();
  it->key = key;
  it->job = [] {};
  it->needs_device = false;
  return it;
}

// One edit: declares geometry 100, sets the instances, commits.
void install(Scene& scene, Desc& d) {
  auto e = scene.begin_edit();
  REQUIRE(e);
  REQUIRE(e->stage(geometry(100)) == AR_OK);
  REQUIRE(e->item_commit(100) == AR_OK);
  REQUIRE(set_instances(*e, d.get()) == AR_OK);
  uint64_t g = 0;
  REQUIRE(e->commit(&g) == AR_OK);
  e.reset();
  scene.generations().wait_idle();
  REQUIRE(scene.generations().failed() == 0);
}

// True when the report holds a GENERATION_REJECTED entry for `subject` whose detail has all of `parts`.
bool rejected(Scene& scene, uint64_t subject, std::vector<std::string> parts) {
  scene.report().snapshot();
  ar_report_info info{};
  scene.report().info(&info);
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry r{};
    scene.report().entry(i, &r);
    std::string detail(reinterpret_cast<const char*>(r.detail));
    bool all = r.code == AR_REPORT_GENERATION_REJECTED && r.subject == subject;
    for (const auto& part : parts) all = all && detail.find(part) != std::string::npos;
    if (all) return true;
  }
  return false;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("instances/mask from visibility") {
  const uint32_t bits[6] = {AR_VISIBILITY_CAMERA, AR_VISIBILITY_DIFFUSE, AR_VISIBILITY_GLOSSY,
                            AR_VISIBILITY_TRANSMISSION, AR_VISIBILITY_SHADOW, AR_VISIBILITY_VOLUME};
  for (unsigned b = 0; b < 6; ++b) CHECK(visibility_mask(bits[b]) == (1u << b));
  CHECK(visibility_mask(0) == 0);
  CHECK(visibility_mask(AR_VISIBILITY_CAMERA | AR_VISIBILITY_VOLUME) == 0x21);

  // Through a generation: the mask is stored, flags never become mask bits, bits 6 and 7 stay clear.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  Desc d(6);
  for (unsigned i = 0; i < 6; ++i) {
    d.vis[i] = bits[i];
    d.flags[i] = AR_INSTANCE_HOLDOUT | AR_INSTANCE_SHADOW_CATCHER | AR_INSTANCE_CASTER_OPT_OUT;
  }
  install(scene, d);
  auto t = scene.instances();
  REQUIRE(t);
  REQUIRE(t->instances.size() == 6);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(t->instances[i].mask == (1u << i));
    CHECK((t->instances[i].mask & 0xC0) == 0);
    CHECK(t->instances[i].flags == 7u);
    CHECK(t->instances[i].light_index == kNoLight);
  }
  CHECK(t->instances[2].material_count == 2);
}

TEST_CASE("instances/update patches listed only") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  Desc d(5);
  install(scene, d);
  uint64_t epoch = scene.tlas_epoch();
  REQUIRE(epoch > 0);

  auto e = scene.begin_edit();
  REQUIRE(e);
  uint32_t idx[2] = {1, 3};
  ar_xform nx[2]{};
  for (auto& x : nx) x.m[0] = x.m[5] = x.m[10] = 1.0f;
  nx[0].m[3] = 50.0f;
  nx[1].m[3] = 60.0f;
  CHECK(update_transforms(*e, 2, idx, nx) == AR_OK);
  uint32_t bad_idx = 5;
  CHECK(update_transforms(*e, 1, &bad_idx, nx) == AR_ERR_INVALID_ARGUMENT);
  CHECK(e->last_error().find("out of range") != std::string::npos);
  ar_xform nan_xf = nx[0];
  nan_xf.m[7] = std::numeric_limits<float>::quiet_NaN();
  CHECK(update_transforms(*e, 1, idx, &nan_xf) == AR_ERR_INVALID_ARGUMENT);
  uint64_t g = 0;
  REQUIRE(e->commit(&g) == AR_OK);
  e.reset();
  scene.generations().wait_idle();

  auto t = scene.instances();
  REQUIRE(t);
  CHECK(scene.tlas_epoch() > epoch);
  for (uint32_t i = 0; i < 5; ++i) {
    float want = i == 1 ? 50.0f : i == 3 ? 60.0f : float(i);
    CHECK(t->step(i, 0).m[3] == want);
    CHECK(t->instances[i].object_key == 1000 + i);
  }
}

TEST_CASE("instances/rejected generation") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  Desc good(3);
  install(scene, good);
  auto before = scene.instances();

  {  // unknown geometry key
    Desc d(4);
    d.geom[2] = 999;
    auto e = scene.begin_edit();
    REQUIRE(e->stage(geometry(100)) == AR_OK);
    REQUIRE(e->item_commit(100) == AR_OK);
    REQUIRE(set_instances(*e, d.get()) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    e.reset();
    scene.generations().wait_idle();
    CHECK(scene.generations().failed() == g);
    CHECK(rejected(scene, 1002, {"instance 2", "unknown geometry key 999"}));
    CHECK(scene.instances() == before);
  }
  {  // NaN transform
    Desc d(4);
    d.xf[1].m[3] = std::numeric_limits<float>::quiet_NaN();
    auto e = scene.begin_edit();
    REQUIRE(e->stage(geometry(100)) == AR_OK);
    REQUIRE(e->item_commit(100) == AR_OK);
    REQUIRE(set_instances(*e, d.get()) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    e.reset();
    scene.generations().wait_idle();
    CHECK(scene.generations().failed() == g);
    CHECK(rejected(scene, 1001, {"instance 1", "not finite"}));
    CHECK(scene.instances() == before);
  }
  {  // material slice out of range, rejected at the call or on the worker
    Desc d(2);
    d.moff[1] = 1;
    d.mcount[1] = 5;
    auto e = scene.begin_edit();
    REQUIRE(e->stage(geometry(100)) == AR_OK);
    REQUIRE(e->item_commit(100) == AR_OK);
    REQUIRE(set_instances(*e, d.get()) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    e.reset();
    scene.generations().wait_idle();
    CHECK(scene.generations().failed() == g);
    CHECK(scene.instances() == before);
  }
}

}  // TEST_SUITE
