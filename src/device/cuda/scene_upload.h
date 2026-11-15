// SPDX-License-Identifier: Apache-2.0
// Device scene data: mesh arrays, the mesh table, instance records and the material-table
// pool, uploaded as flat arrays for the acceleration code (040) and its tests. Every allocation
// goes through the Device's ledger (0007 d14): mesh arrays are GEOMETRY, the tables TABLES. The
// copies are host/scene/scene_tables.h's DeviceTable, freed by their last holder (RULINGS 70),
// and the mesh copy and instance record are built by the same functions the scene table sets use.
// Ref: docs/decisions/0002-kernel-architecture.md D13 (scene data layout, ledger);
// 0007 d10 (corner-indexed lookups), d14; RULINGS 70.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "host/device.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene_tables.h"
#include "kernel/scene/scene_view.h"

namespace astroray::host {

// The device layouts live in the kernel-visible scene view: one definition for CPU and GPU.
using ar::DeviceInstanceRecord;
using ar::DeviceMeshView;

// The geometry of one key on the device. `version` moves whenever an array is replaced, so
// acceleration code rebuilds exactly the keys whose data changed.
struct UploadedMesh {
  ar_key key = 0;
  uint64_t topology_id = 0, version = 0;
  uint32_t mesh_index = 0;  // slot in the current mesh table
  DeviceMeshView view{};
  std::shared_ptr<const MeshCopy> copy;  // the arrays view points to
};

class SceneUpload {
 public:
  SceneUpload(Device& device, Owner owner) : target_{&device, nullptr, owner} {}
  SceneUpload(const SceneUpload&) = delete;
  SceneUpload& operator=(const SceneUpload&) = delete;

  // Uploads or replaces the arrays of `rec`. A record sharing the topology of the one
  // already uploaded (a positions-only update) replaces positions and normals only.
  // Unchanged records (same pointer) are skipped. A failure leaves the key as it was.
  void sync_mesh(const std::shared_ptr<const MeshRecord>& rec);
  // Drops every uploaded mesh whose key is not in `keep`; returns how many were dropped.
  size_t retain_only(const std::vector<ar_key>& keep);
  // Rebuilds the mesh table, then the instance and material-pool arrays for `table`, every
  // record's mesh index re-resolved from its geometry key so indices never go stale. Throws
  // std::runtime_error naming the instance when its geometry key was never synced.
  void upload_instances(const InstanceTable& table);

  const UploadedMesh* find(ar_key key) const;
  std::vector<const UploadedMesh*> meshes() const;  // in mesh-table order
  const DeviceBuffer& mesh_table() const { return buffer(mesh_table_); }
  const DeviceBuffer& instance_records() const { return buffer(instances_); }
  const DeviceBuffer& material_pool() const { return buffer(material_pool_); }
  uint32_t instance_count() const { return instance_count_; }

 private:
  struct Entry {
    UploadedMesh mesh;
    std::shared_ptr<const MeshRecord> record;
  };
  static const DeviceBuffer& buffer(const std::shared_ptr<const DeviceTable>& t);
  void rebuild_mesh_table();

  UploadTarget target_;
  std::map<ar_key, Entry> entries_;
  std::shared_ptr<const DeviceTable> mesh_table_, instances_, material_pool_;
  uint32_t instance_count_ = 0;
  uint64_t next_version_ = 1;
};

}  // namespace astroray::host
