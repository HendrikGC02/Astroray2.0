// SPDX-License-Identifier: Apache-2.0
// Scene upload: host records to flat device arrays. See scene_upload.h for the layout.
// Ref: docs/decisions/0002-kernel-architecture.md D13; 0007 d10, d14.
#include "device/cuda/scene_upload.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace astroray::host {

SceneUpload::~SceneUpload() {
  // Teardown cannot throw; the device outlives this object by contract.
  for (auto& kv : entries_) free_entry(kv.second);
  device_.free(mesh_table_);
  device_.free(instances_);
  device_.free(material_pool_);
}

DeviceBuffer SceneUpload::put(LedgerCategory category, const void* data, uint64_t bytes) {
  if (bytes == 0) return DeviceBuffer();  // the device refuses zero-byte allocations
  DeviceBuffer b = device_.allocate(category, bytes, owner_);
  try {
    device_.upload(b, 0, data, bytes);
  } catch (...) {
    device_.free(b);
    throw;
  }
  return b;
}

void SceneUpload::free_entry(Entry& e) {
  UploadedMesh& m = e.mesh;
  for (DeviceBuffer* b : {&m.positions, &m.tri_vertex, &m.tri_corners, &m.corner_normals, &m.uv, &m.tri_material})
    device_.free(*b);
}

void SceneUpload::sync_mesh(const std::shared_ptr<const MeshRecord>& rec) {
  auto it = entries_.find(rec->key);
  if (it != entries_.end() && it->second.record == rec) return;
  const MeshTopology& t = *rec->topo;
  const uint32_t V = t.vertex_count;
  // Ruling 68: the representative motion step is (S - 1) / 2 in integer division.
  const size_t step = size_t((t.motion_steps - 1) / 2);

  Entry fresh;
  UploadedMesh& m = fresh.mesh;
  const bool same_topology = it != entries_.end() && it->second.mesh.topology_id == t.id;
  try {
    if (same_topology) {
      // Positions-only update: topology arrays stay, ownership moves to the new entry.
      UploadedMesh& old = it->second.mesh;
      m = old;
      old.tri_vertex = old.tri_corners = old.uv = old.tri_material = DeviceBuffer();
      m.positions = m.corner_normals = DeviceBuffer();
    } else {
      std::vector<uint32_t> tri_vertex(t.tri_corners.size());
      for (size_t i = 0; i < tri_vertex.size(); ++i) tri_vertex[i] = t.corner_vert[t.tri_corners[i]];
      m.tri_vertex = put(LedgerCategory::GEOMETRY, tri_vertex.data(), tri_vertex.size() * 4);
      m.tri_corners = put(LedgerCategory::GEOMETRY, t.tri_corners.data(), t.tri_corners.size() * 4);
      m.uv = put(LedgerCategory::GEOMETRY, t.uv.data(), t.uv.size() * 4);
      m.tri_material = put(LedgerCategory::GEOMETRY, t.tri_material.data(), t.tri_material.size() * 4);
    }
    m.positions = put(LedgerCategory::GEOMETRY, rec->positions.data() + step * V * 3, uint64_t(V) * 12);
    m.corner_normals = put(LedgerCategory::GEOMETRY, rec->corner_normals.data(), rec->corner_normals.size() * 4);
  } catch (...) {
    free_entry(fresh);
    if (same_topology) {  // the topology arrays moved to `fresh`: drop the key so the next sync uploads it whole
      free_entry(it->second);
      entries_.erase(it);
    }
    throw;
  }
  m.key = rec->key;
  m.topology_id = t.id;
  m.version = next_version_++;
  m.view = DeviceMeshView{};
  m.view.positions = uint64_t(reinterpret_cast<uintptr_t>(m.positions.ptr));
  m.view.tri_vertex = uint64_t(reinterpret_cast<uintptr_t>(m.tri_vertex.ptr));
  m.view.tri_corners = uint64_t(reinterpret_cast<uintptr_t>(m.tri_corners.ptr));
  m.view.corner_normals = uint64_t(reinterpret_cast<uintptr_t>(m.corner_normals.ptr));
  m.view.uv = uint64_t(reinterpret_cast<uintptr_t>(m.uv.ptr));
  m.view.tri_material = uint64_t(reinterpret_cast<uintptr_t>(m.tri_material.ptr));
  m.view.vertex_count = V, m.view.triangle_count = t.triangle_count(), m.view.corner_count = t.corner_count;
  m.view.uv_layer_count = t.uv_layer_count;
  if (it != entries_.end()) {
    free_entry(it->second);
    entries_.erase(it);
  }
  fresh.record = rec;
  entries_.emplace(rec->key, std::move(fresh));
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
    free_entry(it->second);
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
  DeviceBuffer next = put(LedgerCategory::TABLES, views.data(), views.size() * sizeof(DeviceMeshView));
  device_.free(mesh_table_);
  mesh_table_ = next;
}

void SceneUpload::upload_instances(const InstanceTable& table) {
  std::vector<DeviceInstanceRecord> recs(table.instances.size());
  for (size_t i = 0; i < recs.size(); ++i) {
    const Instance& s = table.instances[i];
    const UploadedMesh* m = find(s.geom_key);
    if (!m)
      throw std::runtime_error("instance " + std::to_string(i) + " references geometry key " + std::to_string(s.geom_key) +
                               " which was never uploaded");
    DeviceInstanceRecord& d = recs[i];
    d.mesh = m->mesh_index;
    d.random_id = s.random_id, d.pass_index = s.pass_index, d.crypto_object = s.crypto_object;
    d.crypto_asset = s.crypto_asset, d.flags = s.flags;
    d.material_offset = s.material_offset, d.material_count = s.material_count, d.light_index = s.light_index;
    d.mask = s.mask, d.light_group = s.light_group;
    std::memcpy(d.color, s.color, sizeof d.color);
    std::memcpy(d.object_to_world, table.centre(uint32_t(i)).m, sizeof d.object_to_world);
  }
  DeviceBuffer inst = put(LedgerCategory::TABLES, recs.data(), recs.size() * sizeof(DeviceInstanceRecord));
  DeviceBuffer pool;
  try {
    // The pool holds keys until the material compiler resolves them to table slots (059).
    pool = put(LedgerCategory::TABLES, table.material_pool.data(), table.material_pool.size() * sizeof(ar_key));
  } catch (...) {
    device_.free(inst);
    throw;
  }
  device_.free(instances_);
  device_.free(material_pool_);
  instances_ = inst, material_pool_ = pool;
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
