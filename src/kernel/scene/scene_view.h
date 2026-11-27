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

struct ConstantBlock;  // material/constant_block.h
struct MediumDesc;     // medium/medium.h
struct MediumBlock;    // medium/medium_block.h
struct ExtParams;      // material/ext_params.h

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
  // The medium inside the instance's closed mesh: 0 for none, else 1 + its index in
  // SceneData::medium_descs (instance_medium below). 0 so that a zeroed record has no medium.
  uint32_t medium;
  float color[4];
  float object_to_world[12];  // row-major 3x4, the representative motion step
};
static_assert(sizeof(DeviceInstanceRecord) == 112, "device layout");

// light_index of a geometry instance (host/scene/instances.h kNoLight); a lamp instance
// carries the index of its light.
inline constexpr uint32_t kNoLightIndex = ~0u;

// The instance mask bit of AR_VISIBILITY_SHADOW: the only bit a shadow ray traces. One definition
// for the host mask builder, Embree and the OptiX raygen (0002 D11 mask table).
inline constexpr uint32_t kShadowMaskBit = 1u << 4;

// Instance mask bit 6, MEDIUM_BOUNDARY: set on every medium-bearing instance whatever its ray visibility,
// so every ray type updates the medium stack at it (0006 D20, 0002 D11). Bit 7 stays spare.
inline constexpr uint32_t kMediumBoundaryMaskBit = 1u << 6;

// The mask of a closest-hit ray: its type bit, plus MEDIUM_BOUNDARY when the scene has media, so a
// medium-bearing instance invisible to the ray type is still met and the ray updates its stack there
// (0002 D11 mask row 6, 0006 D20). A scene without media traces with the bare ray type. One rule for
// Embree and the OptiX raygen.
AR_FN uint32_t closest_hit_mask(uint32_t ray_type, bool media) {
  return media ? (ray_type | kMediumBoundaryMaskBit) : ray_type;
}

