// SPDX-License-Identifier: Apache-2.0
// Ref: 0007 V18; RULINGS.md 77 (NEE modes); host/session/test_hooks.h.
#include "host/session/test_hooks.h"

#include <cstdlib>
#include <stdexcept>

#include "kernel/kernel_globals.h"

namespace astroray::host {
namespace {

struct Row {
  const char* name;  // the variable is ASTRORAY_TEST_<name>
  uint32_t bit;
};
[[maybe_unused]] constexpr Row kRows[] = {
    {"PIXEL_CENTRE", ar::TEST_HOOK_PIXEL_CENTRE},
};

// ASTRORAY_TEST_NEE names one mode; "on" is the default MIS and sets no bit.
struct Mode {
  const char* value;
  uint32_t bits;
};
[[maybe_unused]] constexpr Mode kNeeModes[] = {
    {"on", 0u},
    {"off", ar::TEST_HOOK_NEE_OFF},
    {"light_only", ar::TEST_HOOK_NEE_LIGHT_ONLY},
};

#ifdef AR_GATE_HOOKS
// The variable ASTRORAY_TEST_<name>, or false when it is unset or empty.
bool env_value(const char* name, std::string* out) {
  const std::string var = std::string("ASTRORAY_TEST_") + name;
  char* v = nullptr;
  size_t n = 0;
  if (_dupenv_s(&v, &n, var.c_str()) != 0 || v == nullptr) return false;
  *out = v;
  std::free(v);
  return !out->empty();
}
#endif

}  // namespace

TestHooks read_test_hooks() {
  TestHooks out;
#ifdef AR_GATE_HOOKS
  std::string value;
  for (const Row& r : kRows) {
    if (!env_value(r.name, &value) || value == "0") continue;
    out.bits |= r.bit;
    out.active.push_back(r.name);
  }
  if (env_value("NEE", &value)) {
    const Mode* mode = nullptr;
    for (const Mode& m : kNeeModes)
      if (value == m.value) mode = &m;
    // A misspelt mode would quietly render the default estimator: refuse it.
    if (mode == nullptr)
      throw std::invalid_argument("ASTRORAY_TEST_NEE=" + value + " is not one of on, off, light_only");
    out.bits |= mode->bits;
    out.active.push_back("NEE=" + value);
  }
#endif
  return out;
}

}  // namespace astroray::host
