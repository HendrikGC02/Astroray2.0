// SPDX-License-Identifier: Apache-2.0
#include "host/ledger.h"

#include <cstdio>
#include <string>

namespace astroray::host {
namespace {

// Names in enum order, spelled as 0007 d14's budget table spells its rows.
constexpr const char* kNames[kLedgerCategoryCount] = {
    "main path pool",    "shadow pool",     "queue and sort indices", "shadow hit records",
    "shadow-pass split", "geodesic record", "random-walk record",     "film",
    "tables",            "display frame",   "denoiser",               "DLSS",
    "geometry",          "acceleration",    "textures",               "volumes",
};

std::string refusal_text(LedgerCategory c, uint64_t need, uint64_t available) {
  constexpr double kGiB = 1073741824.0;
  char buf[160];
  std::snprintf(buf, sizeof buf, "needs %.2f GiB for %s, %.2f GiB available", static_cast<double>(need) / kGiB,
                ledger_category_name(c), static_cast<double>(available) / kGiB);
  return buf;
}

}  // namespace

const char* ledger_category_name(LedgerCategory c) {
  const uint32_t i = static_cast<uint32_t>(c);
  if (i >= kLedgerCategoryCount) throw std::invalid_argument("ledger: unknown category " + std::to_string(i));
  return kNames[i];
}

AllocationRefused::AllocationRefused(LedgerCategory category, uint64_t need_bytes, uint64_t available_bytes)
    : std::runtime_error(refusal_text(category, need_bytes, available_bytes)),
      category_(category),
      need_(need_bytes),
      available_(available_bytes) {}

uint64_t Ledger::add(Owner owner, LedgerCategory category, uint64_t bytes) {
  if (bytes == 0) throw std::invalid_argument("ledger: an allocation of zero bytes");
  ledger_category_name(category);  // rejects an out-of-range category before recording
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t id = next_id_++;
  live_.emplace(id, LedgerRecord{owner, category, bytes});
  return id;
}

void Ledger::remove(uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (live_.erase(id) == 0)
    throw std::invalid_argument("ledger: free of allocation " + std::to_string(id) + " that is not live");
}

uint64_t Ledger::total() const {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t sum = 0;
  for (const auto& kv : live_) sum += kv.second.bytes;
  return sum;
}

uint64_t Ledger::scene_total(uint64_t scene) const {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t sum = 0;
  for (const auto& kv : live_)
    if (kv.second.owner.scene == scene) sum += kv.second.bytes;
  return sum;
}

uint64_t Ledger::session_total(uint64_t session) const {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t sum = 0;
  for (const auto& kv : live_)
    if (kv.second.owner.session == session) sum += kv.second.bytes;
  return sum;
}

uint64_t Ledger::category_total(LedgerCategory category) const {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t sum = 0;
  for (const auto& kv : live_)
    if (kv.second.category == category) sum += kv.second.bytes;
  return sum;
}

std::vector<LedgerRecord> Ledger::records() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LedgerRecord> out;
  out.reserve(live_.size());
  for (const auto& kv : live_) out.push_back(kv.second);
  return out;
}

}  // namespace astroray::host
