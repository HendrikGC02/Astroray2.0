// SPDX-License-Identifier: Apache-2.0
// The stream-ordered step transfers of CudaDevice (0002 D7 remedy (a)): upload_async stages its
// bytes in a 4 KiB pinned ring and does not wait, so a ring that wraps must wait for the stream to
// consume the staged bytes first. Here the stream is held busy by large device copies while more
// uploads than the ring holds are queued; every destination must still receive its own bytes. The
// step-table address read_step_table caches follows the KernelGlobals: a write into the bound
// globals and a rebind to another buffer both make the next read use the new table.
// Ref: docs/decisions/0002-kernel-architecture.md D7 (step 2, remedy (a)); CUDA Driver API
// (cuMemcpyHtoDAsync reads page-locked host memory when the copy runs, not when it is queued).
#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "device/cuda/cuda_device.h"
#include "host/device.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr Owner kOwner{1, 0};

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("cuda_transfers/async uploads survive a ring wrap") {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev = make_cuda_device(ledger, 0);
  // 256 MiB device-to-device copies, queued ahead of the uploads, keep the stream busy for
  // milliseconds: without the wait at the wrap the host would overwrite staged bytes long before
  // the stream reached their copies.
  constexpr uint64_t kBig = 256ull << 20;
  DeviceBuffer big_a = dev->allocate(LedgerCategory::TABLES, kBig, kOwner);
  DeviceBuffer big_b = dev->allocate(LedgerCategory::TABLES, kBig, kOwner);
  // 96 uploads of 112 bytes: 10.5 KiB, more than twice the ring.
  constexpr uint32_t kUploads = 96, kBytes = 112;
  DeviceBuffer dst = dev->allocate(LedgerCategory::TABLES, uint64_t(kUploads) * kBytes, kOwner);
  dev->memset(dst, 0, 0, uint64_t(kUploads) * kBytes);
  for (int i = 0; i < 4; ++i) dev->copy(big_b, 0, big_a, 0, kBig);
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
  dev->free(big_b);
  dev->free(big_a);
}

TEST_CASE("cuda_transfers/step table follows the kernel globals") {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev = make_cuda_device(ledger, 0);
  DeviceBuffer tables = dev->allocate(LedgerCategory::TABLES, 2 * sizeof(ar::StepTable), kOwner);
  DeviceBuffer kg_a = dev->allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), kOwner);
  DeviceBuffer kg_b = dev->allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), kOwner);
  ar::StepTable t[2] = {};
  t[0].refill_count = 11;
  t[1].refill_count = 22;
  dev->upload(tables, 0, t, sizeof t);
  const auto* first = static_cast<const ar::StepTable*>(tables.ptr);
  ar::KernelGlobals kg{};
  kg.step_table = first;
  dev->upload(kg_a, 0, &kg, sizeof kg);
  kg.step_table = first + 1;
  dev->upload(kg_b, 0, &kg, sizeof kg);

  ar::StepTable out{};
  dev->set_kernel_globals(kg_a);
  dev->read_step_table(out);
  CHECK(out.refill_count == 11u);
  // A rebind to globals that name the other table.
  dev->set_kernel_globals(kg_b);
  dev->read_step_table(out);
  CHECK(out.refill_count == 22u);
  // A write into the bound globals (the scheduler re-uploads them every pass).
  kg.step_table = first;
  dev->upload(kg_b, 0, &kg, sizeof kg);
  dev->read_step_table(out);
  CHECK(out.refill_count == 11u);
  dev->free(kg_b);
  dev->free(kg_a);
  dev->free(tables);
}

}  // TEST_SUITE
