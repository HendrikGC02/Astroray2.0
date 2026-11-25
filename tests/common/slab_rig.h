// SPDX-License-Identifier: Apache-2.0
// The slab scene of tests/cpu/test_shade_volume.cpp and tests/gpu/test_shade_volume_agree.cpp: a closed
// box holding a medium (sigma = 0) with opacity 0 on its surface, between a pinhole camera and a
// view-filling emitter that does not scatter (base colour 0, no specular), world black, no bounces.
// Each camera sample crosses the box front (alpha pass-through), ends a segment inside on the back face
// (SHADE_VOLUME, then alpha pass-through) and adds the emitter's radiance once: one contribution per
// sample, so float addition order cannot differ between a render with the medium and one without it.
// A SlabSpec gives the box a medium with coefficients, a larger image, more samples and bounce limits,
// for the tracking cases, whose references (absorbing_slab_truth, scattering_slab_reference) are below.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md V1; 0004 D3 (opacity pass-through).
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <doctest.h>

#include "common/stat.h"
#include "host/data_resource.h"
#include "host/film/film_view.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/media.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "medium/medium_eval.h"
#include "sched/step_table.h"
#include "vm/ops.h"
#include "vm/ops/store_slot.h"

namespace slab {

using namespace astroray;
using namespace astroray::host;
using ar::Stage;


inline constexpr uint32_t kW = 6, kH = 4, kSpp = 2;
inline constexpr ar_key kEmitterMesh = 100, kBoxMesh = 101, kGlow = 1, kShell = 2, kMedium = 50;
inline constexpr float kGap = 0.05f, kThick = 0.5f;
// Every ray type sees both instances: a volume-scattered ray must find the box and the emitter.
inline constexpr uint32_t kAllRays = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE | AR_VISIBILITY_GLOSSY |
                                     AR_VISIBILITY_TRANSMISSION | AR_VISIBILITY_SHADOW | AR_VISIBILITY_VOLUME;

struct GraphBuilder {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  uint32_t add(ar::Op op, std::vector<float> fp = {}, std::vector<int32_t> ip = {}) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    n.i32_first = uint32_t(i.size()), n.i32_count = uint32_t(ip.size());
    f.insert(f.end(), fp.begin(), fp.end()), i.insert(i.end(), ip.begin(), ip.end());
    nodes.push_back(n);
    return uint32_t(nodes.size()) - 1u;
  }
  void link(uint32_t from, uint32_t to, uint32_t port) { links.push_back({from, 0u, to, port}); }
  void store(ar::RecordSlot slot, uint32_t value) { link(value, add(ar::Op::STORE_SLOT, {}, {int32_t(slot)}), 0u); }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size()), d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = i.data();
    return d;
  }
};

// What a rig renders: the medium (sigma = 0 unless given), the image size, samples per pixel and the
// bounce limits (none by default: the emitter is seen through the box only).
struct SlabSpec {
  bool with_medium = true;
  ar_medium_desc medium{};
  uint32_t width = kW, height = kH, spp = kSpp;
  uint32_t max_bounces = 0, volume_bounces = 0;
};

