// SPDX-License-Identifier: Apache-2.0
// Mesh ingestion: staged arrays, validation, triangulation, host records (0007 d9, d10, d22).
#include <doctest.h>

#include <cmath>
#include <cstring>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include "host/geometry/triangulate.h"
#include "host/scene/edit.h"
#include "host/scene/mesh.h"

using namespace astroray;
using namespace astroray::host;

namespace {

// Unit quad in the z = 0 plane: one face, four corners.
struct Quad {
  std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  std::vector<int32_t> cv{0, 1, 2, 3};
  std::vector<int32_t> fs{0};
};

ar_mesh_desc desc_of(ar_key key, uint32_t V, uint32_t C, uint32_t F, const char* name) {
  ar_mesh_desc d{};
  d.key = key;
  d.vertex_count = V, d.corner_count = C, d.face_count = F;
  d.motion_steps = 1;
  memcpy(d.name, name, std::min(strlen(name), sizeof(d.name) - 1));
  return d;
}

// Stages `q` under `key`, lets `tweak` edit the staged arrays, commits and waits.
// Returns the generation number.
uint64_t ingest(Scene& scene, MeshStore&, ar_key key, const Quad& q, const std::function<void(ar_mesh_buffers&)>& tweak = {},
                const std::vector<int32_t>* material = nullptr) {
  ar_mesh_desc d = desc_of(key, uint32_t(q.pos.size() / 3), uint32_t(q.cv.size()), uint32_t(q.fs.size()), "quad");
  if (material) d.flags |= AR_MESH_HAS_MATERIAL_INDEX;
  auto e = scene.begin_edit();
  REQUIRE(e);
  ar_mesh_buffers b{};
  REQUIRE(mesh_begin(scene, *e, &d, &b) == AR_OK);
  CHECK(b.positions_count == q.pos.size());
  memcpy(*b.positions, q.pos.data(), q.pos.size() * 4);
  memcpy(*b.corner_vert, q.cv.data(), q.cv.size() * 4);
  memcpy(*b.face_start, q.fs.data(), q.fs.size() * 4);
  for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
  if (material) memcpy(*b.material_index, material->data(), material->size() * 4);
  if (tweak) tweak(b);
  REQUIRE(e->item_commit(key) == AR_OK);
  uint64_t g = 0;
  REQUIRE(e->commit(&g) == AR_OK);
  e.reset();
  scene.generations().wait_idle();
  return g;
}

std::string rejection_detail(Scene& scene, uint64_t* subject) {
  scene.report().snapshot();
  ar_report_info info{};
  REQUIRE(scene.report().info(&info) == AR_OK);
  REQUIRE(info.entry_count == 1);
  ar_report_entry r{};
  REQUIRE(scene.report().entry(0, &r) == AR_OK);
  CHECK(r.code == AR_REPORT_GENERATION_REJECTED);
  *subject = r.subject;
  return reinterpret_cast<const char*>(r.detail);
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("mesh/quad triangulates to corner triples") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  scene.meshes = std::make_shared<MeshStore>();
  MeshStore& store = *scene.meshes;
  ingest(scene, store, 5, Quad{});
  CHECK(scene.generations().active() == 1);
  auto rec = store.find(5);
  REQUIRE(rec);
  REQUIRE(rec->topo->triangle_count() == 2);
  // Both triangles split the quad along one diagonal; corner values index corner_vert.
  std::vector<uint32_t> a(rec->topo->tri_corners.begin(), rec->topo->tri_corners.begin() + 3);
  std::vector<uint32_t> b(rec->topo->tri_corners.begin() + 3, rec->topo->tri_corners.end());
  std::vector<uint32_t> all = rec->topo->tri_corners;
  std::sort(all.begin(), all.end());
  int shared = 0;
  for (uint32_t x : a)
    for (uint32_t y : b) shared += x == y;
  CHECK(shared == 2);
  CHECK(all.front() == 0);
  CHECK(all.back() == 3);
  CHECK(rec->topo->tri_face == std::vector<uint32_t>{0, 0});
  CHECK(rec->topo->tri_material == std::vector<int32_t>{0, 0});
  CHECK(rec->bounds_min[0] == 0);
  CHECK(rec->bounds_max[1] == 1);
  CHECK(rec->topo->max_material_index.value() == 0);
  CHECK(rec->corner_normals.size() == 12);
  CHECK(rec->host_bytes == 24 * sizeof(float));
  CHECK(store.geometry_bytes() > rec->host_bytes);  // topology bytes are counted too
}

TEST_CASE("mesh/corner_vert out of range rejected") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  scene.meshes = std::make_shared<MeshStore>();
  MeshStore& store = *scene.meshes;
  ingest(scene, store, 5, Quad{});
  auto before = store.find(5);
  const uint64_t held = store.geometry_bytes();
  uint64_t g = ingest(scene, store, 5, Quad{}, [](ar_mesh_buffers& b) { (*b.corner_vert)[2] = 4; });
  CHECK(g == 2);
  CHECK(scene.generations().active() == 1);
  CHECK(scene.generations().failed() == 2);
  CHECK(store.find(5) == before);
  uint64_t subject = 0;
  std::string detail = rejection_detail(scene, &subject);
  CHECK(subject == 5);
  CHECK(detail.find("corner_vert[2] = 4") != std::string::npos);
  CHECK(detail.find("quad") != std::string::npos);
  CHECK(store.geometry_bytes() == held);  // the rejected record left no ledger bytes
}

