// SPDX-License-Identifier: Apache-2.0
// Test-only module (never embedded): runs one VM program per thread on host-generated inputs,
// built twice, no-FMA and production (027), so the test can compare the interpreter's registers,
// Le and record on the GPU with the CPU build of the same headers.
// Ref: docs/decisions/0005-material-model.md Verification 6; 0002 D12; 0009 d17.
#include "medium/medium_eval.h"
#include "vm/interp.h"

// Input row (27 floats): r0..r15, P (3), uv set 0 (2), uv set 1 (2), four wavelengths.
// Output row (30 floats): r16..r31, Le (4), record base_weight, base_color (4), specular_ior (4),
// the run's status.
constexpr uint32_t kVmAgreeIn = 27;
constexpr uint32_t kVmAgreeOut = 30;

extern "C" __global__ void ar_vm_agree(const float* in, float* out, uint32_t n, const uint32_t* prog, uint32_t entry,
                                       const float* z_nodes, const float* coeffs, int32_t res, const float* illum) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float* row = in + kVmAgreeIn * i;
  ar::VmRegs regs;
  for (uint32_t k = 0; k < 16; ++k) {
    regs.r[k] = row[k];
    regs.r[16 + k] = 0.0f;
  }
  ar::ShadeContext ctx = {};
  ctx.P = ar::Real3{row[16], row[17], row[18]};
  ctx.uv[0] = ar::Real2{row[19], row[20]};
  ctx.uv[1] = ar::Real2{row[21], row[22]};
  for (int l = 0; l < 4; ++l) ctx.lambda_t.nm[l] = row[23 + l];
  ar::Rgb2SpecTable table = {};
  table.res = res;
  table.z_nodes = z_nodes;
  table.coeffs = coeffs;
  ar::SceneTables tables = {};
  tables.images = nullptr;  // no image table: TEX_IMAGE samples magenta (tex_agree.cu has images)
  tables.image_count = 0;
  tables.rgb2spec = &table;
  tables.observer.cmf = nullptr;  // the ops use the illuminant only
  tables.observer.illum = illum;
  ar::OpenPBRParams rec = {};
  ar::Spec le = {};
  float select_u = row[19];  // the selection uniform BRANCH_MIX reads and rescales
  const ar::VmStatus status = ar::run_entry(prog, entry, &regs, ctx, tables, &rec, &le, &select_u);
  float* o = out + kVmAgreeOut * i;
  for (uint32_t k = 0; k < 16; ++k) o[k] = regs.r[16 + k];
  o[15] = select_u;  // in r31's column: no program writes r31
  for (int l = 0; l < 4; ++l) {
    o[16 + l] = le.v[l];
    o[21 + l] = rec.base_color.v[l];
    o[25 + l] = rec.specular_ior.v[l];
  }
  o[20] = rec.base_weight;
  o[29] = float(uint32_t(status));
}

// The volume entry: VOLUME_COEFFS through run_volume_entry on the medium block at `block`. Input
// rows are the same as above (only the wavelengths are read). Output row (23 floats): sigma_a,
// sigma_s, emission, the two lobes' weights (4 each), the lobes' g, the run's status.
constexpr uint32_t kVmAgreeVolumeOut = 23;

extern "C" __global__ void ar_vm_agree_volume(const float* in, float* out, uint32_t n, const uint32_t* prog,
                                              uint32_t entry, const ar::MediumBlock* block, const float* illum) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float* row = in + kVmAgreeIn * i;
  ar::VmRegs regs = {};
  ar::ShadeContext ctx = {};
  ctx.P = ar::Real3{row[16], row[17], row[18]};
  for (int l = 0; l < 4; ++l) ctx.lambda_t.nm[l] = row[23 + l];
  ar::SceneTables tables = {};
  tables.observer.illum = illum;
  ar::MediumSample s = ar::medium_sample_zero();
  const ar::VmStatus status = ar::run_volume_entry(prog, entry, &regs, ctx, tables, block, &s);
  float* o = out + kVmAgreeVolumeOut * i;
  for (int l = 0; l < 4; ++l) {
    o[l] = s.sigma_a.v[l];
    o[4 + l] = s.sigma_s.v[l];
    o[8 + l] = s.emission.v[l];
    o[12 + l] = s.phase[0].weight.v[l];
    o[16 + l] = s.phase[1].weight.v[l];
  }
  o[20] = s.phase[0].p0;
  o[21] = s.phase[1].p0;
  o[22] = float(uint32_t(status));
}
