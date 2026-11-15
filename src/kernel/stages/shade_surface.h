// SPDX-License-Identifier: Apache-2.0
// shade_surface (ShadeKind::Common): a camera path's surface hit. In stage order, as Cycles'
// integrate_surface: build the shading context from the hit, run the material's surface entry
// into the OpenPBR record (a CONSTANT_SURFACE material only resolves its slots), add the emission
// the path sees, write the primary-hit AOVs, then continue: the total bounce limit, a sample of
// the lobe stack, Russian roulette and the next ray. NEE and emission MIS arrive with 074: until
// then emission found by any ray has weight 1.
// Continuation, as pbrt-v4 VolPathIntegrator::Li: a path that arrives with bounces[total] equal
// to the limit ends MAX_BOUNCE (pbrt's depth++ == maxDepth). Otherwise the stack is sampled in
// the frame of N with the event block's lobe choice and direction u, v (0003 d23); a reflected
// direction below the geometric surface, or no sample, ends ZERO_THROUGHPUT (0005 item 8). Then
// beta *= f |cos theta_i| / pdf (f excludes the cosine, 0004 D1) and r_l = r_u / pdf; r_u does
// not change, because a surface decision has the same pdf in every lane (0003 d6), which is
// pbrt's update for a non-hero event. A delta sample sets LAST_BOUNCE_SPECULAR for 074's emitter
// MIS. Russian roulette then uses the event block's uniform. A survivor advances event_index,
// bounces[total] and the diffuse or glossy counter, stores the vertex for 074 (prev_ng, prev_ns,
// prev_dist = 0 at the vertex itself) and leaves along the sample from the offset origin
// (0002 D16) with the lobe's ray type.
// Emission Le is the constant block's emission (a lone constant Emission folds there) plus the
// EMIT_LE sum of the surface entry's emission segment (every other emission); the compiler puts a
// material's emission in exactly one of the two, so nothing is counted twice. Le is two-sided at
// a hit: emission sampling only decides which side NEE samples (Cycles emissive_simple_eval
// returns 1 for the facing normal). Le carries no radiance scale, so it is multiplied by s here,
// once (068 folded s into the light records only).
// A VM error or a non-finite contribution marks the sample invalid and ends the path INVALID,
// so the resolve counts it (0004 D17, 0008 d6, 0003 d22). Film writes use the sample's slot in the
// per-sample buffer, its sample_id (0004 D5).
// Ref: pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::Li (Apache-2.0): the BSDF-sample
// update of beta, r_u and r_l, specularBounce, and the order limit / sample / roulette;
// docs/decisions/0004-integrator.md D1, D2, D3, D14; 0003 d23 (per-event block); 0002 D16.
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
#include "../geom/ray_offset.h"
#include "../kernel_globals.h"
#include "../material/constant_block.h"
#include "../material/lobes/stack.h"
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

// ray_type of a continuation ray: the instance-mask bits AR_VISIBILITY_DIFFUSE and
// AR_VISIBILITY_GLOSSY of include/astroray/astroray.h, which a kernel header cannot include (a
// test pins them, as kRayTypeCamera).
inline constexpr uint8_t kRayTypeDiffuse = 0x2;
inline constexpr uint8_t kRayTypeGlossy = 0x4;

