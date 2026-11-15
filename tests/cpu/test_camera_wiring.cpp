// SPDX-License-Identifier: Apache-2.0
// Camera paths through a session (0008 d3; 0002 D7 step 4, D8): each pass builds the active-pixel
// list, the scheduler's refill launches generate_camera over it, and the resolve folds slot
// j * k + m into list[j]. The device is a CPU device with the real scheduler, film kernels and
// generate_camera, an empty scene and two stub stages: INTERSECT_CLOSEST sends every path to
// SHADE_BACKGROUND, which counts it and terminates it.
// Ref: docs/decisions/0008-film-outputs-denoise.md d3; 0002-kernel-architecture.md D7, D8;
// 0007 V18 (the PIXEL_CENTRE hook).
#include <doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "camera/camera.h"
#include "device/cpu/cpu_device.h"
#include "host/scene/edit.h"
#include "host/scene/scene.h"
#include "host/session/session.h"
#include "host/thread_pool.h"
#include "kernel/kernel_globals.h"
#include "kernel/stages/generate_camera.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kW = 7, kH = 5, kPasses = 3;

std::atomic<uint32_t> g_terminated{0};
// The film position of the camera ray of sample j in the last pass, recovered from the stored ray
// by inverting the camera map (not by the code that made it).
struct Pos {
  float x = -1.0f, y = -1.0f;
};
std::vector<Pos> g_pos;

// Sample j takes slot main_queue[...]; every path goes straight to SHADE_BACKGROUND.
void stub_intersect(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  const ar::CameraView& c = *kg.camera;
  for (uint32_t i = first; i < last; ++i) {
    const uint32_t slot = kg.main_queue[step.start + i];
    const ar::Real3 v = ar::camera_from_world_dir(c, ar::ps_ray_d(kg, slot));
    Pos& p = g_pos[ar::ps_sample_id(kg, slot)];
    p.x = float(((v.x / -v.z - c.film_origin.x) / c.film_extent.x) * c.width);
    p.y = float(((v.y / -v.z - c.film_origin.y) / c.film_extent.y) * c.height);
    ar::ps_next_stage(kg, slot) = static_cast<uint8_t>(ar::Stage::SHADE_BACKGROUND);
  }
}

void stub_background(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; ++i) {
    ar::ps_next_stage(kg, kg.main_queue[step.start + i]) = static_cast<uint8_t>(ar::Stage::TERMINATED);
    g_terminated.fetch_add(1u);
  }
}

// The refill launch as CpuDevice runs it: the real generate_camera over the TERMINATED entry.
void generate_camera_kernel(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  cpu_stage_generate_camera(kg, kg.step_table->stage[static_cast<uint32_t>(ar::Stage::TERMINATED)], first, last);
}

ar_view_desc view_of(uint32_t w, uint32_t h) {
  ar_view_desc v{};
  v.region_width = w;
  v.region_height = h;
  v.render_scale = 1.0f;
  v.camera.cam_to_world.m[0] = v.camera.cam_to_world.m[5] = v.camera.cam_to_world.m[10] = 1.0f;
  // NDC to camera space of a pinhole with plane half extents 0.5 x 0.375, looking down -z.
  const float p[16] = {0.5f, 0, 0, 0, 0, 0.375f, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
  for (int i = 0; i < 16; ++i) v.camera.projection_inverse[i] = p[i];
  v.camera.type = AR_CAMERA_PERSP;
  return v;
}

// Settings with a three-sample limit and a box filter, committed and active.
void commit_settings(Scene& scene) {
  ar_settings s{};
  s.samples = s.viewport_samples = kPasses;
  s.max_bounces = 4;
  s.pixel_filter = AR_FILTER_BOX;
  s.pixel_filter_width = 1.0f;
  s.film_exposure = 1.0f;
  s.seed = 5;
  auto e = scene.begin_edit();
  REQUIRE(e);
  REQUIRE(e->set_settings(&s) == AR_OK);
  uint64_t generation = 0;
  REQUIRE(e->commit(&generation) == AR_OK);
  scene.generations().wait_idle();
  REQUIRE(scene.generations().active() == generation);
}

struct Rig {
  ContextState ctx;
  Scene scene;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::unique_ptr<Session> s;  // last: destroyed first
  explicit Rig(const ar_view_desc& view) : scene(ctx, SceneConfig{1, {}}), pool(ctx, 2), dev(ledger, pool) {
    g_terminated = 0;
    g_pos.assign(kW * kH, Pos{});
    dev.register_scheduler_kernels();
    dev.register_kernel(KernelId::GENERATE_CAMERA, &generate_camera_kernel);
    dev.register_stage(ar::Stage::INTERSECT_CLOSEST, &stub_intersect);
    dev.register_stage(ar::Stage::SHADE_BACKGROUND, &stub_background);
    commit_settings(scene);
    SessionOptions o;
    o.max_width = kW;
    o.max_height = kH;
    s = std::make_unique<Session>(scene, dev, o);
    s->set_view(view);
    s->start();
  }
  // The film's n over every plane of the allocated film, read under the render lock.
  std::vector<uint32_t> counts() const {
    std::vector<uint32_t> n;
    s->read_film([&](const FilmReadContext& c) {
      const ar::FilmData& f = c.film.data();
      n.assign(f.n, f.n + f.plane_stride);
    });
    return n;
  }
};

void print_counts(const char* what, const std::vector<uint32_t>& n) {
  std::printf("camera_wiring: %s, sample count per pixel (%ux%u film, rows top to bottom as stored)\n", what, kW, kH);
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) std::printf(" %u", n[y * kW + x]);
    std::printf("\n");
  }
}

