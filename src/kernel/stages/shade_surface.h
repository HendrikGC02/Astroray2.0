// SPDX-License-Identifier: Apache-2.0
// shade_surface (ShadeKind::Common): a camera path's surface hit. In stage order, as Cycles'
// integrate_surface: build the shading context from the hit, run the material's surface entry
// into the OpenPBR record (a CONSTANT_SURFACE material only resolves its slots), add the emission
// the path sees, write the primary-hit AOVs, end the path. Continuation (BSDF sampling, Russian
// roulette) arrives with 073, NEE and emission MIS with 074: until then every path ends here
// with MAX_BOUNCE, and emission at the first vertex has weight 1.
// Emission Le is the constant block's emission (a lone constant Emission folds there) plus the
// EMIT_LE sum of the surface entry's emission segment (every other emission); the compiler puts a
// material's emission in exactly one of the two, so nothing is counted twice. Le is two-sided at
// a hit: emission sampling only decides which side NEE samples (Cycles emissive_simple_eval
// returns 1 for the facing normal). Le carries no radiance scale, so it is multiplied by s here,
// once (068 folded s into the light records only).
// A VM error or a non-finite contribution marks the sample invalid and ends the path INVALID,
// so the resolve counts it (0004 D17, 0008 d6, 0003 d22). Film writes use the sample's slot in the
// per-sample buffer, its sample_id (0004 D5).
// Ref: Cycles kernel/integrator/shade_surface.h integrate_surface (Apache-2.0) for the stage
// structure; Cycles kernel/closure/emissive.h (two-sided emission at a hit);
// docs/decisions/0002-kernel-architecture.md D8, D9; 0005 items 3 (one VmRegs per kernel), 32
// (surface entry, constant entries), 35, 38 (emission segment); 0004 D5, D28; 0003 d10 (s),
// d21 (FRONT_FACE, AOV_WRITTEN), d22; 0008 d10 (primary-hit AOVs).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../film/film_write.h"
#include "../kernel_globals.h"
#include "../material/constant_block.h"
#include "../material/material_header.h"
#include "../material/openpbr_params.h"
#include "../material/slot_resolve.h"
#include "../portability.h"
#include "../scene/scene_view.h"
#include "../sched/step_table.h"
#include "../state/flags.h"
#include "../vm/context.h"
#include "../vm/interp.h"
#include "../vm/vm_regs.h"
#include "shade_common.h"

AR_NS_BEGIN

AR_FN void stage_shade_surface(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  AR_ASSERT((ps_flags(kg, slot) & PATH_KIND) == 0u);
  const uint32_t sample = ps_sample_id(kg, slot);
  const SceneData& s = scene_of(kg);

  ShadeContext ctx;
  shade_context_build(kg, slot, &ctx);
  uint32_t flags = ps_flags(kg, slot);
  flags = ctx.front_face != 0u ? (flags | FRONT_FACE) : (flags & ~FRONT_FACE);

  // hit_material_id gives the default material (material_header_count) for an instance without
  // materials; the host uploads it there, so every id indexes a header. A session's scene always
  // has material tables: their absence is a host bug and fails the sample.
  Term term = Term::MAX_BOUNCE;  // until 073 continues the path
  const uint32_t mat = hit_material_id(kg, ps_hit_inst(kg, slot), ps_hit_prim(kg, slot));
  bool ok = s.material_headers != nullptr && s.material_constants != nullptr && s.material_pass_index != nullptr;
  Spec le = spec_fill(Real(0));
  if (ok) {
    AR_ASSERT(mat <= s.material_header_count);
    const MaterialHeader* hp = s.material_headers + mat;
    const uint32_t mflags = load_ro(&hp->flags), program = load_ro(&hp->program_offset);
    const ConstantBlock& block = s.material_constants[load_ro(&hp->const_offset)];
    const ObserverView& obs = kg.tables->observer;
    OpenPBRParams rec;
    resolve_constant_record(block, ctx, obs, &rec);
    le = resolve(block.emission_color, ctx.lambda_t, obs) * Real(block.emission_luminance);
    if ((mflags & CONSTANT_SURFACE) == 0u) {
      VmRegs regs;
      const uint32_t* prog = s.material_programs + program;
      ok = run_entry(prog, load_ro(prog), &regs, ctx, *kg.tables, &rec, &le) == VmStatus::OK;
    }
    if (ps_bounces(kg, slot).v[0] == 0u) {
      const uint32_t index = kg.session.samples_done + sample % kg.session.samples_per_pass;
      film_primary_aov(kg, sample, index, &flags, ctx.P, ctx.N, ctx.uv[0].x, ctx.uv[0].y,
                       load_ro(s.material_pass_index + mat));
    }
  }
  const Spec contribution = le * radiance_scale(kg) * ps_beta(kg, slot);
  if (ok && is_finite(contribution)) {
    sample_add(kg, sample, contribution);
  } else {
    sample_mark_invalid(kg, sample);
    term = Term::INVALID;
  }
  ps_flags(kg, slot) = flags;
  path_end(kg, slot, term);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_surface.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_surface(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
