// SPDX-License-Identifier: Apache-2.0
// The device failure contract of 0002 D15, one injected case per row that is not a dev-module row:
// a damaged module image (the GPU device is unavailable, AR_ERR_DEVICE naming the architecture), a
// driver below the R590 floor (the context is refused with the found and required driver), an
// asynchronous device init that fails (error posted when it happens, re-raised at the first upload, the
// host scene data kept), and a GPU other than sm_120 (a warning once per session, the render proceeds).
// Each case ends in an ar_status-bearing DeviceError and a report entry; nothing renders silently.
// The sticky-fault row (an out-of-bounds kernel write) breaks CUDA for the process, so it runs in a
// child process: tests/abi/gpu_test_fault_cli.py. The `full` case at the end prints the longest launch
// of a 1080p render of b5m against 0009 B-17.
// Ref: docs/decisions/0002-kernel-architecture.md D15; 0001 D2.5, D2.8, D3.5; 0009 B-17.
#include <doctest.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "device/cuda/cuda_device.h"
#include "device/cuda/module_loader.h"
#include "device/cuda/optix_accel.h"
#include "host/device.h"
#include "host/light/lamps.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/session.h"

using namespace astroray;
using namespace astroray::host;

namespace {

// Sets ASTRORAY_TEST_<name> for one scope (the hooks are read when a device, scene or session is made).
struct Hook {
  std::string var;
  Hook(const char* name, const char* value) : var(std::string("ASTRORAY_TEST_") + name) { _putenv_s(var.c_str(), value); }
  ~Hook() { _putenv_s(var.c_str(), ""); }
};

struct Held {
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::unique_ptr<ModuleDevice> device;
  std::unique_ptr<AccelBuilder> accel;
};

// A CUDA scene whose device init is the one ar_scene_create runs; `populate` adds a lamp, a quad and its
// instance, enough for a session's first pass.
struct Rig {
  ContextState ctx;
  std::shared_ptr<Held> held = std::make_shared<Held>();
  std::unique_ptr<Scene> scene;
  explicit Rig(bool populate) {
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
    if (populate) {
      commit_lamp();
      commit_quad();
    }
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
  void commit_settings() {
    commit([&](Edit& e) {
      ar_settings st{};
      st.samples = st.viewport_samples = 4;
      st.max_bounces = 2;
      st.pixel_filter = AR_FILTER_BOX;
      st.pixel_filter_width = 1.0f;
      st.film_exposure = 1.0f;
      REQUIRE(e.set_settings(&st) == AR_OK);
    });
  }
  void commit_lamp() {
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
  }
  void commit_quad() {
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
  }
};

// Every entry of a report snapshot with this code, as its detail text.
template <class Table>
std::vector<std::string> entries_of(Table& report, uint32_t code) {
  report.snapshot();
  ar_report_info info{};
  REQUIRE(report.info(&info) == AR_OK);
  std::vector<std::string> out;
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    REQUIRE(report.entry(i, &e) == AR_OK);
    if (e.code == code) out.emplace_back(reinterpret_cast<const char*>(e.detail));
  }
  return out;
}

std::vector<std::string> session_entries(Session& s, uint32_t code) {
  const ar_report_info info = s.report_snapshot();
  std::vector<std::string> out;
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    s.report_entry(i, &e);
    if (e.code == code) out.emplace_back(reinterpret_cast<const char*>(e.detail));
  }
  return out;
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// The DeviceError make_cuda_device throws, with the status it carries.
DeviceError refused_device() {
  Ledger ledger;
  try {
    auto device = make_cuda_device(ledger, 0);
  } catch (const DeviceError& e) {
    return e;
  }
  FAIL("make_cuda_device returned a device");
  return DeviceError("unreachable");
}

SessionOptions small_session(bool test_work = true) {
  SessionOptions o;
  o.max_width = o.max_height = 16;
  o.owner = Owner{0, 1};
  // Work for the first pass without a view: only for a session that fails before any kernel runs.
  if (test_work) o.test_camera_samples_per_pass = 16;
  return o;
}

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

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("device_failures/corrupt module image") {
  // The copy of an embedded stage cubin the loader hands over loses its header; the embedded bytes stay.
  const ModuleImage clean = load_module_image("generate_camera");
  {
    const ModuleImage damaged = load_module_image("generate_camera", true);
    REQUIRE(damaged.bytes.size() == clean.bytes.size());
    CHECK(damaged.bytes != clean.bytes);
    CHECK(has(damaged.origin, "ASTRORAY_TEST_CORRUPT_CUBIN"));
    // OptiX PTX is never damaged: the copy equals the undamaged image.
    CHECK(load_module_image("optix_traverse", true).bytes == load_module_image("optix_traverse").bytes);
  }
  CHECK(load_module_image("generate_camera").bytes == clean.bytes);

  // A session whose first launch needs the damaged module: the GPU device is unavailable, the session
  // fails with AR_ERR_DEVICE naming the architecture, and the report says so.
  Rig rig(true);
  Hook h("CORRUPT_CUBIN", "1");  // before the session's device exists: the device reads it once
  Ledger ledger;
  auto device = make_cuda_device(ledger, 0);
  Session s(*rig.scene, *device, small_session());
  s.start();
  REQUIRE(s.wait(60000));
  CHECK(s.progress().state == AR_STATE_FAILED);
  const auto failed = session_entries(s, AR_REPORT_SESSION_FAILED);
  REQUIRE(failed.size() == 1);
  CHECK(has(failed[0], "GPU device unavailable"));
  CHECK(has(failed[0], "no usable module image for sm_"));
  try {
    s.resume();
    FAIL("resume on a failed session did not throw");
  } catch (const DeviceError& e) {
    CHECK(e.status() == AR_ERR_DEVICE);
    CHECK(has(e.what(), "GPU device unavailable"));
  }
}

TEST_CASE("device_failures/old driver refused") {
  {
    Hook h("DRIVER_VERSION", "12080");  // CUDA 12.8: below R590's 13.1
    const DeviceError e = refused_device();
    const std::string text = e.what();
    CHECK(e.status() == AR_ERR_DEVICE);
    CHECK_FALSE(e.sticky());
    CHECK(has(text, "GPU device unavailable"));
    CHECK(has(text, "driver 12.8"));  // found
    CHECK(has(text, "required 13.1 (R590)"));
    CHECK(has(text, "GPU sm_"));
    // The scene's asynchronous init hits the same refusal: posted as DEVICE_INIT_FAILED, re-raised typed.
    Rig rig(false);
    CHECK_THROWS_AS(rig.scene->wait_init(), DeviceError);
    const auto posted = entries_of(rig.scene->report(), AR_REPORT_DEVICE_INIT_FAILED);
    REQUIRE(posted.size() == 1);
    CHECK(has(posted[0], "required 13.1"));
  }
  // Below the compute-capability floor of 0001 D3.5.
  {
    Hook h("COMPUTE_CAPABILITY", "70");
    const DeviceError e = refused_device();
    CHECK(e.status() == AR_ERR_DEVICE);
    CHECK(has(e.what(), "compute capability 7.0 is below the required 7.5"));
    CHECK(has(e.what(), "GPU sm_70"));
  }
  // Neither hook set: the same machine is accepted.
  Ledger ledger;
  CHECK(make_cuda_device(ledger, 0) != nullptr);
}

TEST_CASE("device_failures/failing init worker") {
  Hook h("FAIL_INIT", "1");
  Rig rig(false);
  // Edits that need no device are accepted and kept while the init has failed.
  rig.commit_lamp();
  CHECK(rig.scene->known(AR_KIND_LIGHT, 200));
  // The error is posted by the worker itself: no caller has waited for the init yet.
  for (int i = 0; i < 1000 && !rig.scene->init_done(); ++i) Sleep(10);
  REQUIRE(rig.scene->init_done());
  const auto posted = entries_of(rig.scene->report(), AR_REPORT_DEVICE_INIT_FAILED);
  REQUIRE(posted.size() == 1);
  CHECK(has(posted[0], "ASTRORAY_TEST_FAIL_INIT"));
  // The first device upload re-raises it, typed as a device failure, and the host data is still there.
  try {
    acquire_scene_tables(*rig.scene);
    FAIL("acquire_scene_tables did not throw");
  } catch (const DeviceError& e) {
    CHECK(e.status() == AR_ERR_DEVICE);
    CHECK(has(e.what(), "ASTRORAY_TEST_FAIL_INIT"));
  }
  CHECK(rig.scene->known(AR_KIND_LIGHT, 200));
  // Geometry committed after the failure is kept on the host too: only uploads need the device.
  rig.commit_quad();
  CHECK(rig.scene->meshes->find(100) != nullptr);
  CHECK(rig.scene->known(AR_KIND_LIGHT, 200));
}

TEST_CASE("device_failures/untested architecture warns once") {
  {
    Ledger ledger;
    auto device = make_cuda_device(ledger, 0);
    // sm_120 is the only tested hardware (0001 D3.3, the machine of this suite): no warning.
    CHECK(device->untested_architecture().empty());
  }
  Hook h("COMPUTE_CAPABILITY", "86");
  Rig rig(true);
  rig.commit_settings();
  Ledger ledger;
  auto device = make_cuda_device(ledger, 0);
  CHECK(device->untested_architecture() == "sm_86");
  Session s(*rig.scene, *device, small_session(false));
  s.set_view(small_view());
  const auto warned = session_entries(s, AR_REPORT_UNTESTED_DEVICE);
  REQUIRE(warned.size() == 1);
  CHECK(has(warned[0], "sm_86"));
  // The render proceeds: a pass or two, then an epoch change (an edit), and the warning stays one entry.
  s.start();
  REQUIRE(s.wait(60000));
  CHECK(s.progress().state == AR_STATE_CONVERGED);
  const uint64_t epoch = s.progress().epoch;
  rig.commit_lamp();
  for (int i = 0; i < 3000 && s.progress().epoch == epoch; ++i) Sleep(10);
  CHECK(s.progress().epoch > epoch);
  REQUIRE(s.wait(60000));
  CHECK(s.progress().state != AR_STATE_FAILED);
  CHECK(session_entries(s, AR_REPORT_SESSION_FAILED).empty());
  CHECK(session_entries(s, AR_REPORT_UNTESTED_DEVICE).size() == 1);
  s.close();
  s.join();
}

}  // TEST_SUITE

