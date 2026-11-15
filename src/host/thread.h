// SPDX-License-Identifier: Apache-2.0
// The one way the engine starts a thread.
#pragma once
#include <functional>
#include <thread>

#include "host/fault.h"

namespace astroray {

// Starts fn on a new thread inside run_engine_thread, so a structured exception
// there writes a minidump and poisons ctx (0001 D2.5). ctx must outlive the thread.
std::thread start_engine_thread(ContextState& ctx, std::function<void()> fn);

}  // namespace astroray
