// SPDX-License-Identifier: Apache-2.0
// shade_surface (ShadeKind::Common): a camera path's surface hit. In stage order, as Cycles'
// integrate_surface: build the shading context from the hit, run the material's surface entry
// into the OpenPBR record (a CONSTANT_SURFACE material only resolves its slots), add the emission
// the path sees with its MIS weight, decide between the surface and an opacity null event, write
// the primary-hit AOVs, then the total bounce limit, one light sample (NEE) emitted as a shadow
// item, a sample of the lobe stack, Russian roulette and the next ray.
// Alpha pass-through (0002 D11: camera and indirect rays never run any-hit; 0004 D3, D14): with
// u the event's lobe-choice uniform after the Mix selection, u >= a (a the record's
// geometry_opacity, of the selected child, 0005 item 38) is a null event that re-queues
// intersect_closest from just past the hit with the transparent counter and event index advanced
// (surface_pass_through); u < a takes the surface with u / a as its lobe choice. Le already
// carries a: the compiler multiplies the emission by the opacity, so the hit, NEE on this
// triangle and the emission estimate read one a x Le and nothing scales it here. Shadow rays stay
// opaque to such materials (transparent shadows are M2); the compiler reports that per material.
// Emission MIS (0004 D1, integrator/mis.h): emission reached by a BSDF ray after a non-delta
// bounce is weighted against NEE with pmf(ctx_prev, light) pdf_li(ctx_prev, wi), the previous
// vertex's context rebuilt from prev_ng, prev_ns and prev_dist; at the first vertex or after a
// delta bounce (LAST_BOUNCE_SPECULAR) it is divided by avg(r_u) only. The light of a hit triangle
// is light_of_hit's; a triangle that is no light (or has pmf 0, 0004 D9) gets p_light 0.
// The NEE test modes (kernel_globals.h TEST_HOOK_NEE_*): off emits no shadow item and weights
// emission 1; light_only drops emission after a non-delta bounce and the BSDF pdf from the NEE
// weight (Veach 1997, ch. 9: each strategy alone is unbiased).
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
// material's emission entry with the context rebuilt at the light point. An environment world
// (WORLD_ENV, light/world_env.h) is deferred too but has no phase 1: NEE draws its direction from
// the MIS-compensated table, the item's contribution takes everything but Le (the MIS weight and
// the record's strength, which carries s, as an escaped ray's), and the item carries its lanes and
// ITEM_WORLD_LE so shade_shadow runs the world program on delivery. The world's interpreter thus
// stays out of this kernel (0002 D10 ladder, B-05); the table never gives radiance (0004 D11).
// Continuation, as pbrt-v4 VolPathIntegrator::Li: a path that arrives with bounces[total] equal
// to the limit ends MAX_BOUNCE (pbrt's depth++ == maxDepth). Otherwise the stack is sampled in
// the frame of N with the event block's lobe choice, as the surface entry's Mix selection left it
// in the entry's selection uniform (vm/ops/common.h select_u), and direction u, v (0003 d23); NEE and this
// sample both use the record of the child that selection chose (0005 item 38); a reflected
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
// Denoiser guides (0008 d15, d22), with guide planes requested and before the path's denoising
// surface (DENOISE_SURFACE_DONE clear): after the lobe sample, a delta sample multiplies the path's
// guide tint by the RGB albedo of the stack's delta lobes, and a non-delta sample makes this hit the
// denoising surface: it writes the tinted albedo and the shading normal and sets the bit
// (surface_guides).
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

// The opacity decision of a hit (0005 item 4: top = mix(transparent, surface, opacity)), on the
// event's lobe-choice uniform u as the surface entry's Mix selection left it (0004 D14: Mix,
// opacity and lobe choice share one uniform). With a = clamp(opacity, 0, 1), OpenPBR's defined
// range of geometry_opacity, u >= a is the null event (true). Otherwise the surface is taken and u
// becomes u / a, again uniform on [0, 1) for the lobe choice, kept below 1 as BRANCH_MIX keeps it.
// Ref: pbrt-v4 src/pbrt/util/sampling.h SampleDiscrete (uRemapped, OneMinusEpsilon; Apache-2.0);
// docs/decisions/0005-material-model.md items 4, 38; 0004 D14.
AR_FN bool opacity_null_event(Real opacity, float* u) {
  const float a = opacity > Real(0) ? (opacity < Real(1) ? float(opacity) : 1.0f) : 0.0f;
  if (!(*u < a)) return true;
  // a > u >= 0 here; the select keeps a constant-folded opacity of 0 from reading as a division by 0.
  const float r = *u / (a > 0.0f ? a : 1.0f);
  *u = r < kBranchOneMinusEpsilon ? r : kBranchOneMinusEpsilon;
  return false;
}

