// SPDX-License-Identifier: Apache-2.0
// The slab scene of tests/cpu/test_shade_volume.cpp and tests/gpu/test_shade_volume_agree.cpp: a closed
// box holding a medium (sigma = 0) with opacity 0 on its surface, between a pinhole camera and a
// view-filling emitter that does not scatter (base colour 0, no specular), world black, no bounces.
// Each camera sample crosses the box front (alpha pass-through), ends a segment inside on the back face
// (SHADE_VOLUME, then alpha pass-through) and adds the emitter's radiance once: one contribution per
// sample, so float addition order cannot differ between a render with the medium and one without it.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md V1; 0004 D3 (opacity pass-through).
#pragma once
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include <doctest.h>

#include "host/film/film_view.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/media.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "vm/ops.h"
#include "vm/ops/store_slot.h"

namespace slab {

using namespace astroray;
using namespace astroray::host;
using ar::Stage;


inline constexpr uint32_t kW = 6, kH = 4, kSpp = 2;
inline constexpr ar_key kEmitterMesh = 100, kBoxMesh = 101, kGlow = 1, kShell = 2, kMedium = 50;
inline constexpr float kGap = 0.05f, kThick = 0.5f;

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

struct SlabRig {
  ContextState ctx;
  Scene scene;
  uint64_t generation = 0;
  // `config` makes the scene's device (CPU: SceneConfig{2, {}, {}}).
  SlabRig(SceneConfig config, bool with_medium) : scene(ctx, std::move(config)) {
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
        const ar_medium_desc md{};  // sigma_a = sigma_s = 0
        REQUIRE(set_medium(e, kMedium, &md, scene.materials->table()) == AR_OK);
      }
      ar_settings s{};
      s.samples = s.viewport_samples = kSpp;
      s.max_bounces = 0;
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
    std::vector<uint32_t> zeros(2, 0), vis(2, AR_VISIBILITY_CAMERA), moff{0, 1}, mcount{1, 1};
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

inline ar_view_desc slab_view() {
  ar_view_desc v{};
  v.region_width = kW;
  v.region_height = kH;
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
  uint64_t volume_items = 0, surface_items = 0;
};

// A session on the rig's scene. converge() waits for the next converged epoch of the rig's latest
// generation and reads Combined and the last pass's counters; an edit to the rig between two calls
// starts a new epoch in the same session.
struct SlabSession {
  SlabRig& rig;
  Session session;
  uint64_t epoch = 0;
  static SessionOptions options(Owner owner) {
    SessionOptions o;
    o.max_width = kW;
    o.max_height = kH;
    o.owner = owner;
    return o;
  }
  SlabSession(SlabRig& r, Device& device, Owner owner = Owner{}) : rig(r), session(r.scene, device, options(owner)) {
    session.set_view(slab_view());
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
    });
    const PassCounters c = session.pass_counters();
    r.volume_items = c.items[uint32_t(Stage::SHADE_VOLUME)];
    r.surface_items = c.items[uint32_t(Stage::SHADE_SURFACE)];
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

}  // namespace slab
