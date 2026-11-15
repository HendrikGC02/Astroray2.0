// SPDX-License-Identifier: Apache-2.0
// END: ends the current entry. Ref: docs/decisions/0005-material-model.md item 31.
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN void vm_op_end(VmState& vm) { vm.done = true; }
AR_NS_END
#define handle_END() ::ar::vm_op_end(vm)
