// SPDX-License-Identifier: Apache-2.0
// Instances: the visibility mask, transform patches, generation rejection (task 036).
#include <doctest.h>

#include <cmath>
#include <cstring>
#include <chrono>
#include <future>
#include <thread>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "host/ledger.h"
#include "host/material/material_store.h"
#include "host/scene/closed_mesh.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/media.h"
#include "vm/ops/volume_coeffs.h"

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
  std::vector<ar_key> medium;  // per instance, 0 = none; sent only when with_medium
  bool with_medium = false;
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
      medium.push_back(0);
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
    d.medium_keys = with_medium ? medium.data() : nullptr;
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

// The scene's medium of `key` as its block's absorption density, or -1 when there is none.
float absorption_density(Scene& scene, ar_key key) {
  const auto m = scene.medium(key);
  return m ? m->block.sigma_a.p[3] : -1.0f;
}

ar_medium_desc medium_desc(float sigma_a) {
  ar_medium_desc m{};
  m.sigma_a[0] = sigma_a;
  return m;
}

// One edit: geometry 100, the media in `media` (key, sigma_a), the instances.
void install_media(Scene& scene, const ar::Rgb2SpecTable& table, Desc& d,
                   const std::vector<std::pair<ar_key, float>>& media) {
  auto e = scene.begin_edit();
  REQUIRE(e);
  REQUIRE(e->stage(geometry(100)) == AR_OK);
  REQUIRE(e->item_commit(100) == AR_OK);
  for (const auto& m : media) {
    const ar_medium_desc md = medium_desc(m.second);
    REQUIRE(set_medium(*e, m.first, &md, table) == AR_OK);
  }
  REQUIRE(set_instances(*e, d.get()) == AR_OK);
  uint64_t g = 0;
  REQUIRE(e->commit(&g) == AR_OK);
  e.reset();
  scene.generations().wait_idle();
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
    CHECK(t->instances[i].light_index == ar::kNoLightIndex);
  }
  CHECK(t->instances[2].material_count == 2);
}

TEST_CASE("instances/medium bit 6") {
  // The mask function: bit 6 is the medium flag and nothing else, for every visibility combination.
  for (uint32_t v = 0; v < 64; ++v) {
    CHECK(visibility_mask(v) == visibility_mask(v, false));
    CHECK((visibility_mask(v, false) & 0x40) == 0);
    CHECK(visibility_mask(v, true) == (visibility_mask(v, false) | 0x40));
  }
  CHECK(visibility_mask(0, true) == 0x40);  // invisible to every ray, still a medium boundary

  // Through a generation: bit 6 on exactly the medium instances, whatever their visibility.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  MaterialStore store(std::make_shared<Ledger>());
  Desc d(5);
  d.with_medium = true;
  d.medium[1] = 50;
  d.medium[3] = 51;
  d.vis[3] = 0;
  install_media(scene, store.table(), d, {{50, 1.0f}, {51, 2.0f}});
  auto t = scene.instances();
  REQUIRE(t);
  REQUIRE(t->instances.size() == 5);
  for (unsigned i = 0; i < 5; ++i) {
    const bool bears = i == 1 || i == 3;
    CHECK(t->instances[i].medium_key == d.medium[i]);
    CHECK(((t->instances[i].mask >> 6) & 1u) == (bears ? 1u : 0u));
    CHECK((t->instances[i].mask & 0x80) == 0);
  }
  CHECK(t->instances[3].mask == 0x40);

  // A host that sends no medium array gets no medium on any instance.
  ContextState ctx2;
  Scene plain(ctx2, SceneConfig{2, {}});
  Desc p(3);
  install(plain, p);
  for (const Instance& in : plain.instances()->instances) {
    CHECK(in.medium_key == 0);
    CHECK((in.mask & 0x40) == 0);
  }
}

