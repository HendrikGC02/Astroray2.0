// SPDX-License-Identifier: Apache-2.0
// Test-only module (never embedded): reads every scene table through the device KernelGlobals the
// builder made (host/session/kg_builder.h) and writes, per table, its count and the bytes of its
// first and last element, so the host can compare them with its own arrays (task 102).
// Ref: docs/decisions/0002-kernel-architecture.md D4 (one KernelGlobals pointer), D13.
#include "color/srgb.h"
#include "kernel_globals.h"
#include "light/light_record.h"
#include "light/light_sampler.h"
#include "material/constant_block.h"
#include "material/lobes/tables.h"
#include "scene/scene_view.h"
#include "vm/context.h"

// One row per table, in host/scene/scene_tables.h TableId order up to E_DIEL: count (u32),
// element size (u32), then the first and last element, kKgElemMax bytes each.
constexpr uint32_t kKgElemMax = 432;
constexpr uint32_t kKgRow = 8 + 2 * kKgElemMax;

__device__ void kg_row(uint8_t* out, uint32_t row, const void* p, uint32_t count, uint32_t elem) {
  uint8_t* r = out + uint64_t(row) * kKgRow;
  reinterpret_cast<uint32_t*>(r)[0] = count;
  reinterpret_cast<uint32_t*>(r)[1] = elem;
  if (count == 0u || p == nullptr) return;
  const uint8_t* a = static_cast<const uint8_t*>(p);
  for (uint32_t b = 0; b < elem && b < kKgElemMax; ++b) {
    r[8 + b] = a[b];
    r[8 + kKgElemMax + b] = a[uint64_t(count - 1u) * elem + b];
  }
}

extern "C" __global__ void ar_kg_checksum(const ar::KernelGlobals* kgp, uint8_t* out) {
  if (blockIdx.x != 0 || threadIdx.x != 0) return;
  const ar::KernelGlobals& kg = *kgp;
  const ar::SceneData& s = *kg.scene;
  const ar::SceneTables& t = *kg.tables;
  const ar::LightSamplerView& l = *kg.light_sampler;
  const uint32_t res = t.rgb2spec ? uint32_t(t.rgb2spec->res) : 0u;
  uint32_t r = 0;
  kg_row(out, r++, s.instances, s.instance_count, sizeof(ar::DeviceInstanceRecord));
  kg_row(out, r++, s.meshes, s.mesh_count, sizeof(ar::DeviceMeshView));
  kg_row(out, r++, s.material_slots, kg.material_slot_count, 4);
  // Headers and pass indices end with the default material, one past material_header_count.
  const uint32_t headers = s.material_headers ? s.material_header_count + 1u : 0u;
  kg_row(out, r++, s.material_headers, headers, sizeof(ar::MaterialHeader));
  kg_row(out, r++, s.material_programs, kg.material_program_words, 4);
  kg_row(out, r++, s.material_constants, kg.material_constant_count, sizeof(ar::ConstantBlock));
  kg_row(out, r++, s.material_pass_index, headers, 4);
  kg_row(out, r++, t.images, t.image_count, sizeof(ar::DeviceImageView));
  kg_row(out, r++, kg.lights, l.light_count, sizeof(ar::LightRecord));
  kg_row(out, r++, l.bins, l.bin_count, sizeof(ar::AliasBin));
  kg_row(out, r++, l.bin_light, l.bin_count, 4);
  kg_row(out, r++, l.pmf, l.light_count, 4);
  kg_row(out, r++, kg.light_index, kg.light_index_words, 4);
  kg_row(out, r++, t.rgb2spec ? t.rgb2spec->z_nodes : nullptr, res, 4);
  kg_row(out, r++, t.rgb2spec ? t.rgb2spec->coeffs : nullptr, 9u * res * res * res, 4);
  kg_row(out, r++, t.observer.cmf, 3u * uint32_t(ar::kObsSamples), 4);
  kg_row(out, r++, t.observer.illum, uint32_t(ar::kObsSamples), 4);
  kg_row(out, r++, t.srgb_decode, ar::kSrgbTableSize, 4);
  kg_row(out, r++, kg.e_diel->e, ar::kEDielN * ar::kEDielN * ar::kEDielN, 4);
}
