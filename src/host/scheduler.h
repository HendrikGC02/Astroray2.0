// SPDX-License-Identifier: Apache-2.0
// The host scheduler loop of one pass, written once against the Device interface (0002 D7):
// each step partitions both pools (queue_build), reads the step table, sorts the
// SHADE_SURFACE segment when worthwhile, refills by Cycles' quarter rule from one work counter
// (camera samples, then light samples) and launches every non-empty stage once in Stage order.
// The pass ends when the counter is exhausted and only the two TERMINATED segments are
// non-empty.
//
// Contracts with the kernels (all on one page so 054 and the stages find them):
//  - high_water[SHADOW] is the shadow capacity, every shadow slot starts SHADOW_TERMINATED;
//    high_water[MAIN] is one past the largest main slot in use. main_queue starts as the
//    identity, so the entries past the partition are the fresh slots.
//  - Refill: the free-slot list is the main TERMINATED segment followed by those fresh
//    entries. Before each generate launch the host rewrites the device's
//    stage[TERMINATED] entry to describe that launch only: start = its first free-slot entry
//    in main_queue, count = its item count, shadow_base = the first camera sample index
//    (generate_camera) or light rank (generate_light) of the launch. Item i of the launch gets
//    slot main_queue[start + i] and the work item shadow_base + i. queue_build rewrites the
//    entry next step.
//  - Producers read their shadow slots at shadow_queue[step.shadow_base + k * item].
//  - A GPU generate launch must not start before the stage[TERMINATED] entry upload has
//    completed on the same stream (both are enqueued in order on the device's one stream).
//  - After sort, the sorted indices replace the SHADE_SURFACE segment of main_queue.
// Ref: docs/decisions/0002-kernel-architecture.md D5-D8, D12; Cycles
// integrator/path_trace_work_gpu.cpp (Apache-2.0) for the refill rule.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "host/counters.h"
#include "host/device.h"
#include "host/report.h"
#include "host/state_alloc.h"
#include "kernel/kernel_globals.h"

namespace astroray::host {

// Pass work: one counter over camera samples followed by light samples (0002 D7 step 4).
struct PassWork {
  uint64_t camera_samples = 0;
  uint64_t light_samples = 0;  // 0 when the scene has no casters
};

inline constexpr uint64_t kMaxPoolCapacity = 1ull << 21;  // 0002 D5
inline constexpr uint64_t kCpuPoolCapacity = 1ull << 16;  // 0002 D5, CPU default

struct SchedulerConfig {
  ar::StateFeatures features;
  // The session's pointers and uniforms (scene, film, tables, active pixels, session). The
  // scheduler fills path, shadow, step_table, the queues and the sort and histogram scratch.
  ar::KernelGlobals globals{};
  uint32_t material_header_count = 0;  // the scene's, for the sort threshold and its scratch size
  uint64_t max_capacity = kMaxPoolCapacity;  // the path-pool row the ledger allows
  uint64_t capacity_override = 0;            // tests: force the pool capacity (still bounded by the work)
  uint64_t shadow_capacity_override = 0;     // tests: force the shadow pool below its default 2 x capacity
  bool permute = false;                      // AR_DEBUG_PERMUTE, see Scheduler constructor
  bool direct_launch = false;                // AR_DEBUG_DIRECT_LAUNCH: no graphs, see Scheduler constructor
  Report* report = nullptr;                  // POOL_CAPACITY goes here, once per scheduler
  Owner owner{};
};

enum class PassStatus : uint8_t { COMPLETE, CANCELLED, EPOCH_PENDING };

class Scheduler {
 public:
  // permute defaults to the environment variable AR_DEBUG_PERMUTE (any value but "0"), read
  // here and only in builds with AR_GATE_HOOKS; config.permute turns it on in any build. On a
  // device with graphs, steps replay a captured CUDA graph (0002 D7 remedy (a)) unless permute
  // is on (it reorders queues between launches) or direct launches are asked for:
  // config.direct_launch, or AR_DEBUG_DIRECT_LAUNCH read like AR_DEBUG_PERMUTE.
  Scheduler(Device& device, const SchedulerConfig& config);
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // Runs one pass. Throws std::runtime_error when queue_build finds invalid items or a step
  // makes no progress. Stops at the next launch boundary when `cancel` or `epoch_pending` is
  // set (by the session, 052) and returns that status; the pools are then unfinished.
  PassStatus run_pass(const PassWork& work);

