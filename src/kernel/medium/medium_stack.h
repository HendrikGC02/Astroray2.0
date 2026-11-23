// SPDX-License-Identifier: Apache-2.0
// The per-path medium stack: pure functions over the four u32 entries and the flag word that
// carries the depth. Nothing here touches path state or globals; the caller stores what comes back.
//
// Entry = material id (24 bits) << 8 | priority (8 bits). Entries 0..depth-1 are in order of
// entry, the most recent last; slots at or above depth are 0. Id 0 is reserved (an entry of id 0
// and priority 0 would read as empty), so a caller passes ids >= 1.
//
// Priorities (Schmidt and Budge 2002): the "highest" entry is the one with the greatest priority,
// ties going to the most recently entered. A crossing is a true interface only if the hit material
// is the highest among the stack and itself; the IOR ratio is n(highest after) / n(highest before).
// A false interface only updates the stack.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md C.16-C.17; Schmidt and Budge 2002,
// "Simple nested dielectrics in ray traced images", JGT 7(2).
#pragma once
#include <cstdint>

#include "core/real.h"
#include "portability.h"
#include "state/flags.h"
#include "state/state.h"

AR_NS_BEGIN

inline constexpr uint32_t MEDIUM_STACK_CAPACITY = 4;

AR_FN uint32_t stack_entry(uint32_t id, uint32_t priority) {
  AR_ASSERT(id != 0u && id < (1u << 24) && priority < 256u);
  return (id << 8) | priority;
}
AR_FN uint32_t stack_entry_id(uint32_t e) { return e >> 8; }
AR_FN uint32_t stack_entry_priority(uint32_t e) { return e & 0xffu; }

// The stack and the flag word it lives in. `ok` is false when an update could not be applied; the
// stack and flags are then the unchanged input, and the caller decides (a push overflow ends the
// path with STACK_OVERFLOW).
struct StackUpdate {
  U32x4 stack;
  uint32_t flags;
  bool ok;
};

AR_FN StackUpdate stack_push(U32x4 stack, uint32_t flags, uint32_t id, uint32_t priority) {
  const uint32_t depth = medium_depth(flags);
  if (depth >= MEDIUM_STACK_CAPACITY) return {stack, flags, false};
  stack.v[depth] = stack_entry(id, priority);
  return {stack, set_medium_depth(flags, depth + 1u), true};
}

// Removes the most recent entry of `id`, closing the gap. Not on the stack: ok = false, unchanged.
AR_FN StackUpdate stack_remove(U32x4 stack, uint32_t flags, uint32_t id) {
  const uint32_t depth = medium_depth(flags);
  for (uint32_t i = depth; i-- > 0u;) {
    if (stack_entry_id(stack.v[i]) != id) continue;
    for (uint32_t j = i; j + 1u < depth; ++j) stack.v[j] = stack.v[j + 1u];
    stack.v[depth - 1u] = 0u;
    return {stack, set_medium_depth(flags, depth - 1u), true};
  }
  return {stack, flags, false};
}

// Index of the highest entry (greatest priority, latest among ties), or -1 when empty.
AR_FN int stack_highest_index(U32x4 stack, uint32_t flags) {
  int best = -1;
  uint32_t best_priority = 0;
  for (uint32_t i = 0; i < medium_depth(flags); ++i) {
    const uint32_t p = stack_entry_priority(stack.v[i]);
    if (best < 0 || p >= best_priority) best = int(i), best_priority = p;
  }
  return best;
}

// The priority the active media sit at: that of the highest entry, 0 for an empty stack.
AR_FN uint32_t stack_active_priority(U32x4 stack, uint32_t flags) {
  const int i = stack_highest_index(stack, flags);
  return i < 0 ? 0u : stack_entry_priority(stack.v[i]);
}

// The id of the highest entry, 0 when empty (the world): the material whose interior_ior is the
// current n.
AR_FN uint32_t stack_highest_id(U32x4 stack, uint32_t flags) {
  const int i = stack_highest_index(stack, flags);
  return i < 0 ? 0u : stack_entry_id(stack.v[i]);
}

// Whether crossing the hit material's surface changes what the path is inside. `entering` is the
// front-face case (a push follows), otherwise a back face (a remove follows). Entering: true when
// the hit's priority is at least every entry's (it would be the most recent of any tie). Leaving:
// true when the hit's entry is itself the highest. A back face whose id is not on the stack is
// reported true; the record does not say what such a crossing does, and the caller sees the
// failed stack_remove.
AR_FN bool interface_is_true(U32x4 stack, uint32_t flags, uint32_t hit_id, uint32_t hit_priority, bool entering) {
  if (entering) return hit_priority >= stack_active_priority(stack, flags);
  for (uint32_t i = medium_depth(flags); i-- > 0u;)
    if (stack_entry_id(stack.v[i]) == hit_id) return int(i) == stack_highest_index(stack, flags);
  return true;
}

// The eta of a true interface, from the two sides' resolved interior_ior (1 for the world).
AR_FN Real interface_ior_ratio(Real n_before, Real n_after) { return n_after / n_before; }

AR_NS_END
