// SPDX-License-Identifier: Apache-2.0
// TEST_INJECT_NAN a: r[a] = quiet NaN, so a test can prove the non-finite counters fire
// (0004 D17). Exists only in gate-hook builds; elsewhere it is an invalid opcode.
// Ref: docs/decisions/0009-build-test-process.md (gate hooks); 0004 D17.
#pragma once
#include "common.h"

AR_NS_BEGIN
#if defined(AR_GATE_HOOKS)
AR_FN void vm_op_test_inject_nan(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 1)) return;
  vm.r[vm.a] = bit_cast<float>(0x7fc00000u);
  vm.pc += 1;
}
#else
AR_FN void vm_op_test_inject_nan(VmState& vm) { vm_fail(vm, VmStatus::BAD_OPCODE); }
#endif
AR_NS_END
#define handle_TEST_INJECT_NAN() ::ar::vm_op_test_inject_nan(vm)
