// SPDX-License-Identifier: Apache-2.0
// The per-render report table: keyed, deduplicated diagnostics (0001 D2.11).
// Ref: 0001 D2.11 (schema, levels, deduplication, scope and lifetime, polling).
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "../../include/astroray/astroray.h"

namespace astroray::host {

enum class ReportScope : uint32_t { SCENE, FILM, SESSION };

// At most this many distinct keys (0001 D2.11, PROVISIONAL).
constexpr uint32_t kReportMaxEntries = 512;

// One table per session. A session is driven by one thread at a time, but scene
// workers raise SCENE entries while a generation is built, so every member
// takes the internal mutex (0001 D2.6, 0007 d23).
class Report {
 public:
  // Adds a new entry or adds `count` to the existing one for (code, subject,
  // detail), updating last_generation and value. Returns AR_ERR_INVALID_ARGUMENT
  // for a code outside the table. A full table counts the refused key in `dropped`.
  ar_status raise(uint32_t code, uint64_t subject, const std::string& detail, uint64_t count,
                  const double value[2], uint64_t generation);

  // A generation became active: drop SCENE entries of the replaced or removed
  // subjects (all SCENE entries when `reset`) and GENERATION_REJECTED entries.
  void on_generation_active(uint64_t generation, const std::vector<uint64_t>& subjects, bool reset);
  // The film was reset: drop FILM entries.
  void on_film_reset();
  // Drops every entry of `code` and `subject` whatever its detail: a condition that cleared (a dev
  // kernel module that loads again, 0002 D15). Returns how many were dropped.
  uint32_t remove(uint32_t code, uint64_t subject);

  // Orders the table by level (descending), code, subject, detail and stores it.
  void snapshot();
  ar_status info(ar_report_info* out) const;
  ar_status entry(uint32_t index, ar_report_entry* out) const;

  uint64_t revision() const;

 private:
  using Key = std::tuple<uint32_t, uint64_t, std::string>;
  template <class Pred>
  void remove_if_locked(Pred pred);

  mutable std::mutex mutex_;
  std::map<Key, ar_report_entry> table_;
  uint64_t revision_ = 0;
  uint32_t dropped_ = 0;
  std::vector<ar_report_entry> snap_;
  ar_report_info snap_info_{};
};

// Level and scope of a code (1-based); asserts nothing, returns false when out of range.
bool report_code_traits(uint32_t code, uint32_t* level, ReportScope* scope);

}  // namespace astroray::host
