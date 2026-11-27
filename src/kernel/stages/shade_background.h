// SPDX-License-Identifier: Apache-2.0
// shade_background: a camera path that left the scene sees the world. It adds
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
// escaped along. A program world whose environment table the host baked is a WORLD_ENV light:
// NEE samples its MIS-compensated table and pdf_li here reads the same compensated pdf
// (light/world_env.h, 0004 D11), so both strategies weigh with one density. A program world with
// no table (a black world) has importance 0, so p_light is 0 and the weight is 1 (0005 item 32).
// A program that cannot run (a missing table or a VM error) makes the sample invalid, like a
// non-finite Le.
// Denoiser guides (0008 d15), with guide planes requested: a path that escapes before its
// denoising surface (DENOISE_SURFACE_DONE clear) writes Cycles' background value, the guide tint
// times the contribution it adds (film_write_emission_or_background_pass: denoising albedo =
// denoising_feature_throughput * contribution), in working-space RGB at the sample's film
// wavelengths with the radiance scale removed, and no normal (Cycles writes none for the
// background, so the plane keeps 0). The value is the beauty's, so it carries its wavelength noise.
// A finite contribution can still overflow the float conversion: a non-finite guide value fails the
// sample, as a non-finite contribution does.
// Film Transparent (SessionUniforms::film_transparent): a camera ray that reaches the world with
// bounces[total] == 0 is Cycles' transparent-background ray (PATH_RAY_TRANSPARENT_BACKGROUND, cleared
// only by a scattering event). It adds transparency = average(throughput) to the transparency plane and
// nothing to Combined, guides included, and does not evaluate the world: with no background pass Cycles
// skips the background shader there too (integrate_background, eval_background), and
// film_write_background writes transparency alone for it. The throughput is 1 unless a non-scattering
// event attenuated it (an absorbing medium): M1b's opacity null event leaves it unchanged, so a surface
// pass-through reads 1 and an alpha card's partial coverage comes from the stochastic decision. A path
// through a visible lamp keeps the flag too: Cycles counts the lamp as a transparent bounce
// (integrator/shade_light.h, "count this as a transparent bounce"), and shade_light advances no total
// here. Depth needs no write at the background: the plane holds Cycles' 0 for a path that never hit.
// Ref: Cycles kernel/integrator/shade_background.h integrate_background (is_transparent_background_ray,
// transparent = average(throughput)) and kernel/film/light_passes.h film_write_background,
// film_write_transparent; kernel/integrator/shade_light.h (lines 94-100).
// Ref: docs/decisions/0004-integrator.md D5, D6 (world row), D28; 0002 D8 (shade_background);
// docs/decisions/0005-material-model.md item 32;
// Cycles kernel/integrator/shade_background.h (Apache-2.0) for the stage structure and
// kernel/geom/shader_data.h shader_setup_from_background for the context.
// Ref: pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::Li (escaped rays: infinite-light
// MIS) (Apache-2.0).
#pragma once
#include <cstdint>

#include "../core/packet.h"
#include "../film/camera_wavelengths.h"
#include "../film/film_write.h"
#include "../film/response.h"
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

// The geometry of a background shade for a ray escaping along unit `dir`: P is the direction, N,
// Ng and wo its negative, and no surface (uv, facing and ids 0). Ray type, depth and lanes are left
// as they are.
AR_FN void world_shade_geometry(Real3 dir, ShadeContext* ctx) {
  ctx->P = dir;
  ctx->N = ctx->Ng = ctx->wo = -dir;
  for (int i = 0; i < kMaxUvSets; ++i) ctx->uv[i] = Real2{Real(0), Real(0)};
  ctx->front_face = ctx->object_id = ctx->instance_id = 0u;
}

// The context of a background shade for a ray escaping along unit `dir`, at the lanes `lam`; ray
// type and depth are 0 until the caller sets them.
AR_FN ShadeContext world_shade_context(Real3 dir, const Lambda& lam) {
  ShadeContext ctx{};
  world_shade_geometry(dir, &ctx);
  ctx.lambda_t = lam;
  return ctx;
}

// The world program and its constant block; false when the scene holds no program (the entry table
// is four words; a shorter one is a host bug, not a world). Its emission entry is word 2, as a
// material's.
AR_FN bool world_program_of(const SceneData& s, const uint32_t** prog, const ConstantBlock** block) {
  if (s.world_program == nullptr || s.world_constants == nullptr || s.world_program_words < 4u) return false;
  *prog = s.world_program;
  *block = s.world_constants;
  return true;
}

// The world program's radiance in `ctx`, unscaled: as a material's emission entry, the block's own
// emission first (0 for a program world, whose emissions are all code), then the entry adds its
// terms. The one evaluation of the world program: for escaped rays here, for NEE items toward an
// environment world on delivery (shade_shadow.h) and for the environment bake (bake_environment.h).
// False when the scene holds no program or the VM fails.
AR_FN bool world_program_radiance(const SceneData& s, const SceneTables& tables, const ShadeContext& ctx, Spec* le) {
  const uint32_t* prog = nullptr;
  const ConstantBlock* block = nullptr;
  if (!world_program_of(s, &prog, &block)) return false;
  Spec e = resolve_slot<SpectrumSemantic::ILLUMINANT>(block->emission_color, ctx.lambda_t, tables.observer, s.material_spectra) *
           Real(block->emission_luminance);
  VmRegs regs;
  if (run_entry(prog, load_ro(prog + 2), &regs, ctx, tables, nullptr, &e) != VmStatus::OK) return false;
  *le = e;
  return true;
}

