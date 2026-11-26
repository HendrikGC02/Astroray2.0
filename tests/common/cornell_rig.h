// SPDX-License-Identifier: Apache-2.0
// A small Cornell-like box for the final-denoise tests (tests/cpu/test_session_denoise.cpp,
// tests/gpu/test_session_denoise_gpu.cpp): a 2 x 2 x 2 room open towards a pinhole camera, white
// back wall, floor and ceiling, red left and green right walls, two white blocks, lit only by an
// emissive quad under the ceiling (emission sampling AUTO, so NEE samples it); the world is black.
// Every surface is Lambertian (base colour, specular_weight 0), 64 x 64 at 16 spp. The reference is
// one 4096-spp render of the same scene on the CPU, cached under out/refs/session_denoise/ by a hash
// of the scene description and of every file under src/, as tools/scenes.py caches recordings by
// their sources, so a build tree renders it once and both devices' cases read it.
// Ref: docs/decisions/0008-film-outputs-denoise.md d13a, d16, V11 (relMSE against a reference).
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <doctest.h>

#include "common/noisy_fixture.h"
#include "common/slab_rig.h"

namespace cornell {

using namespace astroray;
using namespace astroray::host;
using slab::GraphBuilder;

inline constexpr uint32_t kSize = 64, kSpp = 16, kRefSpp = 4096;
inline constexpr ar_key kWhiteMesh = 100, kRedMesh = 101, kGreenMesh = 102, kBlockMesh = 103, kLampMesh = 104;
inline constexpr ar_key kWhite = 1, kRed = 2, kGreen = 3, kLamp = 4;
inline constexpr float kLampStrength = 20.0f;
// The camera at z = kCamZ looks down -z; tan of the half field of view in x and y, so the room's open
// front (z = 0, 2 wide) just fills the square image.
inline constexpr float kCamZ = 2.8f, kTan = 0.36f;

using V3 = std::array<float, 3>;
using Quad = std::array<int32_t, 4>;

// Everything the image depends on besides the engine's code, for the reference's cache key.
inline std::string description() {
  char s[256];
  std::snprintf(s, sizeof s, "cornell-v1 size=%u spp=%u lamp=%g cam=%g tan=%g seed=7 bounces=6 box-filter", kSize,
                kRefSpp, double(kLampStrength), double(kCamZ), double(kTan));
  return s;
}

struct CornellRig {
  ContextState ctx;
  Scene scene;
  uint64_t generation = 0;

  CornellRig(SceneConfig config, uint32_t spp, const std::vector<std::string>& passes = {})
      : scene(ctx, std::move(config)) {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    commit([&](Edit& e) {
      // The room x, y in [-1, 1], z in [-2, 0]; each quad wound counter-clockwise seen from inside.
      const std::vector<V3> room = {{-1, -1, -2}, {1, -1, -2}, {1, 1, -2}, {-1, 1, -2},
                                    {-1, -1, 0},  {1, -1, 0},  {1, 1, 0},  {-1, 1, 0}};
      mesh(e, kWhiteMesh, room, {{0, 1, 2, 3}, {4, 5, 1, 0}, {3, 2, 6, 7}});  // back, floor, ceiling
      mesh(e, kRedMesh, room, {{4, 0, 3, 7}});                               // left
      mesh(e, kGreenMesh, room, {{1, 5, 6, 2}});                             // right
      std::vector<V3> blocks;
      std::vector<Quad> faces;
      box(-0.6f, -1.0f, -1.5f, -0.1f, 0.2f, -1.0f, &blocks, &faces);   // the tall block
      box(0.1f, -1.0f, -0.9f, 0.6f, -0.4f, -0.4f, &blocks, &faces);    // the short block
      mesh(e, kBlockMesh, blocks, faces);
      mesh(e, kLampMesh, {{-0.3f, 0.98f, -1.3f}, {0.3f, 0.98f, -1.3f}, {0.3f, 0.98f, -0.7f}, {-0.3f, 0.98f, -0.7f}},
           {{0, 1, 2, 3}});  // facing down
      diffuse(e, kWhite, {0.75f, 0.75f, 0.75f});
      diffuse(e, kRed, {0.63f, 0.065f, 0.05f});
      diffuse(e, kGreen, {0.14f, 0.45f, 0.09f});
      lamp(e);
      world(e);
      std::vector<ar_name> names(passes.size());
      for (size_t i = 0; i < passes.size(); ++i) std::memcpy(names[i].text, passes[i].data(), passes[i].size());
      ar_settings s{};
      s.passes = names.data();
      s.pass_count = uint32_t(names.size());
      s.samples = s.viewport_samples = spp;
      s.max_bounces = s.diffuse_bounces = s.glossy_bounces = s.transmission_bounces = 6;
      s.transparent_bounces = 8;
      s.pixel_filter = AR_FILTER_BOX;
      s.pixel_filter_width = 1.0f;
      s.film_exposure = 1.0f;
      s.seed = 7;
      REQUIRE(e.set_settings(&s) == AR_OK);
    });
    commit([&](Edit& e) { instances(e); });
  }

