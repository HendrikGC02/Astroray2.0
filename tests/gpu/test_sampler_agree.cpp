// SPDX-License-Identifier: Apache-2.0
// The first GPU tests: the sampler is bitwise equal on the CPU and the GPU (0003 check 24,
// 0009 d17(i)), and CudaDevice's own contract through the Device seam (memory, a launch
// of the selftest module, every refusal with its exact message, the reserve rule).
// Expected values are typed from the setups below or from the Python model of 0003 d23
// (fnv constants, as in tests/cpu/test_sampler.cpp), never read back from the code.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d23; 0002 D4, D14, D15;
// 0007 d14 (reserve rule, R = 1.5 GiB); 0009 d11, d17(i).
#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "host/device.h"
#include "host/device_probe.h"
#include "sample/sobol.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint64_t kGiB = 1ull << 30;

std::string module_path(const char* name) { return std::string(AR_TEST_KERNEL_DIR) + "/" + name + ".cubin"; }

// One device per test, created explicitly (0009 d11); a failure to create it fails the test.
std::unique_ptr<ModuleDevice> open_device(Ledger& ledger) { return make_cuda_device(ledger, 0); }

uint64_t ptr_of(const DeviceBuffer& b) { return reinterpret_cast<uint64_t>(b.ptr); }

struct Tuple {
  uint32_t id, index, stream, d, seed;
};
// The draws of tests/cpu/test_sampler.cpp's identity check, so the fnv constants below are
// the ones the Python model produced for them.
Tuple draw_tuple(uint32_t i) {
  return Tuple{i * 0x9e3779b1u, (i % 3u == 0u) ? (i % 4096u) : (i * 0x85ebca6bu + 12345u), i & 3u,
               (i * 13u) % 200u, (i * 0xc2b2ae35u) ^ 0xdeadbeefu};
}