// The null event of an opacity pass-through (0004 D3): only the transparent counter and the event
// index advance (the next vertex's uniforms are a new block, 0004 D14); direction, flags, beta,
// r_u, r_l and the previous vertex stay. The ray leaves from just past the hit and prev_dist grows
// by the distance the origin moved, so ray_o - prev_dist ray_d is still the previous scattering
// vertex (as shade_light's crossing). A path that arrives with the transparent counter at its limit
// ends TRANSPARENT_LIMIT instead of passing; its emission was already added (Blender: the next
// surface after the limit emits and terminates).
// Ref: Cycles kernel/integrator/path_state.h path_state_next (transparent_bounce, a transparent label
// keeps the flags; Apache-2.0) and kernel/integrator/shade_surface.h
// integrate_surface_bsdf_bssrdf_bounce (a transparent bounce only moves the ray start), read in
// third_party/_cache/refs/cycles-ed69ef2c6bd3; docs/decisions/0004-integrator.md D3, D14;
// 0002 D11; 0003 d19 (prev_dist), d23.
AR_FN void surface_pass_through(const KernelGlobals& kg, uint32_t slot, Real3 P, Real3 Ng) {
  U16x6 b = ps_bounces(kg, slot);
  if (b.v[5] >= kg.session.max_bounces[5]) {
    path_end(kg, slot, Term::TRANSPARENT_LIMIT);
    return;
  }
  b.v[5] = uint16_t(b.v[5] + 1u);
  ps_bounces(kg, slot) = b;
  ps_event_index(kg, slot) = uint16_t(ps_event_index(kg, slot) + 1u);
  const Real3 o = ps_ray_o(kg, slot), d = ps_ray_d(kg, slot);
  const Real3 o_next = offset_past_surface(P, Ng, d);
  ps_prev_dist(kg, slot) = ps_prev_dist(kg, slot) + dot(o_next - o, d) / dot(d, d);
  ps_ray_o(kg, slot) = o_next;
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::INTERSECT_CLOSEST);
}

// The lobe stack's directional albedo at mu_o in working-space RGB (0008 d15): the stack's own
// selection quantities (stack_select: the dielectric layer's E_diel, the metal's F E_spec c, the
// EON albedo under the layering scale) evaluated with the material's exact RGB colours in lanes
// 0-2 instead of the path's wavelengths, so no wavelength noise enters. Lane 3 repeats green so
// every lane holds a valid colour. The index is the record's (one value on every lane unless
// dispersive) or, for a Cauchy index, its n_d. `rec` is the caller's record after the lobe sample,
// which this overwrites: its scalars are the shading point's, its colours are replaced.
// diffuse = w (1 - M) (1 - E_spec) E_eon; specular = w ((1 - M) E_spec + M min(F E_m c, 1)).
// Ref: docs/decisions/0008-film-outputs-denoise.md d15 (diffuse and specular albedo from the
// albedo tables OpenPBR albedo scaling uses, exact RGB projections); 0005 items 4-6;
// material/lobes/stack.h stack_select.
struct GuideAlbedo {
  Real3 diffuse, specular;
};
AR_FN GuideAlbedo surface_guide_albedo(const KernelGlobals& kg, OpenPBRParams& rec, const ConstantBlock& block, Real mu_o) {
  for (int i = 0; i < kLanes; ++i) {
    const int c = i < 3 ? i : 1;
    rec.base_color.v[i] = Real(block.guide_base_color[c]);
    rec.specular_color.v[i] = Real(block.guide_specular_color[c]);
  }
  const bool cauchy = block.specular_ior.kind == uint8_t(SpectrumKind::CAUCHY_ABBE);
  rec.specular_ior = spec_fill(cauchy ? Real(block.specular_ior.p[0]) : rec.specular_ior.v[0]);
  const StackSelect sel = stack_select(*kg.e_diel, *kg.e_spec, rec, mu_o);
  const Spec e_diff = eon_albedo(stack_eon_params(rec), mu_o);
  Spec metal = spec_fill(Real(0));
  if (sel.metal > Real(0)) {
    const MetalParams mp = stack_metal_params(rec);
    const Spec f_o = metal_fresnel(mp, mu_o), comp = stack_metal_comp(rec, sel);
    for (int i = 0; i < kLanes; ++i) metal.v[i] = min(max(f_o.v[i] * sel.e_metal * comp.v[i], Real(0)), Real(1));
  }
  const Real w = rec.closure_weight, m = sel.metal;
  Real d[3], sp[3];
  for (int i = 0; i < 3; ++i) {
    d[i] = w * ((Real(1) - m) * (Real(1) - sel.e_spec.v[i]) * e_diff.v[i]);
    sp[i] = w * ((Real(1) - m) * sel.e_spec.v[i] + m * metal.v[i]);
  }
  GuideAlbedo g;
  g.diffuse = Real3{d[0], d[1], d[2]};
  g.specular = Real3{sp[0], sp[1], sp[2]};
  return g;
}

