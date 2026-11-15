// SPDX-License-Identifier: Apache-2.0
// Scene upload: host records to flat device arrays. See scene_upload.h for the layout.
// Ref: docs/decisions/0002-kernel-architecture.md D13; 0007 d10, d14; RULINGS 70.
#include "device/cuda/scene_upload.h"

#include <stdexcept>
#include <string>

namespace astroray::host {

const DeviceBuffer& SceneUpload::buffer(const std::shared_ptr<const DeviceTable>& t) {
  static const DeviceBuffer none{};
  return t ? t->buffer() : none;
}

void SceneUpload::sync_mesh(const std::shared_ptr<const MeshRecord>& rec) {
  auto it = entries_.find(rec->key);
  if (it != entries_.end() && it->second.record == rec) return;
  Entry fresh;
  fresh.mesh.copy = upload_mesh(target_, rec, it != entries_.end() ? it->second.mesh.copy.get() : nullptr);
  fresh.mesh.key = rec->key;
  fresh.mesh.topology_id = rec->topo->id;
  fresh.mesh.version = next_version_++;
  fresh.mesh.view = fresh.mesh.copy->view;
  fresh.record = rec;
  // The replaced copy is freed by its last holder, never under a reader that still holds it.
  entries_[rec->key] = std::move(fresh);
  rebuild_mesh_table();
}

size_t SceneUpload::retain_only(const std::vector<ar_key>& keep) {
  size_t dropped = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    bool kept = false;
    for (ar_key k : keep) kept = kept || k == it->first;
    if (kept) {
      ++it;
      continue;
    }
    it = entries_.erase(it);
    ++dropped;
  }
  if (dropped) rebuild_mesh_table();
  return dropped;
}

void SceneUpload::rebuild_mesh_table() {
  std::vector<DeviceMeshView> views;
  uint32_t i = 0;
  for (auto& kv : entries_) {
    kv.second.mesh.mesh_index = i++;
    views.push_back(kv.second.mesh.view);
  }
  mesh_table_ = DeviceTable::put(target_, LedgerCategory::TABLES, views.data(), views.size() * sizeof(DeviceMeshView));
}

void SceneUpload::upload_instances(const InstanceTable& table) {
  rebuild_mesh_table();
  std::vector<DeviceInstanceRecord> recs(table.instances.size());
  for (size_t i = 0; i < recs.size(); ++i) {
    const Instance& s = table.instances[i];
    // A lamp's instance (068) has no mesh: its shape is 105's unit-shape GAS, which 114 puts in the TLAS.
    if (s.geom_key == 0 && s.light_index != ar::kNoLightIndex) {
      recs[i] = instance_record(table, uint32_t(i), kNoMeshIndex);
      continue;
    }
    const UploadedMesh* m = find(s.geom_key);
    if (!m)
      throw std::runtime_error("instance " + std::to_string(i) + " references geometry key " + std::to_string(s.geom_key) +
                               " which was never uploaded");
    recs[i] = instance_record(table, uint32_t(i), m->mesh_index);
  }
  auto inst = DeviceTable::put(target_, LedgerCategory::TABLES, recs.data(), recs.size() * sizeof(DeviceInstanceRecord));
  // The pool holds material keys; scene table sets resolve them to header indices (scene_tables.h).
  auto pool = DeviceTable::put(target_, LedgerCategory::TABLES, table.material_pool.data(),
                               table.material_pool.size() * sizeof(ar_key));
  instances_ = std::move(inst), material_pool_ = std::move(pool);
  instance_count_ = uint32_t(recs.size());
}

const UploadedMesh* SceneUpload::find(ar_key key) const {
  auto it = entries_.find(key);
  return it == entries_.end() ? nullptr : &it->second.mesh;
}

std::vector<const UploadedMesh*> SceneUpload::meshes() const {
  std::vector<const UploadedMesh*> out;
  for (auto& kv : entries_) out.push_back(&kv.second.mesh);
  return out;
}

}  // namespace astroray::host
