// SPDX-License-Identifier: Apache-2.0
#include "host/light/light_builder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include "../../kernel/material/material_header.h"
#include "../../kernel/material/slot_resolve.h"
#include "host/material/material_store.h"
#include "host/spectral/blackbody.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/world.h"

namespace astroray::host {

ar::LightSamplerView LightTable::view() const {
  ar::LightSamplerView v;
  v.bins = bins.data();
  v.bin_light = bin_light.data();
  v.pmf = pmf.data();
  v.bin_count = uint32_t(bins.size());
  v.light_count = uint32_t(lights.size());
  return v;
}

uint64_t LightTable::bytes() const {
  return lights.size() * sizeof(ar::LightRecord) + importance.size() * sizeof(double) + bins.size() * sizeof(ar::AliasBin) +
         bin_light.size() * sizeof(uint32_t) + pmf.size() * sizeof(float) + light_index.size() * sizeof(uint32_t);
}

LightTable::~LightTable() {
  if (ledger && ledger_id != 0) ledger->remove(ledger_id);
}

double display_energy(const std::vector<EmissionTerm>& terms, const ar::ObserverView& obs) {
  double wsum = 0.0, e = 0.0;
  for (int i = 0; i < ar::kObsSamples; ++i)
    for (int c = 0; c < 3; ++c) wsum += double(obs.cmf[c * ar::kObsSamples + i]);
  if (!(wsum > 0.0)) return 0.0;
  for (const EmissionTerm& t : terms)
    for (int i = 0; i < ar::kObsSamples; i += ar::kLanes) {
      ar::Lambda lam;
      for (int k = 0; k < ar::kLanes; ++k) lam.nm[k] = ar::Real(ar::kObsLambdaMin + std::min(i + k, ar::kObsSamples - 1));
      const ar::Spec s = ar::resolve(t.color, lam, obs);
      for (int k = 0; k < ar::kLanes && i + k < ar::kObsSamples; ++k) {
        double w = 0.0;
        for (int c = 0; c < 3; ++c) w += double(obs.cmf[c * ar::kObsSamples + i + k]);
        e += double(t.strength) * double(s.v[k]) * w / wsum;
      }
    }
  return e;
}

bool same_terms(const LightInput& a, const LightInput& b) {
  // EmissionTerm is 28 bytes without padding (SpectrumRef 24 + float), so memcmp compares values.
  static_assert(sizeof(EmissionTerm) == sizeof(ar::SpectrumRef) + sizeof(float), "EmissionTerm has padding");
  return !a.terms.empty() && a.terms.size() == b.terms.size() &&
         std::memcmp(a.terms.data(), b.terms.data(), a.terms.size() * sizeof(EmissionTerm)) == 0;
}

std::shared_ptr<const LightTable> build_light_table(const std::vector<LightInput>& in, const ar::SceneSphere& scene,
                                                    const ar::ObserverView& obs, std::shared_ptr<Ledger> ledger,
                                                    const ChunkRunner& run) {
  auto t = std::make_shared<LightTable>();
  t->scene = scene;
  // Per light, independently of every other: the display energy (once per run of identical terms,
  // the same function of the same input) times light_power's geometric factor (1, pi A,
  // 4 pi^2 R^2) (D9, D10).
  std::vector<double> imps(in.size());
  const auto importance = [&](uint32_t first, uint32_t last) {
    double energy = 0.0;
    for (uint32_t i = first; i < last; ++i) {
      const LightInput& li = in[i];
      if (i == first || !same_terms(in[i - 1], li))
        energy = li.terms.empty() ? display_energy({{li.record.emission, li.record.strength}}, obs) : display_energy(li.terms, obs);
      imps[i] = energy * double(ar::light_power(li.record, scene, ar::Real(1)));
    }
  };
  if (run) run(uint32_t(in.size()), importance);
  else importance(0, uint32_t(in.size()));
  double sum = 0.0;
  for (size_t i = 0; i < in.size(); ++i) {
    const LightInput& li = in[i];
    const double imp = imps[i];
    if (!std::isfinite(imp) || imp < 0.0) throw std::runtime_error("light importance is not finite and non-negative");
    if (li.record.type == uint8_t(ar::LightType::WORLD_CONST)) {
      if (t->world_light != ar::kNoLight) throw std::runtime_error("light table: more than one world light");
      t->world_light = uint32_t(t->lights.size());
    }
    t->lights.push_back(li.record);
    t->importance.push_back(imp);
    sum += imp;
  }
  // The emissive-hit index from the records just stored (layout: light_sampler.h light_of_hit).
  const auto is_tri = [](const ar::LightRecord& r) { return r.type == uint8_t(ar::LightType::TRIANGLE); };
  std::vector<uint64_t> slice;  // per instance, one past its last emissive triangle
  for (const ar::LightRecord& r : t->lights) {
    if (!is_tri(r)) continue;
    const uint32_t inst = ar::tri_instance(r);
    if (inst >= slice.size()) slice.resize(size_t(inst) + 1, 0);
    slice[inst] = std::max<uint64_t>(slice[inst], uint64_t(ar::tri_index(r)) + 1);
  }
  uint64_t words = 1 + slice.size();
  for (uint64_t c : slice) words += c ? 1 + c : 0;
  if (words >= ar::kLightIndexInvalid) throw std::runtime_error("light table: the emissive-hit index exceeds 32-bit offsets");
  std::vector<uint32_t>& map = t->light_index;
  map.assign(1 + slice.size(), ar::kNoLight);
  map[0] = uint32_t(slice.size());
  map.reserve(size_t(words));
  for (size_t i = 0; i < slice.size(); ++i) {
    if (slice[i] == 0) continue;
    map[1 + i] = uint32_t(map.size());
    map.push_back(uint32_t(slice[i]));
    map.insert(map.end(), size_t(slice[i]), ar::kNoLight);
  }
  for (uint32_t i = 0; i < uint32_t(t->lights.size()); ++i) {
    const ar::LightRecord& r = t->lights[i];
    if (!is_tri(r)) continue;
    uint32_t& e = map[size_t(map[1 + ar::tri_instance(r)]) + 1 + ar::tri_index(r)];
    if (e != ar::kNoLight)
      throw std::runtime_error("light table: lights " + std::to_string(e) + " and " + std::to_string(i) + " are both triangle " +
                               std::to_string(ar::tri_index(r)) + " of instance " + std::to_string(ar::tri_instance(r)));
    e = i;
  }
  t->pmf.assign(in.size(), 0.0f);
  // pbrt-v4 AliasTable::AliasTable over the lights of positive importance only (D9).
  struct Outcome {
    double p_hat;
    uint32_t bin;
  };
  std::vector<double> p;
  for (uint32_t i = 0; i < uint32_t(in.size()); ++i)
    if (t->importance[i] > 0.0) {
      t->pmf[i] = float(t->importance[i] / sum);
      t->bin_light.push_back(i);
      p.push_back(t->importance[i] / sum);
    }
  const size_t n = p.size();
  t->bins.resize(n);
  std::vector<Outcome> under, over;
  for (uint32_t b = 0; b < uint32_t(n); ++b) (p[b] * double(n) < 1.0 ? under : over).push_back({p[b] * double(n), b});
  while (!under.empty() && !over.empty()) {
    const Outcome un = under.back(), ov = over.back();
    under.pop_back();
    over.pop_back();
    t->bins[un.bin] = {float(un.p_hat), ov.bin};
    const double excess = un.p_hat + ov.p_hat - 1.0;
    (excess < 1.0 ? under : over).push_back({excess, ov.bin});
  }
  for (const Outcome& o : over) t->bins[o.bin] = {1.0f, o.bin};
  for (const Outcome& o : under) t->bins[o.bin] = {1.0f, o.bin};
  if (ledger && t->bytes() != 0) t->ledger_id = ledger->add(Owner{}, LedgerCategory::TABLES, t->bytes()), t->ledger = ledger;
  return t;
}

uint32_t emission_sidedness(uint32_t sampling, const std::vector<EmissionTerm>& est) {
  switch (sampling) {
    case AR_EMISSION_SAMPLING_NONE: return 0;
    case AR_EMISSION_SAMPLING_FRONT: return kLightLit;
    case AR_EMISSION_SAMPLING_BACK: return kLightLit | ar::kLightBackFace;
    case AR_EMISSION_SAMPLING_FRONT_BACK: return kLightLit | ar::kLightTwoSided;
    case AR_EMISSION_SAMPLING_AUTO: {
      // Cycles Shader::estimate_emission: FRONT_BACK when reduce_max(|sum of estimates|) > 0.5,
      // else NONE. A CONSTANT term adds the same value to every channel, so it adds to the maximum;
      // an RGB term's largest component is scale / 2 (its unbounded scale is 2 max, rgb2spec.h
      // unbounded_coeffs). That is exact with at most one RGB term; with several, the sum of
      // their maxima bounds the maximum of the sum from above (the terms keep no RGB triple).
      // Cycles' 0.1 scale for auto-converted Emission nodes is not applied: the op graph does not
      // say which nodes the addon converted.
      double m = 0.0;
      for (const EmissionTerm& t : est) {
        double c;
        if (t.color.kind == uint8_t(ar::SpectrumKind::CONSTANT)) c = double(t.color.p[0]);
        else if (t.color.kind == uint8_t(ar::SpectrumKind::RGB)) c = double(t.color.p[3]) / 2.0;
        else if (t.color.kind == uint8_t(ar::SpectrumKind::BLACKBODY))
          // The band sum of the term against ybar, which is its luminance: p[1] is the scale times
          // 1 / Y(Planck), so Y = p[1] / blackbody_y_normalised(T) (0003 d11). Cycles' estimate for
          // a Blackbody is not reproduced; a spectral term is tested by its luminance.
          c = double(t.color.p[1]) / blackbody_y_normalised(double(t.color.p[0]));
        else throw std::invalid_argument("emission estimate SpectrumRef kind " + std::to_string(t.color.kind) + " is not CONSTANT, RGB or BLACKBODY");
        m += double(t.strength) * c;
      }
      return std::fabs(m) > 0.5 ? (kLightLit | ar::kLightTwoSided) : 0;
    }
    default: throw std::invalid_argument("emission_sampling " + std::to_string(sampling) + " is out of range");
  }
}

namespace {

ar::Real3 xform(const ar_xform& x, const float* p) {
  const float* m = x.m;
  return ar::Real3{ar::Real(m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]), ar::Real(m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7]),
                   ar::Real(m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11])};
}

