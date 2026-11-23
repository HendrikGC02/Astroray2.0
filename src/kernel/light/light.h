// SPDX-License-Identifier: Apache-2.0
// The one light interface: sample_li, pdf_li, sample_le, power and bounds, dispatched by a
// closed enum in a switch. "Absent" lives in the type: an infinite light says `infinite`, never a
// distance of 1e30; no sample is pdf == 0 with Le == 0, never a NaN.
// Include this file, not point.h, sphere.h, area_rect.h or area_disk.h: they need the types defined here.
//
// Ref: docs/decisions/0004-integrator.md D7 (interface), D1 (delta lights: NEE weight 1, BSDF
// pdf 0), D6 (energy conventions), D9 (theta_e: pi/2 one-sided); pbrt-v4 src/pbrt/lights.h
// (LightSampleContext, LightLiSample, LightLeSample, LightBounds), util/sampling.h
// (SampleUniformSphere, SampleUniformDiskConcentric, SampleCosineHemisphere); Shirley and
// Chiu 1997, "A Low Distortion Map Between Disk and Square" (Apache-2.0 sources).
#pragma once
#include <cfloat>
#include <cstdint>

#include "../color/observer.h"
#include "../core/frame.h"
#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../portability.h"
#include "light_record.h"

#define AR_LIGHT_TYPES_DEFINED 1

AR_NS_BEGIN

// Closed list; a new type adds an enumerator and one row to each switch below.
enum class LightType : uint8_t {
  POINT = 0,  // radius 0: delta intensity
  SPHERE,     // radius > 0: area-consistent radiance, visible-cone sampling
  AREA_RECT,  // one-sided rectangle or square, spherical-rectangle sampling
  AREA_DISK,  // one-sided disk, area sampling
  TRIANGLE,   // emissive mesh triangle (triangle.h)
  WORLD_CONST // constant world radiance, infinite (world_const.h)
};

enum class LightContextKind : uint8_t { POINT = 0, SEGMENT };

// Where light is gathered. A POINT context is a surface vertex. A SEGMENT context is a stretch
// of a medium ray (Conty Estevez and Kulla 2018 section 5.3): the light sampler (067) reads o,
// d, t_max for its importance, while sample_li and pdf_li always work from `p`, the point on the
// segment the distance sampling picked. No light-linking set (R45).
struct LightSampleContext {
  Real3 p, n_geom, n_shade;
  LightContextKind kind;
  Real3 o, d;
  Real t_max;
};

AR_FN LightSampleContext light_context_point(Real3 p, Real3 n_geom, Real3 n_shade) {
  LightSampleContext c;
  c.p = p;
  c.n_geom = n_geom;
  c.n_shade = n_shade;
  c.kind = LightContextKind::POINT;
  c.o = p;
  c.d = Real3{Real(0), Real(0), Real(0)};
  c.t_max = Real(0);
  return c;
}

// What a BSDF ray found on a lamp (D1, D8); area types need it for the solid-angle conversion,
// point and sphere lamps ignore it.
struct LightHit {
  Real3 p, n;
};

struct LightLiSample {
  Real3 wi;       // unit, from the context point toward the light
  Real dist;      // to p_light; meaningless when `infinite`
  bool infinite;  // distant light: no distance
  Real pdf;       // solid angle, 1 for delta lights; 0 means no sample
  bool delta;     // NEE weight 1, BSDF-strategy pdf 0 (D1)
  Real3 p_light;
  Spec Le;        // radiance toward the context; for delta lights I / dist^2 (pbrt PointLight::SampleLi)
  // Le is not known here: the light's material emits per point (an emissive triangle with
  // kLightEmissionProgram). Le is 0 and the consumer must evaluate the material's `emission` entry
  // at p_light, toward -wi; using Le as it stands would drop the light.
  bool deferred_le = false;
};

struct LightRay {
  Real3 o, d;
};

struct LightLeSample {
  LightRay ray;
  Spec Le;  // emitted radiance along the ray; intensity for delta lights (pbrt PointLight::SampleLe)
  Real pdf_pos;
  Real pdf_dir;
  // As LightLiSample::deferred_le: Le is 0 and the consumer evaluates the `emission` entry at
  // ray.o toward ray.d.
  bool deferred_le = false;
};

// The scene's bounding sphere: distant lights aim light paths at it, and the world's power
// (pbrt UniformInfiniteLight::Phi) scales with its radius.
struct SceneSphere {
  Real3 center;
  Real radius;
};

struct Bounds3 {
  Real3 lo, hi;
};

