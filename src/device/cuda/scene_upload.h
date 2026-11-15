// SPDX-License-Identifier: Apache-2.0
// Device scene data: mesh arrays, the mesh table, instance records and the material-table
// pool, uploaded as flat arrays. Every allocation goes through the Device's ledger
// (0007 d14): mesh arrays are GEOMETRY, the tables TABLES. The structs below are the
// layout kernels read; they hold device addresses as integers so the same definitions
// are plain data on the host.
// Ref: docs/decisions/0002-kernel-architecture.md D13 (scene data layout, ledger);
// 0007 d10 (corner-indexed lookups), d14.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "host/device.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"

namespace astroray::host {

// One entry of the device mesh table, indexed by `DeviceInstanceRecord::mesh`. A hit reads
// the triangle's corners from tri_corners, then normals by corner, UVs by layer and corner,
// and the material slot by triangle (0007 d10).
struct DeviceMeshView {
  uint64_t positions;        // float3 x vertex_count, the representative motion step
  uint64_t tri_vertex;       // uint3 x triangle_count: vertex indices (what the GAS reads)
  uint64_t tri_corners;      // uint3 x triangle_count: corner indices
  uint64_t corner_normals;   // float3 x corner_count
  uint64_t uv;               // float2 x corner_count, uv_layer_count layers back to back
  uint64_t tri_material;     // int32 x triangle_count
  uint32_t vertex_count, triangle_count, corner_count, uv_layer_count;
};
static_assert(sizeof(DeviceMeshView) == 64, "device layout");

struct DeviceInstanceRecord {
  uint32_t mesh;  // index into the mesh table (upload_instances throws for a key that was never uploaded)
  uint32_t random_id, pass_index, crypto_object, crypto_asset;
  uint32_t flags;  // AR_INSTANCE_*
  uint32_t material_offset, material_count, light_index;
  uint32_t mask, light_group;
  float color[4];
  float object_to_world[12];  // row-major 3x4, the representative motion step
};
static_assert(sizeof(DeviceInstanceRecord) == 108, "device layout");

// The geometry of one key on the device. `version` moves whenever an array is replaced, so
// acceleration code rebuilds exactly the keys whose data changed.
struct UploadedMesh {
  ar_key key = 0;
  uint64_t topology_id = 0, version = 0;
  uint32_t mesh_index = 0;  // slot in the current mesh table
  DeviceMeshView view{};
  DeviceBuffer positions, tri_vertex, tri_corners, corner_normals, uv, tri_material;
};

class SceneUpload {
 public:
  SceneUpload(Device& device, Owner owner) : device_(device), owner_(owner) {}
  ~SceneUpload();
  SceneUpload(const SceneUpload&) = delete;
  SceneUpload& operator=(const SceneUpload&) = delete;

  // Uploads or replaces the arrays of `rec`. A record sharing the topology of the one
  // already uploaded (a positions-only update) replaces positions and normals only.
  // Unchanged records (same pointer) are skipped.
  void sync_mesh(const std::shared_ptr<const MeshRecord>& rec);
  // Frees every uploaded mesh whose key is not in `keep`; returns how many were dropped.
  size_t retain_only(const std::vector<ar_key>& keep);
  // Rebuilds the mesh table, then the instance and material-pool arrays for `table`, every
  // record's mesh index re-resolved from its geometry key so indices never go stale. Throws
  // std::runtime_error naming the instance when its geometry key was never synced.
  void upload_instances(const InstanceTable& table);

  const UploadedMesh* find(ar_key key) const;
  std::vector<const UploadedMesh*> meshes() const;  // in mesh-table order
  const DeviceBuffer& mesh_table() const { return mesh_table_; }
  const DeviceBuffer& instance_records() const { return instances_; }
  const DeviceBuffer& material_pool() const { return material_pool_; }
  uint32_t instance_count() const { return instance_count_; }

 private:
  struct Entry {
    UploadedMesh mesh;
    std::shared_ptr<const MeshRecord> record;
  };
  DeviceBuffer put(LedgerCategory category, const void* data, uint64_t bytes);
  void free_entry(Entry& e);
  void rebuild_mesh_table();

  Device& device_;
  Owner owner_;
  std::map<ar_key, Entry> entries_;
  DeviceBuffer mesh_table_, instances_, material_pool_;
  uint32_t instance_count_ = 0;
  uint64_t next_version_ = 1;
};

}  // namespace astroray::host
