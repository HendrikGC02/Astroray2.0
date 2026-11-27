// SPDX-License-Identifier: Apache-2.0
// Pieces every shade stage shares. path_end is the one way a camera path leaves the pool: the
// path-end statistics (bounce count and termination reason), the sample's final flags, and the
// slot returned to the refill through next_stage TERMINATED. The surface stages also share the
// shading context built from a hit, the record filled from a constant block, and the radiance
// scale s that material emission needs.
// Ref: docs/decisions/0004-integrator.md D5 (flags at path end), D28 (path-end statistics);
// 0003 d22 (Term); Cycles kernel/integrator/shade_background.h and state_flow.h
// integrator_path_terminate (Apache-2.0) for the structure.
#pragma once
#include <cstdint>

#include "../core/frame.h"
#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../film/film_write.h"
#include "../kernel_globals.h"
#include "../light/light.h"
#include "../material/constant_block.h"
#include "../material/openpbr_params.h"
#include "../material/slot_resolve.h"
#include "../portability.h"
#include "../sample/dims.h"
#include "../sample/keys.h"
#include "../sample/sobol.h"
#include "../scene/scene_view.h"
#include "../state/flags.h"
#include "../state/stage.h"
#include "../state/termination.h"
#include "../vm/context.h"
#include "../vm/ops/store_slot.h"

AR_NS_BEGIN

// `slot` is the path's pool slot; the film is written at the sample's slot in the per-sample
// buffer, its sample_id (0004 D5), which differs from the pool slot whenever the refill hands out
// pool slots in another order than the sample ids.
AR_FN void path_end(const KernelGlobals& kg, uint32_t slot, Term term) {
  const uint32_t sample = ps_sample_id(kg, slot);
  ps_term(kg, slot) = static_cast<uint8_t>(term);
  film_path_end_stats(kg, sample, uint32_t(ps_bounces(kg, slot).v[0]), term);
  sample_set_flags(kg, sample, ps_flags(kg, slot));
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::TERMINATED);
}

// The radiance scale s = 2^k of 0003 d10. Light records and emission estimates carry it already
// (Scene::lights folds it in); emission that a material program or constant block evaluates does
// not, so a stage that adds such emission multiplies by s exactly once and the resolve divides it
// out. The film holds 1/s = 2^-k as a double. GPU shade kernels may not do double arithmetic
// (0002 D8: only advance_geodesic_f64), so k is read from the double's exponent bits and s is
// built as a float with exponent k: exact, since s is a power of two inside the float range.
AR_FN Real radiance_scale(const KernelGlobals& kg) {
  const uint64_t bits = bit_cast<uint64_t>(kg.film->inv_radiance_scale);
  AR_ASSERT((bits & ((uint64_t(1) << 52) - 1u)) == 0u && (bits >> 63) == 0u);  // a positive power of two
  const int32_t k = 1023 - int32_t((bits >> 52) & 0x7ffu);
  AR_ASSERT(k >= -126 && k <= 127);
  return Real(bit_cast<float>(uint32_t(127 + k) << 23));
}

// Sampler dimension d of a camera sample (stream 0, the session's mode and seed), from the key
// 0003 d23 derives from its sample_id: the same key generate_camera draws dims 0-7 with.
AR_FN Real camera_path_draw(const KernelGlobals& kg, uint32_t sample, uint32_t d) {
  const SamplerKey key = camera_key(kg.active_pixels, sample, kg.session.samples_done, kg.session.samples_per_pass);
  return draw(kg.session.sampler_mode, key.id, key.index, STREAM_FIXED, d, kg.session.seed);
}
// The 32 bits behind camera_path_draw(kg, sample, d).
AR_FN uint32_t camera_path_draw_bits(const KernelGlobals& kg, uint32_t sample, uint32_t d) {
  const SamplerKey key = camera_key(kg.active_pixels, sample, kg.session.samples_done, kg.session.samples_per_pass);
  return draw_bits(kg.session.sampler_mode, key.id, key.index, STREAM_FIXED, d, kg.session.seed);
}