uint64_t fnv_add(uint64_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h = (h ^ (v & 0xffu)) * 0x100000001b3ull;
    v >>= 8;
  }
  return h;
}
uint32_t u24(uint32_t float_bits) {
  return static_cast<uint32_t>(static_cast<double>(ar::bit_cast<float>(float_bits)) * 16777216.0);
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("sampler/cpu gpu bitwise") {
  constexpr uint32_t kN = 1000000;
  // fnv1a-64 over u24 of every draw, from the Python model (ref_sampler.py main loop).
  constexpr uint64_t kFnvSobol = 0x4490de368401daefull;
  constexpr uint64_t kFnvIndependent = 0x7b0aca786001c887ull;

  std::vector<Tuple> tuples(kN);
  for (uint32_t i = 0; i < kN; ++i) tuples[i] = draw_tuple(i);

  Ledger ledger;
  auto dev = open_device(ledger);
  DeviceBuffer in = dev->allocate(LedgerCategory::TABLES, sizeof(Tuple) * kN, Owner{});
  DeviceBuffer out = dev->allocate(LedgerCategory::TABLES, 2ull * sizeof(uint32_t) * kN, Owner{});
  dev->upload(in, 0, tuples.data(), sizeof(Tuple) * kN);
  dev->memset(out, 0, 0xFF, out.bytes);

  const uint32_t module = dev->load_module_file(module_path("sampler_agree"));
  uint64_t in_p = ptr_of(in), out_p = ptr_of(out);
  uint32_t n = kN;
  void* args[] = {&in_p, &out_p, &n};
  dev->launch_module_kernel(module, "ar_sampler_agree", (kN + 255) / 256, 256, args);
  dev->synchronize();

  std::vector<uint32_t> gpu(2ull * kN);
  dev->download(gpu.data(), out, 0, out.bytes);

  uint32_t compared = 0, differences = 0;
  uint64_t hs = 0xcbf29ce484222325ull, hi = hs;
  for (uint32_t i = 0; i < kN; ++i) {
    const Tuple& t = tuples[i];
    const uint32_t cpu_s = ar::bit_cast<uint32_t>(ar::sample(t.id, t.index, t.stream, t.d, t.seed));
    const uint32_t cpu_i = ar::bit_cast<uint32_t>(ar::sample_independent(t.id, t.index, t.stream, t.d, t.seed));
    compared += 1;
    if (cpu_s != gpu[2ull * i]) ++differences;
    if (cpu_i != gpu[2ull * i + 1]) ++differences;
    hs = fnv_add(hs, u24(gpu[2ull * i]));
    hi = fnv_add(hi, u24(gpu[2ull * i + 1]));
  }
  std::printf("compared=%u differences=%u\n", compared, differences);
  CHECK(compared == 1000000u);
  CHECK(differences == 0u);
  // The agreement is not between two copies of one bug: the GPU draws are the model's.
  CHECK(hs == kFnvSobol);
  CHECK(hi == kFnvIndependent);

  dev->free(in);
  dev->free(out);
  CHECK(ledger.total() == 0);
}

TEST_CASE("cuda_device/selftest launch and memory") {
  constexpr uint32_t kN = 1000;
  constexpr uint32_t kTail = 16;  // floats after the output that the kernel must not touch
  Ledger ledger;
  auto dev = open_device(ledger);
  CHECK(dev->kind() == AR_DEVICE_CUDA);

  std::vector<float> in(kN);
  for (uint32_t i = 0; i < kN; ++i) in[i] = static_cast<float>(i * i);  // exact below 2^24
  DeviceBuffer b_in = dev->allocate(LedgerCategory::TABLES, sizeof(float) * kN, Owner{1, 0});
  DeviceBuffer b_out = dev->allocate(LedgerCategory::FILM, sizeof(float) * (kN + kTail), Owner{1, 0});
  DeviceBuffer b_lanes = dev->allocate(LedgerCategory::FILM, sizeof(uint32_t) * kN, Owner{1, 0});
  DeviceBuffer b_acc = dev->allocate(LedgerCategory::FILM, 2 * sizeof(uint32_t), Owner{1, 0});
  CHECK(ledger.scene_total(1) == sizeof(float) * kN + sizeof(float) * (kN + kTail) + sizeof(uint32_t) * kN + 8);

  dev->upload(b_in, 0, in.data(), sizeof(float) * kN);
  dev->memset(b_out, 0, 0xFF, b_out.bytes);
  dev->memset(b_acc, 0, 0, b_acc.bytes);

  const uint32_t module = dev->load_module_file(module_path("selftest"));
  uint64_t in_p = ptr_of(b_in), out_p = ptr_of(b_out), lanes_p = ptr_of(b_lanes);
  uint64_t counter_p = ptr_of(b_acc), ballot_p = ptr_of(b_acc) + 4;
  uint32_t n = kN;
  void* args[] = {&in_p, &out_p, &lanes_p, &counter_p, &ballot_p, &n};
  dev->launch_module_kernel(module, "ar_selftest", (kN + 255) / 256, 256, args);
  dev->synchronize();

  std::vector<float> out(kN + kTail);
  std::vector<uint32_t> lanes(kN), acc(2);
  dev->download(out.data(), b_out, 0, b_out.bytes);
  dev->download(lanes.data(), b_lanes, 0, b_lanes.bytes);
  dev->download(acc.data(), b_acc, 0, b_acc.bytes);

  uint32_t wrong_out = 0, wrong_lane = 0, touched_tail = 0;
  for (uint32_t i = 0; i < kN; ++i) {
    if (out[i] != static_cast<float>(i) + 1.0f) ++wrong_out;  // sqrt(i*i) + 1, correctly rounded
    if (lanes[i] != i % 32u) ++wrong_lane;
  }
  for (uint32_t i = kN; i < kN + kTail; ++i) {
    uint32_t bits;
    std::memcpy(&bits, &out[i], 4);
    if (bits != 0xFFFFFFFFu) ++touched_tail;
  }
  CHECK(wrong_out == 0u);
  CHECK(wrong_lane == 0u);
  CHECK(touched_tail == 0u);
  CHECK(acc[0] == kN);           // every thread with i < n ran once
  CHECK(acc[1] == 0xFFFFFFFFu);  // the largest warp ballot: a full warp of i < n

  // A partial download and an offset upload hit the right bytes.
  uint32_t patch[2] = {7u, 9u};
  dev->upload(b_lanes, 4 * 10, patch, sizeof(patch));
  uint32_t back[4] = {};
  dev->download(back, b_lanes, 4 * 9, sizeof(back));
  CHECK(back[0] == 9u % 32u);  // lane of thread 9 is untouched
  CHECK(back[1] == 7u);
  CHECK(back[2] == 9u);
  CHECK(back[3] == 12u);

  // read_step_table copies the table behind KernelGlobals::step_table.
  ar::StepTable table_in = {};
  table_in.stage[5] = ar::StepEntry{11u, 22u, 33u};
  table_in.refill_count = 44u;
  DeviceBuffer b_table = dev->allocate(LedgerCategory::QUEUE_SORT_INDICES, sizeof(ar::StepTable), Owner{1, 0});
  dev->upload(b_table, 0, &table_in, sizeof(table_in));
  ar::KernelGlobals kg = {};
  kg.step_table = static_cast<const ar::StepTable*>(b_table.ptr);
  DeviceBuffer b_kg = dev->allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), Owner{1, 0});
  dev->upload(b_kg, 0, &kg, sizeof(kg));
  dev->set_kernel_globals(b_kg);
  ar::StepTable table_out = {};
  dev->read_step_table(table_out);
  CHECK(table_out.stage[5].start == 11u);
  CHECK(table_out.stage[5].count == 22u);
  CHECK(table_out.stage[5].shadow_base == 33u);
  CHECK(table_out.refill_count == 44u);

  for (DeviceBuffer* b : {&b_in, &b_out, &b_lanes, &b_acc, &b_table, &b_kg}) dev->free(*b);
  CHECK(ledger.total() == 0);
  CHECK(b_in.ptr == nullptr);
}