TEST_CASE("instances/medium coefficient fold") {
  // A chromatic triple s becomes density max(s) with colour s / max(s) for scattering (Cycles: weight =
  // colour * density) and 1 - s / max(s) for absorption (weight = (1 - colour) * density), so the refs the
  // block holds are volume_term_ref of those colours. The expected colours are written out, not derived.
  MaterialStore store(std::make_shared<Ledger>());
  ar_medium_desc d{};
  const float sa[3] = {0.5f, 1.0f, 2.0f}, ss[3] = {1.0f, 2.0f, 4.0f};
  for (int k = 0; k < 3; ++k) d.sigma_a[k] = sa[k], d.sigma_s[k] = ss[k];
  d.g = 0.25f;
  const auto m = build_medium(7, d, store.table());
  uint32_t fl = 0;
  auto rgb = [](float r, float g, float b) { return ar::Rgb{{ar::Real(r), ar::Real(g), ar::Real(b)}}; };
  auto same = [](const ar::SpectrumRef& a, const ar::SpectrumRef& b) { return std::memcmp(&a, &b, sizeof a) == 0; };
  const ar::SpectrumRef want_a = ar::volume_term_ref(store.table(), rgb(0.75f, 0.5f, 0.0f), 2.0f, ar::VolumeTerm::ABSORPTION, &fl);
  const ar::SpectrumRef want_s = ar::volume_term_ref(store.table(), rgb(0.25f, 0.5f, 1.0f), 4.0f, ar::VolumeTerm::SCATTER, &fl);
  CHECK(same(m->block.sigma_a, want_a));
  REQUIRE(m->block.lobe_count == 1);
  CHECK(same(m->block.sigma_s[0], want_s));
  CHECK(m->block.g[0] == 0.25f);
  CHECK(fl == 0);  // nothing was clamped on the way
  CHECK(m->approximations.empty());
  // The density is the largest channel, whichever branch.
  CHECK(m->block.sigma_a.p[3] == 2.0f);
  CHECK(m->block.sigma_s[0].p[3] == 4.0f);
}

TEST_CASE("instances/medium key must exist") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  MaterialStore store(std::make_shared<Ledger>());
  Desc d(2);
  d.with_medium = true;
  d.medium[1] = 99;  // never set
  install_media(scene, store.table(), d, {{50, 1.0f}});
  CHECK(scene.generations().failed() != 0);
  CHECK(rejected(scene, 1001, {"unknown medium key 99"}));
  CHECK_FALSE(scene.instances());
}

TEST_CASE("instances/medium replace and remove take effect with the generation") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  MaterialStore store(std::make_shared<Ledger>());
  Desc d(2);
  d.with_medium = true;
  d.medium[0] = 50;
  install_media(scene, store.table(), d, {{50, 1.0f}});
  CHECK(absorption_density(scene, 50) == 1.0f);

  // Replaced inside an open edit: the old medium is still the scene's until the edit commits.
  auto e = scene.begin_edit();
  REQUIRE(e);
  const ar_medium_desc md = medium_desc(2.0f);
  REQUIRE(set_medium(*e, 50, &md, store.table()) == AR_OK);
  scene.generations().wait_idle();
  CHECK(absorption_density(scene, 50) == 1.0f);
  uint64_t g = 0;
  REQUIRE(e->commit(&g) == AR_OK);
  e.reset();
  scene.generations().wait_idle();
  CHECK(absorption_density(scene, 50) == 2.0f);
  CHECK(scene.media().size() == 1);

  // Removing a medium that an instance still names rejects the generation and keeps the medium.
  e = scene.begin_edit();
  REQUIRE(e);
  REQUIRE(e->remove(AR_KIND_MEDIUM, 50) == AR_OK);
  REQUIRE(e->commit(&g) == AR_OK);
  e.reset();
  scene.generations().wait_idle();
  CHECK(rejected(scene, 1000, {"medium key 50", "still instanced"}));
  CHECK(absorption_density(scene, 50) == 2.0f);

  // Removed with its last instance, it is gone.
  Desc none(2);
  e = scene.begin_edit();
  REQUIRE(e);
  REQUIRE(e->remove(AR_KIND_MEDIUM, 50) == AR_OK);
  REQUIRE(set_instances(*e, none.get()) == AR_OK);
  REQUIRE(e->commit(&g) == AR_OK);
  e.reset();
  scene.generations().wait_idle();
  CHECK(absorption_density(scene, 50) == -1.0f);
  CHECK(scene.media().empty());
}