TEST_CASE("mesh/nan position rejected") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  scene.meshes = std::make_shared<MeshStore>();
  MeshStore& store = *scene.meshes;
  ingest(scene, store, 5, Quad{});
  auto before = store.find(5);
  ingest(scene, store, 5, Quad{}, [](ar_mesh_buffers& b) { (*b.positions)[7] = std::nanf(""); });
  CHECK(scene.generations().active() == 1);
  CHECK(store.find(5) == before);
  uint64_t subject = 0;
  std::string detail = rejection_detail(scene, &subject);
  CHECK(detail.find("positions[7] is not finite") != std::string::npos);
}

TEST_CASE("mesh/non-monotonic face_start rejected") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  scene.meshes = std::make_shared<MeshStore>();
  MeshStore& store = *scene.meshes;
  Quad two;  // two triangles over the quad's vertices
  two.cv = {0, 1, 2, 0, 2, 3};
  two.fs = {0, 3};
  ingest(scene, store, 6, two);
  REQUIRE(store.find(6));
  CHECK(store.find(6)->topo->triangle_count() == 2);
  auto before = store.find(6);
  ingest(scene, store, 6, two, [](ar_mesh_buffers& b) { (*b.face_start)[1] = 0; });
  CHECK(scene.generations().failed() == 2);
  CHECK(store.find(6) == before);
  uint64_t subject = 0;
  CHECK(rejection_detail(scene, &subject).find("face_start[1] = 0 is not greater") != std::string::npos);
  // A face of fewer than three corners is also rejected.
  ingest(scene, store, 6, two, [](ar_mesh_buffers& b) { (*b.face_start)[1] = 5; });
  CHECK(scene.generations().failed() == 3);
}

TEST_CASE("mesh/descriptor counts rejected at begin") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  scene.meshes = std::make_shared<MeshStore>();
  auto e = scene.begin_edit();
  ar_mesh_buffers b{};
  ar_mesh_desc d = desc_of(1, 4, 5, 2, "bad");  // 5 corners cannot hold 2 faces
  CHECK_THROWS_AS(mesh_begin(scene, *e, &d, &b), std::invalid_argument);
  d = desc_of(1, 4, 4, 1, "bad");
  d.motion_steps = 0;
  CHECK_THROWS_AS(mesh_begin(scene, *e, &d, &b), std::invalid_argument);
  d = desc_of(1, 4, 4, 1, "bad");
  d.flags = AR_MESH_HAS_ATTRIBUTES;  // flag without attributes
  CHECK_THROWS_AS(mesh_begin(scene, *e, &d, &b), std::invalid_argument);
  CHECK_THROWS_AS(positions_begin(scene, *e, 99, nullptr), std::invalid_argument);
  d = desc_of(1, 4, 4, 1, "ok");
  REQUIRE(mesh_begin(scene, *e, &d, &b) == AR_OK);
  CHECK(mesh_begin(scene, *e, &d, &b) == AR_ERR_INVALID_ARGUMENT);  // key staged twice
  CHECK(e->abort() == AR_OK);
}