TEST_CASE("cuda_device/refusals") {
  Ledger ledger;
  auto dev = open_device(ledger);

  // No globals bound: both launches and the step-table read refuse.
  const std::string unbound = "CudaDevice: no KernelGlobals bound (call set_kernel_globals first)";
  ar::StepTable table;
  CHECK_THROWS_WITH_AS(dev->launch(ar::Stage::SHADE_VOLUME, 1), unbound.c_str(), std::logic_error);
  CHECK_THROWS_WITH_AS(dev->launch(KernelId::FILM_RESOLVE, 1), unbound.c_str(), std::logic_error);
  CHECK_THROWS_WITH_AS(dev->read_step_table(table), unbound.c_str(), std::logic_error);

  // A KernelGlobals buffer that is too small, and a null one.
  DeviceBuffer small = dev->allocate(LedgerCategory::TABLES, 8, Owner{});
  const std::string too_small =
      "CudaDevice: the KernelGlobals buffer holds 8 bytes, need " + std::to_string(sizeof(ar::KernelGlobals));
  CHECK_THROWS_WITH_AS(dev->set_kernel_globals(small), too_small.c_str(), std::invalid_argument);
  const std::string null_buffer =
      "CudaDevice: the KernelGlobals buffer holds 0 bytes, need " + std::to_string(sizeof(ar::KernelGlobals));
  CHECK_THROWS_WITH_AS(dev->set_kernel_globals(DeviceBuffer()), null_buffer.c_str(), std::invalid_argument);

  // With globals bound, the two terminal stages are not launchable and the table is empty.
  DeviceBuffer zero = dev->allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), Owner{});
  dev->memset(zero, 0, 0, zero.bytes);
  dev->set_kernel_globals(zero);
  CHECK_THROWS_WITH_AS(dev->launch(ar::Stage::TERMINATED, 1), "CudaDevice: stage 13 is not launchable",
                       std::invalid_argument);
  CHECK_THROWS_WITH_AS(dev->launch(ar::Stage::SHADOW_TERMINATED, 1), "CudaDevice: stage 14 is not launchable",
                       std::invalid_argument);
  CHECK_THROWS_WITH_AS(dev->read_step_table(table), "CudaDevice: the bound KernelGlobals has no step table",
                       std::logic_error);

  // Buffer ranges are checked before any CUDA call.
  uint8_t byte = 0;
  CHECK_THROWS_AS(dev->upload(small, 8, &byte, 1), std::out_of_range);
  CHECK_THROWS_AS(dev->download(&byte, small, 0, 9), std::out_of_range);
  CHECK_THROWS_AS(dev->memset(small, 4, 0, 5), std::out_of_range);

  // The test-module entry: a missing file, a module handle never loaded, an unknown kernel.
  const std::string missing = module_path("no_such_module");
  const std::string missing_msg = "cannot read kernel module file " + missing;
  CHECK_THROWS_WITH_AS(dev->load_module_file(missing), missing_msg.c_str(), DeviceError);
  CHECK_THROWS_WITH_AS(dev->launch_module_kernel(99, "ar_selftest", 1, 1, nullptr),
                       "CudaDevice: module handle 99 was not loaded", std::invalid_argument);
  const uint32_t module = dev->load_module_file(module_path("selftest"));
  CHECK_THROWS_WITH_AS(dev->launch_module_kernel(module, "ar_selftest", 0, 1, nullptr),
                       "CudaDevice: a launch needs at least one block and one thread", std::invalid_argument);
  try {
    dev->launch_module_kernel(module, "ar_no_such_kernel", 1, 1, nullptr);
    FAIL("an unknown kernel name must throw");
  } catch (const DeviceError& e) {
    CHECK(e.status() == AR_ERR_DEVICE);
    CHECK(!e.sticky());
    const std::string want = "CUDA error in cuModuleGetFunction (ar_no_such_kernel): CUDA_ERROR_NOT_FOUND: ";
    CHECK(std::string(e.what()).compare(0, want.size(), want) == 0);
  }

  dev->free(small);
  dev->free(zero);
}

