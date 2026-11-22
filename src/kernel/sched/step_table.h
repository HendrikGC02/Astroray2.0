// SPDX-License-Identifier: Apache-2.0
// The step table: per Stage the segment of its pool's queue, the launch count and, for
// shadow producers, the first reserved shadow slot; the refill count; per pool the
// high-water mark that queue_build partitions up to. queue_build_scan fills it on the
// device with step_table_finish below, the same code on both targets, so the CPU and the
// GPU launch the same items. The step table is read back whole by the host (under 256 B).
//
// Contract of the fields:
//  - high_water[p] is an input, written by the host before queue_build: the largest slot
//    index used this pass plus one (items [0, high_water) are partitioned). queue_build
//    never changes it. Slots at or above it belong to no segment.
//  - shadow_capacity is an input written with high_water: the shadow pool's slot count. Free
//    shadow slots are the SHADOW_TERMINATED segment in index order followed by the fresh slots
//    [high_water[SHADOW], shadow_capacity) in index order (RULINGS 84), so the host keeps
//    shadow_queue[j] = j for every j at or above high_water[SHADOW]: the fresh entries follow
//    the segment, which ends at high_water[SHADOW] when invalid_items is 0. A table whose
//    shadow_capacity is not above high_water[SHADOW] has no fresh slots.
//  - shadow_fresh (output) is the number of fresh shadow slots this step reserved, the slots
//    [high_water[SHADOW], high_water[SHADOW] + shadow_fresh); shadow_reserved (output) is the
//    number of free-list entries the producers reserved, segment and fresh slots together.
//  - live_end[p] (output; the host writes 0 there with the marks) is the end of the pool's last
//    queue_build tile (kQueueTile slots, the last one ending at high_water[p]) that holds a slot
//    whose next_stage is not the pool's TERMINATED value, 0 when there is none: the largest such
//    slot + 1, rounded up to its tile's end. queue_build_scan raises it with an integer atomic max,
//    so the value does not depend on the order of the scan tiles. Every slot in
//    [live_end[p], high_water[p]) is terminated, and the pool's TERMINATED segment lists them last
//    and in index order (when invalid_items is 0), so its entries from position
//    live_end[p] on are live_end[p], live_end[p] + 1, ...: the free list is the same slots in the
//    same order whether the high-water mark is high_water[p] or live_end[p], and the host may lower
//    the mark to live_end[p] (plus the fresh slots the step takes) for the next partition
//    (RULINGS 85).
//  - stage[s].start indexes the queue array of the stage's pool (main_queue for main-pool
//    stages, shadow_queue for INTERSECT_SHADOW, SHADE_SHADOW and SHADOW_TERMINATED).
//  - stage[s].count is the launch count: the segment length, except for a shadow producer
//    whose segment the free shadow slots cannot cover; it then launches on its first
//    floor(free_left / k) items and the rest keep their next_stage (0002 D5). The next
//    segment of the pool starts after the whole segment, never after the launch count.
//  - stage[s].shadow_base (producers only) is an index into shadow_queue: item i of the
//    segment owns shadow_queue[shadow_base + k*i .. + k*(i+1)), whether or not it emits.
//  - refill_count is the number of free main-pool slots below the high-water mark: the
//    size of the main TERMINATED segment, whose queue entries are the slots to refill.
//    Slots at or above high_water[MAIN] are fresh; the host counts them from its capacity.
//    For the generate_camera and generate_light launches the host overwrites the device's
//    stage[TERMINATED] entry (queue_build rewrites it next step): start = the launch's first
//    free-slot entry of main_queue, count = its items, shadow_base = the first camera sample
//    index or light rank of the launch. Item i takes slot main_queue[start + i], work shadow_base + i.
//  - invalid_items counts items whose next_stage is no Stage value of their pool. They are
//    in no segment; the host must fail the render when it is not zero (no silent fallback).
// Ref: docs/decisions/0002-kernel-architecture.md D5 (deterministic, atomic-free shadow
// slot assignment), D6 (partition, step table), D7 step 2 (read back under 256 B);
// docs/decisions/RULINGS.md 84 (the shadow free list's fresh tail), 85 (the mark
// falls to the end of the last tile holding a live slot);
// docs/decisions/0003-spectral-model-and-path-state.md d19-d20.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../launch_config.h"
#include "../portability.h"
#include "../state/stage.h"

AR_NS_BEGIN