TEST_CASE("mesh/positions-only update keeps triangles") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  scene.meshes = std::make_shared<MeshStore>();
  MeshStore& store = *scene.meshes;
  ingest(scene, store, 5, Quad{});
  auto before = store.find(5);
  const uint64_t held = store.geometry_bytes();
  {
    auto e = scene.begin_edit();
    ar_positions_buffers pb{};
    REQUIRE(positions_begin(scene, *e, 5, &pb) == AR_OK);
    CHECK(pb.positions_count == 12);
    CHECK(pb.corner_normals_count == 12);
    for (uint64_t i = 0; i < pb.positions_count; ++i) (*pb.positions)[i] = before->positions[i] * 3.0f;
    for (uint64_t i = 0; i < pb.corner_normals_count; ++i) (*pb.corner_normals)[i] = i % 3 == 0 ? 1.0f : 0.0f;
    REQUIRE(e->item_commit(5) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
  }
  scene.generations().wait_idle();
  auto after = store.find(5);
  REQUIRE(after);
  CHECK(after != before);
  CHECK(after->topo->tri_corners == before->topo->tri_corners);
  CHECK(after->topo->tri_material == before->topo->tri_material);
  auto before_topo = before->topo;
  before.reset();  // the replaced record's ledger bytes go with its last reference
  CHECK(after->bounds_max[0] == 3.0f);
  CHECK(after->corner_normals[0] == 1.0f);
  CHECK(store.geometry_bytes() == held);  // same sizes: the replaced positions released theirs
  CHECK(after->topo == before_topo);
  // A NaN in a positions-only commit rejects the generation and keeps the record.
  {
    auto e = scene.begin_edit();
    ar_positions_buffers pb{};
    REQUIRE(positions_begin(scene, *e, 5, &pb) == AR_OK);
    for (uint64_t i = 0; i < pb.positions_count; ++i) (*pb.positions)[i] = 0;
    for (uint64_t i = 0; i < pb.corner_normals_count; ++i) (*pb.corner_normals)[i] = 0;
    (*pb.corner_normals)[4] = INFINITY;
    REQUIRE(e->item_commit(5) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
  }
  scene.generations().wait_idle();
  CHECK(store.find(5) == after);
}

TEST_CASE("mesh/material slots and largest index") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  scene.meshes = std::make_shared<MeshStore>();
  MeshStore& store = *scene.meshes;
  Quad two;
  two.cv = {0, 1, 2, 0, 2, 3};
  two.fs = {0, 3};
  std::vector<int32_t> mat{2, 7};
  ingest(scene, store, 8, two, {}, &mat);
  auto rec = store.find(8);
  REQUIRE(rec);
  CHECK(rec->topo->tri_material == std::vector<int32_t>{2, 7});
  CHECK(rec->topo->max_material_index.value() == 7);
  std::vector<int32_t> neg{2, -1};
  ingest(scene, store, 9, two, {}, &neg);
  CHECK(scene.generations().failed() == 2);
  CHECK(!store.find(9));
  // An empty mesh is valid and has no largest index.
  Quad empty;
  empty.pos.clear(), empty.cv.clear(), empty.fs.clear();
  ingest(scene, store, 10, empty);
  REQUIRE(store.find(10));
  CHECK(!store.find(10)->topo->max_material_index.has_value());
  CHECK(store.find(10)->topo->triangle_count() == 0);
}

}  // TEST_SUITE