TEST_CASE("cuda_device/reserve rule refusal") {
  Ledger ledger;
  auto dev = open_device(ledger);
  uint64_t total = 0;
  for (const DeviceRecord& r : list_devices().devices)
    if (r.kind == AR_DEVICE_CUDA && r.index == 0) total = r.memory_bytes;
  REQUIRE(total > 2 * kGiB);

  constexpr uint64_t kHuge = 1ull << 40;  // 1 TiB: more than any card has free
  try {
    dev->allocate(LedgerCategory::GEOMETRY, kHuge, Owner{});
    FAIL("a 1 TiB allocation must be refused");
  } catch (const AllocationRefused& e) {
    CHECK(e.status() == AR_ERR_OUT_OF_MEMORY);
    CHECK(e.category() == LedgerCategory::GEOMETRY);
    CHECK(e.need() == kHuge);
    // available() is the raw free VRAM at the refusal, never more than the card has.
    CHECK(e.available() <= total);
    CHECK(e.available() > 0);
    char want[160];
    std::snprintf(want, sizeof want, "needs 1024.00 GiB for geometry, %.2f GiB free (reserve 1.50 GiB)",
                  static_cast<double>(e.available()) / static_cast<double>(kGiB));
    CHECK(std::string(e.what()) == want);
  }
  CHECK(ledger.total() == 0);
  CHECK(ledger.records().empty());

  // A small allocation passes the rule and is recorded, then freed.
  DeviceBuffer b = dev->allocate(LedgerCategory::TABLES, 1ull << 20, Owner{3, 4});
  CHECK(b.ptr != nullptr);
  CHECK(ledger.total() == (1ull << 20));
  CHECK(ledger.session_total(4) == (1ull << 20));
  dev->free(b);
  CHECK(ledger.total() == 0);
  CHECK(b.ptr == nullptr);
  CHECK_THROWS_AS(dev->allocate(LedgerCategory::TABLES, 0, Owner{}), std::invalid_argument);
}