  // Set by the session (052) from another thread.
  std::atomic<bool> cancel{false};
  std::atomic<bool> epoch_pending{false};

  // The session updates config between passes; pools and scratch follow it at the next pass.
  SchedulerConfig config;
  const PassCounters& counters() const { return counters_; }
  const ar::KernelGlobals& globals() const { return kg_; }
  uint64_t capacity() const { return capacity_; }

 private:
  void ensure_buffers(uint64_t capacity, uint64_t shadow_capacity);
  void release_buffers();
  bool at_boundary(PassStatus* status);
  void launch_stage(ar::Stage stage, const ar::StepTable& table);
  void launch_kernel(KernelId kernel, uint32_t count);
  // One step's D7 steps 3-5, decided from its table: sort, the two generate entries, the stages.
  struct StepPlan {
    bool sort = false;
    uint32_t sort_start = 0, sort_count = 0;
    ar::StepEntry camera{}, light{};  // count 0: no launch
    std::vector<uint32_t> stages;     // non-empty, in Stage order
    uint32_t counts[ar::kStageCount] = {};
  };
  // The step inputs a graph replay copies on the device: the two generate entries for the
  // TERMINATED slot and the high-water marks.
  struct StepInputs {
    ar::StepEntry camera;
    ar::StepEntry light;
    uint32_t high_water[ar::kPoolCount];
  };
  // What the step and sort graphs depend on (ensure_graphs).
  struct GraphKey {
    const void* kg = nullptr;
    const void* step = nullptr;
    const void* inputs = nullptr;
    const void* main_queue = nullptr;
    const void* sort_queue = nullptr;
    uint64_t capacity = 0, shadow_capacity = 0;
    bool light = false;
    bool operator==(const GraphKey& o) const {
      return kg == o.kg && step == o.step && inputs == o.inputs && main_queue == o.main_queue &&
             sort_queue == o.sort_queue && capacity == o.capacity && shadow_capacity == o.shadow_capacity &&
             light == o.light;
    }
  };
  bool run_plan(const StepPlan& plan, const ar::StepTable& table, PassStatus* status);
  void ensure_graphs(uint64_t capacity, uint64_t shadow_capacity, bool light);
  void free_graphs();
  void issue_graph_step(const StepPlan& plan, const uint32_t high_water[ar::kPoolCount]);
  void count_stage(ar::Stage stage, uint32_t count);
  void derive_counters();
  void permute_range(bool shadow_pool, uint32_t start, uint32_t count);

  Device& device_;
  PassCounters counters_;
  ar::KernelGlobals kg_{};
  uint64_t capacity_ = 0;
  uint64_t shadow_capacity_ = 0;
  ar::StateFeatures features_{};
  uint32_t sort_material_count_ = ~0u;
  bool capacity_reported_ = false;
  bool permute_ = false;
  bool direct_ = false;
  uint32_t refill_graph_ = 0, step_graph_ = 0, sort_graph_ = 0;  // device graph handles, 0 for none
  GraphKey graph_key_{};
  uint64_t permute_state_ = 0;
  std::vector<uint32_t> launched_;  // stages launched in the current step, in order
  DeviceBuffer path_buf_, shadow_buf_, main_queue_buf_, shadow_queue_buf_, step_buf_, counts_buf_, sort_queue_buf_,
      sort_counts_buf_, kg_buf_, soa_buf_, identity_buf_, inputs_buf_;
  ar::PathStateSoA path_{};
  ar::ShadowItemSoA shadow_{};
};

}  // namespace astroray::host
