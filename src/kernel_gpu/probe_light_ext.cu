// SPDX-License-Identifier: Apache-2.0
// Light-path extension probe (0005 items 17-18, 25; 0004 D21-D23; ROADMAP M-1, M-2): a gate-only
// kernel, never launched or embedded, that stands in for shade_light_path_ext until M5's light pass
// builds it, so `check full` can read its registers, stack and spills capped and uncapped (0002 D10,
// ruling 17). Its body is one light-path vertex on an extension material, in the order of pbrt-v4's
// LightPathIntegrator: the shading context from the hit, the material's surface entry into the
// OpenPBR record in the kernel's one VmRegs (0005 items 2-3), the total limit, a camera connection
// written as a shadow item with a film position (0004 D21, D23), the kind's sample through
// shade_surface_ext.h's ext_sample, terminate_secondaries on LOBE_WL_DEPENDENT_DIR inside the Ext
// throughput update (0003 d5, 0004 D22), Russian roulette and the next ray. Every function it calls
// is the camera path's own; nothing of them is copied.
// Stubs, each standing for a decision M5 makes:
// - stub: the sampler key. Draws use camera_path_draw at the path's sample_id; a light path's key
//   is its rank in the light work (0003 d19, d23), which has no draw function yet.
// - stub: transport mode. ext_sample and ext_eval fix TransportMode::Radiance (shade_surface_ext.h); 0004 D21
//   and 0005 item 25 call for Importance. Radiance is the larger code (grating.h adds the
//   transmitted (n_i / n_o)^2 factor only in Radiance mode), so the probe over-counts if anything.
// - stub: the Veach 1997 ch. 5 shading-normal correction of 0004 D21 is not applied.
// - stub: the connection (0004 D23). camera_sample_wi with no lens sample (the pinhole ignores it),
//   no medium fields, no packet flags in item_flags (0003 d20 has no connection bit yet), and the
//   item's target is the sample_id; sh_lambda_t and sh_item_flags stay unwritten, and the light
//   group indexes kg.lights[prev_ns] unchecked (M5 fills both and bounds the index). It runs only
//   for a kind that is not all delta (ext_all_delta), as NEE does in shade_surface_ext: GRATING
//   never connects, the M7 kinds will.
// - stub: a light path's end sets term and TERMINATED only: path_end writes a camera sample's
//   statistics and flags, which a light path does not have.
// - stub: no alpha pass-through and no false interface; 0004 D21 shares both with camera paths and
//   M5 decides where light paths meet them.
// Ref: docs/decisions/0004-integrator.md D21-D23; 0005 items 2, 3, 17, 18, 25; 0002 D10;
// stages/shade_surface.h and stages/shade_surface_ext.h (the functions called); pbrt-v4
// src/pbrt/cpu/integrators.cpp LightPathIntegrator (the depth limit, then the camera SampleWi
// connection beta f(Importance) |cos| Wi / pdf, then Sample_f in Importance and the beta update),
// read in third_party/_cache/refs/pbrt-v4-092b23a47ad4.
#include "launch_config.h"
#include "camera/camera.h"
#include "stages/shade_surface_ext.h"

AR_NS_BEGIN
namespace probe_light_ext {

AR_FN void end_light_path(const KernelGlobals& kg, uint32_t slot, Term term) {
  ps_term(kg, slot) = static_cast<uint8_t>(term);
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::TERMINATED);
}