TEST_CASE("cuda_device/reserve rule decision") {
  // R is the record's 1.5 GiB (0007 d14), exactly.
  CHECK(kCudaReserveBytes == 1610612736ull);
  const uint64_t R = kCudaReserveBytes;
  const uint64_t free10 = 10 * kGiB;  // free - R = 8.5 GiB
  const uint64_t edge = free10 - R;
  CHECK(reserve_allows(edge - 1, free10, R));       // just below free - R
  CHECK(reserve_allows(edge, free10, R));           // need + R == free is allowed
  CHECK(!reserve_allows(edge + 1, free10, R));      // just above free - R
  CHECK(!reserve_allows(9 * kGiB, free10, R));      // between free - R and free: the reserve refuses it
  CHECK(!reserve_allows(free10, free10, R));        // all of the free memory
  CHECK(!reserve_allows(free10 + 1, free10, R));
  CHECK(!reserve_allows(~0ull, free10, R));         // no overflow in need + R
  CHECK(reserve_allows(0, R, R));                   // free == R: only an empty request fits
  CHECK(!reserve_allows(1, R, R));
  CHECK(!reserve_allows(0, R - 1, R));              // free below the reserve: nothing fits
  CHECK(!reserve_allows(0, 0, R));
  CHECK(reserve_allows(1, 1, 0));                   // with no reserve the rule is need <= free
  CHECK(!reserve_allows(2, 1, 0));
}

TEST_CASE("cuda_device/failure messages") {
  // A sticky result names the call and, after a launch, the kernel (0002 D15).
  CHECK(cuda_failure_message(700, "CUDA_ERROR_ILLEGAL_ADDRESS", "an illegal memory access was encountered",
                             "cuEventSynchronize", "shade_surface") ==
        "GPU error: CUDA_ERROR_ILLEGAL_ADDRESS: an illegal memory access was encountered in cuEventSynchronize "
        "(shade_surface). Save your work and restart Blender.");
  CHECK(cuda_failure_message(719, "CUDA_ERROR_LAUNCH_FAILED", "unspecified launch failure", "cuEventRecord", nullptr) ==
        "GPU error: CUDA_ERROR_LAUNCH_FAILED: unspecified launch failure in cuEventRecord. "
        "Save your work and restart Blender.");
  CHECK(cuda_failure_message(719, "CUDA_ERROR_LAUNCH_FAILED", "unspecified launch failure", "cuEventRecord", "") ==
        "GPU error: CUDA_ERROR_LAUNCH_FAILED: unspecified launch failure in cuEventRecord. "
        "Save your work and restart Blender.");
  // A recoverable result keeps the call and the kernel and is not "GPU error".
  CHECK(cuda_failure_message(701, "CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES", "too many resources requested for launch",
                             "cuLaunchKernel", "film_resolve") ==
        "CUDA error in cuLaunchKernel (film_resolve): CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES: too many resources "
        "requested for launch");
  CHECK(cuda_failure_message(1, nullptr, nullptr, "cuMemAlloc", nullptr) ==
        "CUDA error in cuMemAlloc: CUDA_ERROR_?: unknown error");
}

TEST_CASE("cuda_device/sticky error classification") {
  // CUresult values from cuda.h that are documented as needing a process restart.
  for (int sticky : {214, 226, 700, 702, 710, 714, 715, 716, 717, 718, 719, 721, 810, 911})
    CHECK_MESSAGE(cuda_result_is_sticky(sticky), "CUresult ", sticky);
  // Errors a caller can recover from: success, invalid value, out of memory, not found,
  // too many resources for one launch, destroyed context, unknown.
  for (int other : {0, 1, 2, 500, 701, 709, 999}) CHECK_MESSAGE(!cuda_result_is_sticky(other), "CUresult ", other);

  const DeviceError sticky(DeviceError::sticky_text("CUDA_ERROR_ILLEGAL_ADDRESS: an illegal memory access was encountered"),
                           true);
  CHECK(std::string(sticky.what()) ==
        "GPU error: CUDA_ERROR_ILLEGAL_ADDRESS: an illegal memory access was encountered. "
        "Save your work and restart Blender.");
  CHECK(sticky.sticky());
  CHECK(sticky.status() == AR_ERR_DEVICE);
  const DeviceError plain("launch refused");
  CHECK(!plain.sticky());
  CHECK(plain.status() == AR_ERR_DEVICE);
}

}  // TEST_SUITE
