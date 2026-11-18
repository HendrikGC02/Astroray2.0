// SPDX-License-Identifier: Apache-2.0
// Dev kernel reload (0002 D14, D15). A development build with ASTRORAY_KERNEL_DIR set checks each loaded
// stage module's file at every step boundary: a changed file is loaded and swapped before the next
// launch and starts a new epoch; a file that fails to load leaves the previous image running and raises
// KERNEL_MODULE_FALLBACK, one entry per module, removed when a good file loads. The cases copy the stage
// modules into a temporary kernel directory, render, then replace shade_background with reload_variant
// (the same stage plus a known constant) and with a truncated copy. The swap time is printed against
// 0009 B-04.
// Ref: docs/decisions/0002-kernel-architecture.md D14, D15; 0009 B-04.
#include <doctest.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "device/cuda/cuda_device.h"
#include "device/cuda/optix_accel.h"
#include "host/device.h"
#include "host/film/film_view.h"
#include "host/light/lamps.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/readback.h"
#include "host/session/session.h"

using namespace astroray;
using namespace astroray::host;
namespace fs = std::filesystem;

namespace {

// A private kernel directory holding every stage module (cubins and the OptiX PTX of the build tree),
// named by ASTRORAY_KERNEL_DIR for the scope. The devices created inside read it once, at creation.
struct KernelDir {
  fs::path dir;
  KernelDir() {
    dir = fs::temp_directory_path() / ("astroray_108_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path cubins(AR_TEST_KERNEL_DIR);
    for (const fs::path& from : {cubins, cubins.parent_path() / "optix"})
      for (const auto& f : fs::directory_iterator(from))
        if (f.path().extension() == ".cubin" || f.path().extension() == ".ptx") fs::copy_file(f.path(), dir / f.path().filename());
    _putenv_s("ASTRORAY_KERNEL_DIR", dir.string().c_str());
  }
  ~KernelDir() {
    _putenv_s("ASTRORAY_KERNEL_DIR", "");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  fs::path module(const char* name) const { return dir / (std::string(name) + ".cubin"); }
};

std::vector<char> read_bytes(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path& p, const char* data, size_t n) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(data, static_cast<std::streamsize>(n));
}

struct Held {
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::unique_ptr<ModuleDevice> device;
  std::unique_ptr<AccelBuilder> accel;
};

// A CUDA scene with a point lamp and a quad, as device_failures' sessions use, and settings with so many
// samples that the session keeps rendering: every swap meets a running pass.
struct Rig {
  ContextState ctx;
  std::shared_ptr<Held> held = std::make_shared<Held>();
  std::unique_ptr<Scene> scene;
  Rig() {
    SceneConfig cfg;
    cfg.workers = 2;
    auto h = held;
    cfg.init = [h]() -> std::shared_ptr<void> {
      h->device = make_cuda_device(*h->ledger, 0);
      h->accel = make_optix_accel_builder(*h->device, 0, "");
      return h;
    };
    cfg.device_of = [](void* p) -> Device* { return static_cast<Held*>(p)->device.get(); };
    cfg.accel_of = [](void* p) -> AccelBuilder* { return static_cast<Held*>(p)->accel.get(); };
    scene = std::make_unique<Scene>(ctx, std::move(cfg));
    scene->meshes = std::make_shared<MeshStore>();
    scene->materials = std::make_shared<MaterialStore>(scene->meshes->shared_ledger());
    commit([&](Edit& e) {
      ar_light_desc l = {};
      l.type = AR_LIGHT_POINT;
      l.color.kind = AR_SPECTRUM_RGB;
      l.color.rgb[0] = l.color.rgb[1] = l.color.rgb[2] = 1.0f;
      l.xf.m[0] = l.xf.m[5] = l.xf.m[10] = 1.0f, l.xf.m[3] = -5.0f;
      l.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE;
      l.power = 10.0f, l.normalize = 1, l.cast_shadow = 1;
      REQUIRE(set_light(e, 200, &l, scene->materials->table()) == AR_OK);
    });
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    commit([&](Edit& e) {
      ar_mesh_desc d = {};
      d.key = 100, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(*scene, e, &d, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(100) == AR_OK);
    });
    commit([&](Edit& e) {
      std::vector<ar_key> geom{100}, object{1};
      std::vector<ar_xform> xf(1);
      xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
      std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA | AR_VISIBILITY_SHADOW), moff{0}, mcount{0};
      std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
      std::vector<uint8_t> lg(1, 0);
      ar_instances_desc d = {};
      d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = 0;
      d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
      d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
      d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = nullptr;
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
    commit([&](Edit& e) {
      ar_settings st{};
      st.samples = st.viewport_samples = 1000000;
      st.max_bounces = 2;
      st.pixel_filter = AR_FILTER_BOX;
      st.pixel_filter_width = 1.0f;
      st.film_exposure = 1.0f;
      REQUIRE(e.set_settings(&st) == AR_OK);
    });
  }
  template <class F>
  void commit(F&& body) {
    auto e = scene->begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    scene->materials->end_edit();
    e.reset();
    scene->generations().wait_idle();
  }
};

ar_view_desc small_view() {
  ar_view_desc v{};
  v.region_width = v.region_height = 16;
  v.render_scale = 1.0f;
  v.camera.cam_to_world.m[0] = v.camera.cam_to_world.m[5] = v.camera.cam_to_world.m[10] = 1.0f;
  const float p[16] = {0.5f, 0, 0, 0, 0, 0.375f, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
  for (int i = 0; i < 16; ++i) v.camera.projection_inverse[i] = p[i];
  v.camera.type = AR_CAMERA_PERSP;
  return v;
}

// A running session on the rig's scene whose device was created inside the kernel directory's scope.
struct Running {
  Rig rig;
  Ledger ledger;
  std::unique_ptr<ModuleDevice> device = make_cuda_device(ledger, 0);
  std::unique_ptr<Session> session;
  Running() {
    SessionOptions o;
    o.max_width = o.max_height = 16;
    o.owner = Owner{0, 1};
    session = std::make_unique<Session>(*rig.scene, *device, o);
    session->set_view(small_view());
    session->start();
  }
  ~Running() {
    session->close();
    session->join();
  }
  // The session has resolved at least `passes` passes in its current epoch.
  bool wait_passes(uint32_t passes, uint32_t timeout_ms = 20000) const {
    for (uint32_t t = 0; t < timeout_ms; t += 5) {
      try {
        if (session->film_samples(0) >= passes) return true;
      } catch (const std::logic_error&) {  // no film yet
      }
      Sleep(5);
    }
    return false;
  }
  // Combined averaged over the render rectangle, first channel.
  double combined_mean() const {
    const std::vector<PassDesc> passes = list_passes(session->layout());
    std::vector<float> m;
    session->read_film([&](const FilmReadContext& c) {
      FilmView view(c.device, c.film);
      m.resize(pass_float_count(passes.front(), AR_QUANTITY_MEAN, view.width(), view.height()));
      read_pass(view, passes.front(), AR_QUANTITY_MEAN, uint32_t(m.size()), m.data());
    });
    double sum = 0.0;
    for (size_t j = 0; j < m.size() / 3; ++j) sum += m[j * 3];
    return sum / double(m.size() / 3);
  }
  std::vector<ar_report_entry> fallback_entries() const {
    const ar_report_info info = session->report_snapshot();
    std::vector<ar_report_entry> out;
    for (uint32_t i = 0; i < info.entry_count; ++i) {
      ar_report_entry e{};
      session->report_entry(i, &e);
      if (e.code == AR_REPORT_KERNEL_MODULE_FALLBACK) out.push_back(e);
    }
    return out;
  }
};

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("dev_reload/swap at step boundary") {
  KernelDir kd;
  Running r;
  Session& s = *r.session;
  REQUIRE(r.wait_passes(2));
  const double before = r.combined_mean();
  const uint64_t epoch = s.progress().epoch;
  CHECK(s.progress().state != AR_STATE_FAILED);

  // The edit-to-launch step of B-04 after nvcc: the rebuilt module appears in the directory.
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<char> variant = read_bytes(fs::path(AR_TEST_KERNEL_DIR) / "reload_variant.cubin");
  REQUIRE_FALSE(variant.empty());
  write_bytes(kd.module("shade_background"), variant.data(), variant.size());
  while (s.progress().epoch == epoch && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) Sleep(1);
  REQUIRE(s.progress().epoch == epoch + 1);  // exactly the swap's epoch: nothing else changed the image
  REQUIRE(r.wait_passes(1));                 // the first pass of the new epoch ran on the new module
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("dev_reload: swap to first pass with the new module %.1f ms (B-04 limit 5000 ms for the whole edit; nvcc separate)\n", ms);
  CHECK(ms <= 5000.0);
  REQUIRE(r.wait_passes(4));
  const double after = r.combined_mean();
  std::printf("dev_reload: Combined mean before %.6g, after %.6g\n", before, after);
  CHECK(after > before + 1e-3);  // every escaped path now carries the variant's constant
  CHECK(s.progress().state != AR_STATE_FAILED);
  CHECK(r.fallback_entries().empty());
}

TEST_CASE("dev_reload/corrupted module keeps previous") {
  KernelDir kd;
  Running r;
  Session& s = *r.session;
  REQUIRE(r.wait_passes(2));
  const uint64_t epoch = s.progress().epoch;
  const std::vector<char> good = read_bytes(kd.module("shade_background"));
  REQUIRE(good.size() > 64);

  // A truncated module: the previous image keeps running and exactly one entry names module and error.
  write_bytes(kd.module("shade_background"), good.data(), 64);
  std::vector<ar_report_entry> e;
  for (int i = 0; i < 4000 && e.empty(); ++i) {
    Sleep(5);
    e = r.fallback_entries();
  }
  REQUIRE(e.size() == 1);
  const std::string detail = reinterpret_cast<const char*>(e[0].detail);
  CHECK(detail.find("shade_background") != std::string::npos);
  CHECK(detail.find("CUDA_ERROR") != std::string::npos);  // the loader's own error
  CHECK(e[0].level == AR_LEVEL_WARNING);
  // It keeps rendering on the old image: same epoch, passes still accumulate, no storm of repeats.
  const uint32_t n = s.film_samples(0);
  REQUIRE(r.wait_passes(n + 3));
  Sleep(200);
  CHECK(s.progress().epoch == epoch);
  CHECK(s.progress().state != AR_STATE_FAILED);
  e = r.fallback_entries();
  REQUIRE(e.size() == 1);
  CHECK(e[0].count == 1);

  // The file is fixed: it loads, the epoch resets and the entry is gone.
  write_bytes(kd.module("shade_background"), good.data(), good.size());
  for (int i = 0; i < 4000 && s.progress().epoch == epoch; ++i) Sleep(5);
  CHECK(s.progress().epoch == epoch + 1);
  CHECK(r.fallback_entries().empty());
  CHECK(s.progress().state != AR_STATE_FAILED);
}

}  // TEST_SUITE