// Light-tree bounds (Conty Estevez and Kulla 2018): w is the unit axis of the normal cone, theta_o
// its half-angle, theta_e the emission spread around it. w matters only when theta_o < pi; an
// isotropic light (theta_o = pi) sets an arbitrary unit w, as Cycles does (scene/light_tree.cpp:
// a point light's "measure.bcone.axis" is its transform's -Z with "theta_o = M_PI_F").
struct LightBounds {
  Bounds3 bbox;
  Real3 w;
  Real theta_o, theta_e;
  bool two_sided;
};

inline constexpr Real kLightPi = Real(3.14159265358979323846);

// The cone axis of a light whose normals cover every direction (theta_o = pi): any unit vector
// works; Cycles takes (0, 0, 1) for the background ("Set an arbitrary direction for the background
// light.", scene/light_tree.cpp).
AR_FN Real3 light_isotropic_axis() { return Real3{Real(0), Real(0), Real(1)}; }
// pbrt-v4 FloatOneMinusEpsilon / DoubleOneMinusEpsilon (util/float.h): the largest Real below 1.
inline constexpr Real kLightOneMinusEpsilon =
    sizeof(Real) == 4 ? Real(float(0x1.fffffep-1)) : Real(double(0x1.fffffffffffffp-1));

AR_FN LightLiSample light_li_none() {
  LightLiSample s;
  s.wi = Real3{Real(0), Real(0), Real(1)};
  s.dist = Real(0);
  s.infinite = false;
  s.pdf = Real(0);
  s.delta = false;
  s.p_light = Real3{Real(0), Real(0), Real(0)};
  s.Le = spec_fill(Real(0));
  return s;
}

AR_FN LightLeSample light_le_none() {
  LightLeSample s;
  s.ray.o = Real3{Real(0), Real(0), Real(0)};
  s.ray.d = Real3{Real(0), Real(0), Real(1)};
  s.Le = spec_fill(Real(0));
  s.pdf_pos = Real(0);
  s.pdf_dir = Real(0);
  return s;
}

AR_FN Real light_safe_sqrt(Real x) { return ar::sqrt(ar::max(Real(0), x)); }

// pbrt-v4 SampleUniformSphere: z = 1 - 2 u0, phi = 2 pi u1; pdf 1 / (4 pi).
AR_FN Real3 sample_uniform_sphere(Real2 u) {
  const Real z = Real(1) - Real(2) * u.x;
  const Real r = light_safe_sqrt(Real(1) - z * z);
  // phi = 2 pi u.y, through cospi / sinpi (portability.h: no double slow path on the GPU).
  return Real3{r * ar::cospi(Real(2) * u.y), r * ar::sinpi(Real(2) * u.y), z};
}

// pbrt-v4 SampleUniformDiskConcentric (Shirley and Chiu 1997).
AR_FN Real2 sample_disk_concentric(Real2 u) {
  const Real ox = Real(2) * u.x - Real(1), oy = Real(2) * u.y - Real(1);
  if (ox == Real(0) && oy == Real(0)) return Real2{Real(0), Real(0)};
  // t = theta / pi, so the angle goes through cospi / sinpi (portability.h).
  Real r, t;
  if (ar::abs(ox) > ar::abs(oy)) {
    r = ox;
    t = (oy / ox) / Real(4);
  } else {
    r = oy;
    t = Real(0.5) - (ox / oy) / Real(4);
  }
  return Real2{r * ar::cospi(t), r * ar::sinpi(t)};
}

// pbrt-v4 SampleCosineHemisphere about +z; pdf = z / pi (zero at the horizon).
AR_FN Real3 sample_cosine_hemisphere(Real2 u) {
  const Real2 d = sample_disk_concentric(u);
  return Real3{d.x, d.y, light_safe_sqrt(Real(1) - d.x * d.x - d.y * d.y)};
}

AR_NS_END

#include "point.h"
#include "sphere.h"
#include "area_rect.h"  // before area_disk.h: the disk reuses its frame helpers
#include "area_disk.h"
#include "triangle.h"
#include "world_const.h"

AR_NS_BEGIN