AR_FN void stage(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  const uint32_t sample = ps_sample_id(kg, slot);
  const SceneData& s = scene_of(kg);
  const ObserverView& obs = kg.tables->observer;
  uint32_t flags = ps_flags(kg, slot);
  const uint32_t inst = ps_hit_inst(kg, slot), prim = ps_hit_prim(kg, slot);
  bool ok = s.material_headers != nullptr && s.material_constants != nullptr;
  ShadeContext ctx;
  ctx.ray_type = ps_ray_type(kg, slot);
  {
    const U16x6 b = ps_bounces(kg, slot);
    for (int i = 0; i < 6; ++i) ctx.depth[i] = b.v[i];
    const Spec lt = ps_lambda_t(kg, slot);
    for (int i = 0; i < kLanes; ++i) ctx.lambda_t.nm[i] = lt.v[i];
  }
  const uint32_t total = ctx.depth[0];
  if (!shade_context_geometry(kg, inst, prim, ps_hit_uv(kg, slot), ps_ray_d(kg, slot), &ctx)) {
    end_light_path(kg, slot, Term::INVALID);
    return;
  }
  const uint32_t mat = hit_material_id(kg, inst, prim);
  const uint32_t e = ps_event_index(kg, slot);
  OpenPBRParams rec;
  VmRegs regs;  // the kernel's one register file (0005 item 3)
  Spec le = spec_fill(Real(0));  // the entry's emission segment: a light path adds no emission at a hit
  float uc = float(camera_path_draw(kg, sample, event_dim(e, EV_LOBE_CHOICE)));
  if (ok) {
    const MaterialHeader* hp = s.material_headers + mat;
    const uint32_t mflags = load_ro(&hp->flags);
    const ConstantBlock& block = s.material_constants[load_ro(&hp->const_offset)];
    resolve_constant_record(block, ctx, obs, s.material_spectra, &rec);
    const uint32_t* prog = s.material_programs + load_ro(&hp->program_offset);
    const uint32_t entry = (mflags & CONSTANT_SURFACE) == 0u ? load_ro(prog) : 0u;
    ok = run_entry(prog, entry, &regs, ctx, *kg.tables, &rec, &le, &uc) == VmStatus::OK;
  }
  if (!ok) {
    end_light_path(kg, slot, Term::INVALID);
    return;
  }
  const Real3 P = ctx.P, Ng = ctx.Ng, N = ctx.N, wo = ctx.wo;
  flags = ctx.front_face != 0u ? (flags | FRONT_FACE) : (flags & ~FRONT_FACE);
  if (total >= kg.session.max_bounces[0]) {
    end_light_path(kg, slot, Term::MAX_BOUNCE);
    return;
  }
  const Spec beta = ps_beta(kg, slot);

  // Camera connection (pbrt-v4 LightPathIntegrator, before the BSDF sample).
  if (!SurfaceKind<ShadeKind::Ext>::nee_skipped(kg, mat)) {
    const CameraWiSample cs = camera_sample_wi(*kg.camera, P, Real2{Real(0), Real(0)});
    if (cs.pdf > Real(0) && cs.We > Real(0)) {
      const LobeEval ev = ext_eval(kg, rec, mat, N, wo, cs.wi);
      const Spec L = beta * ev.f * (abs_dot(cs.wi, N) * cs.We / cs.pdf);
      if (!is_finite(L)) {
        end_light_path(kg, slot, Term::INVALID);
        return;
      }
      if (max_lane(L) > Real(0)) {
        const ShadowSegment seg = shadow_segment_to_lamp(P, Ng, cs.p_lens);
        const uint32_t sh = load_ro(kg.shadow_queue + step.shadow_base + item);  // k = 1 (launch_config.h)
        sh_ray_o(kg, sh) = seg.origin;
        sh_ray_d(kg, sh) = seg.dir;
        sh_tmax(kg, sh) = seg.tmax;
        sh_contribution(kg, sh) = L;
        sh_target(kg, sh) = sample;
        sh_film_pos(kg, sh) = cs.film;
        // A light path keeps its light's index in prev_ns (path_state.def).
        if (kg.session.features.light_groups) sh_light_group(kg, sh) = load_ro(&kg.lights[ps_prev_ns(kg, slot)].light_group);
        sh_next_stage(kg, sh) = static_cast<uint8_t>(Stage::INTERSECT_SHADOW);
      }
    }
  }

  // The kind's sample and the throughput update (terminate_secondaries inside, 0003 d5).
  const Spec r_u = ps_r_u(kg, slot);
  const LobeSample bs = ext_sample(kg, slot, sample, e, rec, N, wo);
  if (!is_finite(bs.f) || !(bs.pdf - bs.pdf == Real(0))) {
    end_light_path(kg, slot, Term::INVALID);
    return;
  }
  if (!(bs.pdf > Real(0)) || ((bs.flags & LOBE_REFLECTION) != 0u && !(dot(bs.wi, Ng) > Real(0)))) {
    end_light_path(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  Spec beta_next = SurfaceKind<ShadeKind::Ext>::throughput(kg, slot, bs, beta, N, &flags);
  if (!is_finite(beta_next)) {
    end_light_path(kg, slot, Term::INVALID);
    return;
  }
  if (!(max_lane(beta_next) > Real(0))) {
    end_light_path(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  if (!roulette(&beta_next, ps_eta_scale(kg, slot), r_u, total + 1u, kg.session.min_bounces,
                camera_path_draw(kg, sample, event_dim(e, EV_ROULETTE)))) {
    end_light_path(kg, slot, Term::RUSSIAN_ROULETTE);
    return;
  }
  ps_beta(kg, slot) = beta_next;
  const bool delta = (bs.flags & LOBE_DELTA) != 0u;
  ps_flags(kg, slot) = delta ? (flags | LAST_BOUNCE_SPECULAR) : (flags & ~LAST_BOUNCE_SPECULAR);
  const bool glossy = (bs.flags & LOBE_GLOSSY) != 0u;
  U16x6 b = ps_bounces(kg, slot);
  b.v[0] = uint16_t(b.v[0] + 1u);
  b.v[glossy ? 2 : 1] = uint16_t(b.v[glossy ? 2 : 1] + 1u);
  ps_bounces(kg, slot) = b;
  ps_event_index(kg, slot) = uint16_t(e + 1u);
  ps_ray_o(kg, slot) = offset_ray_origin(P, Ng, bs.wi);
  ps_ray_d(kg, slot) = bs.wi;
  ps_ray_type(kg, slot) = glossy ? kRayTypeGlossy : kRayTypeDiffuse;
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::INTERSECT_CLOSEST);
}
}  // namespace probe_light_ext
AR_NS_END

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_ar_probe_light_ext.threads, ar::kLaunch_ar_probe_light_ext.min_blocks)
    ar_probe_light_ext(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const ar::StepEntry step = g.step_table->stage[static_cast<uint32_t>(ar::Stage::SHADE_LIGHT_PATH_EXT)];
  for (uint32_t item = blockIdx.x * blockDim.x + threadIdx.x; item < step.count; item += gridDim.x * blockDim.x)
    ar::probe_light_ext::stage(g, item, step);
}
