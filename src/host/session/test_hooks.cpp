// SPDX-License-Identifier: Apache-2.0
// Ref: 0007 V18; RULINGS.md 77 (NEE modes); 0004 D10 (the light tree's alias-table oracle); 0002 D5 (shadow-slot reservation, k per producer);
// host/session/test_hooks.h.
#include "host/session/test_hooks.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "host/scheduler.h"
#include "kernel/kernel_globals.h"
#include "kernel/launch_config.h"

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

// A positive decimal integer no larger than `max`, else throws naming the variable.
uint64_t positive(const std::string& var, const std::string& text, uint64_t max) {
  uint64_t v = 0;
  bool ok = !text.empty() && text.size() <= 19;
  for (char c : text) ok = ok && c >= '0' && c <= '9';
  if (ok) v = std::stoull(text);
  if (!ok || v == 0 || v > max)
    throw std::invalid_argument("ASTRORAY_TEST_" + var + ": '" + text + "' is not an integer in [1, " +
                                std::to_string(max) + "]");
  return v;
}

// The largest k of any producer stage: a shadow pool below it never fits one producer item.
uint32_t max_shadow_items() {
  uint32_t k = 0;
  for (uint32_t s = 0; s < ar::kStageCount; ++s) k = std::max(k, ar::stage_shadow_items(static_cast<ar::Stage>(s)));
  return k;
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
  if (env_value("LIGHT_SAMPLER", &value)) {
    // "tree" is the default sampler and sets no bit; "alias" selects the tree's oracle.
    if (value != "tree" && value != "alias")
      throw std::invalid_argument("ASTRORAY_TEST_LIGHT_SAMPLER=" + value + " is not one of tree, alias");
    if (value == "alias") out.bits |= ar::TEST_HOOK_LIGHT_ALIAS;
    out.active.push_back("LIGHT_SAMPLER=" + value);
  }
  if (env_value("POOL", &value)) {
    const size_t comma = value.find(',');
    // The main pool's ceiling is 0002 D5's; the shadow pool's is twice it, as the scheduler's default.
    out.pool_main = positive("POOL", value.substr(0, comma), kMaxPoolCapacity);
    if (comma != std::string::npos) {
      out.pool_shadow = positive("POOL", value.substr(comma + 1), 2 * kMaxPoolCapacity);
      if (out.pool_shadow < max_shadow_items())
        throw std::invalid_argument("ASTRORAY_TEST_POOL: shadow capacity " + std::to_string(out.pool_shadow) +
                                    " is below the " + std::to_string(max_shadow_items()) +
                                    " shadow items one producer item may emit per step");
    }
    out.active.push_back("POOL=" + value);
  }
  if (env_value("THREADS", &value)) {
    out.threads = uint32_t(positive("THREADS", value, 256));
    out.active.push_back("THREADS=" + value);
  }
  if (env_value("RADIANCE_SCALE", &value)) {
    // Only s = 1 is a test of 0003 check 15; any other value would be read as a scale and is refused.
    if (value != "1") throw std::invalid_argument("ASTRORAY_TEST_RADIANCE_SCALE=" + value + " is not 1");
    out.unit_radiance_scale = true;
    out.active.push_back("RADIANCE_SCALE=1");
  }
  // 0002 D15 injections. A flag hook is exactly "1": any other value would inject nothing, silently.
  const struct {
    const char* name;
    bool* flag;
  } flags[] = {{"CORRUPT_CUBIN", &out.corrupt_cubin}, {"FAIL_INIT", &out.fail_init}, {"GPU_FAULT", &out.gpu_fault},
                                          {"LAUNCH_TIMING", &out.launch_timing}};
  for (const auto& f : flags) {
    if (!env_value(f.name, &value)) continue;
    if (value != "1") throw std::invalid_argument(std::string("ASTRORAY_TEST_") + f.name + "=" + value + " is not 1");
    *f.flag = true;
    out.active.push_back(std::string(f.name) + "=1");
  }
  if (env_value("DRIVER_VERSION", &value)) {
    out.driver_version = uint32_t(positive("DRIVER_VERSION", value, 99999));
    out.active.push_back("DRIVER_VERSION=" + value);
  }
  if (env_value("COMPUTE_CAPABILITY", &value)) {
    out.compute_capability = uint32_t(positive("COMPUTE_CAPABILITY", value, 999));
    out.active.push_back("COMPUTE_CAPABILITY=" + value);
  }
  if (env_value("DENOISER_FAIL", &value)) {
    if (value == "library") out.denoiser_fail = TestHooks::DenoiserFail::LIBRARY;
    else if (value == "device") out.denoiser_fail = TestHooks::DenoiserFail::DEVICE;
    else throw std::invalid_argument("ASTRORAY_TEST_DENOISER_FAIL=" + value + " is not one of library, device");
    out.active.push_back("DENOISER_FAIL=" + value);
  }
#endif
  return out;
}

}  // namespace astroray::host