TEST_CASE("instances/closed mesh") {
  auto closed = [](const std::vector<uint32_t>& tris) {  // triangles as polygons
    std::vector<uint32_t> start;
    for (size_t i = 0; i <= tris.size(); i += 3) start.push_back(uint32_t(i));
    return polygons_closed(tris.data(), start.data(), tris.size() / 3);
  };
  // The box of scenes/lib/meshes.py: 8 shared vertices, 12 triangles counter-clockwise from outside.
  std::vector<uint32_t> cube = {1, 3, 7, 1, 7, 5, 0, 4, 6, 0, 6, 2, 2, 6, 7, 2, 7, 3,
                                0, 1, 5, 0, 5, 4, 4, 5, 7, 4, 7, 6, 0, 2, 3, 0, 3, 1};
  CHECK(closed(cube));
  // The same box as 6 quads: the check is on the polygons' own edges.
  std::vector<uint32_t> quads = {1, 3, 7, 5, 0, 4, 6, 2, 2, 6, 7, 3, 0, 1, 5, 4, 4, 5, 7, 6, 0, 2, 3, 1};
  std::vector<uint32_t> quad_start = {0, 4, 8, 12, 16, 20, 24};
  CHECK(polygons_closed(quads.data(), quad_start.data(), 6));
  // A face of two corners is no face.
  std::vector<uint32_t> two_corners = {0, 1};
  std::vector<uint32_t> two_start = {0, 2};
  CHECK_FALSE(polygons_closed(two_corners.data(), two_start.data(), 1));
  // A tetrahedron, and two disjoint ones, are closed.
  std::vector<uint32_t> tet = {0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3};
  CHECK(closed(tet));
  std::vector<uint32_t> two = tet;
  for (uint32_t v : tet) two.push_back(v + 4);
  CHECK(closed(two));
  // Open: a triangle missing.
  CHECK_FALSE(closed(std::vector<uint32_t>(cube.begin(), cube.end() - 3)));
  // Inconsistent winding: one triangle reversed.
  std::vector<uint32_t> flipped = cube;
  std::swap(flipped[1], flipped[2]);
  CHECK_FALSE(closed(flipped));
  // The same triangle twice (an edge used by four triangles, two of them the same way).
  std::vector<uint32_t> doubled = tet;
  doubled.insert(doubled.end(), {0, 2, 1});
  CHECK_FALSE(closed(doubled));
  // The whole shell twice: every edge has an opposite partner but is used by four triangles.
  std::vector<uint32_t> shell_twice = tet;
  shell_twice.insert(shell_twice.end(), tet.begin(), tet.end());
  CHECK_FALSE(closed(shell_twice));
  // Three triangles on one edge (a fin on a closed surface).
  std::vector<uint32_t> fin = tet;
  fin.insert(fin.end(), {0, 1, 4});
  CHECK_FALSE(closed(fin));
  // A degenerate triangle, and no triangles at all.
  CHECK_FALSE(closed({0, 0, 1}));
  CHECK_FALSE(polygons_closed(cube.data(), quad_start.data(), 0));
  // Faces that share positions but not vertex indices are open: closedness is by index.
  std::vector<uint32_t> split = tet;
  split[0] = 9;
  CHECK_FALSE(closed(split));
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
    CHECK(rejected(scene, 1002, {"instance 2 (object key 1002): unknown geometry key 999"}));
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
    CHECK(rejected(scene, 1001, {"instance 1 (object key 1001): transform is not finite"}));
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
    CHECK(rejected(scene, 1001, {"instance 1 (object key 1001): material slice [1, 6) exceeds"}));
    CHECK(scene.instances() == before);
  }
}

