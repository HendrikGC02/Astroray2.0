// SPDX-License-Identifier: Apache-2.0
#include "host/light/lamps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "host/scene/edit.h"
#include "host/session/test_hooks.h"
#include "host/spectral/blackbody.h"

namespace astroray::host {

namespace {

constexpr double kPi = 3.14159265358979323846;
// planck.h's float domain is [3 K, 1e9 K] (0003 d10, d25); a lamp above it could overflow a float.
constexpr double kBlackbodyTMax = 1.0e9;

ar::Real3 r3(double x, double y, double z) { return ar::Real3{ar::Real(x), ar::Real(y), ar::Real(z)}; }

double col_len(const ar_xform& x, int c) {
  const double a = x.m[c], b = x.m[4 + c], d = x.m[8 + c];
  return std::sqrt(a * a + b * b + d * d);
}

ar::Real3 col_dir(const ar_xform& x, int c) {
  const double len = col_len(x, c);
  if (!(len > 0.0)) throw std::invalid_argument("light transform has a zero-length axis");
  return r3(x.m[c] / len, x.m[4 + c] / len, x.m[8 + c] / len);
}

}  // namespace

std::shared_ptr<const Lamp> build_lamp(ar_key key, const ar_light_desc& d, const ar::Rgb2SpecTable& table) {
  auto lamp = std::make_shared<Lamp>();
  lamp->key = key;
  lamp->xf = d.xf;
  for (float f : d.xf.m)
    if (!std::isfinite(f)) throw std::invalid_argument("light transform is not finite");
  if (d.type != AR_LIGHT_POINT && d.type != AR_LIGHT_AREA_RECT && d.type != AR_LIGHT_AREA_ELLIPSE)
    throw std::invalid_argument("light type " + std::to_string(d.type) + " is not supported in this build");
  if (d.visibility & ~uint32_t(kLampRayVisibility | AR_VISIBILITY_SHADOW | AR_VISIBILITY_VOLUME))
    throw std::invalid_argument("light visibility has bits outside AR_VISIBILITY_*");
  if (d.light_group > 255) throw std::invalid_argument("light_group above 255");
  if (!std::isfinite(d.power) || d.power < 0.0f) throw std::invalid_argument("light power must be finite and not negative");
  if (!std::isfinite(d.size_x) || d.size_x < 0.0f || !std::isfinite(d.size_y) || d.size_y < 0.0f)
    throw std::invalid_argument("light size must be finite and not negative");
  auto approx = [&](const std::string& s) { lamp->approximations.push_back(s); };
  // A descriptor the engine cannot read is refused; one it can read but not honour is approximated
  // and raised (CLAUDE.md rule 5).
  if (d.color.kind != AR_SPECTRUM_RGB && d.color.kind != AR_SPECTRUM_BLACKBODY && d.color.kind != AR_SPECTRUM_TABLE)
    throw std::invalid_argument("light color kind " + std::to_string(d.color.kind) + " is out of range");
  for (float c : d.color.rgb)
    if (!std::isfinite(c)) throw std::invalid_argument("light color is not finite");
  if (d.color.table_count != 0 && d.color.kind != AR_SPECTRUM_TABLE)
    throw std::invalid_argument("light color table_count must be 0 unless the kind is TABLE (0007 appendix ar_spectrum_ref)");
  if (d.color.kind == AR_SPECTRUM_TABLE && d.color.table_count != 0 && (!d.color.table_wavelength_nm || !d.color.table_value))
    throw std::invalid_argument("light color table pointer is null with a nonzero count");
  if (d.cast_shadow == 0) approx("light cast_shadow off renders with shadows");

  // Colour: RGB packs to an ILLUMINANT ref. BLACKBODY packs {T, scale x 1 / Y(Planck(T))} through
  // the function the material compiler uses (0003 d11); its rgb field is ignored. TABLE is M4 and
  // renders as the colour's RGB when given, else white.
  ar::SpectrumRef color;
  double bb_t = 0.0, bb_scale = 0.0, bb_y_norm = 1.0;  // the BLACKBODY colour's inputs, re-checked against the strength
  if (d.color.kind == AR_SPECTRUM_BLACKBODY) {
    const double t = double(d.color.temperature), scale = double(d.color.scale);
    if (!std::isfinite(d.color.temperature) || !(t > 0.0))
      throw std::invalid_argument("light color temperature must be finite and positive");
    if (t > kBlackbodyTMax) throw std::invalid_argument("light color temperature is above 1e9 K, planck.h's float domain");
    if (!std::isfinite(d.color.scale) || d.color.scale < 0.0f)
      throw std::invalid_argument("light color scale must be finite and not negative");
    // At a few kelvin Planck underflows over the whole observer support, so Y is 0 and no scale
    // can make the lamp visible (0003 d11). Only that case skips the normalisation, loudly.
    double y_norm = 1.0;
    const double y = blackbody_visible_y(t);
    if (y > 0.0 && std::isfinite(1.0 / y)) {
      y_norm = 1.0 / y;
    } else {
      char kelvin[32];
      std::snprintf(kelvin, sizeof kelvin, "%g", double(d.color.temperature));
      approx("light " + std::to_string(key) + " BLACKBODY colour at " + kelvin + " K: no visible radiance, not normalised");
    }
    bb_t = t, bb_scale = scale, bb_y_norm = y_norm;
    try {
      color = make_blackbody_ref(t, scale, y_norm, 1.0);
    } catch (const std::invalid_argument& e) {
      throw std::invalid_argument(std::string("light color: ") + e.what());
    }
  } else {
    ar::Rgb rgb{{ar::Real(d.color.rgb[0]), ar::Real(d.color.rgb[1]), ar::Real(d.color.rgb[2])}};
    if (d.color.kind != AR_SPECTRUM_RGB) {
      const bool given = d.color.rgb[0] != 0.0f || d.color.rgb[1] != 0.0f || d.color.rgb[2] != 0.0f;
      if (!given) rgb = ar::Rgb{{ar::Real(1), ar::Real(1), ar::Real(1)}};
      approx(std::string("light color TABLE renders as ") + (given ? "its RGB" : "white"));
    }
    uint32_t flags = 0;
    ar::make_rgb_ref(table, rgb, ar::SpectrumSemantic::ILLUMINANT, &color, &flags);
    if (flags & ar::kRgbNegativeClamped) approx("light color clamped (a negative component became 0)");
  }
  if (d.graph != 0) approx("light node graph " + std::to_string(d.graph) + " ignored");

  const ar::Real3 pos = r3(d.xf.m[3], d.xf.m[7], d.xf.m[11]);
  lamp->centre[0] = double(pos.x), lamp->centre[1] = double(pos.y), lamp->centre[2] = double(pos.z);
  double cycles_area = 0.0;  // Cycles' light area, what `normalize` divides by
  if (d.type == AR_LIGHT_POINT) {
    // Cycles uses the radius as given; the object's scale does not enter (light.cpp PointLight).
    const double r = d.size_x;
    cycles_area = r == 0.0 ? 4.0 : 4.0 * kPi * r * r;
    lamp->record = ar::light_make(r == 0.0 ? ar::LightType::POINT : ar::LightType::SPHERE, pos, ar::Real(r), ar::Real(1), color);
    lamp->bound_radius = r;
  } else {
    const double w = double(d.size_x) * col_len(d.xf, 0), h = double(d.size_y) * col_len(d.xf, 1);
    const ar::Real3 ax = col_dir(d.xf, 0), ay = col_dir(d.xf, 1);
    // Spread is in radians like Blender's RNA (pi = a full hemisphere); narrower is M4.
    if (d.spread < float(kPi) - 1e-4f) approx("light spread " + std::to_string(d.spread) + " rad renders as pi (M4)");
    // Cycles aims the lamp along -column 2; the record's front is -cross(col 0, col 1), which
    // differs for a mirrored transform. D6 fixes no such case, so it is raised, not followed.
    {
      const ar::Real3 z = ar::cross(ax, ay);
      const double c2[3] = {d.xf.m[2], d.xf.m[6], d.xf.m[10]};
      if (double(z.x) * c2[0] + double(z.y) * c2[1] + double(z.z) * c2[2] < 0.0)
        approx("light transform is mirrored; it faces the opposite way to Cycles' -column 2");
    }
    if (d.type == AR_LIGHT_AREA_RECT) {
      cycles_area = w * h;
      lamp->record = ar::light_make_area(ar::LightType::AREA_RECT, pos, ax, ay, ar::Real(w), ar::Real(h), ar::Real(1), color);
      lamp->bound_radius = 0.5 * std::sqrt(w * w + h * h);
    } else {
      // The descriptor's sizes are diameters; the disk record takes a radius.
      cycles_area = w * h * kPi / 4.0;
      double diameter = w;
      if (std::fabs(w - h) > 1e-6 * std::max(w, h)) {
        // The disk of equal area, so a normalised lamp keeps its power.
        diameter = std::sqrt(w * h);
        approx("light ellipse axes " + std::to_string(w) + " x " + std::to_string(h) + " render as a disk of diameter " +
               std::to_string(diameter));
      }
      lamp->record = ar::light_make_area(ar::LightType::AREA_DISK, pos, ax, ay, ar::Real(0.5 * diameter), ar::Real(0), ar::Real(1), color);
      lamp->bound_radius = 0.5 * std::max(w, h);
    }
  }
  lamp->record.strength = float(double(d.power) * (d.normalize ? 1.0 : cycles_area));
  // The same float-range check the compiler runs with its emission strength: the lamp's brightest
  // value is its strength x the spectrum's peak. The record's own radius check covers the geometry.
  if (d.color.kind == AR_SPECTRUM_BLACKBODY) {
    try {
      make_blackbody_ref(bb_t, bb_scale, bb_y_norm, double(lamp->record.strength));
    } catch (const std::invalid_argument& e) {
      throw std::invalid_argument(std::string("light power and color: ") + e.what());
    }
  }
  lamp->record.light_group = uint8_t(d.light_group);
  lamp->record.visibility = d.visibility;
  lamp->visibility = d.visibility & kLampRayVisibility;  // the instance never carries the shadow bit
  if (const char* why = ar::light_problem(lamp->record)) throw std::invalid_argument(why);
  return lamp;
}

ar_status set_light(Edit& edit, ar_key key, const ar_light_desc* desc, const ar::Rgb2SpecTable& table) {
  if (desc == nullptr) return edit.fail("invalid argument: ar_edit_set_light desc is null");
  const std::shared_ptr<const Lamp> lamp = build_lamp(key, *desc, table);  // throws invalid_argument
  auto item = std::make_shared<EditItem>();
  item->key = key;
  item->kind = AR_KIND_LIGHT;
  item->needs_device = false;
  item->job = [] {};
  Scene* scene = &edit.scene();
  // Raised from the activation callback, after the replaced subject's old entries are cleared.
  item->on_active = [scene, lamp] {
    scene->publish_lamp(lamp);
    for (const std::string& a : lamp->approximations) {
      scene->ctx().log.line("WARNING", "light " + std::to_string(lamp->key) + ": " + a);
      scene->report().raise(AR_REPORT_NODE_APPROXIMATED, lamp->key, a, 1, nullptr, 0);
    }
  };
  if (ar_status st = edit.stage(item)) return st;
  return edit.item_commit(key);
}

ar::SceneSphere enclose(const ar::SceneSphere& s, const Lamp& lamp) {
  ar::SceneSphere out;
  const double lc[3] = {lamp.centre[0], lamp.centre[1], lamp.centre[2]};
  const double lr = lamp.bound_radius;
  auto lamp_sphere = [&] {
    out.center = r3(lc[0], lc[1], lc[2]);
    out.radius = ar::Real(lr);
    return out;
  };
  if (!(double(s.radius) > 0.0)) return lamp_sphere();
  const double sc[3] = {double(s.center.x), double(s.center.y), double(s.center.z)};
  const double sr = double(s.radius);
  const double dx = lc[0] - sc[0], dy = lc[1] - sc[1], dz = lc[2] - sc[2];
  const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (d + lr <= sr) return s;
  if (d + sr <= lr) return lamp_sphere();
  const double r = 0.5 * (d + sr + lr), t = (r - sr) / d;  // the centre moves from s toward the lamp
  out.center = r3(sc[0] + dx * t, sc[1] + dy * t, sc[2] + dz * t);
  out.radius = ar::Real(r);
  return out;
}

double emitter_peak(const LightInput& light, const ar::ObserverView& obs) {
  std::vector<EmissionTerm> own;
  if (light.terms.empty()) own.push_back({light.record.emission, light.record.strength});
  const std::vector<EmissionTerm>& terms = light.terms.empty() ? own : light.terms;
  // 0003 d10 asks for the emitter's radiance; a lamp's record holds power P, so divide by D6's
  // geometric factor: intensity P / (4 pi) for a delta point, radiance P / (4 pi^2 r^2) for a
  // sphere and P / (pi A) for an area lamp. Triangles and the world already hold radiance.
  double geometry = 1.0;
  if (light.terms.empty()) {
    const ar::LightRecord& r = light.record;
    switch (ar::LightType(r.type)) {
      case ar::LightType::POINT: geometry = 4.0 * kPi; break;
      case ar::LightType::SPHERE: geometry = 4.0 * kPi * kPi * double(r.size[0]) * double(r.size[0]); break;
      case ar::LightType::AREA_RECT: geometry = kPi * double(r.size[0]) * double(r.size[1]); break;
      case ar::LightType::AREA_DISK: geometry = kPi * kPi * double(r.size[0]) * double(r.size[0]); break;
      default: break;
    }
  }
  double peak = 0.0;
  for (int i = 0; i < ar::kObsSamples; i += ar::kLanes) {
    ar::Lambda lam;
    for (int k = 0; k < ar::kLanes; ++k) lam.nm[k] = ar::Real(ar::kObsLambdaMin + std::min(i + k, ar::kObsSamples - 1));
    double sum[ar::kLanes] = {};
    for (const EmissionTerm& t : terms) {
      const ar::Spec s = ar::resolve(t.color, lam, obs, nullptr);
      for (int k = 0; k < ar::kLanes; ++k) sum[k] += double(t.strength) * double(s.v[k]);
    }
    for (int k = 0; k < ar::kLanes && i + k < ar::kObsSamples; ++k) peak = std::max(peak, sum[k]);
  }
  return peak / geometry;
}

double radiance_scale(const std::vector<double>& peaks) {
  double m = 0.0;
  for (double p : peaks) {
    if (!std::isfinite(p) || p < 0.0) throw std::invalid_argument("emitter radiance peak is not finite and non-negative");
    m = std::max(m, p);
  }
  // ASTRORAY_TEST_RADIANCE_SCALE=1 (gate builds): s = 1, so a render compares bit for bit with the
  // automatic s (0003 Verification check 15). Read here, where every light table takes its s.
  if (m == 0.0 || read_test_hooks().unit_radiance_scale) return 1.0;
  int e = 0;
  std::frexp(m, &e);  // m = f 2^e with f in [0.5, 1), so m 2^(1 - e) is in [1, 2)
  return std::ldexp(1.0, 1 - e);
}

}  // namespace astroray::host
