// SPDX-License-Identifier: Apache-2.0
// The VRAM ledger: one record per device allocation, tagged by category, with totals
// per scene, session and category (0007 d14). It only records and sums; whether an
// allocation may proceed is the device's hook (Device::may_allocate) and a refusal
// is an AllocationRefused. Trim and spill are later tasks: the ledger fails only.
// Ref: docs/decisions/0007-blender-integration.md d14 (ledger, budget table, "fail"
// message "needs X GiB for <category>, Y available"); 0002 D13, D15 (allocation row).
#pragma once
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "astroray/astroray.h"

namespace astroray::host {

// Every row of 0007 d14's budget table, then the scene categories that table excludes
// (its `tables` row is the one category both lists share), then DEVICE_CONTEXT: device memory
// the driver or OptiX takes outside any allocation of ours, recorded as the drop in free bytes
// around the call that takes it (0002 D13's check compares the ledger total with that drop).
enum class LedgerCategory : uint8_t {
  MAIN_PATH_POOL,
  SHADOW_POOL,
  QUEUE_SORT_INDICES,
  SHADOW_HIT_RECORDS,
  SHADOW_PASS_SPLIT,
  GEODESIC_RECORD,
  RANDOM_WALK_RECORD,
  FILM,
  TABLES,
  DISPLAY_FRAME,
  DENOISER,
  DLSS,
  GEOMETRY,
  ACCELERATION,
  TEXTURES,
  VOLUMES,
  DEVICE_CONTEXT,
};
inline constexpr uint32_t kLedgerCategoryCount = static_cast<uint32_t>(LedgerCategory::DEVICE_CONTEXT) + 1u;

// The name used in messages. Throws std::invalid_argument for a value outside the enum.
const char* ledger_category_name(LedgerCategory c);

// Who an allocation belongs to. A scene-level allocation (geometry, textures, ...) has
// session 0; ids are the host's handle ids and 0 means none.
struct Owner {
  uint64_t scene = 0;
  uint64_t session = 0;
};

struct LedgerRecord {
  Owner owner;
  LedgerCategory category;
  uint64_t bytes;
};

// Thrown when the device refuses an allocation. The capi layer turns it into
// AR_ERR_OUT_OF_MEMORY and a SESSION_FAILED entry whose detail is what().
class AllocationRefused : public std::runtime_error {
 public:
  AllocationRefused(LedgerCategory category, uint64_t need_bytes, uint64_t available_bytes);
  ar_status status() const noexcept { return AR_ERR_OUT_OF_MEMORY; }
  LedgerCategory category() const noexcept { return category_; }
  uint64_t need() const noexcept { return need_; }
  uint64_t available() const noexcept { return available_; }

 private:
  LedgerCategory category_;
  uint64_t need_;
  uint64_t available_;
};

// Thread safe: scene workers allocate concurrently. One ledger per device owner (no
// process-global ledger); the caller keeps it alive longer than every Device using it.
class Ledger {
 public:
  // Returns the record id (never 0). Throws std::invalid_argument for zero bytes or a
  // category outside the enum.
  uint64_t add(Owner owner, LedgerCategory category, uint64_t bytes);
  // Throws std::invalid_argument for an id that is not live (a double free).
  void remove(uint64_t id);

  uint64_t total() const;
  uint64_t scene_total(uint64_t scene) const;
  uint64_t session_total(uint64_t session) const;
  uint64_t category_total(LedgerCategory category) const;
  std::vector<LedgerRecord> records() const;  // live records, in no particular order

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, LedgerRecord> live_;
  uint64_t next_id_ = 1;
};

}  // namespace astroray::host
