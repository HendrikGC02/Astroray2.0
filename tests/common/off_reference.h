// SPDX-License-Identifier: Apache-2.0
// The machine rule for a timing CHECK inside a test (owner decision 47 c; tools/machine_id.py timing_ok is the
// Python twin). The tools set ASTRORAY_OFF_REFERENCE to the machine id when the machine is not the reference
// one; with it set, a measurement over its limit prints one `off-reference <id> <row>: ...` line and the check
// passes. Unset (the reference machine, hosted ci, a hand run) an overrun fails exactly as before.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace astroray::test {

// `id` is the standing (null or empty: the reference machine); `row` is the budget row and the measurement
// ("B-21 cancel during denoise").
inline bool timing_ok_for(const char* id, const char* row, double measured, double limit, const char* unit = "ms") {
  if (measured <= limit) return true;
  if (!id || !*id) return false;
  std::printf("off-reference %s %s: %.4g %s against %.4g %s\n", id, row, measured, unit, limit, unit);
  return true;
}

inline bool timing_ok(const char* row, double measured, double limit, const char* unit = "ms") {
  char* v = nullptr;
  size_t n = 0;
  std::string id;
  if (_dupenv_s(&v, &n, "ASTRORAY_OFF_REFERENCE") == 0 && v) {
    id = v;
    std::free(v);
  }
  return timing_ok_for(id.c_str(), row, measured, limit, unit);
}

}  // namespace astroray::test
