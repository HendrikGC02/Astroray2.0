// SPDX-License-Identifier: Apache-2.0
// shade_surface (ShadeKind::Common): a camera path's surface hit. In stage order, as Cycles'
// integrate_surface: build the shading context from the hit, run the material's surface entry
// into the OpenPBR record (a CONSTANT_SURFACE material only resolves its slots), add the emission
// the path sees with its MIS weight, write the primary-hit AOVs, then the total bounce limit, one
// light sample (NEE) emitted as a shadow item, a sample of the lobe stack, Russian roulette and
// the next ray.
// Emission MIS (0004 D1, integrator/mis.h): emission reached by a BSDF ray after a non-delta
// bounce is weighted against NEE with pmf(ctx_prev, light) pdf_li(ctx_prev, wi), the previous
// vertex's context rebuilt from prev_ng, prev_ns and prev_dist; at the first vertex or after a
// delta bounce (LAST_BOUNCE_SPECULAR) it is divided by avg(r_u) only. The light of a hit triangle
// is light_of_hit's; a triangle that is no light (or has pmf 0, 0004 D9) gets p_light 0.
// NEE, as pbrt-v4 VolPathIntegrator::SampleLd: the context point is the hit offset toward wo
// (pbrt offsets it for a reflection-only BSDF); the light is picked by the alias sampler from the
// event block's pick dimension (32 bits: the bin) with the alias test's uniform from per-path
// dimension 7 mixed with the event index (RULINGS.md 73), and sampled with the block's light u, v
// (0003 d23). f and pdf come from the lobe stack toward wi, and the
// contribution beta f |cos| Le times mis_nee_scale goes into a shadow item in the path's reserved
// shadow slot (0002 D5, k = 1), with the segment's ends offset (0002 D16: the far end on an
// emissive triangle, none on a lamp, which never blocks shadows, and an unbounded segment toward
// an infinite light). A zero contribution emits nothing and leaves the slot free.
// A triangle light whose emission is a material program (kLightEmissionProgram) returns Le 0 with
// deferred_le from sample_li: its Le is the emitting material evaluated at the sampled point, seen
// from the shading point, exactly as this stage evaluates emission at a hit (constant block plus
// the program's emission entry), times s. To keep one interpreter call site in the TU (B-05:
// the inlined VM is most of its compile time), the material evaluation sits in a two-phase loop:
// phase 0 runs the hit material's surface entry, phase 1, only for a deferred light, the light
// material's emission entry with the context rebuilt at the light point.
// Continuation, as pbrt-v4 VolPathIntegrator::Li: a path that arrives with bounces[total] equal
// to the limit ends MAX_BOUNCE (pbrt's depth++ == maxDepth). Otherwise the stack is sampled in
// the frame of N with the event block's lobe choice and direction u, v (0003 d23); a reflected
// direction below the geometric surface, or no sample, ends ZERO_THROUGHPUT (0005 item 8). Then
// beta *= f |cos theta_i| / pdf (f excludes the cosine, 0004 D1) and r_l = r_u / pdf; r_u does
// not change, because a surface decision has the same pdf in every lane (0003 d6), which is
// pbrt's update for a non-hero event. A delta sample sets LAST_BOUNCE_SPECULAR for the emitter
// MIS. Russian roulette then uses the event block's uniform. A survivor advances event_index,
// bounces[total] and the diffuse or glossy counter, stores the vertex for the emitter MIS (prev_ng,
// prev_ns, prev_dist = 0 at the vertex itself) and leaves along the sample from the offset origin
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
// Ref: pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::Li (Apache-2.0): emitter-hit MIS,
// the BSDF-sample update of beta, r_u and r_l, specularBounce, the order limit / NEE / sample /
// roulette; VolPathIntegrator::SampleLd (context offset, light pick, f, pdfs, weight);
// Veach and Guibas 1995; docs/decisions/0004-integrator.md D1, D2, D3, D4, D5, D14; 0003 d20,
// d23 (per-event block); 0002 D5, D16.
// Ref: Cycles kernel/integrator/shade_surface.h integrate_surface (Apache-2.0) for the stage
// structure; Cycles kernel/closure/emissive.h (two-sided emission at a hit);
// docs/decisions/0002-kernel-architecture.md D8, D9; 0005 items 3 (one VmRegs per kernel), 32
// (surface and emission entries, constant entries), 35, 38 (emission segment); 0004 D28; 0003 d10
// (s), d21 (FRONT_FACE, AOV_WRITTEN), d22; 0008 d10 (primary-hit AOVs).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../film/film_write.h"
#include "../geom/ray_offset.h"
#include "../integrator/mis.h"
#include "../kernel_globals.h"
#include "../light/light.h"
#include "../light/light_sampler.h"
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

