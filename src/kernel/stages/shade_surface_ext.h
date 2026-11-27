// SPDX-License-Identifier: Apache-2.0
// shade_surface_ext (ShadeKind::Ext): a camera path's hit on a material with an extension kind
// (0005 items 14-17). It is the body of shade_surface (stages/shade_surface.h) instantiated for Ext, so
// the VM, emission with MIS, the opacity pass-through, the primary-hit AOVs (Depth included), the
// medium stack, the total limit, NEE and Russian roulette are the shared code; only SurfaceKind<Ext>
// below differs, a switch on the material's ext_kind in place of the OpenPBR lobe stack (item 15: the
// kind replaces the whole stack). This header is the only one that reaches extension code, so the
// Common kernel's TU holds none.
// The kind's constants are the ExtParams block at the header's ext_index; a kind's table is a 16 B
// aligned byte offset into the scene TableArena (item 16; GRATING: ExtParams w[2]).
// GRATING (items 21-23, material/ext/grating.h): the frame is frame_from_normal_tangent(N,
// geometry_tangent), so local x runs along the grooves; the order choice uses the event's extension
// dimension (0003 d23, EV_EXT_CHOICE). Its orders are delta, so a sample is a mirror-like singular
// event: the ray type and bounce counter are glossy, as Cycles counts a reflection without
// LABEL_DIFFUSE and gives a singular one glossy visibility (path_state_next). A sample whose direction
// depends on wavelength (LOBE_WL_DEPENDENT_DIR) terminates the secondary lanes before the throughput
// update (0003 d5): every lane then evaluates at lambda_t[0], so the update takes lane 0's value on
// every lane, and r_u, r_l keep their ratios (0003 d7). Eval and pdf are 0, so a grating hit takes no
// light sample at all: the body asks ext_all_delta before the light selection and picks no light
// (item 17; NEE stays in the body for the non-delta kinds of M7).
// Denoiser guides (PROVISIONAL, lead ruling on 0008 d15): an extension delta sample leaves the guide
// tint unchanged, the grating being treated as a lossless delta for the guide, and a non-delta
// extension sample writes nothing; the open alternative is a tint by the grating's total efficiency
// projected to RGB.
// A kind the kernel does not build, a header whose ext_index or table lies outside the scene's arrays,
// or a tangent along N has no defined lobe: the sample is marked invalid and the path ends INVALID
// (fail loudly; 0004 D17).
// Ref: docs/decisions/0005-material-model.md items 15-17, 21-23; 0003 d5, d7, d23; 0002 D8; pbrt-v4
// src/pbrt/util/spectrum.h SampledWavelengths::TerminateSecondary (read in
// third_party/_cache/refs/pbrt-v4-092b23a47ad4: "for (int i = 1; i < NSpectrumSamples; ++i) pdf[i] = 0;"),
// as core/packet.h terminate_secondaries does it in the lanes; Cycles
// src/kernel/integrator/path_state.h path_state_next (glossy counter for a non-diffuse reflection,
// glossy visibility for LABEL_SINGULAR), read in third_party/_cache/refs/cycles-ed69ef2c6bd3.
#pragma once
#include <cstdint>

#include "../core/frame.h"
#include "../core/packet.h"
#include "../material/ext/grating.h"
#include "../material/ext/grating_table.h"
#include "../material/ext_params.h"
#include "shade_surface.h"

AR_NS_BEGIN

// Whether every lobe a kind can sample is delta, so NEE can add nothing at its hits; one row per
// kind of ext_fields.def. GRATING: delta orders, eval and pdf 0 (item 22).
AR_FN constexpr bool ext_all_delta(uint32_t kind) {
  return kind == uint32_t(ExtKind::GRATING);
}

// A lobe value that fails the sample: the body's finiteness tests end the path INVALID.
AR_FN Real ext_nan() { return Real(bit_cast<float>(0x7fc00000u)); }

// The kind and constants of header `mat`: kind 0 when the header is the default material or its
// ext_index lies outside the scene's ExtParams (a host bug the caller fails).
struct ExtHit {
  uint32_t kind;
  ExtParams params;
};
AR_FN ExtHit ext_hit(const KernelGlobals& kg, uint32_t mat) {
  const SceneData& s = scene_of(kg);
  ExtHit x;
  x.kind = 0u;
  for (int i = 0; i < 16; ++i) x.params.w[i] = 0u;
  if (s.material_headers == nullptr || mat >= s.material_header_count) return x;
  MaterialHeader h;
  h.ext = load_ro(&s.material_headers[mat].ext);
  if (s.ext_params == nullptr || ext_index(h) >= s.ext_param_count) return x;
  const ExtParams* p = s.ext_params + ext_index(h);
  for (int i = 0; i < 16; ++i) x.params.w[i] = load_ro(&p->w[i]);
  x.kind = ext_kind(h);
  return x;
}

// The grating's efficiency table in the arena; false when its orders or its extent do not fit
// (grating_table.h layout: order_count x kGratingCosBins x kGratingWavelengths floats).
AR_FN bool ext_grating_table(const SceneData& s, const GratingParams& g, GratingTable* t) {
  const int orders = g.m_max - g.m_min + 1;
  if (orders < 1 || orders > kGratingMaxOrders || (g.table & 15u) != 0u || s.table_arena == nullptr) return false;
  const uint64_t bytes = uint64_t(orders) * uint64_t(kGratingCosBins) * uint64_t(kGratingWavelengths) * sizeof(float);
  if (uint64_t(g.table) + bytes > uint64_t(s.table_arena_bytes)) return false;
  t->eta = reinterpret_cast<const float*>(s.table_arena + g.table);
  t->m_min = g.m_min;
  t->order_count = orders;
  return true;
}