AR_FN void stage_shade_surface(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  AR_ASSERT((ps_flags(kg, slot) & PATH_KIND) == 0u);
  const uint32_t sample = ps_sample_id(kg, slot);
  const SceneData& s = scene_of(kg);

  ShadeContext ctx;
  if (!shade_context_build(kg, slot, &ctx)) {  // a degenerate triangle has no normal: fail loudly
    sample_mark_invalid(kg, sample);
    path_end(kg, slot, Term::INVALID);
    return;
  }
  uint32_t flags = ps_flags(kg, slot);
  flags = ctx.front_face != 0u ? (flags | FRONT_FACE) : (flags & ~FRONT_FACE);

  // hit_material_id gives the default material (material_header_count) for an instance without
  // materials; the host uploads it there, so every id indexes a header. A session's scene always
  // has material tables: their absence is a host bug and fails the sample.
  const uint32_t mat = hit_material_id(kg, ps_hit_inst(kg, slot), ps_hit_prim(kg, slot));
  bool ok = s.material_headers != nullptr && s.material_constants != nullptr && s.material_pass_index != nullptr;
  Spec le = spec_fill(Real(0));
  OpenPBRParams rec;
  if (ok) {
    AR_ASSERT(mat <= s.material_header_count);
    const MaterialHeader* hp = s.material_headers + mat;
    const uint32_t mflags = load_ro(&hp->flags), program = load_ro(&hp->program_offset);
    const ConstantBlock& block = s.material_constants[load_ro(&hp->const_offset)];
    const ObserverView& obs = kg.tables->observer;
    resolve_constant_record(block, ctx, obs, &rec);
    le = resolve(block.emission_color, ctx.lambda_t, obs) * Real(block.emission_luminance);
    if ((mflags & CONSTANT_SURFACE) == 0u) {
      VmRegs regs;
      const uint32_t* prog = s.material_programs + program;
      ok = run_entry(prog, load_ro(prog), &regs, ctx, *kg.tables, &rec, &le) == VmStatus::OK;
    }
    if (ctx.depth[0] == 0u) {
      const uint32_t index = kg.session.samples_done + sample % kg.session.samples_per_pass;
      film_primary_aov(kg, sample, index, &flags, ctx.P, ctx.N, ctx.uv[0].x, ctx.uv[0].y,
                       load_ro(s.material_pass_index + mat));
    }
  }
  Spec beta = ps_beta(kg, slot);
  const Spec contribution = le * radiance_scale(kg) * beta;
  if (!ok || !is_finite(contribution)) {
    sample_mark_invalid(kg, sample);
    ps_flags(kg, slot) = flags;
    path_end(kg, slot, Term::INVALID);
    return;
  }
  sample_add(kg, sample, contribution);
  ps_flags(kg, slot) = flags;

  const uint32_t total = ctx.depth[0];
  if (total >= kg.session.max_bounces[0]) {
    path_end(kg, slot, Term::MAX_BOUNCE);
    return;
  }
  const uint32_t e = ps_event_index(kg, slot);
  const Real uc = camera_path_draw(kg, sample, event_dim(e, EV_LOBE_CHOICE));
  const Real2 u2{camera_path_draw(kg, sample, event_dim(e, EV_BSDF_U)), camera_path_draw(kg, sample, event_dim(e, EV_BSDF_V))};
  const LobeSample bs = stack_sample(*kg.e_diel, rec, frame_from_normal(ctx.N), ctx.wo, uc, u2, TransportMode::Radiance);
  // A reflection below the geometric surface would leave through the wrong side (0005 item 8).
  if (!(bs.pdf > Real(0)) || ((bs.flags & LOBE_REFLECTION) != 0u && !(dot(bs.wi, ctx.Ng) > Real(0)))) {
    path_end(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  beta *= bs.f * (abs_dot(bs.wi, ctx.N) / bs.pdf);
  if (!is_finite(beta)) {
    sample_mark_invalid(kg, sample);
    path_end(kg, slot, Term::INVALID);
    return;
  }
  if (!(max_lane(beta) > Real(0))) {  // pbrt: if (!beta) break, before the roulette
    path_end(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  const Spec r_u = ps_r_u(kg, slot);
  const Real q = roulette_q(beta, ps_eta_scale(kg, slot), r_u, total + 1u, kg.session.min_bounces);
  if (q > Real(0)) {
    if (camera_path_draw(kg, sample, event_dim(e, EV_ROULETTE)) < q) {
      path_end(kg, slot, Term::RUSSIAN_ROULETTE);
      return;
    }
    beta = beta / (Real(1) - q);  // q < 1 here: beta has a positive lane
  }

  ps_beta(kg, slot) = beta;
  ps_r_l(kg, slot) = r_u / bs.pdf;
  const bool delta = (bs.flags & LOBE_DELTA) != 0u;
  ps_flags(kg, slot) = delta ? (flags | LAST_BOUNCE_SPECULAR) : (flags & ~LAST_BOUNCE_SPECULAR);
  const bool glossy = (bs.flags & LOBE_GLOSSY) != 0u;
  U16x6 b = ps_bounces(kg, slot);
  b.v[0] = uint16_t(b.v[0] + 1u);
  b.v[glossy ? 2 : 1] = uint16_t(b.v[glossy ? 2 : 1] + 1u);
  ps_bounces(kg, slot) = b;
  ps_event_index(kg, slot) = uint16_t(e + 1u);
  ps_prev_ng(kg, slot) = oct_encode(ctx.Ng);
  ps_prev_ns(kg, slot) = oct_encode(ctx.N);
  ps_prev_dist(kg, slot) = Real(0);
  ps_ray_o(kg, slot) = offset_ray_origin(ctx.P, ctx.Ng, bs.wi);
  ps_ray_d(kg, slot) = bs.wi;
  ps_ray_type(kg, slot) = glossy ? kRayTypeGlossy : kRayTypeDiffuse;
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::INTERSECT_CLOSEST);
}
AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_surface.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_surface(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
