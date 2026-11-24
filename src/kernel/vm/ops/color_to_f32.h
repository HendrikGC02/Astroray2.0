// SPDX-License-Identifier: Apache-2.0
// COLOR_TO_F32 a, b: r[a] = dot(r[b..b+2], Y row), the luminance of a scene-linear colour in the
// working space. Blender converts a colour linked to a float socket this way, so the lowering
// emits it where such a link exists. The row is the host's inverse(M_ws) row 1, carried in the
// working-space table (Rgb2SpecTable::y_row); the handler and the compiler's fold call the one
// function below with that same row. A zero row means the host never filled it: the run stops.
// Ref: Cycles kernel/svm/convert.h (NODE_CONVERT_CF) and kernel/util/colorspace.h
// (linear_rgb_to_gray = dot(c, film.rgb_to_y)); scene/shader.cpp sets rgb_to_y = rgb_to_xyz.y.
// Ref: docs/decisions/0005-material-model.md items 36, 42.
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN float op_color_to_f32(const float* c, const float* y_row) {
  return c[0] * y_row[0] + c[1] * y_row[1] + c[2] * y_row[2];
}

AR_FN void vm_op_color_to_f32(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 1) || !vm_regs_ok(vm, vm.b, 3)) return;
  if (vm.tables == nullptr || vm.tables->rgb2spec == nullptr) return vm_fail(vm, VmStatus::NULL_POINTER);
  const float* y = vm.tables->rgb2spec->y_row;
  if (y[0] == 0.0f && y[1] == 0.0f && y[2] == 0.0f) return vm_fail(vm, VmStatus::NULL_POINTER);
  vm.r[vm.a] = op_color_to_f32(vm.r + vm.b, y);
  vm.pc += 1;
}
AR_NS_END
#define handle_COLOR_TO_F32() ::ar::vm_op_color_to_f32(vm)
