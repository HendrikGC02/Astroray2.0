// SPDX-License-Identifier: Apache-2.0
// The device failure contract of 0002 D15, one injected case per row that is not a dev-module row:
// a damaged module image (the GPU device is unavailable, AR_ERR_DEVICE naming the architecture), a
// driver below the R590 floor (the context is refused with the found and required driver), an
// asynchronous device init that fails (error posted when it happens, re-raised at the first upload, the
// host scene data kept), and a GPU other than sm_120 (a warning once per session, the render proceeds).
// Each case ends in an ar_status-bearing DeviceError and a report entry; nothing renders silently.
// The sticky-fault row (an out-of-bounds kernel write) breaks CUDA for the process, so it runs in a
// child process: tests/abi/gpu_test_fault_cli.py. The `full` cases at the end print the longest launch
// of a 1080p render of b5m, and of the dense v1_slab at the per-launch medium cap, against 0009 B-17.
// Ref: docs/decisions/0002-kernel-architecture.md D15; 0001 D2.5, D2.8, D3.5; 0009 B-17; 0006 D8.
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
#include "host/build_info.h"
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
    // Refused by the layout check before the driver sees it, naming the damaged image (the first
    // module the pass loads, whichever that is).
    CHECK_MESSAGE(has(e.what(), "module image truncated or invalid: embedded K_"), std::string(e.what()));
    CHECK_MESSAGE(has(e.what(), " (damaged by ASTRORAY_TEST_CORRUPT_CUBIN) (no ELF magic)"), std::string(e.what()));
  }
}