// Russian roulette (0004 D2, after pbrt-v4 VolPathIntegrator::Li): from depth max(2, min_bounces)
// on, a path survives with probability 1 - q, q = max(0, 1 - max_lane(beta eta_scale / avg(r_u))),
// and a survivor's beta is divided by 1 - q (Arvo and Kirk 1990). depth is pbrt's: the scattering
// vertices so far, this one included. max_lane and avg are lane-symmetric (0003 d6).
// roulette_q returns q, 0 where roulette does not apply; roulette applies it with the event
// block's uniform u: false means the path ends, true leaves the survivor's weight in *beta.
// Ref: pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::Li (Apache-2.0), "Possibly
// terminate volumetric path with Russian roulette"; docs/decisions/0004-integrator.md D2.
AR_FN Real roulette_q(const Spec& beta, Real eta_scale, const Spec& r_u, uint32_t depth, uint32_t min_bounces) {
  if (depth < (min_bounces > 2u ? min_bounces : 2u)) return Real(0);
  const Real m = max_lane(beta * (eta_scale / avg(r_u)));
  return m < Real(1) ? Real(1) - m : Real(0);
}
AR_FN bool roulette(Spec* beta, Real eta_scale, const Spec& r_u, uint32_t depth, uint32_t min_bounces, Real u) {
  const Real q = roulette_q(*beta, eta_scale, r_u, depth, min_bounces);
  if (q > Real(0)) {
    if (u < q) return false;
    *beta = *beta / (Real(1) - q);  // q < 1 when beta has a positive lane
  }
  return true;
}

// A unit vector as 32 bits: the octahedral map, each coordinate quantised to snorm16 with
// rounding to nearest. prev_ng and prev_ns hold it (0003 d19 rows 15-16).
// Ref: Cigolle, Donow, Evangelakos, Mara, McGuire and Meyer 2014, "A Survey of Efficient
// Representations for Independent Unit Vectors", JCGT 3(2), listing 1 (float32x3_to_oct,
// oct_to_float32x3, signNotZero) and section 3 (snorm quantisation).
AR_FN Real oct_sign(Real x) { return x >= Real(0) ? Real(1) : Real(-1); }
AR_FN uint32_t oct_snorm16(Real x) {
  const Real c = min(max(x, Real(-1)), Real(1));
  return uint32_t(uint16_t(int16_t(ar::floor(c * Real(32767) + Real(0.5)))));
}
AR_FN uint32_t oct_encode(Real3 n) {
  const Real inv = Real(1) / (ar::abs(n.x) + ar::abs(n.y) + ar::abs(n.z));
  Real px = n.x * inv, py = n.y * inv;
  if (n.z <= Real(0)) {
    const Real qx = (Real(1) - ar::abs(py)) * oct_sign(px), qy = (Real(1) - ar::abs(px)) * oct_sign(py);
    px = qx, py = qy;
  }
  return oct_snorm16(px) | (oct_snorm16(py) << 16);
}
AR_FN Real3 oct_decode(uint32_t e) {
  const Real ex = Real(int16_t(uint16_t(e & 0xffffu))) / Real(32767);
  const Real ey = Real(int16_t(uint16_t(e >> 16))) / Real(32767);
  Real3 v{ex, ey, Real(1) - ar::abs(ex) - ar::abs(ey)};
  if (v.z < Real(0)) {
    const Real vx = (Real(1) - ar::abs(ey)) * oct_sign(ex), vy = (Real(1) - ar::abs(ex)) * oct_sign(ey);
    v.x = vx, v.y = vy;
  }
  return normalize(v);
}

// The light-sampling context of the path's previous scattering vertex, for the MIS weight of an
// emitter its BSDF ray reached (0004 D1): the point ray_o - prev_dist ray_d and the stored
// octahedral normals (0003 d19 rows 15-17). Valid only after a scattering vertex (depth > 0).
AR_FN LightSampleContext prev_light_context(const KernelGlobals& kg, uint32_t slot) {
  const Real3 p = ps_ray_o(kg, slot) - ps_ray_d(kg, slot) * ps_prev_dist(kg, slot);
  return light_context_point(p, oct_decode(ps_prev_ng(kg, slot)), oct_decode(ps_prev_ns(kg, slot)));
}

// A row-major 3x4 object-to-world transform (DeviceInstanceRecord::object_to_world).
struct Xform3x4 {
  Real3 r0, r1, r2;  // rows of the linear part
  Real3 t;           // translation
};

AR_FN Xform3x4 load_xform(const float* m) {
  Xform3x4 x;
  x.r0 = Real3{Real(load_ro(m + 0)), Real(load_ro(m + 1)), Real(load_ro(m + 2))};
  x.r1 = Real3{Real(load_ro(m + 4)), Real(load_ro(m + 5)), Real(load_ro(m + 6))};
  x.r2 = Real3{Real(load_ro(m + 8)), Real(load_ro(m + 9)), Real(load_ro(m + 10))};
  x.t = Real3{Real(load_ro(m + 3)), Real(load_ro(m + 7)), Real(load_ro(m + 11))};
  return x;
}