// The world's radiance along the escaped ray of pool slot `slot` at the lanes `lam`. False when a
// program world cannot be evaluated. A constant world reads nothing of the path but its lanes.
AR_FN bool world_radiance(const KernelGlobals& kg, const LightRecord& world, uint32_t slot, const Lambda& lam, Spec* le) {
  if ((world.flags & kLightWorldProgram) == 0u) {
    *le = light_emission(world, lam, kg.tables->observer);
    return true;
  }
  if (kg.scene == nullptr) return false;
  ShadeContext ctx = world_shade_context(normalize(ps_ray_d(kg, slot)), lam);
  ctx.ray_type = ps_ray_type(kg, slot);
  const U16x6 b = ps_bounces(kg, slot);
  for (int i = 0; i < 6; ++i) ctx.depth[i] = b.v[i];
  Spec e;
  if (!world_program_radiance(*kg.scene, *kg.tables, ctx, &e)) return false;
  *le = e * Real(world.strength);
  return true;
}

// The escaped path's albedo guide: tint x the contribution in display RGB (float conversion: a GPU
// shade kernel has no double arithmetic), divided by the radiance scale as the resolve divides it.
// The tint is the path's (generate_camera starts it at 1). False when the value is not finite:
// nothing is written then. Ref: Cycles kernel/film/light_passes.h film_write_emission_or_background_pass and
// kernel/film/denoising_passes.h film_write_denoising_features_background (Apache-2.0), read in
// third_party/_cache/refs/cycles-ed69ef2c6bd3; docs/decisions/0008-film-outputs-denoise.md d15.
AR_FN bool background_guides(const KernelGlobals& kg, uint32_t slot, uint32_t sample, uint32_t flags, const Spec& contribution) {
  const FilmData& f = *kg.film;
  const uint32_t pixel = film_slot_pixel(kg, sample);
  const uint32_t index = kg.session.samples_done + sample % kg.session.samples_per_pass;
  const FilmWavelengths fw = camera_film_wavelengths(kg.session, f.wavelengths, pixel, index);
  const DisplayRgbOf<float> rgb = display_rgb<float>(f.response, contribution, fw.lf, film_weights(flags, fw.pf), f.film_exposure);
  AR_ASSERT(kg.path->guide_tint != nullptr);
  const Real3 tint = ps_guide_tint(kg, slot);
  const Real inv_s = Real(1) / radiance_scale(kg);
  const Real3 albedo = Real3{tint.x * Real(rgb.v[0]), tint.y * Real(rgb.v[1]), tint.z * Real(rgb.v[2])} * inv_s;
  if (!(albedo.x - albedo.x == Real(0) && albedo.y - albedo.y == Real(0) && albedo.z - albedo.z == Real(0))) return false;
  film_write_guides(kg, sample, albedo, Real3{Real(0), Real(0), Real(0)});
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
  // Film Transparent: a ray with no scattering event so far (bounces[total] == 0, which alpha
  // pass-throughs, false interfaces and lamp crossings leave at 0 as Cycles' transparent bounces leave
  // PATH_RAY_TRANSPARENT_BACKGROUND set) reaching the world adds average(throughput) as transparency and
  // no radiance. A path that scattered still sees the world.
  const bool transparent_ray = kg.session.film_transparent != 0u && ps_bounces(kg, slot).v[0] == 0u;
  if (transparent_ray) {
    film_add_transparency(kg, ps_sample_id(kg, slot), avg(ps_beta(kg, slot)));
  } else if (world != nullptr && !(after_scatter && (hooks & TEST_HOOK_NEE_LIGHT_ONLY) != 0u)) {
    const Spec lt = ps_lambda_t(kg, slot);
    Lambda lam;
    for (int i = 0; i < kLanes; ++i) lam.nm[i] = lt.v[i];
    // The film slot is the sample's, not the pool slot (0004 D5).
    const uint32_t sample = ps_sample_id(kg, slot);
    const bool weighted = after_scatter && (hooks & TEST_HOOK_NEE_OFF) == 0u;
    Spec le = spec_fill(Real(0));
    const bool ran = world_radiance(kg, *world, slot, lam, &le);
    // After the world program, whose registers are free by then, the tree walk of the pmf.
    Real p_light = Real(0);
    if (weighted) {
      // The previous vertex's context differs from NEE's: oct-decoded normals, origin offset toward wi;
      // the tree's pmf moves up to 1.55e-5 relative (measured) and a tangent box can flip the box test.
      const LightSampleContext prev = prev_light_context(kg, slot);
      const Real3 wi = normalize(ps_ray_d(kg, slot));
      p_light = light_sampler_pmf(*kg.light_sampler, prev, kg.world_light, (hooks & TEST_HOOK_LIGHT_ALIAS) != 0u) *
                light_pdf_li(*world, prev, wi, LightHit{prev.p, prev.n_geom}, kg.env);
    }
    const Spec contribution = le * ps_beta(kg, slot) *
                              mis_emitter_scale(ps_r_u(kg, slot), weighted ? ps_r_l(kg, slot) : Spec{}, p_light, weighted);
    const bool guide_ok = !(ran && is_finite(contribution)) || kg.film->guide_albedo < 0 ||
                          (flags & DENOISE_SURFACE_DONE) != 0u || background_guides(kg, slot, sample, flags, contribution);
    if (ran && is_finite(contribution) && guide_ok) {
      sample_add(kg, sample, contribution);
    } else {
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