  template <class F>
  void commit(F&& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    const ar_status st = e->commit(&generation);
    INFO(e->last_error());
    REQUIRE(st == AR_OK);
    scene.materials->end_edit();
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.generations().failed() == 0);
  }

  // The axis-aligned box [x0, x1] x [y0, y1] x [z0, z1], faces wound counter-clockwise from outside.
  static void box(float x0, float y0, float z0, float x1, float y1, float z1, std::vector<V3>* v, std::vector<Quad>* f) {
    const int32_t b = int32_t(v->size());
    for (const V3& p : std::vector<V3>{{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                                       {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}})
      v->push_back(p);
    for (const Quad& q : std::vector<Quad>{{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}})
      f->push_back({b + q[0], b + q[1], b + q[2], b + q[3]});
  }

  // A mesh of quads; only the vertices a quad names are used, with the face normal of its winding.
  void mesh(Edit& e, ar_key key, const std::vector<V3>& verts, const std::vector<Quad>& quads) {
    ar_mesh_desc d = {};
    d.key = key, d.vertex_count = uint32_t(verts.size()), d.corner_count = uint32_t(4 * quads.size());
    d.face_count = uint32_t(quads.size()), d.motion_steps = 1;
    ar_mesh_buffers b = {};
    REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
    for (size_t v = 0; v < verts.size(); ++v) std::memcpy(*b.positions + 3 * v, verts[v].data(), 12);
    for (uint32_t q = 0; q < quads.size(); ++q) {
      (*b.face_start)[q] = int32_t(4 * q);
      const V3& a = verts[quads[q][0]];
      const V3& p1 = verts[quads[q][1]];
      const V3& p2 = verts[quads[q][2]];
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

  void diffuse(Edit& e, ar_key key, V3 rgb) {
    GraphBuilder g;
    g.store(ar::RecordSlot::base_color, g.add(ar::Op::CONST_F32X3, {rgb[0], rgb[1], rgb[2]}));
    g.store(ar::RecordSlot::specular_weight, g.add(ar::Op::CONST_F32, {0}));
    material(e, key, g);
  }

  void lamp(Edit& e) {
    GraphBuilder g;
    const uint32_t colour = g.add(ar::Op::CONST_F32X3, {1, 1, 1}), strength = g.add(ar::Op::CONST_F32, {kLampStrength});
    const uint32_t emit = g.add(ar::Op::EMIT_LE);
    g.link(colour, emit, 0u), g.link(strength, emit, 1u);
    g.store(ar::RecordSlot::base_color, g.add(ar::Op::CONST_F32X3, {0, 0, 0}));
    g.store(ar::RecordSlot::specular_weight, g.add(ar::Op::CONST_F32, {0}));
    material(e, kLamp, g);
  }

  void material(Edit& e, ar_key key, const GraphBuilder& g) {
    const ar_graph_desc gd = g.desc();
    REQUIRE(scene.materials->set_graph(e, key + 1000, &gd) == AR_OK);
    ar_material_desc m = {};
    m.surface_graph = key + 1000;
    m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
    REQUIRE(scene.materials->set_material(e, key, &m) == AR_OK);
  }

  void world(Edit& e) {
    GraphBuilder w;  // black: radiance 0
    const uint32_t wc = w.add(ar::Op::CONST_F32X3, {1, 1, 1}), ws = w.add(ar::Op::CONST_F32, {0});
    const uint32_t we = w.add(ar::Op::EMIT_LE);
    w.link(wc, we, 0u), w.link(ws, we, 1u);
    const ar_graph_desc wd = w.desc();
    REQUIRE(scene.materials->set_graph(e, 1010, &wd) == AR_OK);
    ar_world_desc wdesc = {};
    wdesc.surface_graph = 1010;
    REQUIRE(scene.materials->set_world(e, &wdesc) == AR_OK);
  }

  void instances(Edit& e) {
    std::vector<ar_key> geom{kWhiteMesh, kRedMesh, kGreenMesh, kBlockMesh, kLampMesh};
    std::vector<ar_key> pool{kWhite, kRed, kGreen, kWhite, kLamp}, object{500, 501, 502, 503, 504};
    const uint32_t n = uint32_t(geom.size());
    std::vector<ar_xform> xf(n);
    for (ar_xform& x : xf) x.m[0] = x.m[5] = x.m[10] = 1.0f;
    std::vector<uint32_t> zeros(n, 0), vis(n, slab::kAllRays), moff{0, 1, 2, 3, 4}, mcount(n, 1);
    std::vector<ar_rgba> colors(n, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(n, 0);
    std::vector<ar_key> medium(n, 0);
    ar_instances_desc d = {};
    d.instance_count = n, d.motion_steps = 1, d.transform_count = n, d.material_key_count = n;
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = pool.data();
    d.medium_keys = medium.data();
    REQUIRE(set_instances(e, &d) == AR_OK);
  }
};

inline ar_view_desc view() {
  ar_view_desc v{};
  v.region_width = kSize;
  v.region_height = kSize;
  v.render_scale = 1.0f;
  v.camera.cam_to_world.m[0] = v.camera.cam_to_world.m[5] = v.camera.cam_to_world.m[10] = 1.0f;
  v.camera.cam_to_world.m[11] = kCamZ;
  const float p[16] = {kTan, 0, 0, 0, 0, kTan, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
  for (int i = 0; i < 16; ++i) v.camera.projection_inverse[i] = p[i];
  v.camera.type = AR_CAMERA_PERSP;
  return v;
}

inline SessionOptions options(uint32_t denoiser, Owner owner = Owner{0, 1}) {
  SessionOptions o;
  o.mode = AR_SESSION_MODE_FINAL;
  o.max_width = kSize;
  o.max_height = kSize;
  o.denoiser = denoiser;
  o.owner = owner;
  return o;
}

// The session reached CONVERGED on the rig's last commit (after its final denoise, if it has one).
inline void settle(Session& s, const CornellRig& rig, int timeout_ms = 120000) {
  REQUIRE(slab::until(
      [&] {
        const SessionProgress p = s.progress();
        return p.state == AR_STATE_CONVERGED && p.active_generation == rig.generation;
      },
      timeout_ms));
  REQUIRE(s.failure().empty());
}

// The names of the session's passes, as ar_session_pass_count lists them.
inline std::vector<std::string> pass_names(const Session& s) {
  std::vector<std::string> out;
  for (const PassDesc& p : list_passes(s.layout(), s.denoiser_selected())) out.push_back(p.name);
  return out;
}

// The MEAN of pass `name` as ar_session_read_pass returns it (3 floats per pixel, rows bottom-up).
inline std::vector<float> read_mean(Session& s, const char* name, uint32_t quantity = AR_QUANTITY_MEAN) {
  std::vector<float> out;
  s.read_film([&](const FilmReadContext& c) {
    const std::vector<PassDesc> passes = list_passes(c.film.layout(), c.denoiser);
    const auto it = std::find_if(passes.begin(), passes.end(), [&](const PassDesc& p) { return p.name == name; });
    REQUIRE_MESSAGE(it != passes.end(), "no pass " << name);
    FilmView view(c.device, c.film);
    out.resize(pass_float_count(*it, quantity, view.width(), view.height()));
    read_pass(view, *it, quantity, uint32_t(out.size()), out.data(), c.denoised);
  });
  return out;
}

struct Entry {
  uint32_t code = 0;
  std::string detail;
};

inline std::vector<Entry> entries(Session& s, uint32_t code) {
  std::vector<Entry> out;
  const ar_report_info info = s.report_snapshot();
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    s.report_entry(i, &e);
    if (e.code != code) continue;
    const char* d = reinterpret_cast<const char*>(e.detail);
    out.push_back({e.code, std::string(d, std::find(d, d + sizeof e.detail, '\0'))});
  }
  return out;
}

inline double rel_mse(const std::vector<float>& x, const std::vector<float>& ref) {
  REQUIRE(x.size() == ref.size());
  return astroray::test::rel_mse(x.data(), 3, ref);
}

inline std::filesystem::path repo_root() {
  const std::filesystem::path here = __FILE__;  // <root>/tests/common/cornell_rig.h
  return here.parent_path().parent_path().parent_path();
}

// FNV-1a 64 over the description, then every file under src/ by relative path and contents.
inline std::string source_hash() {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(p)[i]) * 1099511628211ull;
  };
  const std::string d = description();
  mix(d.data(), d.size());
  std::vector<std::filesystem::path> files;
  for (const auto& f : std::filesystem::recursive_directory_iterator(repo_root() / "src"))
    if (f.is_regular_file()) files.push_back(f.path());
  std::sort(files.begin(), files.end());
  for (const std::filesystem::path& f : files) {
    const std::string rel = std::filesystem::relative(f, repo_root()).generic_string();
    mix(rel.data(), rel.size());
    std::ifstream in(f, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    mix(bytes.data(), bytes.size());
  }
  char s[17];
  std::snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(h));
  return s;
}

// The 4096-spp CPU reference's Combined, from the cache or rendered once and stored there (a temporary
// file renamed into place, so a CPU and a GPU case racing on a cold cache never read half a file).
inline std::vector<float> reference() {
  const std::filesystem::path dir = repo_root() / "out" / "refs" / "session_denoise";
  const std::filesystem::path file = dir / ("cornell64-" + source_hash() + ".f32");
  const size_t floats = size_t(kSize) * kSize * 3;
  std::vector<float> ref(floats);
  if (std::ifstream in{file, std::ios::binary}) {
    in.read(reinterpret_cast<char*>(ref.data()), std::streamsize(floats * sizeof(float)));
    if (in.gcount() == std::streamsize(floats * sizeof(float))) {
      std::printf("session_denoise reference: cached %s\n", file.filename().string().c_str());
      return ref;
    }
  }
  const auto t0 = std::chrono::steady_clock::now();
  {
    CornellRig rig(SceneConfig{2, {}, {}}, kRefSpp);
    const unsigned cores = std::thread::hardware_concurrency();
    std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, cores ? cores : 1u);
    Session s(rig.scene, *dev, options(AR_DENOISER_NONE));
    s.set_view(view());
    s.start();
    settle(s, rig, 600000);
    ref = read_mean(s, "Combined");
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("session_denoise reference: rendered %u spp on the CPU in %.1f s, cached as %s\n", kRefSpp, secs,
              file.filename().string().c_str());
  std::filesystem::create_directories(dir);
  const std::filesystem::path tmp = file.string() + ".tmp" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  {
    std::ofstream out(tmp, std::ios::binary);
    out.write(reinterpret_cast<const char*>(ref.data()), std::streamsize(floats * sizeof(float)));
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file, ec);
  if (ec) std::filesystem::remove(tmp, ec);  // the other case stored the same image first
  return ref;
}

// Sets an environment variable for one scope (the test hooks are read when a session is created).
struct EnvGuard {
  std::string name;
  EnvGuard(const char* n, const char* value) : name(n) { REQUIRE(_putenv_s(n, value) == 0); }
  ~EnvGuard() { _putenv_s(name.c_str(), ""); }
};

}  // namespace cornell
