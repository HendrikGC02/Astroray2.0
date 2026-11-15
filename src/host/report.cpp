// SPDX-License-Identifier: Apache-2.0
// Ref: 0001 D2.11.
#include "report.h"

#include <algorithm>
#include <cstring>

namespace astroray::host {

namespace {

struct CodeRow {
  uint32_t level;
  ReportScope scope;
};

constexpr CodeRow kRows[] = {
#define AR_REPORT_CODE(NAME, LEVEL, SCOPE) {AR_LEVEL_##LEVEL, ReportScope::SCOPE},
#include "report_codes.def"
#undef AR_REPORT_CODE
};
constexpr uint32_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

// The def and the generated header must agree on every code number.
constexpr int kCounterBase = __COUNTER__ + 1;
#define AR_REPORT_CODE(NAME, LEVEL, SCOPE) \
  static_assert(AR_REPORT_##NAME == __COUNTER__ + 1 - kCounterBase, #NAME " out of table order");
#include "report_codes.def"
#undef AR_REPORT_CODE

}  // namespace

bool report_code_traits(uint32_t code, uint32_t* level, ReportScope* scope) {
  if (code < 1 || code > kRowCount) return false;
  *level = kRows[code - 1].level;
  *scope = kRows[code - 1].scope;
  return true;
}

ar_status Report::raise(uint32_t code, uint64_t subject, const std::string& detail, uint64_t count,
                        const double value[2], uint64_t generation) {
  uint32_t level;
  ReportScope scope;
  if (!report_code_traits(code, &level, &scope)) return AR_ERR_INVALID_ARGUMENT;
  // detail[128] is NUL-terminated, so 127 bytes at most; the key uses the stored text.
  std::string d = detail.substr(0, 127);
  std::lock_guard<std::mutex> lock(mutex_);
  Key key{code, subject, d};
  auto it = table_.find(key);
  if (it == table_.end()) {
    if (table_.size() >= kReportMaxEntries) {
      ++dropped_;
      ++revision_;  // `dropped` is part of the polled summary
      return AR_OK;
    }
    ar_report_entry e{};
    e.code = code;
    e.level = level;
    e.subject = subject;
    e.first_generation = generation;
    e.last_generation = generation;
    e.count = count;
    std::memcpy(e.detail, d.data(), d.size());
    it = table_.emplace(std::move(key), e).first;
  } else {
    it->second.count += count;
    it->second.last_generation = generation;
  }
  it->second.value[0] = value ? value[0] : 0.0;
  it->second.value[1] = value ? value[1] : 0.0;
  ++revision_;
  return AR_OK;
}

template <class Pred>
void Report::remove_if_locked(Pred pred) {
  bool removed = false;
  for (auto it = table_.begin(); it != table_.end();) {
    if (pred(it->second)) {
      it = table_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  if (removed) ++revision_;
}

void Report::on_generation_active(uint64_t, const std::vector<uint64_t>& subjects, bool reset) {
  std::lock_guard<std::mutex> lock(mutex_);
  remove_if_locked([&](const ar_report_entry& e) {
    if (e.code == AR_REPORT_GENERATION_REJECTED) return true;
    if (kRows[e.code - 1].scope != ReportScope::SCENE) return false;
    return reset || std::find(subjects.begin(), subjects.end(), e.subject) != subjects.end();
  });
}

void Report::on_film_reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  remove_if_locked([](const ar_report_entry& e) { return kRows[e.code - 1].scope == ReportScope::FILM; });
}

void Report::snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  snap_.clear();
  snap_.reserve(table_.size());
  for (const auto& kv : table_) snap_.push_back(kv.second);
  // The map order is (code, subject, detail); only the level needs sorting, and
  // stable_sort keeps the rest.
  std::stable_sort(snap_.begin(), snap_.end(),
                   [](const ar_report_entry& a, const ar_report_entry& b) { return a.level > b.level; });
  snap_info_ = {};
  snap_info_.revision = revision_;
  snap_info_.entry_count = static_cast<uint32_t>(snap_.size());
  snap_info_.dropped = dropped_;
  snap_info_.highest_level = snap_.empty() ? AR_LEVEL_INFO : snap_.front().level;
}

ar_status Report::info(ar_report_info* out) const {
  if (!out) return AR_ERR_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> lock(mutex_);
  *out = snap_info_;
  return AR_OK;
}

ar_status Report::entry(uint32_t index, ar_report_entry* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!out || index >= snap_.size()) return AR_ERR_INVALID_ARGUMENT;
  *out = snap_[index];
  return AR_OK;
}

uint64_t Report::revision() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return revision_;
}

}  // namespace astroray::host