struct SlabRig {
  ContextState ctx;
  Scene scene;
  uint64_t generation = 0;
  SlabSpec spec;
  // `config` makes the scene's device (CPU: SceneConfig{2, {}, {}}).
  SlabRig(SceneConfig config, bool with_medium) : SlabRig(std::move(config), with_medium_spec(with_medium)) {}
  static SlabSpec with_medium_spec(bool with_medium) {
    SlabSpec s;
    s.with_medium = with_medium;
    return s;
  }
  SlabRig(SceneConfig config, const SlabSpec& sp) : scene(ctx, std::move(config)), spec(sp) {
    const bool with_medium = sp.with_medium;
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    commit([&](Edit& e) {
      mesh(e, kEmitterMesh, 4, {{-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, {-2, 2, 0}}, {{0, 1, 2, 3}});
      // A box of half-size 1.5 spanning z in [0, kThick], faces wound counter-clockwise from outside.
      const float h = 1.5f, z1 = kThick;
      mesh(e, kBoxMesh, 8,
           {{-h, -h, 0}, {h, -h, 0}, {h, h, 0}, {-h, h, 0}, {-h, -h, z1}, {h, -h, z1}, {h, h, z1}, {-h, h, z1}},
           {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}});
      GraphBuilder glow;
      const uint32_t colour = glow.add(ar::Op::CONST_F32X3, {1, 1, 1}), strength = glow.add(ar::Op::CONST_F32, {1});
      const uint32_t emit = glow.add(ar::Op::EMIT_LE);
      glow.link(colour, emit, 0u), glow.link(strength, emit, 1u);
      glow.store(ar::RecordSlot::base_color, glow.add(ar::Op::CONST_F32X3, {0, 0, 0}));
      glow.store(ar::RecordSlot::specular_weight, glow.add(ar::Op::CONST_F32, {0}));
      const ar_graph_desc gd = glow.desc();
      REQUIRE(scene.materials->set_graph(e, kGlow + 1000, &gd) == AR_OK);
      ar_material_desc m = {};
      m.surface_graph = kGlow + 1000;
      m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
      REQUIRE(scene.materials->set_material(e, kGlow, &m) == AR_OK);
      GraphBuilder shell;
      shell.store(ar::RecordSlot::geometry_opacity, shell.add(ar::Op::CONST_F32, {0}));  // the surface only encloses the medium
      const ar_graph_desc sd = shell.desc();
      REQUIRE(scene.materials->set_graph(e, kShell + 1000, &sd) == AR_OK);
      m.surface_graph = kShell + 1000;
      REQUIRE(scene.materials->set_material(e, kShell, &m) == AR_OK);
      GraphBuilder world;  // black: radiance 0
      const uint32_t wc = world.add(ar::Op::CONST_F32X3, {1, 1, 1}), ws = world.add(ar::Op::CONST_F32, {0});
      const uint32_t we = world.add(ar::Op::EMIT_LE);
      world.link(wc, we, 0u), world.link(ws, we, 1u);
      const ar_graph_desc wd = world.desc();
      const ar_status wst = scene.materials->set_graph(e, 1010, &wd);
      INFO(e.last_error());
      REQUIRE(wst == AR_OK);
      ar_world_desc w = {};
      w.surface_graph = 1010;
      REQUIRE(scene.materials->set_world(e, &w) == AR_OK);
      if (with_medium) {
        REQUIRE(set_medium(e, kMedium, &sp.medium, scene.materials->table()) == AR_OK);
      }
      ar_settings s{};
      s.samples = s.viewport_samples = sp.spp;
      s.max_bounces = sp.max_bounces;
      s.volume_bounces = sp.volume_bounces;
      s.transparent_bounces = 8;  // the box surface is crossed twice
      s.pixel_filter = AR_FILTER_BOX;
      s.pixel_filter_width = 1.0f;
      s.film_exposure = 1.0f;
      s.seed = 7;
      REQUIRE(e.set_settings(&s) == AR_OK);
    });
    commit([&](Edit& e) { instances(e, with_medium); });
  }
  // The emitter and the box, the box bearing the medium or not.
  void instances(Edit& e, bool with_medium) {
    std::vector<ar_key> geom{kEmitterMesh, kBoxMesh}, object{500, 501}, pool{kGlow, kShell}, medium{0, with_medium ? kMedium : 0};
    std::vector<ar_xform> xf(2);
    for (ar_xform& x : xf) x.m[0] = x.m[5] = x.m[10] = 1.0f;
    xf[1].m[11] = kGap;
    std::vector<uint32_t> zeros(2, 0), vis(2, kAllRays), moff{0, 1}, mcount{1, 1};
    std::vector<ar_rgba> colors(2, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(2, 0);
    ar_instances_desc d = {};
    d.instance_count = 2, d.motion_steps = 1, d.transform_count = 2, d.material_key_count = 2;
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = pool.data();
    d.medium_keys = medium.data();
    REQUIRE(set_instances(e, &d) == AR_OK);
  }
  // Takes the medium out: the box keeps its surface and no instance bears a medium.
  void drop_medium() {
    commit([&](Edit& e) {
      instances(e, false);
      REQUIRE(e.remove(AR_KIND_MEDIUM, kMedium) == AR_OK);
    });
  }
  template <class F>
  void commit(F&& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    REQUIRE(e->commit(&generation) == AR_OK);
    scene.materials->end_edit();
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.generations().failed() == 0);
  }
  // A mesh of quads over `verts` with outward face normals taken from each quad's winding.
  void mesh(Edit& e, ar_key key, uint32_t nverts, const std::vector<std::array<float, 3>>& verts,
            const std::vector<std::array<int32_t, 4>>& quads) {
    ar_mesh_desc d = {};
    d.key = key, d.vertex_count = nverts, d.corner_count = uint32_t(4 * quads.size()), d.face_count = uint32_t(quads.size());
    d.motion_steps = 1;
    ar_mesh_buffers b = {};
    REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
    for (uint32_t v = 0; v < nverts; ++v) std::memcpy(*b.positions + 3 * v, verts[v].data(), 12);
    for (uint32_t q = 0; q < quads.size(); ++q) {
      (*b.face_start)[q] = int32_t(4 * q);
      const auto& a = verts[quads[q][0]];
      const auto& p1 = verts[quads[q][1]];
      const auto& p2 = verts[quads[q][2]];
      const float e1[3] = {p1[0] - a[0], p1[1] - a[1], p1[2] - a[2]}, e2[3] = {p2[0] - a[0], p2[1] - a[1], p2[2] - a[2]};
      const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
      const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      for (uint32_t c = 0; c < 4; ++c) {
        (*b.corner_vert)[4 * q + c] = quads[q][c];
        for (int k = 0; k < 3; ++k) (*b.corner_normals)[3 * (4 * q + c) + k] = n[k] / len;
      }
    }
    REQUIRE(e.item_commit(key) == AR_OK);
  }
};