// Barycentrics (weights of corners 1 and 2) of a point p in the plane of triangle (a, b, c).
// Ref: Ericson 2005, "Real-Time Collision Detection", section 3.4 (Barycentric(), the
// dot-product form).
AR_FN Real2 triangle_barycentrics(Real3 a, Real3 b, Real3 c, Real3 p) {
  const Real3 v0 = b - a, v1 = c - a, v2 = p - a;
  const Real d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
  const Real inv = Real(1) / (d00 * d11 - d01 * d01);
  return Real2{(d11 * d20 - d01 * d21) * inv, (d00 * d21 - d01 * d20) * inv};
}


// The far end of an unbounded shadow segment toward an infinite light.
inline constexpr float kShadowUnbounded = 0x1.fffffep+127f;

AR_FN void stage_shade_surface(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  AR_ASSERT((ps_flags(kg, slot) & PATH_KIND) == 0u);
  const uint32_t sample = ps_sample_id(kg, slot);
  const SceneData& s = scene_of(kg);
  const ObserverView& obs = kg.tables->observer;
  uint32_t flags = ps_flags(kg, slot);
  const bool arrived_specular = (flags & LAST_BOUNCE_SPECULAR) != 0u;
  const uint32_t inst = ps_hit_inst(kg, slot), prim = ps_hit_prim(kg, slot);
  // A session's scene always has material tables: their absence is a host bug and fails the sample.
  bool ok = s.material_headers != nullptr && s.material_constants != nullptr && s.material_pass_index != nullptr;

  ShadeContext ctx;
  ctx.ray_type = ps_ray_type(kg, slot);
  {
    const U16x6 b = ps_bounces(kg, slot);
    for (int i = 0; i < 6; ++i) ctx.depth[i] = b.v[i];
    const Spec lt = ps_lambda_t(kg, slot);
    for (int i = 0; i < kLanes; ++i) ctx.lambda_t.nm[i] = lt.v[i];
  }
  const uint32_t total = ctx.depth[0];
  OpenPBRParams rec;
  VmRegs regs;
  Spec le = spec_fill(Real(0));
  // The hit's geometry, kept for the continuation while ctx holds a light point (phase 1).
  Real3 P{}, Ng{}, N{}, wo{};
  // The shadow item is written into the reserved slot as soon as its segment is known, so nothing
  // of it stays live across the light point's VM run. 0: no item; 1: an item with its whole
  // contribution; 2: an item whose contribution still lacks the deferred Le (phase 1).
  uint32_t nee_state = 0;
  uint32_t sh = 0;  // the reserved shadow slot, read when an item is written
  // Where ctx is built: the path's hit in phase 0, the light sample's point in phase 1.
  uint32_t g_inst = inst, g_prim = prim;
  Real2 g_bary = ps_hit_uv(kg, slot);
  Real3 g_dir = ps_ray_d(kg, slot);
  uint32_t mat = 0;
  for (uint32_t phase = 0;; ++phase) {
    // One site builds the context and evaluates the material, for both phases: phase 0 the hit
    // material's surface entry (record and emission), phase 1 the light material's emission entry
    // only. Entry 0 runs nothing (0005 item 32: absent or constant). hit_material_id gives the
    // default material (material_header_count) for an instance without materials; the host
    // uploads it there, so every id indexes a header.
    if (!shade_context_geometry(kg, g_inst, g_prim, g_bary, g_dir, &ctx)) {
      sample_mark_invalid(kg, sample);  // a degenerate triangle has no normal: fail loudly
      path_end(kg, slot, Term::INVALID);
      return;
    }
    mat = hit_material_id(kg, g_inst, g_prim);
    if (ok) {
      AR_ASSERT(mat <= s.material_header_count);
      const MaterialHeader* hp = s.material_headers + mat;
      const uint32_t mflags = load_ro(&hp->flags);
      const ConstantBlock& block = s.material_constants[load_ro(&hp->const_offset)];
      if (phase == 0u) resolve_constant_record(block, ctx, obs, &rec);
      le = resolve(block.emission_color, ctx.lambda_t, obs) * Real(block.emission_luminance);
      const uint32_t* prog = s.material_programs + load_ro(&hp->program_offset);
      const bool program = phase == 0u ? (mflags & CONSTANT_SURFACE) == 0u : (mflags & CONSTANT_EMISSION) == 0u;
      const uint32_t entry = program ? load_ro(prog + (phase == 0u ? 0u : 2u)) : 0u;
      ok = run_entry(prog, entry, &regs, ctx, *kg.tables, phase == 0u ? &rec : nullptr, &le) == VmStatus::OK;
    }
    if (phase != 0u) break;

    P = ctx.P, Ng = ctx.Ng, N = ctx.N, wo = ctx.wo;
    flags = ctx.front_face != 0u ? (flags | FRONT_FACE) : (flags & ~FRONT_FACE);
    // Written even when the VM failed (the AOVs name the hit), whenever the material tables exist.
    if (s.material_pass_index != nullptr && s.material_headers != nullptr && total == 0u) {
      const uint32_t index = kg.session.samples_done + sample % kg.session.samples_per_pass;
      film_primary_aov(kg, sample, index, &flags, P, N, ctx.uv[0].x, ctx.uv[0].y, load_ro(s.material_pass_index + mat));
    }
    Spec contribution = spec_fill(Real(0));
    if (ok && max_lane(le) > Real(0)) {
      const bool weighted = total > 0u && !arrived_specular;
      Real p_light = Real(0);
      if (weighted) {
        const uint32_t light = light_of_hit(kg, inst, prim);
        ok = light != kLightIndexInvalid;  // a hit outside the scene's tables: fail the sample
        if (ok && light != kNoLight) {
          // light_of_hit names emissive triangles only (lamps cross in shade_light), so the
          // triangle's own pdf_li stands for the dispatch.
          AR_ASSERT(LightType(kg.lights[light].type) == LightType::TRIANGLE);
          const LightSampleContext prev = prev_light_context(kg, slot);
          p_light = light_sampler_pmf(*kg.light_sampler, prev, light) *
                    tri_pdf_li(kg.lights[light], prev, -wo, LightHit{P, Ng});
        }
      }
      contribution = le * ps_beta(kg, slot) *
                     (radiance_scale(kg) * mis_emitter_scale(ps_r_u(kg, slot), weighted ? ps_r_l(kg, slot) : Spec{}, p_light, weighted));
    }
    if (!ok || !is_finite(contribution)) {
      sample_mark_invalid(kg, sample);
      ps_flags(kg, slot) = flags;
      path_end(kg, slot, Term::INVALID);
      return;
    }
    sample_add(kg, sample, contribution);
    ps_flags(kg, slot) = flags;

    if (total >= kg.session.max_bounces[0]) {
      path_end(kg, slot, Term::MAX_BOUNCE);
      return;
    }

    // NEE from this vertex.
    const uint32_t e = ps_event_index(kg, slot);
    const LightSamplerView& lsv = *kg.light_sampler;
    const LightSampleContext lctx = light_context_point(offset_ray_origin(P, Ng, wo), Ng, N);
    const uint32_t bits = camera_path_draw_bits(kg, sample, event_dim(e, EV_LIGHT_PICK));
    // The alias test's uniform (RULINGS.md 73): per-path dimension 7, XORed with MixBits of the
    // event index, a bijection of the 32 bits for each vertex, so it is uniform and independent of
    // every dimension of the event block.
    const uint32_t q_bits = camera_path_draw_bits(kg, sample, DIM_CAM_ALIAS_Q) ^ uint32_t(mix_bits(uint64_t(e)));
    const Real u_alias = bits_to_unit(q_bits);
    const SampledLight sl = light_sampler_sample(lsv, lctx, bits, u_alias);
    if (!(sl.pmf > Real(0))) break;
    const LightRecord& lr = kg.lights[sl.light];
    const Real2 ul{camera_path_draw(kg, sample, event_dim(e, EV_LIGHT_U)), camera_path_draw(kg, sample, event_dim(e, EV_LIGHT_V))};
    const LightLiSample ls = light_sample_li(lr, lctx, ul, ctx.lambda_t, obs);
    if (!(ls.pdf > Real(0))) break;
    const LobeEval ev = stack_eval(*kg.e_diel, rec, frame_from_normal(N), wo, ls.wi, TransportMode::Radiance);
    Spec nee = ps_beta(kg, slot) * ev.f * (abs_dot(ls.wi, N) * mis_nee_scale(ps_r_u(kg, slot), sl.pmf * ls.pdf, ev.pdf, ls.delta));
    if (!ls.deferred_le) nee *= ls.Le;
    if (!is_finite(nee)) {
      sample_mark_invalid(kg, sample);
      path_end(kg, slot, Term::INVALID);
      return;
    }
    if (!(max_lane(nee) > Real(0))) break;  // no item; the slot stays free
    const bool triangle = LightType(lr.type) == LightType::TRIANGLE;
    ShadowSegment seg;
    if (ls.infinite)
      seg = ShadowSegment{offset_ray_origin(P, Ng, ls.wi), ls.wi, Real(kShadowUnbounded)};
    else if (triangle)
      seg = shadow_segment(P, Ng, ls.p_light, tri_normal(lr));
    else
      seg = shadow_segment_to_lamp(P, Ng, ls.p_light);
    sh = load_ro(kg.shadow_queue + step.shadow_base + item);  // k = 1 (launch_config.h)
    sh_ray_o(kg, sh) = seg.origin;
    sh_ray_d(kg, sh) = seg.dir;
    sh_tmax(kg, sh) = seg.tmax;
    sh_contribution(kg, sh) = nee;
    sh_target(kg, sh) = sample;
    if (kg.session.features.light_groups) sh_light_group(kg, sh) = load_ro(&lr.light_group);
    nee_state = ls.deferred_le ? 2u : 1u;
    if (!ls.deferred_le) break;
    // Phase 1: the light's material at the sampled point, seen from this vertex (wo = -wi).
    AR_ASSERT(triangle);
    g_inst = tri_instance(lr), g_prim = tri_index(lr);
    g_bary = triangle_barycentrics(tri_vertex(lr, 0), tri_vertex(lr, 1), tri_vertex(lr, 2), ls.p_light);
    g_dir = ls.wi;
  }
  if (nee_state == 2u) {
    // The deferred Le carries no radiance scale (068 folds s into light records only).
    const Spec nee = sh_contribution(kg, sh) * le * radiance_scale(kg);
    if (!ok || !is_finite(nee)) {
      sample_mark_invalid(kg, sample);
      path_end(kg, slot, Term::INVALID);
      return;
    }
    sh_contribution(kg, sh) = nee;
    if (!(max_lane(nee) > Real(0))) nee_state = 0u;
  }
  if (nee_state != 0u) sh_next_stage(kg, sh) = static_cast<uint8_t>(Stage::INTERSECT_SHADOW);

  const uint32_t e = ps_event_index(kg, slot);
  const Spec beta = ps_beta(kg, slot);
  const Spec r_u = ps_r_u(kg, slot);
  const Real uc = camera_path_draw(kg, sample, event_dim(e, EV_LOBE_CHOICE));
  const Real2 u2{camera_path_draw(kg, sample, event_dim(e, EV_BSDF_U)), camera_path_draw(kg, sample, event_dim(e, EV_BSDF_V))};
  const LobeSample bs = stack_sample(*kg.e_diel, rec, frame_from_normal(N), wo, uc, u2, TransportMode::Radiance);
  // A reflection below the geometric surface would leave through the wrong side (0005 item 8).
  if (!(bs.pdf > Real(0)) || ((bs.flags & LOBE_REFLECTION) != 0u && !(dot(bs.wi, Ng) > Real(0)))) {
    path_end(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  Spec beta_next = beta * (bs.f * (abs_dot(bs.wi, N) / bs.pdf));
  if (!is_finite(beta_next)) {
    sample_mark_invalid(kg, sample);
    path_end(kg, slot, Term::INVALID);
    return;
  }
  if (!(max_lane(beta_next) > Real(0))) {  // pbrt: if (!beta) break, before the roulette
    path_end(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  if (!roulette(&beta_next, ps_eta_scale(kg, slot), r_u, total + 1u, kg.session.min_bounces,
                camera_path_draw(kg, sample, event_dim(e, EV_ROULETTE)))) {
    path_end(kg, slot, Term::RUSSIAN_ROULETTE);
    return;
  }

  ps_beta(kg, slot) = beta_next;
  ps_r_l(kg, slot) = r_u / bs.pdf;
  const bool delta = (bs.flags & LOBE_DELTA) != 0u;
  ps_flags(kg, slot) = delta ? (flags | LAST_BOUNCE_SPECULAR) : (flags & ~LAST_BOUNCE_SPECULAR);
  const bool glossy = (bs.flags & LOBE_GLOSSY) != 0u;
  U16x6 b = ps_bounces(kg, slot);
  b.v[0] = uint16_t(b.v[0] + 1u);
  b.v[glossy ? 2 : 1] = uint16_t(b.v[glossy ? 2 : 1] + 1u);
  ps_bounces(kg, slot) = b;
  ps_event_index(kg, slot) = uint16_t(e + 1u);
  ps_prev_ng(kg, slot) = oct_encode(Ng);
  ps_prev_ns(kg, slot) = oct_encode(N);
  ps_prev_dist(kg, slot) = Real(0);
  ps_ray_o(kg, slot) = offset_ray_origin(P, Ng, bs.wi);
  ps_ray_d(kg, slot) = bs.wi;
  ps_ray_type(kg, slot) = glossy ? kRayTypeGlossy : kRayTypeDiffuse;
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::INTERSECT_CLOSEST);
}AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_surface.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_surface(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
