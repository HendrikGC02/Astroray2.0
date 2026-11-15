// SPDX-License-Identifier: Apache-2.0
// The kernel-visible scene: the device layouts of the mesh table and the instance records
// (uploaded by src/device/cuda/scene_upload.cpp, filled by the CPU device the same way) and
// read-only lookups over them. One definition for the host, the CPU stages and the CUDA
// stages; addresses are integers so the structs are plain data everywhere.
// Ref: docs/decisions/0002-kernel-architecture.md D13 (scene data layout);
// 0007 d10 (corner-indexed lookups), d14.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../kernel_globals.h"
#include "../material/material_header.h"
#include "../portability.h"

AR_NS_BEGIN

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

// light_index of a geometry instance (host/scene/instances.h kNoLight); a lamp instance
// carries the index of its light.
inline constexpr uint32_t kNoLightIndex = ~0u;

// What KernelGlobals::scene points to. Arrays are device (or, on the CPU, host) memory.
struct SceneData {
  const DeviceInstanceRecord* instances;
  uint32_t instance_count;
  const DeviceMeshView* meshes;
  uint32_t mesh_count;
  // Material table slot per material-pool entry, and the MaterialHeader array they index.
  // Both are null until the material compiler fills them; every material is then Common.
  const uint32_t* material_slots;
  const MaterialHeader* material_headers;
  uint32_t material_header_count;
  // The traversal structure of the backend that runs the stages (the CPU's EmbreeScene);
  // opaque to kernels. Backend-specific: a CPU stage handed a GPU SceneData is invalid.
  const void* accel;
};

AR_FN const SceneData& scene_of(const KernelGlobals& kg) {
  AR_ASSERT(kg.scene != nullptr);
  return *kg.scene;
}

AR_FN uint32_t instance_light_index(const KernelGlobals& kg, uint32_t inst) {
  const SceneData& s = scene_of(kg);
  AR_ASSERT(inst < s.instance_count);
  return load_ro(&s.instances[inst].light_index);
}

// Material flags (material_header.h) of the material a hit triangle uses: the triangle's
// slot picks an entry of the instance's slice of the pool (0007 d10), the entry a header.
AR_FN uint32_t hit_material_flags(const KernelGlobals& kg, uint32_t inst, uint32_t prim) {
  const SceneData& s = scene_of(kg);
  if (s.material_headers == nullptr || s.material_slots == nullptr) return 0u;
  AR_ASSERT(inst < s.instance_count);
  const DeviceInstanceRecord& rec = s.instances[inst];
  const uint32_t mesh = load_ro(&rec.mesh);
  AR_ASSERT(mesh < s.mesh_count);
  AR_ASSERT(prim < load_ro(&s.meshes[mesh].triangle_count));
  const int32_t* tri_material = reinterpret_cast<const int32_t*>(uintptr_t(load_ro(&s.meshes[mesh].tri_material)));
  const int32_t slot = load_ro(tri_material + prim);
  const uint32_t count = load_ro(&rec.material_count);
  if (count == 0u) return 0u;  // the default material
  // With tables present a slot outside the instance's slice is malformed upload data.
  AR_ASSERT(slot >= 0 && uint32_t(slot) < count);
  const uint32_t table = load_ro(&s.material_slots[load_ro(&rec.material_offset) + uint32_t(slot)]);
  AR_ASSERT(table < s.material_header_count);
  return load_ro(&s.material_headers[table].flags);
}

AR_NS_END
