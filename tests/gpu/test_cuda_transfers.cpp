// SPDX-License-Identifier: Apache-2.0
// The stream-ordered step transfers of CudaDevice (0002 D7 remedy (a)). upload_async stages its
// bytes in a 4 KiB pinned ring and does not wait, so a ring that wraps must first wait for the
// stream to consume the staged bytes; read_step_table must wait for its own copy before it reads
// the pinned area; and the step-table address read_step_table caches must follow the KernelGlobals
// through a rebind and through every way the host writes the bound globals (upload, upload_async,
// memset, copy). The timing cases hold the stream with tests/gpu/kernels/hold.cu, so the copies
// queued behind it certainly have not run when the host moves on: a missing wait shows every time.
// Ref: docs/decisions/0002-kernel-architecture.md D7 (step 2, remedy (a)); CUDA Driver API
// (cuMemcpyHtoDAsync and cuMemcpyDtoHAsync touch page-locked host memory when the copy runs).
#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "device/cuda/cuda_device.h"
#include "host/device.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr Owner kOwner{1, 0};
constexpr uint64_t kHoldNs = 50'000'000;  // 50 ms: far longer than the host side of any case below

// Queues ar_test_hold on the device's stream; it returns at once.
void hold_stream(ModuleDevice& dev) {
  const uint32_t module = dev.load_module_file(std::string(AR_TEST_KERNEL_DIR) + "/hold.cubin");
  uint64_t ns = kHoldNs;
  void* args[] = {&ns};
  dev.launch_module_kernel(module, "ar_test_hold", 1, 1, args);
}

// Two step tables and two KernelGlobals buffers naming them; refill_count tells the tables apart.
struct Tables {
  ModuleDevice& dev;
  DeviceBuffer tables, kg_a, kg_b;
  const ar::StepTable* first = nullptr;
  explicit Tables(ModuleDevice& d) : dev(d) {
    tables = dev.allocate(LedgerCategory::TABLES, 2 * sizeof(ar::StepTable), kOwner);
    kg_a = dev.allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), kOwner);
    kg_b = dev.allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), kOwner);
    ar::StepTable t[2] = {};
    t[0].refill_count = 11;
    t[1].refill_count = 22;
    dev.upload(tables, 0, t, sizeof t);
    first = static_cast<const ar::StepTable*>(tables.ptr);
    write(kg_a, first);
    write(kg_b, first + 1);
  }
  ~Tables() {
    dev.free(kg_b);
    dev.free(kg_a);
    dev.free(tables);
  }
  void write(const DeviceBuffer& kg_buf, const ar::StepTable* table) {
    ar::KernelGlobals kg{};
    kg.step_table = table;
    dev.upload(kg_buf, 0, &kg, sizeof kg);
  }
  uint32_t read() {
    ar::StepTable out{};
    dev.read_step_table(out);
    return out.refill_count;
  }
};

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("cuda_transfers/async uploads survive a ring wrap") {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev = make_cuda_device(ledger, 0);
  // 96 uploads of 112 bytes: 10.5 KiB, more than twice the ring, all queued behind the hold.
  constexpr uint32_t kUploads = 96, kBytes = 112;
  DeviceBuffer dst = dev->allocate(LedgerCategory::TABLES, uint64_t(kUploads) * kBytes, kOwner);
  dev->memset(dst, 0, 0, uint64_t(kUploads) * kBytes);
  hold_stream(*dev);
  std::vector<uint8_t> want(uint64_t(kUploads) * kBytes);
  for (uint32_t u = 0; u < kUploads; ++u) {
    uint8_t chunk[kBytes];
    for (uint32_t b = 0; b < kBytes; ++b) chunk[b] = uint8_t(u * 7u + b + 1u);
    std::memcpy(want.data() + uint64_t(u) * kBytes, chunk, kBytes);
    dev->upload_async(dst, uint64_t(u) * kBytes, chunk, kBytes);  // chunk is reused at once
  }
  dev->synchronize();
  std::vector<uint8_t> got(want.size());
  dev->download(got.data(), dst, 0, got.size());
  uint32_t wrong = 0;
  for (uint32_t u = 0; u < kUploads; ++u)
    wrong += std::memcmp(got.data() + uint64_t(u) * kBytes, want.data() + uint64_t(u) * kBytes, kBytes) != 0;
  CHECK(wrong == 0u);
  CHECK_THROWS_AS(dev->upload_async(dst, 0, want.data(), 4096), std::invalid_argument);  // above the ring
  dev->free(dst);
}

TEST_CASE("cuda_transfers/step table read waits for its copy") {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev = make_cuda_device(ledger, 0);
  Tables t(*dev);
  dev->set_kernel_globals(t.kg_a);
  CHECK(t.read() == 11u);  // the pinned readback area now holds table A
  // A new value for table A, queued behind the hold: the read must wait for it.
  hold_stream(*dev);
  ar::StepTable next{};
  next.refill_count = 33;
  dev->upload_async(t.tables, 0, &next, sizeof next);
  CHECK(t.read() == 33u);
}

TEST_CASE("cuda_transfers/step table follows the kernel globals") {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev = make_cuda_device(ledger, 0);
  Tables t(*dev);
  dev->set_kernel_globals(t.kg_a);
  CHECK(t.read() == 11u);
  // A rebind to globals that name the other table.
  dev->set_kernel_globals(t.kg_b);
  CHECK(t.read() == 22u);
  // Writes into the bound globals, by each transfer that can reach them.
  t.write(t.kg_b, t.first);  // upload (the scheduler re-uploads its globals every pass)
  CHECK(t.read() == 11u);
  const ar::StepTable* second = t.first + 1;
  dev->upload_async(t.kg_b, offsetof(ar::KernelGlobals, step_table), &second, sizeof second);
  CHECK(t.read() == 22u);
  dev->copy(t.kg_b, 0, t.kg_a, 0, sizeof(ar::KernelGlobals));  // kg_a names table A
  CHECK(t.read() == 11u);
  dev->memset(t.kg_b, offsetof(ar::KernelGlobals, step_table), 0, sizeof second);  // no table at all
  CHECK_THROWS_AS(t.read(), std::logic_error);
}

}  // TEST_SUITE
