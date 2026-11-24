// SPDX-License-Identifier: Apache-2.0
// SPECTRUM_TABLE: a graph op, never program code. Its parameters (scale, then wavelength and value
// pairs) are the f32 pool of the graph; the compiler folds the node into a TABLE SpectrumRef that
// indexes the scene spectrum arena, so a program holds no instruction for it and executing one is
// a compiler bug that stops the run with BAD_OPERAND. The lookup is table_spectrum (slot_resolve.h).
// Ref: docs/decisions/0005-material-model.md items 9, 31.
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN void vm_op_spectrum_table(VmState& vm) { vm_fail(vm, VmStatus::BAD_OPERAND); }
AR_NS_END
#define handle_SPECTRUM_TABLE() ::ar::vm_op_spectrum_table(vm)
