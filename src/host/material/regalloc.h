// SPDX-License-Identifier: Apache-2.0
// Linear-scan register allocation over the VM's allocatable f32 registers. A value is a live interval
// [start, end] in instruction positions with a width (1 f32, 3 f32x3, 4 spec, or a group); wider
// values need consecutive registers. There is no spilling: a program that does not fit fails.
// Ref: Poletto and Sarkar 1999, "Linear Scan Register Allocation", ACM TOPLAS 21(5), section 3.
#pragma once

#include <string>
#include <vector>

namespace astroray::host {

struct LiveRange {
  int start;  // position of the defining instruction
  int end;    // position of the last reader (>= start)
  int width;  // consecutive registers
};

// r0-r30 of the 32-register file: r31 is the selection register the surface kernels load
// (vm_regs.h kSelectReg; compiler.cpp asserts the two agree).
inline constexpr int kRegisterCount = 31;

// reg[i] is the first register of ranges[i]. A register is reused only when the earlier range
// ends strictly before the later one starts, so an op never writes a register it still reads.
// On failure *err is "needs <n> registers, 31 available".
bool allocate_registers(const std::vector<LiveRange>& ranges, std::vector<int>* reg, std::string* err);

}  // namespace astroray::host