// The guide step of a camera path's vertex after its lobe sample, before the denoising surface
// (0008 d15; the caller tests DENOISE_SURFACE_DONE). generate_camera starts the tint at 1 and
// clears the bit, and a real volume scatter sets it (shade_volume.h), so the tint is read here
// whatever the bounce count. A delta sample multiplies it by the RGB albedo of the delta lobes: the
// specular and metal lobes share specular_roughness, so they are delta together, and the guide
// takes both, as Cycles defers the whole specular albedo, rather than the one the lobe choice drew.
// A non-delta sample writes tint x (diffuse + specular) and the front-facing shading normal and
// returns the flags with DENOISE_SURFACE_DONE. A material whose base_color or specular_color is
// set per point has no exact RGB yet: its hit is the denoising surface with magenta albedo, the
// project's colour for unsupported input. `rec` is overwritten.
// Ref: Cycles kernel/film/denoising_passes.h film_write_denoising_features_surface (Apache-2.0;
// denoising_feature_throughput times the deferred specular albedo; the normal is not tinted), read
// in third_party/_cache/refs/cycles-ed69ef2c6bd3; docs/decisions/0008-film-outputs-denoise.md
// d15, d22, d23; 0003 d21 (DENOISE_SURFACE_DONE).
AR_FN uint32_t surface_guides(const KernelGlobals& kg, uint32_t slot, uint32_t sample, uint32_t flags, OpenPBRParams& rec,
                              const ConstantBlock& block, Real3 N, Real3 wo, bool delta) {
  AR_ASSERT(kg.path->guide_tint != nullptr);
  if ((block.guide_flags & GUIDE_RGB_RUNTIME) != 0u) {
    film_write_guides(kg, sample, Real3{Real(1), Real(0), Real(1)}, N);
    return flags | DENOISE_SURFACE_DONE;
  }
  const Real3 tint = ps_guide_tint(kg, slot);
  const GuideAlbedo a = surface_guide_albedo(kg, rec, block, dot(wo, N));
  if (delta) {
    ps_guide_tint(kg, slot) = Real3{tint.x * a.specular.x, tint.y * a.specular.y, tint.z * a.specular.z};
    return flags;
  }
  const Real3 albedo = a.diffuse + a.specular;
  film_write_guides(kg, sample, Real3{tint.x * albedo.x, tint.y * albedo.y, tint.z * albedo.z}, N);
  return flags | DENOISE_SURFACE_DONE;
}

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
  // contribution (an environment world's included: shade_shadow multiplies its Le in on delivery);
  // 2: an item whose contribution still lacks a triangle's deferred Le and s (phase 1).
  uint32_t nee_state = 0;
  uint32_t sh = 0;  // the reserved shadow slot, read when an item is written
  // Where ctx is built: the path's hit in phase 0, the light sample's point in phase 1.
  uint32_t g_inst = inst, g_prim = prim;
  Real2 g_bary = ps_hit_uv(kg, slot);
  Real3 g_dir = ps_ray_d(kg, slot);
  uint32_t mat = 0;
  // The event's lobe-choice uniform goes through the surface entry as its selection uniform: a
  // Mix Shader's BRANCH_MIX rescales it, and the lobe choice below uses what it left (0005 items
  // 31, 38; 0004 D14). Without a Mix the program leaves it alone.
  float uc = float(camera_path_draw(kg, sample, event_dim(ps_event_index(kg, slot), EV_LOBE_CHOICE)));
  Real alpha = Real(1);  // the hit's geometry_opacity, read from the record after the surface entry
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
      if (phase == 0u) resolve_constant_record(block, ctx, obs, s.material_spectra, &rec);
      le = resolve_slot<SpectrumSemantic::ILLUMINANT>(block.emission_color, ctx.lambda_t, obs, s.material_spectra) *
           Real(block.emission_luminance);
      const uint32_t* prog = s.material_programs + load_ro(&hp->program_offset);
      const bool program = phase == 0u ? (mflags & CONSTANT_SURFACE) == 0u : (mflags & CONSTANT_EMISSION) == 0u;
      const uint32_t entry = program ? load_ro(prog + (phase == 0u ? 0u : 2u)) : 0u;
      ok = run_entry(prog, entry, &regs, ctx, *kg.tables, phase == 0u ? &rec : nullptr, &le,
                     phase == 0u ? &uc : nullptr) == VmStatus::OK;
      if (phase == 0u) alpha = rec.geometry_opacity;
    }
    if (phase != 0u) break;

    P = ctx.P, Ng = ctx.Ng, N = ctx.N, wo = ctx.wo;
    flags = ctx.front_face != 0u ? (flags | FRONT_FACE) : (flags & ~FRONT_FACE);
    Spec contribution = spec_fill(Real(0));
    const uint32_t hooks = kg.session.test_hooks;
    const bool after_scatter = total > 0u && !arrived_specular;
    // NEE test modes (kernel_globals.h): light_only drops emission NEE could have sampled.
    if (ok && max_lane(le) > Real(0) && !(after_scatter && (hooks & TEST_HOOK_NEE_LIGHT_ONLY) != 0u)) {
      const bool weighted = after_scatter && (hooks & TEST_HOOK_NEE_OFF) == 0u;
      Real p_light = Real(0);
      if (weighted) {
        const uint32_t light = light_of_hit(kg, inst, prim);
        ok = light != kLightIndexInvalid;  // a hit outside the scene's tables: fail the sample
        if (ok && light != kNoLight) {
          // light_of_hit names emissive triangles only (lamps cross in shade_light), so the
          // triangle's own pdf_li stands for the dispatch.
          AR_ASSERT(LightType(kg.lights[light].type) == LightType::TRIANGLE);
          const LightSampleContext prev = prev_light_context(kg, slot);
          p_light = light_sampler_pmf(*kg.light_sampler, prev, light, (hooks & TEST_HOOK_LIGHT_ALIAS) != 0u) *
                    tri_pdf_li(kg.lights[light], prev, -wo, LightHit{P, Ng});
        }
      }
      contribution = le * ps_beta(kg, slot) *
                     (radiance_scale(kg) * mis_emitter_scale(ps_r_u(kg, slot), weighted ? ps_r_l(kg, slot) : Spec{}, p_light, weighted));
    }
    // A NaN opacity has no decision: like a VM error it fails the sample.
    const bool valid = ok && is_finite(contribution) && alpha == alpha;
    if (valid) sample_add(kg, sample, contribution);
    // The opacity decision after the emission (a hit's Le already carries the opacity: the compiler
    // scales it, 0005 item 38), before the AOVs and the total limit, so a pass-through at the total
    // limit still passes, writes no AOV and leaves the flags (FRONT_FACE included) as they were.
    if (valid && opacity_null_event(alpha, &uc)) {
      surface_pass_through(kg, slot, P, Ng);
      return;
    }
    // Written even when the VM failed (the AOVs name the hit), whenever the material tables exist.
    if (s.material_pass_index != nullptr && s.material_headers != nullptr && total == 0u) {
      const uint32_t index = kg.session.samples_done + sample % kg.session.samples_per_pass;
      film_primary_aov(kg, sample, index, &flags, P, N, ctx.uv[0].x, ctx.uv[0].y, load_ro(s.material_pass_index + mat));
    }
    ps_flags(kg, slot) = flags;
    if (!valid) {
      sample_mark_invalid(kg, sample);
      path_end(kg, slot, Term::INVALID);
      return;
    }

    if (total >= kg.session.max_bounces[0]) {
      path_end(kg, slot, Term::MAX_BOUNCE);
      return;
    }

    // NEE from this vertex; none in the NEE_OFF test mode (BSDF sampling alone).
    if ((hooks & TEST_HOOK_NEE_OFF) != 0u) break;
    const uint32_t e = ps_event_index(kg, slot);
    const LightSamplerView& lsv = *kg.light_sampler;
    const LightSampleContext lctx = light_context_point(offset_ray_origin(P, Ng, wo), Ng, N);
    const uint32_t bits = camera_path_draw_bits(kg, sample, event_dim(e, EV_LIGHT_PICK));
    // The alias test's uniform (RULINGS.md 73): per-path dimension 7, XORed with MixBits of the
    // event index, a bijection of the 32 bits for each vertex, so it is uniform and independent of
    // every dimension of the event block.
    const uint32_t q_bits = camera_path_draw_bits(kg, sample, DIM_CAM_ALIAS_Q) ^ uint32_t(mix_bits(uint64_t(e)));
    const Real u_alias = bits_to_unit(q_bits);
    const SampledLight sl = light_sampler_sample(lsv, lctx, bits, u_alias, (hooks & TEST_HOOK_LIGHT_ALIAS) != 0u);
    if (!(sl.pmf > Real(0))) break;
    const LightRecord& lr = kg.lights[sl.light];
    const Real2 ul{camera_path_draw(kg, sample, event_dim(e, EV_LIGHT_U)), camera_path_draw(kg, sample, event_dim(e, EV_LIGHT_V))};
    // NEE_LIGHT_ONLY has no BSDF strategy, so an environment world samples its complete table.
    const LightLiSample ls = light_sample_li(lr, lctx, ul, ctx.lambda_t, obs, kg.env, (hooks & TEST_HOOK_NEE_LIGHT_ONLY) != 0u);
    if (!(ls.pdf > Real(0))) break;
    const LobeEval ev = stack_eval(*kg.e_diel, *kg.e_spec, rec, frame_from_normal(N), wo, ls.wi, TransportMode::Radiance);
    // NEE_LIGHT_ONLY: the BSDF strategy is off, so its pdf leaves the weight.
    const Real p_bsdf = (hooks & TEST_HOOK_NEE_LIGHT_ONLY) != 0u ? Real(0) : ev.pdf;
    Spec nee = ps_beta(kg, slot) * ev.f * (abs_dot(ls.wi, N) * mis_nee_scale(ps_r_u(kg, slot), sl.pmf * ls.pdf, p_bsdf, ls.delta));
    const bool triangle = LightType(lr.type) == LightType::TRIANGLE;
    if (!ls.deferred_le) nee *= ls.Le;
    else if (!triangle) nee *= Real(lr.strength);  // WORLD_ENV: its strength carries s
    if (!is_finite(nee)) {
      sample_mark_invalid(kg, sample);
      path_end(kg, slot, Term::INVALID);
      return;
    }
    if (!(max_lane(nee) > Real(0))) break;  // no item; the slot stays free
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
    const bool world_le = ls.deferred_le && !triangle;
    if (kg.shadow->item_flags != nullptr) sh_item_flags(kg, sh) = world_le ? ITEM_WORLD_LE : uint8_t(0);
    if (world_le) {
      // WORLD_ENV: shade_shadow runs the world program along ray_d at these lanes on delivery. The
      // host allocates both fields with the env_world feature; without them the item cannot be
      // finished, a host bug that fails the sample.
      if (kg.shadow->item_flags == nullptr || kg.shadow->lambda_t == nullptr) {
        sample_mark_invalid(kg, sample);
        path_end(kg, slot, Term::INVALID);
        return;
      }
      Spec lt;
      for (int i = 0; i < kLanes; ++i) lt.v[i] = ctx.lambda_t.nm[i];
      sh_lambda_t(kg, sh) = lt;
    }
    nee_state = (ls.deferred_le && triangle) ? 2u : 1u;
    if (nee_state == 1u) break;
    // Phase 1: the light's material at the sampled point, seen from this vertex (wo = -wi), or the
    // world program along wi.
    g_inst = tri_instance(lr), g_prim = tri_index(lr);
    g_bary = triangle_barycentrics(tri_vertex(lr, 0), tri_vertex(lr, 1), tri_vertex(lr, 2), ls.p_light);
    g_dir = ls.wi;
  }
  if (nee_state == 2u) {
    // A triangle's deferred Le carries no radiance scale (068 folds s into light records only); the
    // world's strength, which carries it, is already in the contribution.
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
  const Real2 u2{camera_path_draw(kg, sample, event_dim(e, EV_BSDF_U)), camera_path_draw(kg, sample, event_dim(e, EV_BSDF_V))};
  const LobeSample bs = stack_sample(*kg.e_diel, *kg.e_spec, rec, frame_from_normal(N), wo, Real(uc), u2, TransportMode::Radiance);
  // Guides at the denoising surface (surface_guides); a branch on the constant plane offset.
  if (kg.film->guide_albedo >= 0 && (flags & DENOISE_SURFACE_DONE) == 0u && bs.pdf > Real(0)) {
    // The material and wo are read again from the path rather than kept live across the lobe sample
    // (registers); phase 0 built the context from these same fields, so the values are identical.
    const uint32_t m = hit_material_id(kg, ps_hit_inst(kg, slot), ps_hit_prim(kg, slot));
    const ConstantBlock& block = s.material_constants[load_ro(&s.material_headers[m].const_offset)];
    flags = surface_guides(kg, slot, sample, flags, rec, block, N, -normalize(ps_ray_d(kg, slot)),
                           (bs.flags & LOBE_DELTA) != 0u);
    ps_flags(kg, slot) = flags;
  }
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
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_surface.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_surface(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
