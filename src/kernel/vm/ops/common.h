// SPDX-License-Identifier: Apache-2.0
// What every op body shares: the interpreter state, the status an aborted run reports, and the
// operand checks. An op reads its operands from the instruction word (a, b, c) and its
// immediates (the words after it), writes through the state, and advances pc past itself.
// Ref: docs/decisions/0005-material-model.md item 31 (encoding, registers, forward-only flow).
#pragma once
#include <cstdint>

#include "../../core/real.h"
#include "../../core/spectrum.h"
#include "../../material/openpbr_params.h"
#include "../../portability.h"
#include "../context.h"
#include "../vm_regs.h"

AR_NS_BEGIN

inline constexpr uint32_t kVmRegisterCount = 32;
static_assert(sizeof(VmRegs) == kVmRegisterCount * sizeof(float), "the register file is 32 f32");

// Why a run stopped early. Kernels turn a non-OK status into a counter (the host reports it);
// nothing continues past an error and nothing is clamped to hide one.
enum class VmStatus : uint32_t {
  OK = 0,
  BAD_OPCODE,    // opcode byte outside the table, or a test-only op in a build without the hooks
  BAD_REGISTER,  // an operand's registers leave the 32-register file
  BAD_OPERAND,   // an operand byte or immediate that the op does not accept
  BAD_JUMP,      // JMP with a non-positive offset (control flow is forward only)
  UNSUPPORTED,   // an UNSUPPORTED op ran: the compiler should have replaced the program
  NULL_POINTER   // the op needs the record, Le or the tables and the caller passed none
};

struct VmState {
  const uint32_t* prog;
  uint32_t pc;  // word index of the instruction being run
  float* r;
  const ShadeContext* ctx;
  const SceneTables* tables;
  OpenPBRParams* rec;
  Spec* le;
  // The selection uniform of a surface entry (0005 items 31, 38): the stage sets it to the event's
  // lobe-choice uniform, only BRANCH_MIX rewrites it, and the stage's lobe choice reads it after the
  // entry (0004 D14: Mix selection and lobe choice share one uniform). Null for an entry run
  // without one; a BRANCH_MIX there stops the run.
  float* select_u;
  uint32_t a, b, c;  // operand bytes of the instruction word
  VmStatus status;
  bool done;
};

AR_FN uint32_t vm_imm(const VmState& vm, uint32_t i) { return vm.prog[vm.pc + 1u + i]; }

AR_FN void vm_fail(VmState& vm, VmStatus s) { vm.status = s; }

// Sets BAD_REGISTER and returns false when [first, first + width) leaves the register file.
AR_FN bool vm_regs_ok(VmState& vm, uint32_t first, uint32_t width) {
  if (first + width > kVmRegisterCount) {
    vm_fail(vm, VmStatus::BAD_REGISTER);
    return false;
  }
  return true;
}

AR_NS_END
