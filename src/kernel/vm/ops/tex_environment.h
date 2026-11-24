// SPDX-License-Identifier: Apache-2.0
// TEX_ENVIRONMENT a, b, imm slot, imm mode: r[a..a+2] = the colour of image `slot` in the direction
// r[b..b+2], read as an equirectangular environment map. The direction is normalised, mapped to
// (u, v) as Blender's Environment Texture node does, and looked up by tex_image.h's one sampler, so
// filtering, the image table and the sRGB decode are TEX_IMAGE's. mode bit 0 selects LINEAR over
// CLOSEST. Blender fixes the extension at REPEAT for environment images (no EXTEND), so the EXTEND
// bit is a bad operand here rather than an ignored one. A world program feeds it the ray direction
// through LOAD_CTX_P (the context position of a background shade is the ray direction). Cycles'
// Mirror Ball projection has no op: the addon reports it and the world renders magenta.
// u = (atan2(d.y, d.x) - pi) / (-2 pi) and v = (acos(d.z / |d|) - pi) / (-pi): +x is u = 0.5, +y is
// u = 0.25, -y is u = 0.75, and +z is v = 1, the image's top row (rows are stored bottom-up).
// Cycles has no guard on the acos argument; a normalised vector's z can round just above 1, which
// would turn a texel on the pole into NaN, so the argument is clamped to [-1, 1]. Every value that
// was finite is unchanged.
// Ref: docs/decisions/0005-material-model.md items 31, 34;
// third_party/_cache/refs/cycles-ed69ef2c6bd3/src/kernel/svm/image.h svm_node_tex_environment,
// svm_node_tex_environment_projection (safe_normalize, then equirectangular);
// src/kernel/camera/projection.h direction_to_equirectangular_range with range
// (-2 pi, pi, -pi, pi); src/scene/shader_nodes.cpp EnvironmentTextureNode::image_params
// (extension REPEAT). Apache-2.0.
#pragma once
#include "tex_image.h"

AR_NS_BEGIN

inline constexpr float kEnvPi = 3.14159265358979323846f;  // Cycles M_PI_F
inline constexpr float kEnvTwoPi = 6.283185307179586232f;  // Cycles M_2PI_F

// (u, v) of direction d in Cycles' equirectangular convention; (0, 0) for the zero vector.
AR_FN void env_direction_uv(const float* d, float* uv) {
  float x = d[0], y = d[1], z = d[2];
  const float n = ar::sqrt(x * x + y * y + z * z);  // Cycles len(): sqrtf(dot)
  if (n != 0.0f) {  // safe_normalize
    const float inv = 1.0f / n;
    x = x * inv, y = y * inv, z = z * inv;
  }
  if (x == 0.0f && y == 0.0f && z == 0.0f) {  // is_zero: direction_to_equirectangular returns (0, 0)
    uv[0] = 0.0f, uv[1] = 0.0f;
    return;
  }
  const float len = ar::sqrt(x * x + y * y + z * z);
  float c = z / len;
  c = c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c);  // see the header: NaN-free on the poles
  uv[0] = (ar::atan2(y, x) - kEnvPi) / -kEnvTwoPi;
  uv[1] = (ar::acos(c) - kEnvPi) / -kEnvPi;
}

// `dir` is read before `out` is written, so they may alias.
AR_FN void op_tex_environment(const SceneTables& t, uint32_t slot, uint32_t mode, const float* dir, float* out) {
  float uv[2];
  env_direction_uv(dir, uv);
  op_tex_image(t, slot, mode, uv, out);
}

AR_FN void vm_op_tex_environment(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 3) || !vm_regs_ok(vm, vm.b, 3)) return;
  const uint32_t mode = vm_imm(vm, 1);
  if ((mode & ~kTexLinear) != 0u) return vm_fail(vm, VmStatus::BAD_OPERAND);
  if (vm.tables == nullptr ||
      (vm.tables->image_count != 0u && (vm.tables->images == nullptr || vm.tables->srgb_decode == nullptr)))
    return vm_fail(vm, VmStatus::NULL_POINTER);
  op_tex_environment(*vm.tables, vm_imm(vm, 0), mode, vm.r + vm.b, vm.r + vm.a);
  vm.pc += 3;
}
AR_NS_END
#define handle_TEX_ENVIRONMENT() ::ar::vm_op_tex_environment(vm)
