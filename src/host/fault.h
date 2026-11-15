// SPDX-License-Identifier: Apache-2.0
// Structured-exception handling for engine threads (0001 D2.5): an access
// violation on an engine thread writes a minidump beside the context log and
// poisons the context. There is no process-wide handler (0001 D2.1, D2.5); only
// code that runs through run_engine_thread is covered.
#pragma once
#include <atomic>
#include <string>
#include <type_traits>

#include "host/log.h"

struct ar_context_s;

namespace astroray {

// What every handle's context owns that fault handling needs. The capi context
// derives from it so a scene or session can reach the poisoned flag via its context.
struct ContextState {
  ContextLog log;
  std::string log_path;  // dumps are written as <log_path>.<pid>.<n>.dmp
  std::atomic<bool> poisoned{false};
  // Written once by the first faulting thread before `poisoned` is stored with
  // release; read only after `poisoned` is loaded with acquire.
  std::string poison_message;
  std::atomic<bool> fault_claimed{false};
  std::atomic<unsigned> dump_count{0};
};

// Runs fn(arg) inside __try/__except. Returns true when it returned normally and
// false when an exception reached the top of the thread (dump written, context
// poisoned). A thread body that wants a C++ exception handled catches it itself.
bool run_engine_thread_raw(ContextState& ctx, void (*fn)(void*), void* arg);

template <class F>
bool run_engine_thread(ContextState& ctx, F&& fn) {
  auto thunk = [](void* p) { (*static_cast<std::remove_reference_t<F>*>(p))(); };
  return run_engine_thread_raw(ctx, thunk, &fn);
}

#ifdef AR_GATE_HOOKS
// Gate-tier hook (dev builds only): starts an engine thread that writes through
// a null pointer and joins it, so the caller sees the poisoned context on return.
void fault_test_access_violation(ContextState& ctx);
// The fault state of a context created through the C API (defined in capi/context.cpp).
ContextState& fault_test_context_state(ar_context_s* ctx);
#endif

}  // namespace astroray