// Sets ASTRORAY_TEST_PIXEL_CENTRE for the scope of a session's construction (the session reads
// it once at create) and clears it afterwards.
struct EnvScope {
  explicit EnvScope(bool on) { _putenv_s("ASTRORAY_TEST_PIXEL_CENTRE", on ? "1" : ""); }
  ~EnvScope() { _putenv_s("ASTRORAY_TEST_PIXEL_CENTRE", ""); }
};

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("camera_wiring/every pixel sampled") {
  Rig r(view_of(kW, kH));
  REQUIRE(r.s->wait(20000));
  REQUIRE(r.s->progress().state == AR_STATE_CONVERGED);
  const std::vector<uint32_t> n = r.counts();
  print_counts("full 7x5 film, 3 passes at 1 spp", n);
  uint32_t wrong = 0;
  for (uint32_t p = 0; p < kW * kH; ++p) wrong += n[p] != kPasses;
  CHECK(wrong == 0);
  CHECK(r.s->stats().passes_resolved == kPasses);
  CHECK(g_terminated.load() == kW * kH * kPasses);
  CHECK(r.s->pass_counters().camera_paths == kW * kH);
}

TEST_CASE("camera_wiring/render sub-rectangle") {
  Rig r(view_of(3, 2));
  REQUIRE(r.s->wait(20000));
  const std::vector<uint32_t> n = r.counts();
  print_counts("3x2 render rectangle, 3 passes at 1 spp", n);
  uint32_t counted = 0, wrong = 0;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) {
      const bool inside = x < 3 && y < 2;
      counted += n[y * kW + x] != 0;
      wrong += n[y * kW + x] != (inside ? kPasses : 0u);
    }
  CHECK(counted == 6);
  CHECK(wrong == 0);
  CHECK(g_terminated.load() == 6 * kPasses);
}

TEST_CASE("camera_wiring/pixel centre hook") {
  // Without the hook the filter offset moves the ray off the pixel centre; with
  // ASTRORAY_TEST_PIXEL_CENTRE set at create, every slot's ray crosses its centre.
  const auto spread = [](bool hook, std::vector<std::string>* active) {
    EnvScope env(hook);
    Rig r(view_of(kW, kH));
    *active = r.s->active_test_hooks();
    REQUIRE(r.s->wait(20000));
    double worst = 0.0;
    for (uint32_t j = 0; j < kW * kH; ++j) {
      worst = std::max(worst, double(std::abs(g_pos[j].x - (float(j % kW) + 0.5f))));
      worst = std::max(worst, double(std::abs(g_pos[j].y - (float(j / kW) + 0.5f))));
    }
    return worst;
  };
  std::vector<std::string> off_hooks, on_hooks;
  const double off = spread(false, &off_hooks);
  const double on = spread(true, &on_hooks);
  CHECK(off_hooks.empty());
  REQUIRE(on_hooks.size() == 1);  // an AR_GATE_HOOKS build: the reader is compiled in
  CHECK(on_hooks[0] == "PIXEL_CENTRE");
  CHECK(on < 1e-3);
  CHECK(off > 0.05);
}

TEST_CASE("camera_wiring/production device registers the camera kernels") {
  ContextState ctx;
  const std::shared_ptr<Device> dev = make_cpu_session_device(ctx, 1);
  const CpuDevice& cpu = static_cast<const CpuDevice&>(*dev);
  CHECK(cpu.kernel_fn(KernelId::GENERATE_CAMERA) != nullptr);
  CHECK(cpu.kernel_fn(KernelId::FILM_BUILD_ACTIVE) != nullptr);
}

}  // TEST_SUITE
