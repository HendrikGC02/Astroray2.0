// SPDX-License-Identifier: Apache-2.0
// OptiX acceleration structures and device scene data: compacted GAS per key, TLAS rebuilt
// per generation, ledger accounting. Meshes are built directly as host records (035's
// ingest is not the thing under test).
// Ref: docs/decisions/0007-blender-integration.md d11, d14; OptiX 9.1 Programming Guide
// (acceleration structure build and compaction).
#include <doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "device/cuda/optix_accel.h"
#include "device/cuda/optix_context.h"
#include "device/cuda/scene_upload.h"
#include "host/device.h"

using namespace astroray;
using namespace astroray::host;

namespace {

// An nx x ny grid of quads in the z = 0 plane, two triangles each, one face per triangle.
std::shared_ptr<const MeshRecord> grid_mesh(ar_key key, uint32_t nx, uint32_t ny, uint64_t topology_id) {
  auto t = std::make_shared<MeshTopology>();
  t->id = topology_id, t->key = key;
  t->vertex_count = (nx + 1) * (ny + 1);
  const uint32_t tris = nx * ny * 2;
  t->corner_count = tris * 3, t->face_count = tris, t->uv_layer_count = 1, t->motion_steps = 1;
  auto rec = std::make_shared<MeshRecord>();
  rec->key = key;
  rec->positions.resize(size_t(t->vertex_count) * 3);
  for (uint32_t y = 0; y <= ny; ++y)
    for (uint32_t x = 0; x <= nx; ++x) {
      float* p = &rec->positions[(size_t(y) * (nx + 1) + x) * 3];
      p[0] = float(x) / float(nx), p[1] = float(y) / float(ny), p[2] = 0.f;
    }
  for (uint32_t y = 0; y < ny; ++y)
    for (uint32_t x = 0; x < nx; ++x) {
      const uint32_t a = y * (nx + 1) + x, b = a + 1, c = a + nx + 1, d = c + 1;
      for (uint32_t v : {a, b, d, a, d, c}) t->corner_vert.push_back(v);
    }
  for (uint32_t i = 0; i < tris * 3; ++i) t->tri_corners.push_back(i);
  for (uint32_t f = 0; f < tris; ++f) {
    t->tri_face.push_back(f);
    t->tri_material.push_back(0);
    t->face_start.push_back(f * 3);
  }
  t->face_start.push_back(tris * 3);
  t->uv.assign(size_t(tris) * 3 * 2, 0.f);
  rec->corner_normals.resize(size_t(t->corner_count) * 3, 0.f);
  rec->topo = t;
  return rec;
}

InstanceTable make_table(const std::vector<ar_key>& keys, float shift) {
  InstanceTable table;
  table.motion_steps = 1;
  for (size_t i = 0; i < keys.size(); ++i) {
    Instance in;
    in.geom_key = keys[i];
    in.mask = 0xFF;
    table.instances.push_back(in);
    ar_xform x = {{1, 0, 0, shift + float(i), 0, 1, 0, 0, 0, 0, 1, 0}};
    table.xf.push_back(x);
  }
  return table;
}

struct Fixture {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev;
  std::unique_ptr<OptixContext> optix;
  std::unique_ptr<SceneUpload> upload;
  std::unique_ptr<OptixAccel> accel;
  Fixture() {
    dev = make_cuda_device(ledger, 0);
    optix = std::make_unique<OptixContext>(*dev, 0);
    upload = std::make_unique<SceneUpload>(*dev, Owner{1, 0});
    accel = std::make_unique<OptixAccel>(*optix, Owner{1, 0});
  }
  // Members that hold device memory are destroyed before the device they use.
  ~Fixture() {
    accel.reset();
    upload.reset();
    optix.reset();
  }
};

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("optix_accel/ledger sums") {
  Fixture f;
  f.upload->sync_mesh(grid_mesh(10, 20, 20, 1));
  f.upload->sync_mesh(grid_mesh(11, 40, 40, 2));
  f.upload->sync_mesh(grid_mesh(12, 10, 10, 3));
  std::vector<ar_key> keys;
  for (int i = 0; i < 10; ++i) keys.push_back(10 + i % 3);
  const InstanceTable table = make_table(keys, 0.f);
  f.upload->upload_instances(table);
  CHECK(f.accel->update_gas(*f.upload) == 3);
  f.accel->build_tlas(*f.upload, table);

  uint64_t gas_sum = 0;
  for (ar_key k : {10, 11, 12}) {
    CHECK(f.accel->gas_bytes(k) > 0);
    CHECK(f.accel->gas_handle(k) != 0);
    gas_sum += f.accel->gas_bytes(k);
  }
  CHECK(f.accel->tlas_handle() != 0);
  CHECK(f.accel->tlas_bytes() > 0);
  // Compacted GAS plus the TLAS are all the ACCELERATION records left: the build arena is gone.
  CHECK(f.ledger.category_total(LedgerCategory::ACCELERATION) == gas_sum + f.accel->tlas_bytes());
  CHECK(f.accel->gas_bytes_total() == gas_sum);
  CHECK(f.ledger.category_total(LedgerCategory::GEOMETRY) > 0);
  CHECK(f.ledger.category_total(LedgerCategory::TABLES) > 0);
}

TEST_CASE("optix_accel/transform update rebuilds only tlas") {
  Fixture f;
  f.upload->sync_mesh(grid_mesh(10, 20, 20, 1));
  f.upload->sync_mesh(grid_mesh(11, 40, 40, 2));
  std::vector<ar_key> keys;
  for (int i = 0; i < 10; ++i) keys.push_back(10 + i % 2);
  InstanceTable table = make_table(keys, 0.f);
  f.upload->upload_instances(table);
  f.accel->update_gas(*f.upload);
  f.accel->build_tlas(*f.upload, table);
  const uint64_t gas_builds = f.accel->gas_builds(), tlas_builds = f.accel->tlas_builds();
  const uint64_t h10 = f.accel->gas_handle(10), h11 = f.accel->gas_handle(11);

  table = make_table(keys, 5.f);  // every transform moved
  f.upload->upload_instances(table);
  CHECK(f.accel->update_gas(*f.upload) == 0);
  f.accel->build_tlas(*f.upload, table);
  CHECK(f.accel->gas_builds() == gas_builds);
  CHECK(f.accel->tlas_builds() == tlas_builds + 1);
  CHECK(f.accel->gas_handle(10) == h10);
  CHECK(f.accel->gas_handle(11) == h11);
}

TEST_CASE("optix_accel/remove frees gas") {
  Fixture f;
  f.upload->sync_mesh(grid_mesh(10, 20, 20, 1));
  f.upload->sync_mesh(grid_mesh(11, 40, 40, 2));
  const InstanceTable table = make_table({10, 11}, 0.f);
  f.upload->upload_instances(table);
  f.accel->update_gas(*f.upload);
  f.accel->build_tlas(*f.upload, table);
  const uint64_t baseline = f.ledger.total();
  const uint64_t baseline_accel = f.ledger.category_total(LedgerCategory::ACCELERATION);

  f.upload->sync_mesh(grid_mesh(12, 30, 30, 3));
  CHECK(f.accel->update_gas(*f.upload) == 1);
  CHECK(f.accel->gas_bytes(12) > 0);
  CHECK(f.ledger.total() > baseline);

  CHECK(f.upload->retain_only({10, 11}) == 1);
  CHECK(f.accel->update_gas(*f.upload) == 0);
  CHECK(f.accel->gas_bytes(12) == 0);
  CHECK(f.ledger.category_total(LedgerCategory::ACCELERATION) == baseline_accel);
  CHECK(f.ledger.total() == baseline);
}

TEST_CASE("optix_accel/positions only update rebuilds that key") {
  Fixture f;
  auto a = grid_mesh(10, 20, 20, 1);
  f.upload->sync_mesh(a);
  f.upload->sync_mesh(grid_mesh(11, 20, 20, 2));
  f.accel->update_gas(*f.upload);
  const uint64_t h11 = f.accel->gas_handle(11);
  auto moved = std::make_shared<MeshRecord>(*a);  // same topology object, new record
  for (float& p : moved->positions) p += 1.f;
  moved->ledger.reset(), moved->ledger_id = 0;
  f.upload->sync_mesh(moved);
  CHECK(f.accel->update_gas(*f.upload) == 1);
  CHECK(f.accel->gas_handle(11) == h11);
  CHECK(f.accel->gas_bytes(10) > 0);
}

TEST_CASE("optix_accel/gas build time 1M triangles") {
  Fixture f;
  f.upload->sync_mesh(grid_mesh(10, 708, 708, 1));  // 1,002,528 triangles
  const uint32_t tris = f.upload->find(10)->view.triangle_count;
  REQUIRE(tris > 1000000);
  CHECK(f.accel->update_gas(*f.upload) == 1);
  const double per_million = f.accel->last_gas_ms() * 1.0e6 / double(tris);
  std::printf("optix_accel: GAS build + compaction %.2f ms for %u triangles = %.2f ms per 10^6 (spike B: 2.9 ms)\n",
              f.accel->last_gas_ms(), tris, per_million);
  CHECK(f.accel->gas_bytes(10) > 0);
}

}  // TEST_SUITE