template <class T>
void put(std::vector<uint8_t>& k, const T& v) {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
  k.insert(k.end(), b, b + sizeof(T));
}

}  // namespace

SceneLights gather_scene_lights(const InstanceTable* inst, const MeshStore& meshes, const MaterialArrays& mats,
                                const WorldOutput* world, bool collect) {
  SceneLights out;
  const bool world_lit = world != nullptr && world->strength > 0.0f;
  if (world) put(out.key, world->color), put(out.key, world->strength), put(out.key, world->desc.light_group);
  double lo[3] = {HUGE_VAL, HUGE_VAL, HUGE_VAL}, hi[3] = {-HUGE_VAL, -HUGE_VAL, -HUGE_VAL};
  const uint32_t n_inst = inst ? uint32_t(inst->instances.size()) : 0u;
  std::vector<uint32_t> side;
  std::vector<int64_t> header;
  for (uint32_t i = 0; i < n_inst; ++i) {
    const Instance& in = inst->instances[i];
    const ar_xform& x = inst->centre(i);
    std::shared_ptr<const MeshRecord> rec;
    if (collect || world_lit) {  // the scene sphere (the world's power and light-path target)
      rec = meshes.find(in.geom_key);
      for (int c = 0; rec && c < 8; ++c) {
        const float p[3] = {(c & 1) ? rec->bounds_max[0] : rec->bounds_min[0], (c & 2) ? rec->bounds_max[1] : rec->bounds_min[1],
                            (c & 4) ? rec->bounds_max[2] : rec->bounds_min[2]};
        const ar::Real3 w = xform(x, p);
        const double v[3] = {double(w.x), double(w.y), double(w.z)};
        for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], v[a]), hi[a] = std::max(hi[a], v[a]);
      }
    }
    // The default material (count 0) emits nothing.
    side.assign(in.material_count, 0u);
    header.assign(in.material_count, -1);
    bool lit = false;
    for (uint32_t s = 0; s < in.material_count; ++s) {
      const ar_key k = inst->material_pool[in.material_offset + s];
      const auto it = std::lower_bound(mats.keys.begin(), mats.keys.end(), k);
      if (it == mats.keys.end() || *it != k) continue;
      const size_t h = size_t(it - mats.keys.begin());
      if (mats.emission_estimates[h].empty()) continue;  // zero estimate: not a light
      header[s] = int64_t(h);
      side[s] = emission_sidedness(mats.emission_sampling[h], mats.emission_estimates[h]);
      lit = lit || side[s] != 0;
    }
    if (!lit) continue;
    if (!rec) rec = meshes.find(in.geom_key);
    if (!rec) continue;  // instances refuse unknown geometry; nothing to light
    out.meshes.push_back(rec);  // pinned while the table lives, so its address keys only this record
    put(out.key, i), put(out.key, rec.get()), put(out.key, x), put(out.key, in.visibility), put(out.key, in.light_group);
    for (uint32_t s = 0; s < in.material_count; ++s) {
      put(out.key, side[s]);
      if (side[s] == 0) continue;
      put(out.key, mats.headers[size_t(header[s])].flags);
      for (const EmissionTerm& t : mats.emission_estimates[size_t(header[s])]) put(out.key, t.color), put(out.key, t.strength);
    }
    if (!collect) continue;
    const MeshTopology& topo = *rec->topo;
    const size_t base = size_t(representative_step(topo.motion_steps)) * topo.vertex_count * 3;
    for (uint32_t t = 0; t < topo.triangle_count(); ++t) {
      const int32_t slot = topo.tri_material[t];
      if (slot < 0 || uint32_t(slot) >= in.material_count || side[size_t(slot)] == 0) continue;
      const size_t h = size_t(header[size_t(slot)]);
      ar::Real3 v[3];
      for (int k = 0; k < 3; ++k) v[k] = xform(x, &rec->positions[base + size_t(topo.corner_vert[topo.tri_corners[3 * t + k]]) * 3]);
      const std::vector<EmissionTerm>& est = mats.emission_estimates[h];
      const bool constant = (mats.headers[h].flags & ar::CONSTANT_EMISSION) != 0 && est.size() == 1;
      const uint16_t flags = uint16_t((side[size_t(slot)] & (ar::kLightTwoSided | ar::kLightBackFace)) |
                                      (constant ? 0u : ar::kLightEmissionProgram));
      LightInput li;
      li.record = ar::light_make_triangle(v[0], v[1], v[2], i, t, flags, ar::Real(constant ? est[0].strength : 0.0f),
                                          constant ? est[0].color : ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(0)));
      li.record.visibility = in.visibility;
      li.record.light_group = in.light_group;
      li.terms = est;
      out.lights.push_back(std::move(li));
    }
  }
  if (lo[0] <= hi[0]) {
    out.scene.center = ar::Real3{ar::Real(0.5 * (lo[0] + hi[0])), ar::Real(0.5 * (lo[1] + hi[1])), ar::Real(0.5 * (lo[2] + hi[2]))};
    const double dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    out.scene.radius = ar::Real(0.5 * std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  if (world_lit) put(out.key, out.scene.center), put(out.key, out.scene.radius);
  if (world && collect) {
    LightInput li;
    li.record = ar::light_make_world(ar::Real(world->strength), world->color);
    li.record.light_group = uint8_t(world->desc.light_group);
    out.lights.push_back(std::move(li));
  }
  return out;
}

}  // namespace astroray::host