AR_FN Real3 xform_dir(const Xform3x4& x, Real3 v) { return Real3{dot(x.r0, v), dot(x.r1, v), dot(x.r2, v)}; }
AR_FN Real3 xform_point(const Xform3x4& x, Real3 p) { return xform_dir(x, p) + x.t; }

AR_FN Real3 load_float3(const float* p, uint32_t i) {
  return Real3{Real(load_ro(p + 3u * i)), Real(load_ro(p + 3u * i + 1u)), Real(load_ro(p + 3u * i + 2u))};
}

// The shading context of the hit stored in the path (0005 item 33). P interpolates the triangle's
// positions with the hit's barycentrics (u, v weight corners 1 and 2); UV sets come from the
// mesh's layers by corner and are 0 where the mesh has no such layer (Cycles reads a missing
// attribute as 0).
// Orientation: the front side of a triangle is the one its object-space winding (p1 - p0) x
// (p2 - p0) faces, and it must stay the front under any instance transform M, mirrors included.
// The world-space normal is therefore M^-T n_obj = cof(M) n_obj / det M, and since
// cof(M) (a x b) = (M a) x (M b), Ng = sign(det M) normalize((M e1) x (M e2)); corner normals go
// through the same sign(det M) cof(M). front_face = dot(Ng, wo) > 0. As Cycles does, Ng and N are
// then flipped to face wo for a back-face hit, so programs and the normal AOV see the facing
// normals and front_face (FRONT_FACE in the path flags, 0003 d21) keeps the side.
// object_id is the instance's pass index (Object Info "Object Index"), instance_id its index.
// A smooth normal that is zero or not finite falls back to Ng, as Cycles triangle_smooth_normal
// does for a zero one. Returns false, with the context incomplete, when the triangle is degenerate
// (its Ng has zero or non-finite length): the caller fails the sample.
// Ref: docs/decisions/0005-material-model.md items 8, 33; 0003 d21 (FRONT_FACE survives instance
// transforms); Cycles kernel/geom/triangle.h triangle_normal and triangle_smooth_normal,
// kernel/geom/object.h object_normal_transform, kernel/geom/shader_data.h shader_setup_from_ray
// (a back-face hit flips Ng and N) (Apache-2.0).
// shade_context_geometry fills the geometric part (P, Ng, N, uv, wo, front_face, object_id,
// instance_id) for triangle `prim` of instance `inst` at barycentrics `bary`, seen along ray_d;
// NEE also uses it for the point a light sample picked on an emissive triangle.
AR_FN bool shade_context_geometry(const KernelGlobals& kg, uint32_t inst, uint32_t prim, Real2 bary, Real3 ray_d,
                                  ShadeContext* ctx) {
  const SceneData& s = scene_of(kg);
  AR_ASSERT(inst < s.instance_count);
  const DeviceInstanceRecord& rec = s.instances[inst];
  const uint32_t mesh = load_ro(&rec.mesh);
  AR_ASSERT(mesh < s.mesh_count);
  const DeviceMeshView& mv = s.meshes[mesh];
  AR_ASSERT(prim < load_ro(&mv.triangle_count));
  const uint32_t* tri_vertex = reinterpret_cast<const uint32_t*>(uintptr_t(load_ro(&mv.tri_vertex)));
  const uint32_t* tri_corners = reinterpret_cast<const uint32_t*>(uintptr_t(load_ro(&mv.tri_corners)));
  const float* positions = reinterpret_cast<const float*>(uintptr_t(load_ro(&mv.positions)));
  const float* normals = reinterpret_cast<const float*>(uintptr_t(load_ro(&mv.corner_normals)));
  const Real w0 = Real(1) - bary.x - bary.y;
  const Xform3x4 x = load_xform(rec.object_to_world);

  const Real3 p0 = load_float3(positions, load_ro(tri_vertex + 3u * prim));
  const Real3 p1 = load_float3(positions, load_ro(tri_vertex + 3u * prim + 1u));
  const Real3 p2 = load_float3(positions, load_ro(tri_vertex + 3u * prim + 2u));
  ctx->P = xform_point(x, p0 * w0 + p1 * bary.x + p2 * bary.y);
  const Real3 c0 = cross(x.r1, x.r2), c1 = cross(x.r2, x.r0), c2 = cross(x.r0, x.r1);  // rows of cof(M)
  const Real det_sign = dot(x.r0, c0) < Real(0) ? Real(-1) : Real(1);
  const Real3 ng_raw = cross(xform_dir(x, p1 - p0), xform_dir(x, p2 - p0)) * det_sign;
  const Real ng_len2 = dot(ng_raw, ng_raw);
  if (!(ng_len2 > Real(0)) || !(ng_len2 - ng_len2 == Real(0))) return false;  // zero, NaN or Inf
  Real3 ng = ng_raw / ar::sqrt(ng_len2);

  const uint32_t k0 = load_ro(tri_corners + 3u * prim), k1 = load_ro(tri_corners + 3u * prim + 1u),
                 k2 = load_ro(tri_corners + 3u * prim + 2u);
  const Real3 n_obj =
      load_float3(normals, k0) * w0 + load_float3(normals, k1) * bary.x + load_float3(normals, k2) * bary.y;
  const Real3 n_raw = Real3{dot(c0, n_obj), dot(c1, n_obj), dot(c2, n_obj)} * det_sign;
  const Real n_len2 = dot(n_raw, n_raw);
  Real3 n = (n_len2 > Real(0) && n_len2 - n_len2 == Real(0)) ? n_raw / ar::sqrt(n_len2) : ng;

  const uint32_t layers = load_ro(&mv.uv_layer_count), corners = load_ro(&mv.corner_count);
  const float* uv = reinterpret_cast<const float*>(uintptr_t(load_ro(&mv.uv)));
  for (uint32_t l = 0; l < kMaxUvSets; ++l) {
    ctx->uv[l] = Real2{Real(0), Real(0)};
    if (l >= layers) continue;
    const float* t = uv + 2ull * uint64_t(l) * corners;
    ctx->uv[l].x = w0 * Real(load_ro(t + 2u * k0)) + bary.x * Real(load_ro(t + 2u * k1)) + bary.y * Real(load_ro(t + 2u * k2));
    ctx->uv[l].y = w0 * Real(load_ro(t + 2u * k0 + 1u)) + bary.x * Real(load_ro(t + 2u * k1 + 1u)) +
                   bary.y * Real(load_ro(t + 2u * k2 + 1u));
  }

  ctx->wo = -normalize(ray_d);
  const bool front = dot(ng, ctx->wo) > Real(0);
  if (!front) ng = -ng, n = -n;
  ctx->Ng = ng;
  ctx->N = n;
  ctx->front_face = front ? 1u : 0u;
  ctx->object_id = load_ro(&rec.pass_index);
  ctx->instance_id = inst;
  return true;
}

