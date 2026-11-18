// SPDX-License-Identifier: Apache-2.0
// Gate-build test hooks: a session reads the environment variables ASTRORAY_TEST_<NAME> once at
// create and sets the matching bits of SessionUniforms::test_hooks. A hook changes sampling or
// sizes, never physics. Release builds (no AR_GATE_HOOKS) compile the reader out: the result is
// always empty, so the variables are never looked at. Each later hook adds one row to the table
// in test_hooks.cpp (NEE mode 106, pool sizes and radiance scale 076, GPU fault 099).
// Ref: 0007 V18 (PIXEL_CENTRE); CLAUDE.md rule 5 (no process-global mutable state: the values
// live in the session that read them).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace astroray::host {

struct TestHooks {
  uint32_t bits = 0;                // SessionUniforms::test_hooks
  std::vector<std::string> active;  // names of the hooks that are on, for the CLI's report
};

// Any value but "" and "0" turns a flag hook on. ASTRORAY_TEST_NEE takes on, off or light_only
// (RULINGS.md 77) and throws std::invalid_argument on any other non-empty value. Empty in builds
// without AR_GATE_HOOKS.
TestHooks read_test_hooks();

}  // namespace astroray::host
