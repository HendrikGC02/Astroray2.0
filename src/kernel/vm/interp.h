// SPDX-License-Identifier: Apache-2.0
// The interpreter: runs one entry of a bytecode program against a register file the calling
// kernel owns, filling the OpenPBRParams record and Le (a surface run) or a MediumSample (the
// volume entry, run_volume_entry, 0006 A.2). Control flow is forward only, so a
// program terminates and its length bounds its time. Each opcode's body is a kernel-library
// function under ops/; the host constant folder calls the CPU build of the same functions, so
// folding cannot disagree with evaluation. The switch is built from ops.def, so a new row
// without a handler macro does not compile.
// Encoding: 32-bit words; the instruction word is opcode u8 | a u8 | b u8 | c u8 (low byte
// first), immediates follow, f32x3 values occupy three consecutive registers, a spectrum four.
// Ref: docs/decisions/0005-material-model.md items 30, 31, 32, 35; Cycles kernel/svm/svm.h
// (svm_eval_nodes: the loop-and-switch structure, Apache-2.0).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../material/openpbr_params.h"
#include "../portability.h"
#include "context.h"
#include "ops.h"
#include "vm_regs.h"

#include "ops/common.h"
#include "ops/add_f32.h"
#include "ops/add_f32x3.h"
#include "ops/blackbody.h"
#include "ops/branch_mix.h"
#include "ops/cauchy_abbe.h"
#include "ops/channel_f32x3.h"
#include "ops/color_to_f32.h"
#include "ops/const_f32.h"
#include "ops/const_f32x3.h"
#include "ops/emit_le.h"
#include "ops/end.h"
#include "ops/jmp.h"
#include "ops/load_ctx_p.h"
#include "ops/load_ctx_uv.h"
#include "ops/mul_f32.h"
#include "ops/mul_f32x3.h"
#include "ops/mul_f32x3_f32.h"
#include "ops/rgb_lift.h"
#include "ops/spectrum_table.h"
#include "ops/store_slot.h"
#include "ops/sub_f32.h"
#include "ops/test_inject_nan.h"
#include "ops/tex_image.h"
#include "ops/unsupported.h"
#include "ops/volume_coeffs.h"

AR_NS_BEGIN

// The one loop: runs from vm.pc until END or an error. A surface run and a volume run differ only
// in which of vm.rec, vm.le and vm.med, vm.mblock they supply.
AR_FN VmStatus vm_run(VmState& vm) {
  while (!vm.done && vm.status == VmStatus::OK) {
    const uint32_t word = vm.prog[vm.pc];
    vm.a = (word >> 8) & 0xffu;
    vm.b = (word >> 16) & 0xffu;
    vm.c = word >> 24;
    switch (static_cast<Op>(word & 0xffu)) {
#define AR_OP(NAME, ...) AR_OP_CASE(NAME, handle_##NAME)
#include "ops.def"
#undef AR_OP
      default: vm_fail(vm, VmStatus::BAD_OPCODE); break;
    }
  }
  return vm.status;
}

// Runs the entry at word `offset` of `prog` until END. Offset 0 is the entry table itself, which
// item 32 uses for "absent or constant": nothing runs and the status is OK. `regs` is the
// kernel's one register file; the interpreter never declares its own, and a program writes every
// register it reads. `rec` and `le` may be null for an entry that has no use for them; an op that
// then needs one stops the run with NULL_POINTER. Any error stops the run at the failing
// instruction and is returned; the caller counts it (kernels never write report entries).
// `select_u` is the surface entry's selection uniform (common.h VmState), read and rewritten by
// BRANCH_MIX; null when the caller has none.
AR_FN VmStatus run_entry(const uint32_t* prog, uint32_t offset, VmRegs* regs, const ShadeContext& ctx,
                         const SceneTables& tables, OpenPBRParams* rec, Spec* le, float* select_u = nullptr) {
  VmState vm;
  vm.prog = prog;
  vm.pc = offset;
  vm.r = regs->r;
  vm.ctx = &ctx;
  vm.tables = &tables;
  vm.rec = rec;
  vm.le = le;
  vm.med = nullptr;
  vm.mblock = nullptr;
  vm.select_u = select_u;
  vm.a = vm.b = vm.c = 0;
  vm.status = VmStatus::OK;
  vm.done = (offset == 0u);
  return vm_run(vm);
}

// The volume entry (0006 A.2): the same interpreter, with the medium's constant block in and its
// MediumSample out instead of the OpenPBR record. ctx supplies P and the wavelengths.
AR_FN VmStatus run_volume_entry(const uint32_t* prog, uint32_t offset, VmRegs* regs, const ShadeContext& ctx,
                                const SceneTables& tables, const MediumBlock* block, MediumSample* out) {
  VmState vm;
  vm.prog = prog;
  vm.pc = offset;
  vm.r = regs->r;
  vm.ctx = &ctx;
  vm.tables = &tables;
  vm.rec = nullptr;
  vm.le = nullptr;
  vm.med = out;
  vm.mblock = block;
  vm.select_u = nullptr;  // a volume program has no Mix selection
  vm.a = vm.b = vm.c = 0;
  vm.status = VmStatus::OK;
  vm.done = (offset == 0u);
  return vm_run(vm);
}

AR_NS_END