AR_FN bool shade_context_build(const KernelGlobals& kg, uint32_t slot, ShadeContext* ctx) {
  if (!shade_context_geometry(kg, ps_hit_inst(kg, slot), ps_hit_prim(kg, slot), ps_hit_uv(kg, slot), ps_ray_d(kg, slot), ctx))
    return false;
  ctx->ray_type = ps_ray_type(kg, slot);
  const U16x6 b = ps_bounces(kg, slot);
  for (int i = 0; i < 6; ++i) ctx->depth[i] = b.v[i];
  const Spec lt = ps_lambda_t(kg, slot);
  for (int i = 0; i < kLanes; ++i) ctx->lambda_t.nm[i] = lt.v[i];
  return true;
}

// A frame vector of the constant block; zero means the shading point's own vector.
AR_FN Real3 block_frame(const float* v, Real3 own) {
  return (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f) ? own : Real3{Real(v[0]), Real(v[1]), Real(v[2])};
}

// The slot-resolution pass (0005 item 35): every record field from the material's constant block,
// scalars copied and each SpectrumRef evaluated at the path's wavelengths. A program then
// overwrites the slots it stores per point. A zero frame vector in the block means the shading
// point's own frame (constant_block.h): N for the normals, the tangent of the orthonormal basis
// around N for the tangents. closure_weight is 1 until a selection scales it (item 38); metal_ior
// is 0 (F82 mode, item 12) because M1a builds no (n, k) pairs.
// Ref: docs/decisions/0005-material-model.md items 2(c), 10, 12, 35, 38; core/frame.h (Duff et al.
// 2017 basis).
AR_FN void resolve_constant_record(const ConstantBlock& b, const ShadeContext& ctx, const ObserverView& obs,
                                   const float* spectra, OpenPBRParams* rec) {
  // The block holds every scalar slot but closure_weight, which is the last one: a new scalar slot
  // stops this from compiling until it is copied below.
  static_assert(uint32_t(RecordSlot::closure_weight) == 23u && uint32_t(RecordSlot::closure_weight) + 1u ==
                                                                   uint32_t(RecordSlot::base_color),
                "a scalar record slot was added: copy it from the block");
  rec->base_weight = Real(b.base_weight);
  rec->base_diffuse_roughness = Real(b.base_diffuse_roughness);
  rec->base_metalness = Real(b.base_metalness);
  rec->specular_weight = Real(b.specular_weight);
  rec->specular_roughness = Real(b.specular_roughness);
  rec->specular_roughness_anisotropy = Real(b.specular_roughness_anisotropy);
  rec->transmission_weight = Real(b.transmission_weight);
  rec->transmission_depth = Real(b.transmission_depth);
  rec->transmission_scatter_anisotropy = Real(b.transmission_scatter_anisotropy);
  rec->subsurface_weight = Real(b.subsurface_weight);
  rec->subsurface_radius = Real(b.subsurface_radius);
  rec->subsurface_scatter_anisotropy = Real(b.subsurface_scatter_anisotropy);
  rec->fuzz_weight = Real(b.fuzz_weight);
  rec->fuzz_roughness = Real(b.fuzz_roughness);
  rec->coat_weight = Real(b.coat_weight);
  rec->coat_roughness = Real(b.coat_roughness);
  rec->coat_roughness_anisotropy = Real(b.coat_roughness_anisotropy);
  rec->coat_ior = Real(b.coat_ior);
  rec->coat_darkening = Real(b.coat_darkening);
  rec->thin_film_weight = Real(b.thin_film_weight);
  rec->thin_film_thickness = Real(b.thin_film_thickness);
  rec->thin_film_ior = Real(b.thin_film_ior);
  rec->geometry_opacity = Real(b.geometry_opacity);
  rec->closure_weight = Real(1);
  // The spectral slots in one loop that is never unrolled: the surface kernel inlines this pass, and
  // one inlined resolve instead of one per slot takes about 0.6 s off its nvcc time (B-05) with the
  // same values in every slot. Each slot keeps its own semantic, so its ref resolves as before.
  // The listed slots are Slot 0 .. kSpectral - 1, so the loop visits each exactly once.
#define AR_X(n, s, sem) +1u
  constexpr uint32_t kSpectral = 0u AR_RECORD_SPECTRAL(AR_X);
#undef AR_X
#define AR_X(n, s, sem) static_assert(uint32_t(Slot::s) < kSpectral, "record spectral slots are Slot 0 .. kSpectral - 1");
  AR_RECORD_SPECTRAL(AR_X)
#undef AR_X
#if defined(__CUDACC__)
#pragma unroll 1
#endif
  for (uint32_t k = 0; k < kSpectral; ++k) {
    const SpectrumRef* ref = nullptr;
    SpectrumSemantic slot_sem = SpectrumSemantic::REFLECTANCE;
    switch (Slot(k)) {
#define AR_X(n, s, sem)            \
  case Slot::s:                    \
    ref = &b.n;                    \
    slot_sem = SpectrumSemantic::sem; \
    break;
      AR_RECORD_SPECTRAL(AR_X)
#undef AR_X
      default: AR_ASSERT(false); continue;
    }
    const Spec v = resolve_slot_as(*ref, slot_sem, ctx.lambda_t, obs, spectra);
    switch (Slot(k)) {
#define AR_X(n, s, sem) \
  case Slot::s: rec->n = v; break;
      AR_RECORD_SPECTRAL(AR_X)
#undef AR_X
      default: break;
    }
  }
  const Real3 t = frame_from_normal(ctx.N).s;
  rec->geometry_normal = block_frame(b.geometry_normal, ctx.N);
  rec->geometry_coat_normal = block_frame(b.geometry_coat_normal, ctx.N);
  rec->geometry_tangent = block_frame(b.geometry_tangent, t);
  rec->geometry_coat_tangent = block_frame(b.geometry_coat_tangent, t);
  rec->metal_ior = 0u;
  rec->flags = b.geometry_thin_walled != 0u ? PARAMS_THIN_WALLED : 0u;
  rec->guide_rgb[0] = rec->guide_rgb[1] = 0u;
}

AR_NS_END
