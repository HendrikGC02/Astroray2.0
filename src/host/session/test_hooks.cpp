// SPDX-License-Identifier: Apache-2.0
// Ref: 0007 V18; host/session/test_hooks.h.
#include "host/session/test_hooks.h"

#include <cstdlib>

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

}  // namespace

TestHooks read_test_hooks() {
  TestHooks out;
#ifdef AR_GATE_HOOKS
  for (const Row& r : kRows) {
    const std::string var = std::string("ASTRORAY_TEST_") + r.name;
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, var.c_str()) != 0 || v == nullptr) continue;
    const std::string value(v);
    std::free(v);
    if (value.empty() || value == "0") continue;
    out.bits |= r.bit;
    out.active.push_back(r.name);
  }
#endif
  return out;
}

}  // namespace astroray::host