namespace {
// Holds the scene's only worker until released, so later edits queue behind it.
struct Blocker {
  std::promise<void> release;
  void stage(Scene& scene) {
    std::shared_future<void> gate = release.get_future().share();
    auto e = scene.begin_edit();
    auto it = std::make_shared<EditItem>();
    it->key = 1000;
    it->needs_device = false;
    it->job = [gate] { gate.wait(); };
    REQUIRE(e->stage(it) == AR_OK);
    REQUIRE(e->item_commit(1000) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
  }
};

// Stages a quad mesh and commits the edit without waiting.
void commit_quad(Scene& scene, ar_key key, const std::function<void(ar_mesh_buffers&)>& tweak = {}) {
  Quad q;
  ar_mesh_desc d = desc_of(key, 4, 4, 1, "quad");
  auto e = scene.begin_edit();
  ar_mesh_buffers b{};
  REQUIRE(mesh_begin(scene, *e, &d, &b) == AR_OK);
  memcpy(*b.positions, q.pos.data(), 48);
  memcpy(*b.corner_vert, q.cv.data(), 16);
  (*b.face_start)[0] = 0;
  for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
  if (tweak) tweak(b);
  REQUIRE(e->item_commit(key) == AR_OK);
  uint64_t g = 0;
  REQUIRE(e->commit(&g) == AR_OK);
}

void commit_positions(Scene& scene, ar_key key, float scale) {
  auto e = scene.begin_edit();
  ar_positions_buffers pb{};
  REQUIRE(positions_begin(scene, *e, key, &pb) == AR_OK);
  Quad q;
  for (uint64_t i = 0; i < pb.positions_count; ++i) (*pb.positions)[i] = q.pos[i] * scale;
  for (uint64_t i = 0; i < pb.corner_normals_count; ++i) (*pb.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
  REQUIRE(e->item_commit(key) == AR_OK);
  uint64_t g = 0;
  REQUIRE(e->commit(&g) == AR_OK);
}
}  // namespace

TEST_SUITE("fast") {

TEST_CASE("mesh/positions-only edit while the topology generation is pending") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  scene.meshes = std::make_shared<MeshStore>();
  Blocker blocker;
  blocker.stage(scene);
  commit_quad(scene, 5);
  CHECK(!scene.meshes->find(5));  // nothing ran yet: the mesh is only declared
  commit_positions(scene, 5, 4.0f);  // validates against the pending mesh, no polling
  blocker.release.set_value();
  scene.generations().wait_idle();
  CHECK(scene.generations().active() == 3);
  auto rec = scene.meshes->find(5);
  REQUIRE(rec);
  CHECK(rec->bounds_max[0] == 4.0f);
  CHECK(rec->topo->triangle_count() == 2);
}

TEST_CASE("mesh/positions-only edit on a rejected base is skipped with a report") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  scene.meshes = std::make_shared<MeshStore>();
  Blocker blocker;
  blocker.stage(scene);
  commit_quad(scene, 5, [](ar_mesh_buffers& b) { (*b.positions)[0] = std::nanf(""); });
  commit_positions(scene, 5, 2.0f);
  blocker.release.set_value();
  scene.generations().wait_idle();
  CHECK(scene.generations().failed() == 2);
  CHECK(scene.generations().active() == 3);  // the positions generation itself is valid
  CHECK(!scene.meshes->find(5));              // stale topology is never reinstalled
  scene.report().snapshot();
  ar_report_info info{};
  REQUIRE(scene.report().info(&info) == AR_OK);
  REQUIRE(info.entry_count == 1);
  ar_report_entry r{};
  REQUIRE(scene.report().entry(0, &r) == AR_OK);
  CHECK(r.code == AR_REPORT_INPUT_IGNORED);
  CHECK(r.subject == 5);
  CHECK(std::string(reinterpret_cast<const char*>(r.detail)).find("skipped") != std::string::npos);
}

TEST_CASE("mesh/positions count mismatch rejected") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  scene.meshes = std::make_shared<MeshStore>();
  ingest(scene, *scene.meshes, 5, Quad{});
  const MeshTopology& t = *scene.meshes->find(5)->topo;
  CHECK_NOTHROW(check_position_counts(t, 12, 12));
  CHECK_THROWS_WITH_AS(check_position_counts(t, 15, 12), doctest::Contains("positions-only update has 15"), std::runtime_error);
  CHECK_THROWS_AS(check_position_counts(t, 12, 9), std::runtime_error);
  // A key the scene has no mesh for.
  auto e = scene.begin_edit();
  ar_positions_buffers pb{};
  CHECK_THROWS_AS(positions_begin(scene, *e, 99, &pb), std::invalid_argument);
}

