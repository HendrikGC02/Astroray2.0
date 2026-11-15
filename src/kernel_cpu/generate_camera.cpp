// SPDX-License-Identifier: Apache-2.0
// generate_camera on the host thread pool. The scheduler's refill launch (101) dispatches it, not
// the CpuDevice stage table: it is a refill, not a Stage (0002 D8).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D7 step 4; stages/generate_camera.h.
#include "../kernel/stages/generate_camera.h"
#include "stage_cpu.h"

AR_CPU_STAGE(generate_camera)
