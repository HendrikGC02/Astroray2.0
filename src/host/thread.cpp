// SPDX-License-Identifier: Apache-2.0
#include "host/thread.h"

#include <utility>

namespace astroray {

// Every thread the engine creates (scene workers, device-init threads, session
// render threads) must be started through this function, never with std::thread
// directly: it is what puts the thread under structured-exception handling.
std::thread start_engine_thread(ContextState& ctx, std::function<void()> fn) {
  return std::thread([&ctx, fn = std::move(fn)]() mutable { run_engine_thread(ctx, fn); });
}

}  // namespace astroray