TEST_CASE("instances/dangling geometry rejects generation") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  Desc good(3);
  install(scene, good);
  auto before = scene.instances();
  {  // removing the geometry without new instances
    auto e = scene.begin_edit();
    REQUIRE(e->remove(AR_KIND_GEOMETRY, 100) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    e.reset();
    scene.generations().wait_idle();
    CHECK(scene.generations().failed() == g);
    CHECK(rejected(scene, 1000, {"instance 0 (object key 1000): geometry key 100 is removed"}));
    CHECK(scene.instances() == before);
    CHECK(scene.known(AR_KIND_GEOMETRY, 100));
  }
  {  // removing it while new instances still use it
    Desc d(2);
    auto e = scene.begin_edit();
    REQUIRE(e->remove(AR_KIND_GEOMETRY, 100) == AR_OK);
    REQUIRE(set_instances(*e, d.get()) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    e.reset();
    scene.generations().wait_idle();
    CHECK(scene.generations().failed() == g);
    CHECK(rejected(scene, 1000, {"instance 0 (object key 1000): unknown geometry key 100"}));
    CHECK(scene.instances() == before);
  }
  {  // geometry declared after set_instances in the same edit is fine
    Desc d(2);
    d.geom[0] = d.geom[1] = 200;
    auto e = scene.begin_edit();
    REQUIRE(set_instances(*e, d.get()) == AR_OK);
    REQUIRE(e->stage(geometry(200)) == AR_OK);
    REQUIRE(e->item_commit(200) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.instances() != before);
    CHECK(scene.instances()->instances.size() == 2);
  }
}

TEST_CASE("instances/remove behind a pending set") {
  // N sets a table that uses geometry 200; N+1 removes 200 without set_instances. Both are
  // committed before N has activated (its geometry job is held), so only the latest
  // committed table can see the reference.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  Desc base(1);
  install(scene, base);
  std::promise<void> release;
  std::shared_future<void> gate = release.get_future().share();
  uint64_t gn = 0, gn1 = 0;
  {
    Desc d(2);
    d.geom[0] = d.geom[1] = 200;
    auto e = scene.begin_edit();
    auto held = geometry(200);
    held->job = [gate] { gate.wait(); };
    REQUIRE(e->stage(held) == AR_OK);
    REQUIRE(e->item_commit(200) == AR_OK);
    REQUIRE(set_instances(*e, d.get()) == AR_OK);
    REQUIRE(e->commit(&gn) == AR_OK);
  }
  {
    auto e = scene.begin_edit();
    REQUIRE(e);
    REQUIRE(e->remove(AR_KIND_GEOMETRY, 200) == AR_OK);
    uint32_t idx = 1;
    ar_xform x{};
    CHECK(update_transforms(*e, 1, &idx, &x) == AR_OK);  // count 2 comes from the pending table
    idx = 2;
    CHECK(update_transforms(*e, 1, &idx, &x) == AR_ERR_INVALID_ARGUMENT);
    REQUIRE(e->commit(&gn1) == AR_OK);
  }
  // Let the removal check run while N is still pending, so the active table cannot answer.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  release.set_value();
  scene.generations().wait_idle();
  CHECK(scene.generations().failed() == gn1);
  CHECK(rejected(scene, 1000, {"instance 0 (object key 1000): geometry key 200 is removed"}));
  CHECK(scene.instances()->instances.size() == 2);  // N+1 was rejected, N stays active
}

TEST_CASE("instances/reset with set_instances") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  Desc old_desc(5);
  install(scene, old_desc);
  Desc d(3);
  d.moff[0] = 99;  // count 0 means the default material: the offset is not checked
  d.mcount[0] = 0;
  auto e = scene.begin_edit();
  REQUIRE(e->reset() == AR_OK);
  REQUIRE(e->stage(geometry(100)) == AR_OK);
  REQUIRE(e->item_commit(100) == AR_OK);
  REQUIRE(set_instances(*e, d.get()) == AR_OK);
  uint64_t g = 0;
  REQUIRE(e->commit(&g) == AR_OK);
  e.reset();
  scene.generations().wait_idle();
  CHECK(scene.generations().failed() == 0);
  auto t = scene.instances();
  REQUIRE(t);
  CHECK(t->instances.size() == 3);
  CHECK(t->instances[0].material_count == 0);
  // A reset alone empties the table; update_transforms after it needs a new set.
  auto e2 = scene.begin_edit();
  REQUIRE(e2->reset() == AR_OK);
  uint32_t idx = 0;
  ar_xform x{};
  CHECK(update_transforms(*e2, 1, &idx, &x) == AR_ERR_INVALID_ARGUMENT);
  CHECK(e2->abort() == AR_OK);
}

TEST_CASE("instances/internal items are not scene keys") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  Desc d(2);
  install(scene, d);
  CHECK(scene.known(AR_KIND_GEOMETRY, 100));
  for (uint64_t k : {0ull, 0xF000000000000000ull, 0xF000000000000001ull}) CHECK_FALSE(scene.known(AR_KIND_GEOMETRY, k));
}

}  // TEST_SUITE