// The two pools. Every Stage value belongs to exactly one; each pool has its own
// TERMINATED value (0002 D6).
enum class Pool : uint8_t { MAIN, SHADOW };
inline constexpr uint32_t kPoolCount = 2;

AR_FN Pool stage_pool(Stage s) {
  switch (s) {
    case Stage::INTERSECT_SHADOW:
    case Stage::SHADE_SHADOW:
    case Stage::SHADOW_TERMINATED:
      return Pool::SHADOW;
    default:
      return Pool::MAIN;
  }
}

// One stage's segment of its pool (0002 D6): start and launch count in the pool's queue
// array, and for shadow producers the first reserved shadow slot (D5).
struct StepEntry {
  uint32_t start;
  uint32_t count;
  uint32_t shadow_base;
};

// Filled on the device by queue_build and read back by the host: one entry per Stage
// value plus the refill count of D7 step 4. 216 B, under the 256 B of D7 step 2. high_water,
// shadow_capacity and live_end are adjacent: the host writes them in one upload.
struct StepTable {
  StepEntry stage[kStageCount];
  uint32_t refill_count;
  uint32_t high_water[kPoolCount];
  uint32_t shadow_capacity;
  uint32_t live_end[kPoolCount];
  uint32_t invalid_items;
  uint32_t shadow_fresh;
  uint32_t shadow_reserved;
};
static_assert(sizeof(StepTable) < 256, "the step table is read back in one under-256-B copy");

// The most shadow items one item of each stage can emit in one step (0002 D5): the
// producers of D8 take at most one by default; 0 for every other stage. A stage task that
// needs more raises its value in launch_config.h.
struct StageShadowItems {
  AR_FN uint32_t operator()(Stage s) const { return stage_shadow_items(s); }
};

// Completes a step table whose stage[s].count holds the segment length of every stage
// (and nothing else is touched): segment starts per pool in Stage order, the shadow-slot
// reservation, the fresh-slot count and the refill count. `shadow_k(stage)` is the producer's
// k (0 = not a producer). Free shadow slots are the SHADOW_TERMINATED segment in index order,
// then the fresh slots up to shadow_capacity (RULINGS 84); the producers, in Stage order,
// receive consecutive ranges of that list (0002 D5). A producer whose
// segment needs more slots than are left launches floor(free_left / k) items. A producer
// with an empty segment still gets shadow_base = the next unreserved free slot, so the
// table has one definition.
template <class KFn>
AR_FN void step_table_finish(StepTable& t, KFn shadow_k) {
  uint32_t main_start = 0;
  uint32_t shadow_start = 0;
  for (uint32_t s = 0; s < kStageCount; ++s) {
    const bool shadow = stage_pool(static_cast<Stage>(s)) == Pool::SHADOW;
    uint32_t& next = shadow ? shadow_start : main_start;
    t.stage[s].start = next;
    t.stage[s].shadow_base = 0;
    next += t.stage[s].count;
  }
  const uint32_t terminated = t.stage[static_cast<uint32_t>(Stage::SHADOW_TERMINATED)].count;
  const uint32_t hw_shadow = t.high_water[static_cast<uint32_t>(Pool::SHADOW)];
  const uint32_t fresh = t.shadow_capacity > hw_shadow ? t.shadow_capacity - hw_shadow : 0u;
  const uint32_t free_slots = terminated + fresh;
  uint32_t free_left = free_slots;
  uint32_t base = t.stage[static_cast<uint32_t>(Stage::SHADOW_TERMINATED)].start;
  for (uint32_t s = 0; s < kStageCount; ++s) {
    const uint32_t k = shadow_k(static_cast<Stage>(s));
    if (k == 0) continue;
    StepEntry& e = t.stage[s];
    // free_left / k bounds the launch, so k * launch cannot overflow.
    const uint32_t fits = free_left / k;
    const uint32_t launch = e.count < fits ? e.count : fits;
    e.shadow_base = base;
    e.count = launch;
    base += launch * k;
    free_left -= launch * k;
  }
  // The segment's slots go first, so the reservation reaches into the fresh tail only past them.
  const uint32_t reserved = free_slots - free_left;
  t.shadow_fresh = reserved > terminated ? reserved - terminated : 0u;
  t.shadow_reserved = reserved;
  t.refill_count = t.stage[static_cast<uint32_t>(Stage::TERMINATED)].count;
}

AR_NS_END
