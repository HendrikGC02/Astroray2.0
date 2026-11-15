// SPDX-License-Identifier: Apache-2.0
// Pieces every shade stage shares. path_end is the one way a camera path leaves the pool: the
// path-end statistics (bounce count and termination reason), the sample's final flags, and the
// slot returned to the refill through next_stage TERMINATED.
// Ref: docs/decisions/0004-integrator.md D5 (flags at path end), D28 (path-end statistics);
// 0003 d22 (Term); Cycles kernel/integrator/shade_background.h and state_flow.h
// integrator_path_terminate (Apache-2.0) for the structure.
#pragma once
#include <cstdint>

#include "../film/film_write.h"
#include "../kernel_globals.h"
#include "../portability.h"
#include "../state/stage.h"
#include "../state/termination.h"

AR_NS_BEGIN

AR_FN void path_end(const KernelGlobals& kg, uint32_t slot, Term term) {
  ps_term(kg, slot) = static_cast<uint8_t>(term);
  film_path_end_stats(kg, slot, uint32_t(ps_bounces(kg, slot).v[0]), term);
  sample_set_flags(kg, slot, ps_flags(kg, slot));
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::TERMINATED);
}

AR_NS_END
