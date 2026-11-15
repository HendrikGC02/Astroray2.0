// SPDX-License-Identifier: Apache-2.0
// The kernel tables of one scene snapshot: the active host arrays that earlier tasks build
// (instances and meshes 035/036, material headers, programs and constants 059, images 060, the
// light table 067/068, the rgb2spec, observer, sRGB and E tables 023/046/061/064), copied where
// the kernels read them, plus one header block holding the SceneData, SceneTables, light sampler,
// E table and rgb2spec structs whose pointers KernelGlobals carries. Session-owned tables (camera,
// filter, response, wavelengths) are the session's (kg_builder.h).
//
// Ownership and lifetime (RULINGS 70): a set is built at a session's latch, once per scene
// snapshot, under the scene's table lock, and shared by every session of the scene. A table whose
// source did not change keeps the copy the previous set holds; a session pins the set it latched,
// and a copy is freed when its last holder drops it, which is 0007 d9's retirement. On a CUDA
// context the copies live on the scene's device and charge the scene's ledger by category
// (TABLES, TEXTURES, GEOMETRY); on a CPU context there is no scene device, so arrays the stores
// own are read in place and only derived arrays are copied into host memory, recorded in the
// scene's ledger.
// Ref: docs/decisions/0002-kernel-architecture.md D13 (scene data layout, ledger); 0007 d9
// (retirement), d10, d14 (categories); RULINGS 70.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "host/device.h"
#include "host/ledger.h"
#include "kernel/light/light_sampler.h"
#include "kernel/material/lobes/tables.h"
#include "kernel/scene/scene_view.h"
#include "kernel/vm/context.h"

namespace astroray {
struct ObserverTables;
}

namespace astroray::host {

class Scene;
struct Image;
struct ImageArrays;
struct InstanceTable;
struct LightTable;
struct MaterialArrays;
struct MeshRecord;

// Where table copies go: the scene's device, or host memory when there is none.
struct UploadTarget {
  Device* device = nullptr;
  std::shared_ptr<Ledger> host_ledger;  // records the host copies of the device-less path
  Owner owner{};
};

// One table copy, freed by its last holder.
class DeviceTable {
 public:
  // Copies `bytes` of `data` to the target's device. Without a device, a non-null `pin` means
  // `data` is read in place and `pin` keeps it alive; otherwise it is copied into host memory.
  // Zero bytes give a table with a null pointer. Throws what Device::allocate and upload throw.
  static std::shared_ptr<const DeviceTable> put(const UploadTarget& t, LedgerCategory category, const void* data,
                                                uint64_t bytes, std::shared_ptr<const void> pin = nullptr);
  DeviceTable() = default;
  DeviceTable(const DeviceTable&) = delete;
  DeviceTable& operator=(const DeviceTable&) = delete;
  ~DeviceTable();

  const void* ptr() const { return ptr_; }
  uint64_t bytes() const { return bytes_; }
  uint64_t moved() const { return moved_; }  // bytes copied when it was made; 0 when read in place
  const DeviceBuffer& buffer() const { return buffer_; }  // the device allocation, empty without one

 private:
  Device* device_ = nullptr;
  DeviceBuffer buffer_;
  std::vector<uint8_t> host_;
  std::shared_ptr<const void> pin_;
  std::shared_ptr<Ledger> ledger_;
  uint64_t ledger_id_ = 0;
  const void* ptr_ = nullptr;
  uint64_t bytes_ = 0, moved_ = 0;
};

// The arrays of one mesh where the kernels read them, and its view with their addresses.
struct MeshCopy {
  uint64_t topology_id = 0;
  std::shared_ptr<const DeviceTable> tri_vertex, tri_corners, uv, tri_material, positions, corner_normals;
  ar::DeviceMeshView view{};
};

// Copies `rec`'s arrays. With `prev` of the same topology (a positions-only update) only the
// positions and normals are copied; the topology arrays are shared with `prev`.
std::shared_ptr<const MeshCopy> upload_mesh(const UploadTarget& t, const std::shared_ptr<const MeshRecord>& rec,
                                            const MeshCopy* prev);

// The device record of instance `i` of `table`, whose geometry is mesh `mesh_index`.
ar::DeviceInstanceRecord instance_record(const InstanceTable& table, uint32_t i, uint32_t mesh_index);
// DeviceInstanceRecord::mesh of a lamp instance: lamps have no mesh (105 gives them primitives).
inline constexpr uint32_t kNoMeshIndex = ~0u;

enum class TableId : uint32_t {
  INSTANCES,
  MESHES,
  MATERIAL_SLOTS,
  MATERIAL_HEADERS,
  MATERIAL_PROGRAMS,
  MATERIAL_CONSTANTS,
  IMAGES,
  LIGHTS,
  LIGHT_BINS,
  LIGHT_BIN_LIGHT,
  LIGHT_PMF,
  RGB2SPEC_NODES,
  RGB2SPEC_COEFFS,
  OBSERVER_CMF,
  OBSERVER_ILLUM,
  SRGB_DECODE,
  E_DIEL,
  HEADER,
};
inline constexpr uint32_t kTableCount = static_cast<uint32_t>(TableId::HEADER) + 1u;
const char* table_name(TableId id);

struct SceneTableSet {
  // The snapshot it was built from; the identity the cache compares.
  std::shared_ptr<const MaterialArrays> materials;
  std::shared_ptr<const ImageArrays> images;
  std::shared_ptr<const LightTable> lights;
  std::shared_ptr<const InstanceTable> base_instances;  // the scene's, before lamp records
  std::vector<std::shared_ptr<const MeshRecord>> mesh_records;  // by mesh index (key order)

  std::array<std::shared_ptr<const DeviceTable>, kTableCount> tables;
  std::array<uint32_t, kTableCount> counts{};  // elements per table
  std::array<bool, kTableCount> uploaded{};    // copied while this set was built
  std::map<ar_key, std::shared_ptr<const MeshCopy>> meshes;
  std::vector<std::shared_ptr<const DeviceTable>> texels;  // per image slot
  std::vector<const Image*> texel_sources;                 // what each texel copy was made from
  uint64_t bytes_moved = 0;  // bytes copied building this set: the per-edit upload figure

  // What KernelGlobals points to (device addresses on CUDA), and a host copy of the SceneData.
  const ar::SceneData* scene = nullptr;
  const ar::SceneTables* vm = nullptr;
  const ar::LightSamplerView* light_sampler = nullptr;
  const ar::EDielTable* e_diel = nullptr;
  ar::SceneData scene_host{};
};

// Held by the scene; acquire_scene_tables takes its lock.
struct SceneTableCache {
  std::mutex mutex;
  std::shared_ptr<const SceneTableSet> current;
  std::shared_ptr<const ObserverTables> observer;
};

// The set for the scene's active state with `lights` (the session's epoch table; null for none):
// the cached one when the snapshot is unchanged, else a new one that reuses every unchanged copy.
// Throws std::runtime_error (or what the device throws) whose text names the table that failed;
// the cache then keeps its previous set.
std::shared_ptr<const SceneTableSet> acquire_scene_tables(Scene& scene, std::shared_ptr<const LightTable> lights);

}  // namespace astroray::host