TEST_CASE("mesh/remove erases the record when its generation activates") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  scene.meshes = std::make_shared<MeshStore>();
  MeshStore& store = *scene.meshes;
  ingest(scene, store, 5, Quad{});
  ingest(scene, store, 6, Quad{});
  REQUIRE(store.find(5));
  {
    auto e = scene.begin_edit();
    REQUIRE(e->remove(AR_KIND_GEOMETRY, 5) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
  }
  scene.generations().wait_idle();
  CHECK(!store.find(5));
  CHECK(store.find(6));
  // Removal then a re-add of the same key in the same edit keeps the new mesh.
  {
    Quad q;
    ar_mesh_desc d = desc_of(6, 4, 4, 1, "quad");
    auto e = scene.begin_edit();
    REQUIRE(e->remove(AR_KIND_GEOMETRY, 6) == AR_OK);
    ar_mesh_buffers b{};
    REQUIRE(mesh_begin(scene, *e, &d, &b) == AR_OK);
    memcpy(*b.positions, q.pos.data(), 48);
    memcpy(*b.corner_vert, q.cv.data(), 16);
    (*b.face_start)[0] = 0;
    memset(*b.corner_normals, 0, 48);
    REQUIRE(e->item_commit(6) == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
  }
  scene.generations().wait_idle();
  CHECK(store.find(6));
  // reset clears the rest.
  {
    auto e = scene.begin_edit();
    REQUIRE(e->reset() == AR_OK);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
  }
  scene.generations().wait_idle();
  CHECK(store.size() == 0);
  CHECK(store.geometry_bytes() == 0);
}

TEST_CASE("mesh/zero-area and unclippable faces do not reject") {
  // Collinear pentagon: zero area, no triangles. Pentagram order: self-intersecting, so no
  // ear exists and the face becomes a fan (ruling 67).
  std::vector<float> pos;
  for (int i = 0; i < 5; ++i) pos.insert(pos.end(), {float(i), 0.0f, 0.0f});
  for (int i = 0; i < 5; ++i) {
    float a = 6.2831853f * float(i) / 5.0f;
    pos.insert(pos.end(), {std::cos(a), std::sin(a), 0.0f});
  }
  std::vector<uint32_t> cv{0, 1, 2, 3, 4, 5, 7, 9, 6, 8};
  std::vector<uint32_t> fs{0, 5, 10};
  Triangulation t = triangulate(pos.data(), cv.data(), fs.data(), 2);
  CHECK(t.skipped_zero_area == 1);
  CHECK(t.fan_fallback == 1);
  CHECK(t.corners == std::vector<uint32_t>{5, 6, 7, 5, 7, 8, 5, 8, 9});
  CHECK(t.face == std::vector<uint32_t>{1, 1, 1});
}

TEST_CASE("mesh/queued positions edits never block the pool") {
  // More positions-only edits than workers wait on one ingest; they run as continuations
  // when it resolves, whether it succeeds or is rejected. A worker that waited for the
  // ingest would hang here, so the wait is bounded.
  for (bool bad : {false, true}) {
    ContextState ctx;
    Scene scene(ctx, SceneConfig{2, {}});
    scene.meshes = std::make_shared<MeshStore>();
    Blocker blocker;
    blocker.stage(scene);
    commit_quad(scene, 5, [bad](ar_mesh_buffers& b) {
      if (bad) (*b.positions)[3] = std::nanf("");
    });
    for (int i = 0; i < 5; ++i) commit_positions(scene, 5, float(i + 2));
    blocker.release.set_value();
    auto done = std::async(std::launch::async, [&] { scene.generations().wait_idle(); });
    REQUIRE(done.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(scene.generations().active() == 7);
    if (bad) {
      CHECK(!scene.meshes->find(5));
    } else {
      REQUIRE(scene.meshes->find(5));
      CHECK(scene.meshes->find(5)->bounds_max[0] == 6.0f);  // the last edit wins
    }
  }
}

TEST_CASE("mesh/zero-area triangle and quad are skipped") {
  // Ruling 67 covers every face size: collinear triangle and quad emit nothing.
  std::vector<float> pos{0, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0, 0, 0, 1, 0};
  std::vector<uint32_t> cv{0, 1, 2, 0, 1, 2, 3, 0, 1, 4};
  std::vector<uint32_t> fs{0, 3, 7, 10};
  Triangulation t = triangulate(pos.data(), cv.data(), fs.data(), 3);
  CHECK(t.skipped_zero_area == 2);
  CHECK(t.corners == std::vector<uint32_t>{7, 8, 9});
  CHECK(t.face == std::vector<uint32_t>{2});
}

TEST_CASE("mesh/positions-only edit on a rejected replacement is skipped") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  scene.meshes = std::make_shared<MeshStore>();
  ingest(scene, *scene.meshes, 5, Quad{});
  auto before = scene.meshes->find(5);
  Blocker blocker;
  blocker.stage(scene);
  commit_quad(scene, 5, [](ar_mesh_buffers& b) { (*b.positions)[0] = std::nanf(""); });  // rejected replacement
  commit_positions(scene, 5, 9.0f);  // based on the pending replacement
  blocker.release.set_value();
  scene.generations().wait_idle();
  CHECK(scene.meshes->find(5) == before);  // the old record is not touched by a stale update
  CHECK(scene.meshes->find(5)->bounds_max[0] == 1.0f);
}

}  // TEST_SUITE
