// SPDX-License-Identifier: Apache-2.0
// shade_surface_ext on the host thread pool (chunks of 1,024 items, stage_cpu.h).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D8; stages/shade_surface_ext.h.
#include "../kernel/stages/shade_surface_ext.h"
#include "stage_cpu.h"

AR_CPU_STAGE(shade_surface_ext)