// What KernelGlobals::scene points to. Arrays are device (or, on the CPU, host) memory.
struct SceneData {
  const DeviceInstanceRecord* instances;
  uint32_t instance_count;
  const DeviceMeshView* meshes;
  uint32_t mesh_count;
  // Material table slot per material-pool entry, and the MaterialHeader array they index.
  // Both are null until the material compiler fills them; every material is then Common.
  // material_header_count counts the scene's materials; the header, constant-block and pass-index
  // arrays hold one entry more (their table counts are material_header_count + 1): the default
  // material (an instance with no material slice, 0007 appendix) sits last, at index
  // material_header_count. The host compiles it from an empty surface graph (every OpenPBR input
  // at its default, no emission, pass index 0), so a kernel treats it like any material.
  const uint32_t* material_slots;
  const MaterialHeader* material_headers;
  uint32_t material_header_count;
  // What the headers index: program words (program_offset counts words; each program starts with
  // its entry table) and constant blocks (const_offset counts blocks). Filled with the headers.
  const uint32_t* material_programs;
  const ConstantBlock* material_constants;
  // Per header, the default's included: the material's pass index (ar_material_desc::pass_index,
  // the IndexMA AOV of 0008 d10); 0 for the default material.
  const uint32_t* material_pass_index;
  // The spectrum arena the TABLE SpectrumRefs of the constant blocks index (slot_resolve.h): floats,
  // each spectrum its wavelengths then its values; material_spectra_count floats, null exactly when
  // no material holds a TABLE.
  const float* material_spectra;
  uint32_t material_spectra_count;
  // One MediumDesc per medium of the scene, in key order (Scene::media()); null exactly when the scene
  // has none. DeviceInstanceRecord::medium indexes it; `program` is the medium's block index.
  const MediumDesc* medium_descs;
  uint32_t medium_count;
  // The world program (0005 item 32): an entry table followed by the emission entry, laid out as a
  // material program, and its constant block (one entry). Both null unless the world's light record
  // carries kLightWorldProgram, which is exactly when shade_background runs them.
  const uint32_t* world_program;
  uint32_t world_program_words;
  const ConstantBlock* world_constants;
  // The media's constant blocks (MediumDesc::program indexes them, one per medium in the same order)
  // and their volume-program words, word 0 reserved so a block entry of 0 means no program
  // (medium/medium_eval.h MediumTables). Null exactly when the scene has no medium.
  const MediumBlock* medium_blocks;
  const uint32_t* medium_programs;
  uint32_t medium_program_words;
  // The traversal structure of the backend that runs the stages, opaque to kernels and never null
  // in a session's scene (an empty scene has an empty structure). CPU: the snapshot's EmbreeScene.
  // CUDA: the OptixTraversableHandle of its TLAS as an integer; the launch reads it from the host
  // copy into OptixLaunchParams::traversable (kernel/launch_config.h), device code never
  // dereferences it. Backend-specific: a CPU stage handed a GPU SceneData is invalid.
  const void* accel;
  // The extension kinds' per-material constants (0005 item 16): one 64 B block per extension
  // material, indexed by MaterialHeader::ext's ext_index; ext_param_count blocks, null exactly when no
  // material has an extension kind.
  const ExtParams* ext_params;
  uint32_t ext_param_count;
  // The scene TableArena (0005 item 16): a byte buffer of the kinds' tables (a grating's efficiency
  // table, ext/grating_table.h), each starting at a 16 B aligned byte offset that its ExtParams word
  // names (GRATING: w[2]); identical tables are stored once. table_arena_bytes bytes, null exactly when
  // no material holds a table.
  const uint8_t* table_arena;
  uint32_t table_arena_bytes;
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

// instance_medium of an instance with no medium.
inline constexpr uint32_t kNoMedium = ~0u;

// The index in SceneData::medium_descs of the medium inside instance `inst`'s closed mesh, kNoMedium
// for an instance without one. The host builds the records from keys it has checked, so an index
// past medium_count is a corrupt upload.
AR_FN uint32_t instance_medium(const KernelGlobals& kg, uint32_t inst) {
  const SceneData& s = scene_of(kg);
  AR_ASSERT(inst < s.instance_count);
  const uint32_t m = load_ro(&s.instances[inst].medium);
  AR_ASSERT(m <= s.medium_count);
  return m == 0u ? kNoMedium : m - 1u;
}

// Whether a ray of `ray_type` met instance `inst` through MEDIUM_BOUNDARY alone: the instance's mask
// lacks the ray type's bit, so the hit is a false interface whatever the priorities (0002 D11 row 6).
AR_FN bool instance_met_by_boundary_bit_only(const KernelGlobals& kg, uint32_t inst, uint32_t ray_type) {
  const SceneData& s = scene_of(kg);
  AR_ASSERT(inst < s.instance_count);
  return (load_ro(&s.instances[inst].mask) & ray_type) == 0u;
}

// The material-header index of the material a hit triangle uses: the triangle's slot picks an
// entry of the instance's slice of the pool (0007 d10), the entry a header. The default material
// (an instance with an empty slice, or a scene without material tables) is exactly
// material_header_count, one past the last counted header, where the host uploads it.
AR_FN uint32_t hit_material_id(const KernelGlobals& kg, uint32_t inst, uint32_t prim) {
  const SceneData& s = scene_of(kg);
  const uint32_t dflt = s.material_header_count;
  if (s.material_headers == nullptr || s.material_slots == nullptr) return dflt;
  AR_ASSERT(inst < s.instance_count);
  const DeviceInstanceRecord& rec = s.instances[inst];
  const uint32_t mesh = load_ro(&rec.mesh);
  AR_ASSERT(mesh < s.mesh_count);
  AR_ASSERT(prim < load_ro(&s.meshes[mesh].triangle_count));
  const int32_t* tri_material = reinterpret_cast<const int32_t*>(uintptr_t(load_ro(&s.meshes[mesh].tri_material)));
  const int32_t slot = load_ro(tri_material + prim);
  const uint32_t count = load_ro(&rec.material_count);
  if (count == 0u) return dflt;  // the default material
  // With tables present a slot outside the instance's slice is malformed upload data.
  AR_ASSERT(slot >= 0 && uint32_t(slot) < count);
  const uint32_t table = load_ro(&s.material_slots[load_ro(&rec.material_offset) + uint32_t(slot)]);
  AR_ASSERT(table < s.material_header_count);
  return table;
}

// Material flags (material_header.h) of a hit's material; 0 for the default material.
AR_FN uint32_t hit_material_flags(const KernelGlobals& kg, uint32_t inst, uint32_t prim) {
  const SceneData& s = scene_of(kg);
  const uint32_t id = hit_material_id(kg, inst, prim);
  if (id == s.material_header_count) return 0u;
  return load_ro(&s.material_headers[id].flags);
}

AR_NS_END