// Host side. A light record is built from these or checked by light_valid before upload; false
// means the host raises a visible error (a bad lamp is never rendered as something else).
AR_FN LightRecord light_make(LightType type, Real3 pos, Real radius, Real strength, const SpectrumRef& emission) {
  LightRecord r;
  r.type = uint8_t(type);
  r.light_group = 0;
  r.flags = 0;
  r.visibility = 0xFFFFFFFFu;  // every ray class until the host narrows it from the descriptor
  for (int i = 0; i < 12; ++i) r.xf[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  r.xf[3] = float(pos.x);
  r.xf[7] = float(pos.y);
  r.xf[11] = float(pos.z);
  r.size[0] = float(radius);
  r.size[1] = 0.0f;
  r.strength = float(strength);
  r.emission = emission;
  return r;
}

// An emissive triangle (light_record.h encoding); flags are kLightTwoSided, kLightBackFace and
// kLightEmissionProgram. The indices must stay below 0x7f800000 (checked by light_problem).
AR_FN LightRecord light_make_triangle(Real3 p0, Real3 p1, Real3 p2, uint32_t instance, uint32_t tri, uint16_t flags,
                                      Real strength, const SpectrumRef& emission) {
  LightRecord r = light_make(LightType::TRIANGLE, p0, Real(0), strength, emission);
  const Real3 v[3] = {p0, p1, p2};
  for (int k = 0; k < 3; ++k) r.xf[3 * k] = float(v[k].x), r.xf[3 * k + 1] = float(v[k].y), r.xf[3 * k + 2] = float(v[k].z);
  r.xf[9] = r.xf[10] = r.xf[11] = 0.0f;
  r.size[0] = bit_cast<float>(instance);
  r.size[1] = bit_cast<float>(tri);
  r.flags = flags;
  return r;
}

AR_FN LightRecord light_make_world(Real strength, const SpectrumRef& emission) {
  return light_make(LightType::WORLD_CONST, Real3{Real(0), Real(0), Real(0)}, Real(0), strength, emission);
}

// An area lamp: xf is the rotation (axis_x, axis_y, axis_z = axis_x x axis_y, front = -axis_z)
// and the centre; size_x x size_y is a rectangle's width and height, a disk's size_x its radius
// and size_y 0, all in world units.
AR_FN LightRecord light_make_area(LightType type, Real3 pos, Real3 axis_x, Real3 axis_y, Real size_x, Real size_y,
                                  Real strength, const SpectrumRef& emission) {
  LightRecord r = light_make(type, pos, size_x, strength, emission);
  const Real3 z = cross(axis_x, axis_y);
  const Real cols[3][3] = {{axis_x.x, axis_x.y, axis_x.z}, {axis_y.x, axis_y.y, axis_y.z}, {z.x, z.y, z.z}};
  for (int c = 0; c < 3; ++c)
    for (int row = 0; row < 3; ++row) r.xf[4 * row + c] = float(cols[c][row]);
  r.size[1] = float(size_y);
  return r;
}

AR_FN bool light_finite(float x) { return x - x == 0.0f; }

// The area lamps' shared checks: the transform is a rotation (a scale would silently resize the
// lamp, it belongs in size), and the area A keeps L = P sigma / (pi A) and the pdf 1 / A finite
// and normal in float. sigma is the emission's own scale, as for the sphere.
AR_FN const char* area_problem(const LightRecord& r, double area) {
  const double tol = double(1e-3);
  double ax[3][3];
  for (int c = 0; c < 3; ++c)
    for (int row = 0; row < 3; ++row) ax[c][row] = double(r.xf[4 * row + c]);
  for (int c = 0; c < 3; ++c) {
    const double len2 = ax[c][0] * ax[c][0] + ax[c][1] * ax[c][1] + ax[c][2] * ax[c][2];
    if (!(len2 > (double(1.0) - tol) * (double(1.0) - tol) && len2 < (double(1.0) + tol) * (double(1.0) + tol)))
      return "an area light transform must be a rotation; scale belongs in the size";
    const int d = (c + 1) % 3;
    const double dp = ax[c][0] * ax[d][0] + ax[c][1] * ax[d][1] + ax[c][2] * ax[d][2];
    if (!(dp > -tol && dp < tol)) return "an area light transform must be a rotation; scale belongs in the size";
  }
  const double sigma = r.emission.kind == uint8_t(SpectrumKind::CONSTANT) ? double(r.emission.p[0]) : double(r.emission.p[3]);
  const double pi = double(3.14159265358979323846);
  if (!(area >= double(r.strength) * sigma / (pi * double(FLT_MAX))) || !(area >= double(FLT_MIN)) || !(area <= double(1.0) / double(FLT_MIN)))
    return "area light is so small or so large that its radiance or area pdf is not representable";
  return nullptr;
}

// nullptr when the record can be uploaded, else the reason the host reports. A radius whose
// radiance L = P sigma / (4 pi^2 r^2) or whose area pdf 1 / (4 pi r^2) would overflow float is
// refused rather than rendered dark: with sigma the emission's own scale (CONSTANT p[0], RGB
// p[3]) that needs r^2 >= P sigma / (4 pi^2 FLT_MAX) and r^2 >= 1 / (4 pi FLT_MAX). The
// illuminant SPD (Y = 1) is not folded in; the kernel's finite guards are the backstop for it.
AR_FN const char* light_problem(const LightRecord& r) {
  for (int i = 0; i < 12; ++i)
    if (!light_finite(r.xf[i])) return "light transform is not finite";
  if (!light_finite(r.size[0])) return "light radius is not finite";
  if (!light_finite(r.strength) || !(r.strength >= 0.0f)) return "light power must be finite and not negative";
  // Only the kinds resolve() evaluates; the illuminant semantic is what makes colour a light.
  if (r.emission.semantic != uint8_t(SpectrumSemantic::ILLUMINANT)) return "light colour must use the illuminant semantic";
  if (r.emission.kind != uint8_t(SpectrumKind::CONSTANT) && r.emission.kind != uint8_t(SpectrumKind::RGB))
    return "light colour kind is not supported";
  switch (LightType(r.type)) {
    case LightType::POINT: return r.size[0] == 0.0f ? nullptr : "a point light has radius 0";
    case LightType::SPHERE: {
      if (!(r.size[0] > 0.0f)) return "a sphere light needs a positive radius";
      const double sigma = r.emission.kind == uint8_t(SpectrumKind::CONSTANT) ? double(r.emission.p[0]) : double(r.emission.p[3]);
      const double r2 = double(r.size[0]) * double(r.size[0]);
      const double pi = double(3.14159265358979323846);
      const double four = double(4.0);
      if (!(r2 >= double(r.strength) * sigma / (four * pi * pi * double(FLT_MAX))) || !(r2 >= double(1.0) / (four * pi * double(FLT_MAX))))
        return "sphere light radius is so small that its radiance overflows";
      return nullptr;
    }
    case LightType::AREA_RECT: {
      if (!(r.size[0] > 0.0f) || !(r.size[1] > 0.0f)) return "a rectangle light needs a positive width and height";
      return area_problem(r, double(r.size[0]) * double(r.size[1]));
    }
    case LightType::AREA_DISK: {
      if (!(r.size[0] > 0.0f)) return "a disk light needs a positive radius";
      if (r.size[1] != 0.0f) return "a disk light has size[1] 0";
      return area_problem(r, double(3.14159265358979323846) * double(r.size[0]) * double(r.size[0]));
    }
    case LightType::TRIANGLE: {
      if (tri_instance(r) >= 0x7f800000u || tri_index(r) >= 0x7f800000u) return "triangle light index out of range";
      if (r.flags & ~uint16_t(kLightTwoSided | kLightBackFace | kLightEmissionProgram)) return "unknown triangle light flag";
      if ((r.flags & kLightTwoSided) && (r.flags & kLightBackFace)) return "a triangle light is two-sided or back-only, not both";
      const Real3 c = tri_cross(r);
      const double a2 = double(c.x) * double(c.x) + double(c.y) * double(c.y) + double(c.z) * double(c.z);
      if (!(a2 >= double(FLT_MIN)) || !(a2 <= double(1.0) / double(FLT_MIN)))
        return "triangle light area is zero or not representable";
      return nullptr;
    }
    case LightType::WORLD_CONST: return nullptr;
  }
  return "unknown light type";
}

AR_FN bool light_valid(const LightRecord& r) { return light_problem(r) == nullptr; }

// Deterministic in (ctx, u). The unknown-type default is a bug that a debug build traps.
AR_FN LightLiSample light_sample_li(const LightRecord& r, const LightSampleContext& ctx, Real2 u, const Lambda& lam,
                                    const ObserverView& obs) {
  switch (LightType(r.type)) {
    case LightType::POINT: return point_sample_li(r, ctx, lam, obs);
    case LightType::SPHERE: return sphere_sample_li(r, ctx, u, lam, obs);
    case LightType::AREA_RECT: return rect_sample_li(r, ctx, u, lam, obs);
    case LightType::AREA_DISK: return disk_sample_li(r, ctx, u, lam, obs);
    case LightType::TRIANGLE: return tri_sample_li(r, ctx, u, lam, obs);
    case LightType::WORLD_CONST: return world_sample_li(r, ctx, u, lam, obs);
  }
  AR_ASSERT(false);
  return light_li_none();
}

// Solid-angle pdf of reaching the light along wi (a BSDF ray's direction at the previous vertex,
// 0004 D1); 0 for delta lights.
AR_FN Real light_pdf_li(const LightRecord& r, const LightSampleContext& ctx, Real3 wi, const LightHit& hit) {
  switch (LightType(r.type)) {
    case LightType::POINT: return Real(0);
    case LightType::SPHERE: return sphere_pdf_li(r, ctx, wi, hit);
    case LightType::AREA_RECT: return rect_pdf_li(r, ctx, wi, hit);
    case LightType::AREA_DISK: return disk_pdf_li(r, ctx, wi, hit);
    case LightType::TRIANGLE: return tri_pdf_li(r, ctx, wi, hit);
    case LightType::WORLD_CONST: return world_pdf_li(r, ctx, wi, hit);
  }
  AR_ASSERT(false);
  return Real(0);
}

// For light paths. u_pos picks the origin and u_dir the direction; delta position lights ignore
// u_pos. Infinite lights aim at `target`, the scene's bounding sphere.
AR_FN LightLeSample light_sample_le(const LightRecord& r, Real2 u_pos, Real2 u_dir, const Lambda& lam,
                                    const ObserverView& obs, const SceneSphere& target) {
  switch (LightType(r.type)) {
    case LightType::POINT: return point_sample_le(r, u_dir, lam, obs);
    case LightType::SPHERE: return sphere_sample_le(r, u_pos, u_dir, lam, obs);
    case LightType::AREA_RECT: return rect_sample_le(r, u_pos, u_dir, lam, obs);
    case LightType::AREA_DISK: return disk_sample_le(r, u_pos, u_dir, lam, obs);
    case LightType::TRIANGLE: return tri_sample_le(r, u_pos, u_dir, lam, obs);
    case LightType::WORLD_CONST: return world_sample_le(r, u_pos, u_dir, lam, obs, target);
  }
  AR_ASSERT(false);
  return light_le_none();
}

AR_FN bool light_is_infinite(const LightRecord& r) { return LightType(r.type) == LightType::WORLD_CONST; }

// The power with D10's weighting: `energy` is the weighted integral of the light's emission,
// strength included (integral of strength s(lambda) w(lambda) d lambda; the strength alone when
// unweighted), times the type's geometric factor. Lamps: 1 (their strength is the power P).
// Triangle: pi A (D9). World: 4 pi^2 R^2 with R the scene radius (pbrt UniformInfiniteLight::Phi).
AR_FN Real light_power(const LightRecord& r, const SceneSphere& scene, Real energy) {
  switch (LightType(r.type)) {
    case LightType::POINT:
    case LightType::SPHERE:
    case LightType::AREA_RECT:
    case LightType::AREA_DISK: return energy;
    case LightType::TRIANGLE: return tri_power(r, energy);
    case LightType::WORLD_CONST: return world_power(scene, energy);
  }
  AR_ASSERT(false);
  return Real(0);
}

// The unweighted scalar power of a finite light, Blender's P times the radiance scale for lamps;
// the world's needs the scene sphere (world_power).
AR_FN Real light_power(const LightRecord& r) {
  switch (LightType(r.type)) {
    case LightType::POINT: return point_power(r);
    case LightType::SPHERE: return sphere_power(r);
    case LightType::AREA_RECT: return rect_power(r);
    case LightType::AREA_DISK: return disk_power(r);
    case LightType::TRIANGLE: return tri_power(r, Real(r.strength));
    case LightType::WORLD_CONST: break;  // a bug: its power depends on the scene radius
  }
  AR_ASSERT(false);
  return Real(0);
}

AR_FN LightBounds light_bounds(const LightRecord& r) {
  switch (LightType(r.type)) {
    case LightType::POINT: return point_bounds(r);
    case LightType::SPHERE: return sphere_bounds(r);
    case LightType::AREA_RECT: return rect_bounds(r);
    case LightType::AREA_DISK: return disk_bounds(r);
    case LightType::TRIANGLE: return tri_bounds(r);
    case LightType::WORLD_CONST: break;
  }
  // Infinite: no box (light_is_infinite), and the cone of Cycles' background light
  // (scene/light_tree.cpp: "Set an arbitrary direction for the background light." axis (0, 0, 1),
  // "theta_o = M_PI_F", "theta_e = 0").
  AR_ASSERT(light_is_infinite(r));
  LightBounds b;
  b.bbox.lo = b.bbox.hi = Real3{Real(0), Real(0), Real(0)};
  b.w = light_isotropic_axis();
  b.theta_o = kLightPi;
  b.theta_e = Real(0);
  b.two_sided = false;
  return b;
}

AR_NS_END
