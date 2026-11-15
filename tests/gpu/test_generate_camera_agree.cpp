// SPDX-License-Identifier: Apache-2.0
// generate_camera and film_build_active on the GPU against the CPU float kernels (0009 d17 step
// iii, the per-stage fixture of 0002 D4): 10^6 camera samples of a 1000x1000 render rectangle in a
// 1024-wide film (so plane indices differ from j), a rotated camera, the production filter and
// wavelength tables, samples_done = 3. Integer fields and the constant fields must be bit-equal;
// ray directions agree to 0002 D12's direction threshold (p99.9 <= 1e-5 rad, FMA-off GPU build vs
// CPU float); film wavelengths and the cone spread to its evaluation threshold.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D12; 0009 d17; 0003 d23 (keys, dims 0-7);
// 0008 d3 (the list). The production modules are launched, so what runs is the shipped code.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "agree.h"
#include "device/cpu/cpu_device.h"
#include "host/device.h"
#include "host/film/active_pixels.h"
#include "host/film/filter_table.h"
#include "host/spectral/wavelength_table.h"
#include "host/thread_pool.h"
#include "stages/film_build_active.h"
#include "stages/generate_camera.h"

using namespace astroray;
using namespace astroray::host;

namespace ar {
// Visits the arrays of the always-present path fields; the others stay null (no feature is on).
template <class F>
void for_each_always_field(PathStateSoA& soa, F&& fn) {
#define AR_PATH_FIELD(n, t, f) \
  if constexpr (StateFeature::f == StateFeature::ALWAYS) fn(#n, sizeof(t), reinterpret_cast<void**>(&soa.n));
#include "state/path_state.def"
#undef AR_PATH_FIELD
}
}  // namespace ar

namespace {

constexpr uint32_t kFilmW = 1024, kFilmH = 1000, kRectW = 1000, kRectH = 1000;
constexpr uint32_t kN = kRectW * kRectH;  // 10^6 samples, k = 1
constexpr uint32_t kSamplesDone = 3;

ar::CameraView make_camera() {
  const double a = 0.6, b = -0.4, tx = 0.5, ty = 0.5;  // yaw about z, pitch about x
  const double cz = std::cos(a), sz = std::sin(a), cx = std::cos(b), sx = std::sin(b);
  const double rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
  const double rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
  double r[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i][j] = rz[i][0] * rx[0][j] + rz[i][1] * rx[1][j] + rz[i][2] * rx[2][j];
  ar::CameraView c{};
  c.type = ar::CameraType::PERSPECTIVE;
  c.origin = {1.5f, -2.0f, 0.75f};
  for (int j = 0; j < 3; ++j) c.axis[j] = {float(r[0][j]), float(r[1][j]), float(r[2][j])};
  c.film_origin = {float(-tx), float(-ty)};
  c.film_extent = {float(2 * tx), float(2 * ty)};
  c.film_area = float(4 * tx * ty);
  c.width = float(kRectW);
  c.height = float(kRectH);
  return c;
}

// What one device produced: the list and every always-present path field, as bytes.
struct Snapshot {
  std::vector<uint32_t> list;
  std::vector<std::string> names;
  std::vector<std::vector<unsigned char>> fields;
  const std::vector<unsigned char>& get(const char* name) const {
    for (size_t i = 0; i < names.size(); ++i)
      if (names[i] == name) return fields[i];
    FAIL("no field " << name);
    return fields[0];
  }
};

// One device's copy of the fixture, run once. `gpu` is null for the CPU device, where a buffer
// address is a host address and the kernels are called directly.
Snapshot run(Device& dev, ModuleDevice* gpu, const WavelengthTable& wt, const ar::FilterTable& filter,
             const ar::CameraView& camera) {
  const Owner owner{1, 1};
  const auto put = [&](const void* p, uint64_t bytes) {
    DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, bytes, owner);
    dev.upload(b, 0, p, bytes);
    return b;
  };
  DeviceBuffer edges = put(wt.edges.data(), wt.edges.size() * sizeof(float));
  DeviceBuffer cdf = put(wt.cdf.data(), wt.cdf.size() * sizeof(float));
  DeviceBuffer filter_buf = put(&filter, sizeof filter);
  DeviceBuffer camera_buf = put(&camera, sizeof camera);

  ar::FilmData film{};
  film.width = kFilmW;
  film.height = kFilmH;
  film.rect_width = kRectW;
  film.rect_height = kRectH;
  film.plane_stride = kFilmW * kFilmH;
  film.wavelengths = {static_cast<const float*>(edges.ptr), static_cast<const float*>(cdf.ptr), wt.bins()};
  DeviceBuffer film_buf = put(&film, sizeof film);

  ActivePixels active(dev, kFilmW, kFilmH, owner);
  REQUIRE(active.plan(film) == kN);

  std::vector<uint32_t> identity(kN);
  for (uint32_t i = 0; i < kN; ++i) identity[i] = i;
  DeviceBuffer queue_buf = put(identity.data(), identity.size() * sizeof(uint32_t));
  ar::StepTable table{};
  table.stage[uint32_t(ar::Stage::TERMINATED)] = ar::StepEntry{0, kN, 0};
  DeviceBuffer table_buf = put(&table, sizeof table);