// The pinhole of every slab render: at z = 2 looking down -z; a pixel's ray leaves along
// (kSlabTanX ndc_x, kSlabTanY ndc_y, -1), ndc in [-1, 1]^2 (projection_inverse below).
inline constexpr double kSlabCamZ = 2.0, kSlabTanX = 0.5, kSlabTanY = 0.375;

inline ar_view_desc slab_view(uint32_t width = kW, uint32_t height = kH) {
  ar_view_desc v{};
  v.region_width = width;
  v.region_height = height;
  v.render_scale = 1.0f;
  v.camera.cam_to_world.m[0] = v.camera.cam_to_world.m[5] = v.camera.cam_to_world.m[10] = 1.0f;
  v.camera.cam_to_world.m[11] = 2.0f;  // on the +z axis, looking down -z at the box and the emitter
  const float p[16] = {0.5f, 0, 0, 0, 0, 0.375f, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
  for (int i = 0; i < 16; ++i) v.camera.projection_inverse[i] = p[i];
  v.camera.type = AR_CAMERA_PERSP;
  return v;
}

inline bool until(const std::function<bool()>& f, int timeout_ms = 60000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!f()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

struct Render {
  std::vector<float> combined;
  std::vector<double> variance;  // of the mean, per pixel and channel (Combined's MOMENT planes)
  uint64_t volume_items = 0, surface_items = 0;
  // Main-pool items of the last pass (refills and stage launches) and the path-state bytes the
  // scheduler counted for them: their ratio is path_state_bytes of the session's feature set.
  uint64_t main_items = 0, state_bytes = 0;
};

// A session on the rig's scene. converge() waits for the next converged epoch of the rig's latest
// generation and reads Combined and the last pass's counters; an edit to the rig between two calls
// starts a new epoch in the same session.
struct SlabSession {
  SlabRig& rig;
  Session session;
  uint64_t epoch = 0;
  static SessionOptions options(const SlabRig& r, Owner owner) {
    SessionOptions o;
    o.max_width = r.spec.width;
    o.max_height = r.spec.height;
    o.owner = owner;
    return o;
  }
  SlabSession(SlabRig& r, Device& device, Owner owner = Owner{}) : rig(r), session(r.scene, device, options(r, owner)) {
    session.set_view(slab_view(r.spec.width, r.spec.height));
    session.start();
  }
  Render converge() {
    REQUIRE(until([&] {
      const SessionProgress p = session.progress();
      return p.state == AR_STATE_CONVERGED && p.active_generation == rig.generation && p.epoch > epoch;
    }));
    epoch = session.progress().epoch;
    REQUIRE(session.failure().empty());
    Render r;
    const std::vector<PassDesc> passes = list_passes(session.layout());
    REQUIRE(passes.front().name == "Combined");
    session.read_film([&](const FilmReadContext& c) {
      FilmView view(c.device, c.film);
      r.combined.resize(pass_float_count(passes.front(), AR_QUANTITY_MEAN, view.width(), view.height()));
      read_pass(view, passes.front(), AR_QUANTITY_MEAN, uint32_t(r.combined.size()), r.combined.data());
      r.variance.assign(r.combined.size(), 0.0);
      for (uint32_t k = 0; k < 3; ++k) {
        const std::vector<double> v = view.variance_of_mean("display", k);
        for (size_t p = 0; p < v.size(); ++p) r.variance[3 * p + k] = v[p];
      }
    });
    const PassCounters c = session.pass_counters();
    r.volume_items = c.items[uint32_t(Stage::SHADE_VOLUME)];
    r.surface_items = c.items[uint32_t(Stage::SHADE_SURFACE)];
    r.main_items = c.camera_paths + c.light_paths;
    for (uint32_t s = 0; s < ar::kStageCount; ++s)
      if (ar::stage_pool(Stage(s)) == ar::Pool::MAIN) r.main_items += c.items[s];
    r.state_bytes = c.state_bytes_moved;
    return r;
  }
};

// One render of `rig`'s scene on `device`.
inline Render render_slab(SlabRig& rig, Device& device, Owner owner = Owner{}) {
  SlabSession s(rig, device, owner);
  return s.converge();
}

// The render with the sigma = 0 medium is bit for bit the render without it, in a picture that is not
// black, and the volume stage launched once per camera sample of the last pass (one per pixel).
inline void check_identity(const Render& with, const Render& without) {
  REQUIRE(with.combined.size() == size_t(kW) * kH * 3);
  REQUIRE(without.combined.size() == with.combined.size());
  // The picture is the emitter seen through the box: not black, so identical bits mean something.
  double sum = 0.0;
  for (float v : without.combined) sum += v;
  CHECK(sum / double(without.combined.size()) > 0.1);
  CHECK(std::memcmp(with.combined.data(), without.combined.data(), with.combined.size() * sizeof(float)) == 0);
  CHECK(with.volume_items == uint64_t(kW) * kH);
  CHECK(without.volume_items == 0u);
  // The stage hands each item on: the box front, its back and the emitter are one shade_surface item each.
  CHECK(with.surface_items == without.surface_items);
  CHECK(with.surface_items == 3u * kW * kH);
}

// ---- references for a slab holding a medium (0006 V1) ----

// Image mean and its standard error per display channel: pixels have equal area, so the image
// mean is the sensor average; the variance of the mean adds over pixels.
struct ImageMean {
  double mean[3], se[3];
};
inline ImageMean image_mean(const Render& r) {
  ImageMean m{};
  const size_t n = r.combined.size() / 3;
  REQUIRE(n > 0u);
  REQUIRE(r.variance.size() == r.combined.size());
  for (uint32_t k = 0; k < 3; ++k) {
    double s = 0.0, v = 0.0;
    for (size_t p = 0; p < n; ++p) s += r.combined[3 * p + k], v += r.variance[3 * p + k];
    m.mean[k] = s / double(n);
    m.se[k] = std::sqrt(v) / double(n);
  }
  return m;
}

// The unit direction of the camera ray through sensor point (nx, ny), ndc in [-1, 1].
inline void slab_dir(double nx, double ny, double d[3]) {
  const double x = kSlabTanX * nx, y = kSlabTanY * ny, l = std::sqrt(x * x + y * y + 1.0);
  d[0] = x / l, d[1] = y / l, d[2] = -1.0 / l;
}

// Image-mean display RGB of the rig's view when the box holds an absorbing medium (no scattering).
// Every ray of the view enters the box top and leaves its bottom (the view's widest ray is 0.98 m
// off axis at the bottom face, inside the 1.5 m half-size, and meets the 2 m emitter), so a ray at
// angle theta sees the white emitter (radiance 1: spectrum D65 with Y = 1, 0003 d12) through
// T(l) = exp(-sigma_a(l) kThick / cos theta). The display value is M_ws . sum over the observer's
// 1 nm grid of cmf(l) D65(l) T(l) / Y_n, as scenes/lib/truths.py forms a spectrum's colour, averaged
// over the sensor by an n x n midpoint rule (n = 64: the integrand is smooth, so the rule's error
// is far below the renders' standard errors). sigma_a(l) is the medium's own lift, its block
// through medium_eval at the grid wavelengths: the lift is 179's and tested there; what this
// reference measures is the tracking.
inline std::array<double, 3> absorbing_slab_truth(SlabRig& rig) {
  const std::shared_ptr<const Medium> med = rig.scene.medium(kMedium);
  REQUIRE(med);
  const MediumArrays arr = medium_arrays({med});
  DataBlob blob;
  REQUIRE(find_data("spectral/observer.bin", &blob));
  static ObserverTables obs;
  REQUIRE(parse_observer(blob.data, blob.size, &obs).empty());
  ar::SceneTables tables{};
  tables.rgb2spec = &rig.scene.materials->table();
  tables.observer.cmf = &obs.cmf[0][0];
  tables.observer.illum = obs.d65;
  const ar::MediumTables mt{arr.programs.data(), arr.blocks.data(), tables};
  ar::MediumDesc desc{};
  std::vector<double> sigma(kObserverSamples, 0.0);
  for (uint32_t i = 0; i < kObserverSamples; i += 4) {
    ar::Lambda lam;
    for (int j = 0; j < 4; ++j) lam.nm[j] = ar::Real(360.0 + std::min<uint32_t>(i + j, kObserverSamples - 1));
    ar::VmRegs regs;
    ar::MediumSample ms;
    REQUIRE(ar::medium_eval(mt, desc, ar::Real3{0, 0, 0}, lam, &regs, &ms) == ar::VmStatus::OK);
    for (uint32_t j = 0; j < 4 && i + j < kObserverSamples; ++j) sigma[i + j] = double(ms.sigma_a.v[j] + ms.sigma_s.v[j]);
  }
  constexpr int kGrid = 64;
  double xyz[3] = {0, 0, 0};
  for (int a = 0; a < kGrid; ++a)
    for (int b = 0; b < kGrid; ++b) {
      double d[3];
      slab_dir(-1.0 + (2.0 * a + 1.0) / kGrid, -1.0 + (2.0 * b + 1.0) / kGrid, d);
      const double path = double(kThick) / -d[2];
      for (uint32_t i = 0; i < kObserverSamples; ++i) {
        const double w = double(obs.d65[i]) * std::exp(-sigma[i] * path);
        for (int c = 0; c < 3; ++c) xyz[c] += double(obs.cmf[c][i]) * w;
      }
    }
  std::array<double, 3> rgb{};
  for (int c = 0; c < 3; ++c) {
    double v = 0.0;
    for (int k = 0; k < 3; ++k) v += obs.m_ws[3 * c + k] * xyz[k];
    rgb[c] = v / (obs.y_n * kGrid * kGrid);
  }
  return rgb;
}

// Image-mean display value (every channel alike) of the rig's view when the box holds a grey
// scattering medium: an independent analog random walk in double, not the engine's estimator.
// Per walk: a uniform sensor point gives the camera ray, which enters the box top; free paths are
// exponential in sigma_t; at a collision the weight is multiplied by the albedo (implicit capture,
// where the engine samples absorb or scatter) and a direction is drawn from Henyey-Greenstein about
// the propagation direction with Cycles' inversion (k = (1 - g^2) / (1 - g + 2 g u),
// cos = (1 + g^2 - k^2) / (2 g)); the walk leaving the box downward collects the emitter's radiance 1
// where it meets the 2 m square at z = 0, upward the black world. The collision that would exceed
// `max_scatter` scatter events ends the walk with weight 0, which is the engine's bounce limit:
// both have the same expectation. Returns the mean and its standard error over n walks.
// Ref: Henyey and Greenstein 1941; Cycles kernel/closure/volume_util.h (the inversion; Apache-2.0,
// read in C:\Astroray2.0\third_party\_cache\refs\cycles-ed69ef2c6bd3).
inline std::pair<double, double> scattering_slab_reference(double sigma_t, double albedo, double g, uint32_t max_scatter,
                                                           uint64_t n, uint64_t seed) {
  uint64_t st = seed;
  const auto uni = [&st]() {
    st += 0x9E3779B97F4A7C15ull;
    uint64_t z = st;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return double(z >> 11) * (1.0 / 9007199254740992.0);
  };
  const double lo[3] = {-1.5, -1.5, double(kGap)}, hi[3] = {1.5, 1.5, double(kGap + kThick)};
  double sum = 0.0, sum2 = 0.0;
  for (uint64_t i = 0; i < n; ++i) {
    double d[3];
    slab_dir(2.0 * uni() - 1.0, 2.0 * uni() - 1.0, d);
    const double t0 = (hi[2] - kSlabCamZ) / d[2];
    double p[3] = {d[0] * t0, d[1] * t0, hi[2]};
    double w = 1.0, value = 0.0;
    for (uint32_t k = 0;; ++k) {
      double t_exit = 1e300;
      for (int a = 0; a < 3; ++a)
        if (d[a] != 0.0) t_exit = std::min(t_exit, ((d[a] > 0.0 ? hi[a] : lo[a]) - p[a]) / d[a]);
      const double s = -std::log(1.0 - uni()) / sigma_t;
      if (s >= t_exit) {
        for (int a = 0; a < 3; ++a) p[a] += d[a] * t_exit;
        if (d[2] < 0.0) {
          const double te = -p[2] / d[2];
          if (std::fabs(p[0] + d[0] * te) <= 2.0 && std::fabs(p[1] + d[1] * te) <= 2.0) value = w;
        }
        break;
      }
      for (int a = 0; a < 3; ++a) p[a] += d[a] * s;
      if (k >= max_scatter) break;
      w *= albedo;
      const double kk = (1.0 - g * g) / (1.0 - g + 2.0 * g * uni());
      const double c = (1.0 + g * g - kk * kk) / (2.0 * g), sn = std::sqrt(std::max(0.0, 1.0 - c * c));
      const double phi = 2.0 * 3.14159265358979323846 * uni();
      // An orthonormal frame about d (Duff et al. 2017), then the new direction in it.
      const double sg = d[2] >= 0.0 ? 1.0 : -1.0, aa = -1.0 / (sg + d[2]), bb = d[0] * d[1] * aa;
      const double e1[3] = {1.0 + sg * d[0] * d[0] * aa, sg * bb, -sg * d[0]}, e2[3] = {bb, sg + d[1] * d[1] * aa, -d[1]};
      double nd[3];
      for (int a = 0; a < 3; ++a) nd[a] = sn * std::cos(phi) * e1[a] + sn * std::sin(phi) * e2[a] + c * d[a];
      const double l = std::sqrt(nd[0] * nd[0] + nd[1] * nd[1] + nd[2] * nd[2]);
      for (int a = 0; a < 3; ++a) d[a] = nd[a] / l;
    }
    sum += value, sum2 += value * value;
  }
  const double mean = sum / double(n);
  return {mean, std::sqrt(std::max(0.0, sum2 / double(n) - mean * mean) / double(n))};
}

// The tracking cases, shared by the CPU and CUDA tests: one SlabSpec and one check each. Every channel
// is one member of the gate family, tested two-sided at its Sidak threshold (0009 d15).
// Chromatic absorbing slab: 0006 V1's coefficients (scenes/v1_slab/meta.toml), 64 x 48 at 64 spp.
inline SlabSpec chromatic_spec() {
  SlabSpec s;
  const float sa[3] = {0.5f, 1.0f, 2.0f};
  for (int k = 0; k < 3; ++k) s.medium.sigma_a[k] = sa[k];
  s.width = 64, s.height = 48, s.spp = 64;
  return s;
}
// Grey scattering slab: sigma_t = 2 / m (optical depth 1 along the axis), albedo 0.8, g = 0.5, up to
// 128 scatter events (0.8^128 ~ 4e-13 of the weight is cut, identically in the reference).
inline constexpr double kScatterSigmaT = 2.0, kScatterAlbedo = 0.8, kScatterG = 0.5;
inline constexpr uint32_t kScatterMax = 128;
inline SlabSpec scattering_spec() {
  SlabSpec s;
  for (int k = 0; k < 3; ++k) {
    s.medium.sigma_a[k] = float(kScatterSigmaT * (1.0 - kScatterAlbedo));
    s.medium.sigma_s[k] = float(kScatterSigmaT * kScatterAlbedo);
  }
  s.medium.g = float(kScatterG);
  s.width = 64, s.height = 48, s.spp = 64;
  s.max_bounces = s.volume_bounces = kScatterMax;
  return s;
}

inline void check_member(const char* what, int channel, double got, double se, double truth) {
  astroray::stat::Family& fam = astroray::stat::gate_family();
  fam.note_check();
  const double z = se > 0.0 ? (got - truth) / se : HUGE_VAL;
  std::printf("%s channel %d: mean %.6f truth %.6f se %.2e z %+.2f (limit %.2f)\n", what, channel, got, truth, se, z, fam.z());
  CHECK_MESSAGE(std::fabs(z) <= fam.z(), what << " channel " << channel << ": z " << z);
}

inline void check_chromatic(SlabRig& rig, const Render& r, const char* what) {
  const ImageMean m = image_mean(r);
  const std::array<double, 3> truth = absorbing_slab_truth(rig);
  for (int k = 0; k < 3; ++k) check_member(what, k, m.mean[k], m.se[k], truth[k]);
  // The medium is chromatic: the channels' transmittances differ well beyond their errors.
  CHECK(truth[0] > truth[2] + 0.1);
}

inline void check_scattering(const Render& r, const char* what) {
  const ImageMean m = image_mean(r);
  // 2^21 walks: the reference's standard error is a fraction of the render's.
  const auto ref = scattering_slab_reference(kScatterSigmaT, kScatterAlbedo, kScatterG, kScatterMax, uint64_t(1) << 21,
                                             astroray::stat::seed_for(what));
  for (int k = 0; k < 3; ++k)
    check_member(what, k, m.mean[k], std::sqrt(m.se[k] * m.se[k] + ref.second * ref.second), ref.first);
}

}  // namespace slab
