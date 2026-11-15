// SPDX-License-Identifier: Apache-2.0
#include "host/light/lamps.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "host/scene/edit.h"

namespace astroray::host {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kLampVisibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE | AR_VISIBILITY_GLOSSY | AR_VISIBILITY_TRANSMISSION;

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
  if (d.visibility & ~uint32_t(kLampVisibility | AR_VISIBILITY_SHADOW | AR_VISIBILITY_VOLUME))
    throw std::invalid_argument("light visibility has bits outside AR_VISIBILITY_*");
  if (d.light_group > 255) throw std::invalid_argument("light_group above 255");
  if (!std::isfinite(d.power) || d.power < 0.0f) throw std::invalid_argument("light power must be finite and not negative");
  if (!std::isfinite(d.size_x) || d.size_x < 0.0f || !std::isfinite(d.size_y) || d.size_y < 0.0f)
    throw std::invalid_argument("light size must be finite and not negative");
  auto approx = [&](const std::string& s) { lamp->approximations.push_back(s); };

  // Colour: RGB packs to an ILLUMINANT ref; the other kinds are M1b / M4 and render as the
  // colour's RGB when given, else white.
  ar::Rgb rgb{{ar::Real(d.color.rgb[0]), ar::Real(d.color.rgb[1]), ar::Real(d.color.rgb[2])}};
  if (d.color.kind != AR_SPECTRUM_RGB) {
    const bool given = d.color.rgb[0] != 0.0f || d.color.rgb[1] != 0.0f || d.color.rgb[2] != 0.0f;
    if (!given) rgb = ar::Rgb{{ar::Real(1), ar::Real(1), ar::Real(1)}};
    const std::string kind = d.color.kind == AR_SPECTRUM_BLACKBODY ? "BLACKBODY"
                             : d.color.kind == AR_SPECTRUM_TABLE   ? "TABLE"
                                                                   : "kind " + std::to_string(d.color.kind);
    approx("light color " + kind + " renders as " + (given ? "its RGB" : "white"));
  }
  if (d.graph != 0) approx("light node graph " + std::to_string(d.graph) + " ignored");
  ar::SpectrumRef color;
  uint32_t flags = 0;
  ar::make_rgb_ref(table, rgb, ar::SpectrumSemantic::ILLUMINANT, &color, &flags);

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
    if (d.spread < 180.0f) approx("light spread " + std::to_string(d.spread) + " degrees renders as 180 (M4)");
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
  lamp->record.light_group = uint8_t(d.light_group);
  lamp->record.visibility = d.visibility;
  lamp->visibility = d.visibility & kLampVisibility;  // the instance never carries the shadow bit
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
  double peak = 0.0;
  for (int i = 0; i < ar::kObsSamples; i += ar::kLanes) {
    ar::Lambda lam;
    for (int k = 0; k < ar::kLanes; ++k) lam.nm[k] = ar::Real(ar::kObsLambdaMin + std::min(i + k, ar::kObsSamples - 1));
    double sum[ar::kLanes] = {};
    for (const EmissionTerm& t : terms) {
      const ar::Spec s = ar::resolve(t.color, lam, obs);
      for (int k = 0; k < ar::kLanes; ++k) sum[k] += double(t.strength) * double(s.v[k]);
    }
    for (int k = 0; k < ar::kLanes && i + k < ar::kObsSamples; ++k) peak = std::max(peak, sum[k]);
  }
  return peak;
}

double radiance_scale(const std::vector<double>& peaks) {
  double m = 0.0;
  for (double p : peaks) {
    if (!std::isfinite(p) || p < 0.0) throw std::invalid_argument("emitter radiance peak is not finite and non-negative");
    m = std::max(m, p);
  }
  if (m == 0.0) return 1.0;
  int e = 0;
  std::frexp(m, &e);  // m = f 2^e with f in [0.5, 1), so m 2^(1 - e) is in [1, 2)
  return std::ldexp(1.0, 1 - e);
}

}  // namespace astroray::host
