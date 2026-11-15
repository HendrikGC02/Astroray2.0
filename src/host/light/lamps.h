// SPDX-License-Identifier: Apache-2.0
// Lamp ingestion: ar_light_desc to kernel LightRecords (the one LightType dispatch of
// kernel/light/light.h), the lamps' bounds, and the per-render radiance scale s = 2^k.
// Ref: docs/decisions/0004-integrator.md D6 (conventions), D8 (lamps never block shadow rays);
// 0003 d10 (radiance scale), d11 (Blender watt = factor 1); 0007-appendix-abi.md ar_light_desc;
// Cycles intern/cycles/scene/light.cpp (Apache-2.0) for `normalize`: PointLight::copy_to_kernel
// `invarea = normalize ? 1.0f / area(...) : 1.0f`, AreaLight::copy_to_kernel
// `invarea = ((normalize || is_portal) && area_ != 0.0f) ? 1.0f / area_ : 1.0f`, with
// PointLight::area = 4 pi r^2 (4 when r = 0) and AreaLight::area = |col0 sizeu| |col1 sizev|
// (times pi/4 for an ellipse).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../../include/astroray/astroray.h"
#include "../../kernel/color/rgb2spec.h"
#include "../../kernel/light/light.h"
#include "host/light/light_builder.h"

namespace astroray::host {

class Edit;

// One lamp as the scene stores it. `record.strength` is P (times the lamp's Cycles area when the
// descriptor is not normalised, so the kernel's 1 / area gives Cycles' unnormalised radiance),
// without the radiance scale, which the host applies per render.
struct Lamp {
  ar_key key = 0;
  ar::LightRecord record = {};
  ar_xform xf = {};         // as given, for the lamp's instance record
  uint32_t visibility = 0;  // camera, diffuse, glossy, transmission; never shadow (0004 D8)
  double centre[3] = {0, 0, 0};
  double bound_radius = 0;  // a sphere around the lamp, for the scene sphere
  std::vector<std::string> approximations;  // one NODE_APPROXIMATED detail each
};

// Throws std::invalid_argument for a type other than POINT, AREA_RECT, AREA_ELLIPSE, or for a
// value the kernel's light_problem refuses (negative power, non-finite size, ...).
std::shared_ptr<const Lamp> build_lamp(ar_key key, const ar_light_desc& d, const ar::Rgb2SpecTable& table);

// ar_edit_set_light: validates now, stages and commits its own item; the lamp joins the scene
// and its approximations are raised when the generation activates.
ar_status set_light(Edit& edit, ar_key key, const ar_light_desc* desc, const ar::Rgb2SpecTable& table);

// The smallest sphere holding `s` and the lamp's bound (an empty `s`, radius 0, is replaced).
// Ref: closed form for two spheres at distance d: radius (d + r1 + r2) / 2, centre on the line of centres
// (Ericson, Real-Time Collision Detection, 2005, section 4.3 on bounding spheres).
ar::SceneSphere enclose(const ar::SceneSphere& s, const Lamp& lamp);

// Largest over the observer table support of the light's spectral radiance: strength * S(lambda),
// divided by the lamp's D6 geometric factor (see lamps.cpp)
// (0004 D6; 0003 d10).
double emitter_peak(const LightInput& light, const ar::ObserverView& obs);

// s = 2^k with the largest peak times s in [1, 2); 1 when every peak is 0. Throws
// std::invalid_argument for a negative or non-finite peak. A power of two is exact in float.
// Ref: 0003 d10.
double radiance_scale(const std::vector<double>& peaks);

}  // namespace astroray::host
