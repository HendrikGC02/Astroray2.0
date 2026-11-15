// SPDX-License-Identifier: Apache-2.0
// UNSUPPORTED imm: marks a node the compiler could not lower. The compiler replaces every
// program that contains one (magenta plus a report entry), so reaching it at run time is a bug
// and stops the run with a status instead of continuing.
// Ref: docs/decisions/0005-material-model.md items 29, 31, 40.
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN void vm_op_unsupported(VmState& vm) { vm_fail(vm, VmStatus::UNSUPPORTED); }
AR_NS_END
#define handle_UNSUPPORTED() ::ar::vm_op_unsupported(vm)