  ar::PathStateSoA soa{};
  std::vector<DeviceBuffer> bufs;
  std::vector<std::string> names;
  std::vector<uint64_t> elem;
  ar::for_each_always_field(soa, [&](const char* name, size_t bytes, void** slot) {
    DeviceBuffer b = dev.allocate(LedgerCategory::MAIN_PATH_POOL, bytes * kN, owner);
    dev.memset(b, 0, 0xAB, b.bytes);  // a field the kernel forgets reads back as poison
    *slot = b.ptr;
    bufs.push_back(b);
    names.push_back(name);
    elem.push_back(bytes);
  });
  DeviceBuffer soa_buf = put(&soa, sizeof soa);

  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(soa_buf.ptr);
  kg.step_table = static_cast<const ar::StepTable*>(table_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(queue_buf.ptr);
  kg.active_pixels = active.list();
  kg.film = static_cast<ar::FilmData*>(film_buf.ptr);
  kg.camera = static_cast<const ar::CameraView*>(camera_buf.ptr);
  kg.filter = static_cast<const ar::FilterTable*>(filter_buf.ptr);
  kg.session.width = kFilmW;
  kg.session.height = kFilmH;
  kg.session.samples_done = kSamplesDone;
  kg.session.samples_per_pass = 1;
  kg.session.seed = 0xC0FFEEu;
  kg.session.sampler_mode = ar::SAMPLER_SOBOL;
  DeviceBuffer kg_buf = put(&kg, sizeof kg);

  if (gpu != nullptr) {
    const std::string dir = AR_TEST_KERNEL_NOFMA_DIR;
    const uint32_t build = gpu->load_module_file(dir + "/film_build_active.cubin");
    const uint32_t generate = gpu->load_module_file(dir + "/generate_camera.cubin");
    const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_buf.ptr);
    void* args[] = {&kg_dev};
    gpu->launch_module_kernel(build, "k_film_build_active", (kN + 255) / 256, 256, args);
    gpu->launch_module_kernel(generate, "k_generate_camera", (kN + 255) / 256, 256, args);
  } else {
    const ar::KernelGlobals host_kg = kg;
    // On the CPU device the uploaded copies are host memory the kernels read directly.
    cpu_film_build_active(host_kg, 0, kN);
    cpu_stage_generate_camera(host_kg, table.stage[uint32_t(ar::Stage::TERMINATED)], 0, kN);
  }
  dev.synchronize();

  Snapshot s;
  s.list.resize(kN);
  dev.download(s.list.data(), active.buffer(), 0, kN * sizeof(uint32_t));
  for (size_t i = 0; i < bufs.size(); ++i) {
    s.names.push_back(names[i]);
    s.fields.emplace_back(bufs[i].bytes);
    dev.download(s.fields.back().data(), bufs[i], 0, bufs[i].bytes);
    dev.free(bufs[i]);
  }
  for (DeviceBuffer* b : {&soa_buf, &kg_buf, &table_buf, &queue_buf, &film_buf, &camera_buf, &filter_buf, &edges, &cdf})
    dev.free(*b);
  return s;
}

// The n * stride floats of a field as doubles (a float widens exactly), for the harness.
agree::Results as_results(const std::vector<unsigned char>& bytes, uint32_t stride) {
  agree::Results r;
  r.n = kN;
  r.stride = stride;
  r.v.resize(size_t(kN) * stride);
  for (size_t i = 0; i < r.v.size(); ++i) {
    float f;
    std::memcpy(&f, bytes.data() + i * sizeof(float), sizeof f);
    r.v[i] = double(f);
  }
  return r;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("generate_camera/agreement") {
  const WavelengthTable wt = WavelengthTableBuilder().build();
  const auto filter = std::make_unique<ar::FilterTable>(build_filter_table({}));
  const ar::CameraView camera = make_camera();

  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const Snapshot c = run(cpu, nullptr, wt, *filter, camera);
  const Snapshot g = run(*gpu, gpu.get(), wt, *filter, camera);
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);

  // The active-pixel list: integers, bit-equal, and the plane index of the j-th pixel of the rectangle.
  CHECK(c.list == g.list);
  uint32_t wrong = 0;
  for (uint32_t j = 0; j < kN; ++j) wrong += g.list[j] != (j / kRectW) * kFilmW + j % kRectW;
  CHECK(wrong == 0u);

  // Integer and constant fields: bitwise.
  for (const char* name : {"sample_id", "next_stage", "term", "ray_type", "flags", "bounces", "event_index",
                           "prev_ng", "prev_ns", "prev_dist", "ray_o", "beta", "r_u", "r_l", "eta_scale"}) {
    const auto& a = c.get(name);
    const auto& b = g.get(name);
    REQUIRE(a.size() == b.size());
    CHECK_MESSAGE(std::memcmp(a.data(), b.data(), a.size()) == 0, "field " << name << " differs");
  }

  // Directions (stride 3 floats of Real3): the D12 direction class.
  const agree::Stats dirs = agree::compare(as_results(g.get("ray_d"), 3), as_results(c.get("ray_d"), 3), agree::Metric::ANGLE);
  CHECK(agree::report("generate_camera", "ray_d nofma vs cpu float", dirs, agree::Cls::DIRECTION,
                      agree::NOFMA_VS_CPU_FLOAT).pass);
  // Film wavelengths (lambda_t, 4 lanes) and the cone (width, spread): the evaluation class.
  const agree::Stats lam = agree::compare(as_results(g.get("lambda_t"), 4), as_results(c.get("lambda_t"), 4), agree::Metric::REL_L2);
  CHECK(agree::report("generate_camera", "lambda_t nofma vs cpu float", lam, agree::Cls::EVAL,
                      agree::NOFMA_VS_CPU_FLOAT).pass);
  const agree::Stats cone = agree::compare(as_results(g.get("cone"), 2), as_results(c.get("cone"), 2), agree::Metric::REL_L2);
  CHECK(agree::report("generate_camera", "cone nofma vs cpu float", cone, agree::Cls::EVAL,
                      agree::NOFMA_VS_CPU_FLOAT).pass);
}

}  // TEST_SUITE