TEST_SUITE("full") {

// 0009 B-17: no kernel launch of a 1080p render of b5m lasts above 100 ms (target 20 ms). The device
// times every launch under ASTRORAY_TEST_LAUNCH_TIMING and prints the longest when the CLI's session ends.
TEST_CASE("device_failures/longest launch within B-17") {
  char exe[MAX_PATH] = {};
  REQUIRE(GetModuleFileNameA(nullptr, exe, MAX_PATH) != 0);
  const std::filesystem::path bin = std::filesystem::path(exe).parent_path();
  const std::filesystem::path cli = bin / "astroray_cli.exe";
  const std::filesystem::path rec = bin.parent_path() / "scenes" / "b5m.arrec";
  const std::filesystem::path fits = std::filesystem::temp_directory_path() / "astroray_099_b5m.fits";
  REQUIRE_MESSAGE(std::filesystem::exists(cli), cli.string() << " missing: run tools/build.ps1");
  REQUIRE_MESSAGE(std::filesystem::exists(rec), rec.string() << " missing: run tools/check.ps1 (scenes)");
  Hook h("LAUNCH_TIMING", "1");
  const std::string cmd = "\"\"" + cli.string() + "\" render \"" + rec.string() + "\" -o \"" + fits.string() +
                          "\" --device cuda --spp 1 --size 1920x1080 2>&1\"";
  FILE* p = _popen(cmd.c_str(), "r");
  REQUIRE(p != nullptr);
  std::string out;
  char buf[512];
  while (std::fgets(buf, sizeof buf, p)) out += buf;
  const int rc = _pclose(p);
  const size_t at = out.find("astroray: longest launch ");
  REQUIRE_MESSAGE(at != std::string::npos, "the CLI printed no launch timing: " << out);
  const double ms = std::atof(out.c_str() + at + std::strlen("astroray: longest launch "));
  std::printf("%s", out.substr(at, out.find('\n', at) - at + 1).c_str());
  CHECK_MESSAGE(rc == 0, out);
  CHECK(ms > 0.0);
  CHECK_MESSAGE(ms <= 100.0, "B-17: the longest launch is above the 100 ms hard cap");
  if (ms > 20.0) std::printf("B-17 target 20 ms exceeded: %.3f ms\n", ms);
  std::filesystem::remove(fits);
}

}  // TEST_SUITE
