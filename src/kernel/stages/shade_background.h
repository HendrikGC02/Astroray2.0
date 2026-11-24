// SPDX-License-Identifier: Apache-2.0
// shade_background: a camera path that left the scene sees the constant world. It adds
// Le(lambda_t) * beta, MIS-weighted against NEE of the world (integrator/mis.h, 0004 D1), to the
// sample's slot and ends the path ESCAPED. After a non-delta bounce the weight uses
// pmf(ctx_prev, world) pdf_li(ctx_prev, wi) from the previous vertex's context; at the first
// vertex or after a delta bounce the contribution is divided by avg(r_u) only. A non-finite
// contribution adds nothing, marks the sample invalid (the resolve counts it and leaves n alone,
// 0008 d6) and ends the path INVALID (rule 5). Light-path misses never get here (route_hit,
// 0002 D8). The NEE test modes (kernel_globals.h TEST_HOOK_NEE_*) weight the world 1 (off) or
// drop it after a non-delta bounce (light_only).
// Radiance already carries the radiance scale s in the light record (068); the film divides it
// out, so it is not applied here.
// A world record with kLightWorldProgram has no constant radiance: Le is the world program's emission
// entry run on this ray, times the record's strength (which carries s and nothing else). The context
// is the one Cycles builds for a background shade: P is the ray direction and N, Ng and wo are its
// negative, so an Environment Texture (TEX_ENVIRONMENT over LOAD_CTX_P) reads the direction the ray
// escaped along. The record's importance is 0, so p_light above is 0 and the weight is 1 until the
// world is sampled (0005 item 32). A program that cannot run (a missing table or a VM error) makes
// the sample invalid, like a non-finite Le.
// Ref: docs/decisions/0004-integrator.md D5, D6 (world row), D28; 0002 D8 (shade_background);
// docs/decisions/0005-material-model.md item 32;
// Cycles kernel/integrator/shade_background.h (Apache-2.0) for the stage structure and
// kernel/geom/shader_data.h shader_setup_from_background for the context.
// Ref: pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::Li (escaped rays: infinite-light
// MIS) (Apache-2.0).
#pragma once
#include <cstdint>

#include "../integrator/mis.h"
#include "../kernel_globals.h"
#include "../light/light_sampler.h"
#include "../material/constant_block.h"
#include "../material/slot_resolve.h"
#include "../portability.h"
#include "../scene/scene_view.h"
#include "../sched/step_table.h"
#include "../vm/context.h"
#include "../vm/interp.h"
#include "../vm/vm_regs.h"
#include "../state/flags.h"
#include "shade_common.h"

AR_NS_BEGIN

// The world's record, or nullptr when the scene has none (the host light table records its
// index, kg_builder copies it).
AR_FN const LightRecord* world_light(const KernelGlobals& kg) {
  if (kg.world_light == kNoLight) return nullptr;
  AR_ASSERT(kg.lights != nullptr && kg.light_sampler != nullptr && kg.world_light < kg.light_sampler->light_count);
  return &kg.lights[kg.world_light];
}

// The world's radiance along the escaped ray of pool slot `slot` at the lanes `lam`. False when a
// program world cannot be evaluated. A constant world reads nothing of the path but its lanes.
AR_FN bool world_radiance(const KernelGlobals& kg, const LightRecord& world, uint32_t slot, const Lambda& lam, Spec* le) {
  const ObserverView& obs = kg.tables->observer;
  if ((world.flags & kLightWorldProgram) == 0u) {
    *le = light_emission(world, lam, obs);
    return true;
  }
  // The entry table is four words; a shorter program is a host bug, not a world.
  if (kg.scene == nullptr || kg.scene->world_program == nullptr || kg.scene->world_constants == nullptr ||
      kg.scene->world_program_words < 4u)
    return false;
  const SceneData& s = *kg.scene;
  const Real3 dir = normalize(ps_ray_d(kg, slot));
  ShadeContext ctx{};
  ctx.P = dir;
  ctx.N = ctx.Ng = ctx.wo = -dir;
  ctx.ray_type = ps_ray_type(kg, slot);
  const U16x6 b = ps_bounces(kg, slot);
  for (int i = 0; i < 6; ++i) ctx.depth[i] = b.v[i];
  ctx.lambda_t = lam;
  // As a material's emission entry: the block's own emission first (0 for a program world, whose
  // emissions are all code), then the entry adds its terms.
  const ConstantBlock& block = s.world_constants[0];
  Spec e = resolve_slot<SpectrumSemantic::ILLUMINANT>(block.emission_color, lam, obs, s.material_spectra) *
           Real(block.emission_luminance);
  VmRegs regs;
  const uint32_t* prog = s.world_program;
  if (run_entry(prog, load_ro(prog + 2), &regs, ctx, *kg.tables, nullptr, &e) != VmStatus::OK) return false;
  *le = e * Real(world.strength);
  return true;
}

AR_FN void stage_shade_background(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  AR_ASSERT((ps_flags(kg, slot) & PATH_KIND) == 0u);
  Term term = Term::ESCAPED;
  const LightRecord* world = world_light(kg);
  const uint32_t flags = ps_flags(kg, slot);
  const uint32_t hooks = kg.session.test_hooks;
  const bool after_scatter = ps_bounces(kg, slot).v[0] > 0u && (flags & LAST_BOUNCE_SPECULAR) == 0u;
  if (world != nullptr && !(after_scatter && (hooks & TEST_HOOK_NEE_LIGHT_ONLY) != 0u)) {
    const Spec lt = ps_lambda_t(kg, slot);
    Lambda lam;
    for (int i = 0; i < kLanes; ++i) lam.nm[i] = lt.v[i];
    // The film slot is the sample's, not the pool slot (0004 D5).
    const uint32_t sample = ps_sample_id(kg, slot);
    const bool weighted = after_scatter && (hooks & TEST_HOOK_NEE_OFF) == 0u;
    Real p_light = Real(0);
    if (weighted) {
      const LightSampleContext prev = prev_light_context(kg, slot);
      const Real3 wi = normalize(ps_ray_d(kg, slot));
      p_light = light_sampler_pmf(*kg.light_sampler, prev, kg.world_light) *
                light_pdf_li(*world, prev, wi, LightHit{prev.p, prev.n_geom});
    }
    Spec le = spec_fill(Real(0));
    const bool ran = world_radiance(kg, *world, slot, lam, &le);
    const Spec contribution = le * ps_beta(kg, slot) *
                              mis_emitter_scale(ps_r_u(kg, slot), weighted ? ps_r_l(kg, slot) : Spec{}, p_light, weighted);
    if (ran && is_finite(contribution))
      sample_add(kg, sample, contribution);
    else {
      sample_mark_invalid(kg, sample);
      term = Term::INVALID;
    }
  }
  path_end(kg, slot, term);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_background.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_background(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first,
                                uint32_t last);
#endif
