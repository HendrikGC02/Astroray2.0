// SPDX-License-Identifier: Apache-2.0
// JMP off: unconditional forward jump; the target is `off` words after the JMP word. A
// non-positive offset would loop or run backwards, so it stops the run in every build.
// Ref: docs/decisions/0005-material-model.md item 31 (JMP, forward-only control flow).
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN void vm_op_jmp(VmState& vm) {
  const int32_t off = int32_t(vm_imm(vm, 0));
  AR_ASSERT(off > 0);
  if (off <= 0) return vm_fail(vm, VmStatus::BAD_JUMP);
  vm.pc += uint32_t(off);
}
AR_NS_END
#define handle_JMP() ::ar::vm_op_jmp(vm)