AR_FN LobeSample ext_invalid_sample() {
  LobeSample out;
  out.wi = Real3{Real(0), Real(0), Real(0)};
  out.f = spec_fill(ext_nan());
  out.pdf = ext_nan();
  out.flags = 0u;
  return out;
}

// Sample of the kind at the path's hit (item 17's interface), wo and the result in world space.
AR_FN LobeSample ext_sample(const KernelGlobals& kg, uint32_t slot, uint32_t sample, uint32_t e, const OpenPBRParams& rec,
                            Real3 N, Real3 wo) {
  const ExtHit x = ext_hit(kg, hit_material_id(kg, ps_hit_inst(kg, slot), ps_hit_prim(kg, slot)));
  const Frame frame = frame_from_normal_tangent(N, rec.geometry_tangent);
  // A tangent along N (or not finite) leaves no groove direction.
  if (!(dot(frame.s, frame.s) > Real(0.5))) return ext_invalid_sample();
  Lambda lam;
  const Spec lt = ps_lambda_t(kg, slot);
  for (int i = 0; i < kLanes; ++i) lam.nm[i] = lt.v[i];
  const Real u = camera_path_draw(kg, sample, event_dim(e, EV_EXT_CHOICE));
  switch (x.kind) {
    case uint32_t(ExtKind::GRATING): {
      const GratingParams g = grating_params(x.params);
      GratingTable tab;
      if (!ext_grating_table(scene_of(kg), g, &tab)) return ext_invalid_sample();
      LobeSample bs = grating_sample(g, tab, frame, wo, TransportMode::Radiance, lam, u);
      bs.flags |= LOBE_GLOSSY;  // a singular event counts and traces as glossy (header comment)
      return bs;
    }
    default:
      return ext_invalid_sample();
  }
}

// f and pdf toward wi; only reached for a kind that is not all delta (ext_all_delta).
AR_FN LobeEval ext_eval(const KernelGlobals& kg, const OpenPBRParams& rec, uint32_t mat, Real3 N, Real3 wo, Real3 wi) {
  const ExtHit x = ext_hit(kg, mat);
  LobeEval ev;
  switch (x.kind) {
    case uint32_t(ExtKind::GRATING): {
      const GratingParams g = grating_params(x.params);
      GratingTable tab;
      if (ext_grating_table(scene_of(kg), g, &tab)) {
        Lambda lam;
        for (int i = 0; i < kLanes; ++i) lam.nm[i] = Real(0);  // eval reads no wavelength (delta)
        return grating_eval(g, tab, frame_from_normal_tangent(N, rec.geometry_tangent), wo, wi, TransportMode::Radiance, lam);
      }
      break;
    }
    default:
      break;
  }
  ev.f = spec_fill(ext_nan());
  ev.pdf = ext_nan();
  return ev;
}

template <>
struct SurfaceKind<ShadeKind::Ext> {
  // A header without a valid kind is not skipped: its eval then fails the sample.
  static AR_FN bool nee_skipped(const KernelGlobals& kg, uint32_t mat) {
    const ExtHit x = ext_hit(kg, mat);
    return x.kind != 0u && ext_all_delta(x.kind);
  }
  static AR_FN LobeEval eval(const KernelGlobals& kg, const OpenPBRParams& rec, uint32_t mat, Real3 N, Real3 wo, Real3 wi) {
    return ext_eval(kg, rec, mat, N, wo, wi);
  }
  // The lobe-choice uniform is the stack's; a kind draws its own choice from EV_EXT_CHOICE.
  static AR_FN LobeSample sample(const KernelGlobals& kg, uint32_t slot, uint32_t sample, uint32_t e, const OpenPBRParams& rec, Real3 N,
                                 Real3 wo, float) {
    return ext_sample(kg, slot, sample, e, rec, N, wo);
  }
  // A wavelength-dependent direction terminates the secondaries first (0003 d5): lambda_t and beta
  // take lane 0 on every lane and the flags SECONDARIES_TERMINATED, stored at once so every later end
  // of the path carries them; the update then applies lane 0's f on every lane.
  static AR_FN Spec throughput(const KernelGlobals& kg, uint32_t slot, const LobeSample& bs, const Spec& beta, Real3 N,
                               uint32_t* flags) {
    const Real w = abs_dot(bs.wi, N) / bs.pdf;
    if ((bs.flags & LOBE_WL_DEPENDENT_DIR) == 0u) return beta * (bs.f * w);
    Lambda lam;
    const Spec lt = ps_lambda_t(kg, slot);
    for (int i = 0; i < kLanes; ++i) lam.nm[i] = lt.v[i];
    Spec b = beta;
    terminate_secondaries(lam, b, *flags);
    Spec stored;
    for (int i = 0; i < kLanes; ++i) stored.v[i] = lam.nm[i];
    ps_lambda_t(kg, slot) = stored;
    ps_flags(kg, slot) = *flags;
    return b * (bs.f.v[0] * w);
  }
  // PROVISIONAL (header comment): the tint and the guide planes are left as they are.
  static AR_FN uint32_t guides(const KernelGlobals&, uint32_t, uint32_t, uint32_t flags, OpenPBRParams&, Real3, const LobeSample&) {
    return flags;
  }
};

AR_FN void stage_shade_surface_ext(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  shade_surface_body<ShadeKind::Ext>(kg, item, step);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_surface_ext.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_surface_ext(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