TEST_CASE("device_failures/cubin layout check refuses truncated images") {
  // The check before every cuModuleLoadData, on a real embedded cubin cut and edited in the ways a
  // half-written or damaged file is. Offsets are the gABI ELF-64 layout (ch. 4): e_phoff at 32,
  // e_shoff 40, e_ehsize 52, e_phentsize 54, e_phnum 56, e_shentsize 58, e_shnum 60, e_shstrndx 62;
  // sh_name at 0, sh_type 4, sh_offset 24, sh_size 32; p_offset at 8, p_filesz 32.
  const ModuleImage clean = load_module_image("generate_camera");
  const std::vector<uint8_t>& b = clean.bytes;
  REQUIRE(b.size() > 256);
  // Every image is checked inside a zero-padded copy, so a broken check that reads past `n` reads
  // zeros of this buffer and fails the case instead of crashing it.
  const auto defect = [](const std::vector<uint8_t>& v, size_t n) {
    std::vector<uint8_t> padded = v;
    padded.resize(v.size() + 65536, 0);
    return cubin_layout_defect(padded.data(), n);
  };
  const auto whole = [&](const std::vector<uint8_t>& v) { return defect(v, v.size()); };
  const auto get = [](const std::vector<uint8_t>& v, size_t at, int bytes) {
    uint64_t x = 0;
    std::memcpy(&x, v.data() + at, size_t(bytes));
    return x;
  };
  const auto put = [](std::vector<uint8_t>& v, size_t at, int bytes, uint64_t x) { std::memcpy(v.data() + at, &x, size_t(bytes)); };
  const auto edited = [&](size_t at, int bytes, uint64_t x) {
    std::vector<uint8_t> v = b;
    put(v, at, bytes, x);
    return v;
  };
  const size_t n = b.size();
  const uint64_t phoff = get(b, 32, 8), phnum = get(b, 56, 2);
  const uint64_t shoff = get(b, 40, 8), shent = get(b, 58, 2), shnum = get(b, 60, 2), shstrndx = get(b, 62, 2);
  REQUIRE(phnum > 0);
  REQUIRE(shnum > 2);
  REQUIRE(shstrndx > 0);
  REQUIRE(shstrndx < shnum);
  const auto sh = [&](uint64_t i) { return size_t(shoff + i * shent); };
  const uint64_t str_off = get(b, sh(shstrndx) + 24, 8), str_size = get(b, sh(shstrndx) + 32, 8);
  REQUIRE(str_size > 1);
  REQUIRE(b[size_t(str_off + str_size - 1)] == 0);

  CHECK(whole(b) == "");
  CHECK(cubin_layout_defect(nullptr, 0) == "shorter than an ELF-64 header");
  // Cuts: the header, the program header table (checked first), and the section header table on a
  // copy without program headers (nvcc places them after the section table).
  CHECK(defect(b, 63) == "shorter than an ELF-64 header");
  CHECK(defect(b, size_t(phoff + phnum * 56 - 1)) == "program header table past the end");
  CHECK(defect(edited(56, 2, 0), size_t(shoff + shnum * shent - 1)) == "section header table past the end");
  // Header fields.
  CHECK(whole(edited(4, 1, 1)) == "not ELF-64 little-endian");  // ELFCLASS32
  CHECK(whole(edited(5, 1, 2)) == "not ELF-64 little-endian");  // ELFDATA2MSB
  CHECK(whole(edited(52, 2, 63)) == "ELF header size not 64");
  CHECK(whole(edited(52, 2, 65)) == "ELF header size not 64");
  CHECK(whole(edited(54, 2, 55)) == "program header entry below 56 bytes");
  CHECK(whole(edited(58, 2, 63)) == "section header entry below 64 bytes");
  CHECK(whole(edited(60, 2, 0)) == "extended ELF numbering");  // e_shnum 0 with e_shoff set
  CHECK(whole(edited(62, 2, 0xffff)) == "extended ELF numbering");  // SHN_XINDEX
  CHECK(whole(edited(62, 2, shnum)) == "section name table index out of range");
  CHECK(whole(edited(62, 2, 0)) == "section name table index out of range");
  // A section with file contents (not the name table) running past the end, by one byte and by an
  // offset near 2^64 whose end wraps around to a small number.
  uint64_t k = 1;
  while (k < shnum && (k == shstrndx || get(b, sh(k) + 4, 4) == 0 || get(b, sh(k) + 4, 4) == 8)) ++k;
  REQUIRE(k < shnum);
  const std::string past_k = "section " + std::to_string(k) + " past the end";
  CHECK(whole(edited(sh(k) + 32, 8, uint64_t(n) - get(b, sh(k) + 24, 8) + 1)) == past_k);
  {
    std::vector<uint8_t> v = edited(sh(k) + 24, 8, ~uint64_t(0) - 7);  // 2^64 - 8
    put(v, sh(k) + 32, 8, 16);
    CHECK(whole(v) == past_k);
  }
  // A segment running past the end, by one byte and by a size whose end wraps around.
  const uint64_t p_off = get(b, size_t(phoff) + 8, 8);
  CHECK(whole(edited(size_t(phoff) + 32, 8, uint64_t(n) - p_off + 1)) == "segment 0 past the end");
  CHECK(whole(edited(size_t(phoff) + 32, 8, (~uint64_t(0) - p_off) + 9)) == "segment 0 past the end");
  // The name table: as a string table its bounds are the section loop's; typed SHT_NULL it is
  // skipped there and must still be bounded before any name is read.
  const std::string past_str = "section " + std::to_string(shstrndx) + " past the end";
  CHECK(whole(edited(sh(shstrndx) + 24, 8, n)) == past_str);
  CHECK(whole(edited(sh(shstrndx) + 32, 8, n - str_off + 1)) == past_str);
  {
    std::vector<uint8_t> v = edited(sh(shstrndx) + 4, 4, 0);
    CHECK(whole(v) == "");  // in bounds, the type alone is no defect
    put(v, sh(shstrndx) + 32, 8, n - str_off + 1);
    CHECK(whole(v) == "section name table past the end");
    std::vector<uint8_t> w = edited(sh(shstrndx) + 4, 4, 0);
    put(w, sh(shstrndx) + 24, 8, n);
    CHECK(whole(w) == "section name table past the end");
  }
  CHECK(whole(edited(sh(shstrndx) + 4, 4, 8)) == "section name table has no file contents");
  // Names: one starting past the table, one whose NUL is gone.
  CHECK(whole(edited(sh(0), 4, str_size)) == "section 0 name outside the name table");
  {
    std::vector<uint8_t> v = edited(sh(0), 4, str_size - 1);
    v[size_t(str_off + str_size - 1)] = 'x';
    CHECK(whole(v) == "section 0 name not terminated");
  }
  // The half file dev_reload writes, and the injected damage, are both refused.
  CHECK(defect(b, n / 2) != "");
  CHECK(cubin_layout_defect(load_module_image("generate_camera", true).bytes.data(), n) == "no ELF magic");
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

TEST_CASE("device_failures/build arch not covering the device is refused") {
  // 0001 D3.4: the modules are cubins of one architecture. A device of another major architecture is
  // refused at creation, naming both, before any module load (0002 D15 "No module image for this GPU").
  const std::string built = astroray::built_cuda_arch();
  REQUIRE(built.size() > 3);
  const std::string other = built[3] == '8' ? "75" : "86";  // a different major than this build's
  {
    Hook h("COMPUTE_CAPABILITY", other.c_str());
    const DeviceError e = refused_device();
    const std::string text = e.what();
    CHECK(e.status() == AR_ERR_DEVICE);
    CHECK(has(text, ("GPU device unavailable: built for " + built + ", device is sm_" + other + ": rebuild").c_str()));
    CHECK_FALSE(has(text, "no usable module image"));
  }
  {
    // The build's own architecture on a device of the same major and a higher minor is covered.
    Hook h("BUILT_ARCH", "81");
    Hook c("COMPUTE_CAPABILITY", "86");
    Ledger ledger;
    CHECK(make_cuda_device(ledger, 0) != nullptr);
  }
  {
    Hook h("BUILT_ARCH", "89");
    Hook c("COMPUTE_CAPABILITY", "86");  // a lower minor than the cubin's: not covered
    CHECK(has(refused_device().what(), "built for sm_89, device is sm_86: rebuild"));
  }
}

TEST_CASE("device_failures/untested architecture warns once") {
  {
    Ledger ledger;
    auto device = make_cuda_device(ledger, 0);
    // sm_120 is the only tested hardware (0001 D3.3, the machine of this suite): no warning.
    CHECK(device->untested_architecture().empty());
  }
  Hook h("COMPUTE_CAPABILITY", "86");
  // The pretended GPU is not the build's architecture; the build is pretended to be sm_86 too, so the
  // refusal of "build arch not covering the device" does not hide the warning under test.
  Hook b("BUILT_ARCH", "86");
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

// 0009 B-17: no kernel launch of a 1080p render (or a square one of as many pixels) lasts above 100 ms (target 20 ms). The device times every
// launch under ASTRORAY_TEST_LAUNCH_TIMING and prints the longest when the CLI's session ends. `scene` is a
// recording under out/scenes, built by tools/check.ps1 (`scenes` for b5m, the integrator checks for
// v1_slab.dense).
static void longest_launch(const char* scene, const char* size) {
  char exe[MAX_PATH] = {};
  REQUIRE(GetModuleFileNameA(nullptr, exe, MAX_PATH) != 0);
  const std::filesystem::path bin = std::filesystem::path(exe).parent_path();
  const std::filesystem::path cli = bin / "astroray_cli.exe";
  const std::filesystem::path rec = bin.parent_path() / "scenes" / (std::string(scene) + ".arrec");
  const std::filesystem::path fits = std::filesystem::temp_directory_path() / ("astroray_b17_" + std::string(scene) + ".fits");
  REQUIRE_MESSAGE(std::filesystem::exists(cli), cli.string() << " missing: run tools/build.ps1");
  REQUIRE_MESSAGE(std::filesystem::exists(rec), rec.string() << " missing: run tools/check.ps1 gate");
  Hook h("LAUNCH_TIMING", "1");
  const std::string cmd = "\"\"" + cli.string() + "\" render \"" + rec.string() + "\" -o \"" + fits.string() +
                          "\" --device cuda --spp 1 --size " + size + " 2>&1\"";
  FILE* p = _popen(cmd.c_str(), "r");
  REQUIRE(p != nullptr);
  std::string out;
  char buf[512];
  while (std::fgets(buf, sizeof buf, p)) out += buf;
  const int rc = _pclose(p);
  const size_t at = out.find("astroray: longest launch ");
  REQUIRE_MESSAGE(at != std::string::npos, "the CLI printed no launch timing: " << out);
  const double ms = std::atof(out.c_str() + at + std::strlen("astroray: longest launch "));
  std::printf("%s: %s", scene, out.substr(at, out.find('\n', at) - at + 1).c_str());
  CHECK_MESSAGE(rc == 0, out);
  CHECK(ms > 0.0);
  CHECK_MESSAGE(ms <= 100.0, "B-17: the longest launch is above the 100 ms hard cap");
  if (ms > 20.0) std::printf("B-17 target 20 ms exceeded: %.3f ms\n", ms);
  std::filesystem::remove(fits);
}

TEST_CASE("device_failures/longest launch within B-17") { longest_launch("b5m", "1920x1080"); }

// The per-launch medium cap's B-17 case (0006 D8, RULINGS 111): the dense v1_slab (sigma_t 64 per m,
// 0.05 m thick) under ASTRORAY_TEST_MAJORANT=2048, so a flight across the box takes about
// 2048 * 64 * 0.05 = 6,554 tentative collisions and the default cap of 4,096 requeues it; without the
// cap one such item would hold its launch for all of them. 1440 x 1440: the slab's view is square, and that is
// 1080p's pixel count.
TEST_CASE("device_failures/longest launch within B-17, dense slab at the medium cap") {
  Hook k("MAJORANT", "2048");
  longest_launch("v1_slab.dense", "1440x1440");
}

}  // TEST_SUITE
