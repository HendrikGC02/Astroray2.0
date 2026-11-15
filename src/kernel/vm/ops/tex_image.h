// SPDX-License-Identifier: Apache-2.0
// TEX_IMAGE a, b, imm: r[a..a+2] = the image colour at the coordinates r[b..b+2]; imm names the
// image and its sampling mode. Filtering is not implemented yet, so every lookup is magenta, the
// project's loud "something is missing" colour; the filter replaces op_tex_image's body.
// Ref: docs/decisions/0005-material-model.md items 31, 34 (a missing image gives magenta).
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN void op_tex_image(float* out) {
  out[0] = 1.0f;
  out[1] = 0.0f;
  out[2] = 1.0f;
}

AR_FN void vm_op_tex_image(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 3) || !vm_regs_ok(vm, vm.b, 3)) return;
  op_tex_image(vm.r + vm.a);
  vm.pc += 2;
}
AR_NS_END
#define handle_TEX_IMAGE() ::ar::vm_op_tex_image(vm)
