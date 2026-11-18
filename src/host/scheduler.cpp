// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0002-kernel-architecture.md D5 (pools, capacity, slot assignment), D6
// (queue_build, step table), D7 (the step; refill rule; pass end), D12 (determinism);
// Cycles integrator/path_trace_work_gpu.cpp (Apache-2.0) for the refill rule.
#include "host/scheduler.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/launch_config.h"
#include "kernel/stages/queue_build.h"
#include "kernel/stages/sort.h"

namespace astroray::host {
namespace {

using ar::Stage;

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
const char* const kStageNames[] = {"INTERSECT_CLOSEST", "INTERSECT_SUBSURFACE", "INTERSECT_SHADOW",
                                   "SHADE_BACKGROUND",  "SHADE_LIGHT",          "SHADE_SURFACE",
                                   "SHADE_SURFACE_EXT", "SHADE_LIGHT_PATH",     "SHADE_LIGHT_PATH_EXT",
                                   "SHADE_SHADOW",      "SHADE_VOLUME",         "SHADE_SUBSURFACE",
                                   "ADVANCE_GEODESIC"};
static_assert(sizeof(kStageNames) / sizeof(kStageNames[0]) == ar::kStageCount - 2, "one name per launchable stage");

constexpr uint32_t kTerm = static_cast<uint32_t>(Stage::TERMINATED);
constexpr uint32_t kShadowTerm = static_cast<uint32_t>(Stage::SHADOW_TERMINATED);

// SplitMix64: the fixed generator of the permute debug flag.
uint64_t splitmix(uint64_t& x) {
  uint64_t z = (x += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

bool env_permute() {
#ifdef AR_GATE_HOOKS
  char* v = nullptr;
  size_t n = 0;
  if (_dupenv_s(&v, &n, "AR_DEBUG_PERMUTE") != 0 || v == nullptr) return false;
  const bool on = std::string(v) != "0" && !std::string(v).empty();
  std::free(v);
  return on;
#else
  return false;
#endif
}

// Points the SoA of a pool at the arrays of its layout inside `base`.
void fill_path(ar::PathStateSoA& soa, const StateLayout& l, char* base) {
  using namespace ar;
  size_t i = 0;
#define AR_PATH_FIELD(n, t, f)                                                                  \
  soa.n = l.fields[i].allocated ? reinterpret_cast<t*>(base + l.fields[i].offset) : nullptr; \
  ++i;
#include "kernel/state/path_state.def"
#undef AR_PATH_FIELD
}

void fill_shadow(ar::ShadowItemSoA& soa, const StateLayout& l, char* base) {
  using namespace ar;
  size_t i = 0;
#define AR_SHADOW_FIELD(n, t, f)                                                                \
  soa.n = l.fields[i].allocated ? reinterpret_cast<t*>(base + l.fields[i].offset) : nullptr; \
  ++i;
#define AR_SHADOW_FIELD_SHARED(n, t, f, nt) AR_SHADOW_FIELD(n, t, f)
#include "kernel/state/shadow_item.def"
#undef AR_SHADOW_FIELD
#undef AR_SHADOW_FIELD_SHARED
}

}  // namespace

Scheduler::Scheduler(Device& device, const SchedulerConfig& cfg) : config(cfg), device_(device) {
  permute_ = cfg.permute || env_permute();
}

Scheduler::~Scheduler() { release_buffers(); }

void Scheduler::release_buffers() {
  for (DeviceBuffer* b : {&path_buf_, &shadow_buf_, &main_queue_buf_, &shadow_queue_buf_, &step_buf_, &counts_buf_,
                          &sort_queue_buf_, &sort_counts_buf_, &kg_buf_, &soa_buf_})
    device_.free(*b);
  capacity_ = 0;
  shadow_capacity_ = 0;
  sort_material_count_ = ~0u;
}

void Scheduler::ensure_buffers(uint64_t cap, uint64_t shadow_cap) {
  const bool same_features = std::memcmp(&features_, &config.features, sizeof(ar::StateFeatures)) == 0;
  if (cap != capacity_ || shadow_cap != shadow_capacity_ || !same_features) {
    release_buffers();
    features_ = config.features;
    const Owner o = config.owner;
    const StateLayout pl = path_state_layout(features_, cap);
    const StateLayout sl = shadow_item_layout(features_, shadow_cap);
    path_buf_ = device_.allocate(LedgerCategory::MAIN_PATH_POOL, pl.total_bytes, o);
    shadow_buf_ = device_.allocate(LedgerCategory::SHADOW_POOL, sl.total_bytes, o);
    main_queue_buf_ = device_.allocate(LedgerCategory::QUEUE_SORT_INDICES, cap * 4, o);
    shadow_queue_buf_ = device_.allocate(LedgerCategory::QUEUE_SORT_INDICES, shadow_cap * 4, o);
    const uint64_t tiles = ar::queue_tiles(uint32_t(cap)) + ar::queue_tiles(uint32_t(shadow_cap));
    counts_buf_ = device_.allocate(LedgerCategory::QUEUE_SORT_INDICES, tiles * ar::kQueueBins * 4, o);
    step_buf_ = device_.allocate(LedgerCategory::TABLES, sizeof(ar::StepTable), o);
    sort_queue_buf_ = device_.allocate(LedgerCategory::QUEUE_SORT_INDICES, cap * 4, o);
    const uint64_t soa_shadow_at = align_up(sizeof(ar::PathStateSoA), kStateArrayAlign);
    soa_buf_ = device_.allocate(LedgerCategory::TABLES, soa_shadow_at + sizeof(ar::ShadowItemSoA), o);
    kg_buf_ = device_.allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), o);
    // The SoA structs live in device memory too: KernelGlobals holds pointers to them.
    ar::PathStateSoA p{};
    ar::ShadowItemSoA s{};
    fill_path(p, pl, static_cast<char*>(path_buf_.ptr));
    fill_shadow(s, sl, static_cast<char*>(shadow_buf_.ptr));
    device_.upload(soa_buf_, 0, &p, sizeof p);
    device_.upload(soa_buf_, soa_shadow_at, &s, sizeof s);
    kg_.path = static_cast<const ar::PathStateSoA*>(soa_buf_.ptr);
    kg_.shadow = reinterpret_cast<const ar::ShadowItemSoA*>(static_cast<char*>(soa_buf_.ptr) + soa_shadow_at);
    path_ = p;
    shadow_ = s;
    capacity_ = cap;
    shadow_capacity_ = shadow_cap;
  }
  // sort_counts holds 2 x tiles x (material_header_count + 1) counters (sort.h), so it follows
  // the scene's material count.
  if (config.material_header_count != sort_material_count_) {
    device_.free(sort_counts_buf_);
    const uint64_t n = 2ull * ar::queue_tiles(uint32_t(cap)) * (config.material_header_count + 1ull);
    sort_counts_buf_ = device_.allocate(LedgerCategory::QUEUE_SORT_INDICES, n * 4, config.owner);
    sort_material_count_ = config.material_header_count;
  }
}

bool Scheduler::at_boundary(PassStatus* status) {
  if (cancel.load(std::memory_order_acquire)) {
    *status = PassStatus::CANCELLED;
    return true;
  }
  if (epoch_pending.load(std::memory_order_acquire)) {
    *status = PassStatus::EPOCH_PENDING;
    return true;
  }
  return false;
}

void Scheduler::launch_kernel(KernelId kernel, uint32_t count) {
  ++counters_.launches;
  device_.launch(kernel, count);
}

// Shuffles queue entries [start, start + count) of one pool's queue (AR_DEBUG_PERMUTE).
void Scheduler::permute_range(bool shadow_pool, uint32_t start, uint32_t count) {
  if (!permute_ || count < 2) return;
  const DeviceBuffer& q = shadow_pool ? shadow_queue_buf_ : main_queue_buf_;
  std::vector<uint32_t> v(count);
  device_.download(v.data(), q, uint64_t(start) * 4, uint64_t(count) * 4);
  for (uint32_t i = count - 1; i > 0; --i) std::swap(v[i], v[splitmix(permute_state_) % (i + 1)]);
  device_.upload(q, uint64_t(start) * 4, v.data(), uint64_t(count) * 4);
  ++counters_.permuted_launches;
}

void Scheduler::launch_stage(Stage stage, const ar::StepTable& t) {
  const uint32_t s = static_cast<uint32_t>(stage);
  const uint32_t count = t.stage[s].count;
  permute_range(ar::stage_pool(stage) == ar::Pool::SHADOW, t.stage[s].start, count);
  ++counters_.launches;
  device_.launch(stage, count);
  launched_.push_back(s);
  counters_.items[s] += count;
  const bool shadow = ar::stage_pool(stage) == ar::Pool::SHADOW;
  (shadow ? counters_.shadow_item_bytes_moved : counters_.state_bytes_moved) +=
      uint64_t(count) * (shadow ? ar::shadow_item_bytes(features_) : ar::path_state_bytes(features_));
}

PassStatus Scheduler::run_pass(const PassWork& work) {
  counters_ = PassCounters{};
  permute_state_ = 0x5eed5eed5eed5eedull;
  const uint64_t total = work.camera_samples + work.light_samples;
  if (total == 0) return PassStatus::COMPLETE;
  if (config.max_capacity == 0) throw std::invalid_argument("Scheduler: max_capacity is zero");

  // Capacity: min(work in the pass, 2^21, the ledger's row); the CPU default is 2^16 (0002 D5).
  uint64_t limit = config.capacity_override ? config.capacity_override
                                            : (device_.kind() == AR_DEVICE_CPU ? kCpuPoolCapacity : kMaxPoolCapacity);
  limit = std::min({limit, kMaxPoolCapacity, config.max_capacity});
  const uint64_t cap = std::min(total, limit);
  // The shadow pool is twice the main pool (0002 D5) unless a test starves it: producers then hold
  // back the items the free slots cannot cover (step_table.h), which ASTRORAY_TEST_POOL exercises.
  const uint64_t shadow_cap = config.shadow_capacity_override ? config.shadow_capacity_override : 2 * cap;
  ensure_buffers(cap, shadow_cap);
  if (config.report && !capacity_reported_) {
    const double value[2] = {double(cap), 0.0};
    config.report->raise(AR_REPORT_POOL_CAPACITY, 0, "main path pool capacity", 1, value, 0);
    capacity_reported_ = true;
  }

  // KernelGlobals: the session's pointers plus ours, bound once per pass.
  const ar::PathStateSoA* path = kg_.path;
  const ar::ShadowItemSoA* shadow = kg_.shadow;
  kg_ = config.globals;
  kg_.path = path;
  kg_.shadow = shadow;
  kg_.step_table = static_cast<const ar::StepTable*>(step_buf_.ptr);
  kg_.main_queue = static_cast<const uint32_t*>(main_queue_buf_.ptr);
  kg_.shadow_queue = static_cast<const uint32_t*>(shadow_queue_buf_.ptr);
  kg_.queue_counts = static_cast<uint32_t*>(counts_buf_.ptr);
  kg_.sort_queue = static_cast<uint32_t*>(sort_queue_buf_.ptr);
  kg_.sort_counts = static_cast<uint32_t*>(sort_counts_buf_.ptr);
  device_.upload(kg_buf_, 0, &kg_, sizeof kg_);
  device_.set_kernel_globals(kg_buf_);

  // Pass start: main_queue is the identity (its tail past the partition is the fresh slots),
  // every shadow slot is SHADOW_TERMINATED and the shadow high-water is the whole capacity: a
  // zero shadow high-water would starve every producer and livelock the pass (step_table.h).
  {
    std::vector<uint32_t> identity(cap);
    for (uint32_t i = 0; i < cap; ++i) identity[i] = i;
    device_.upload(main_queue_buf_, 0, identity.data(), cap * 4);
    const uint64_t at = uint64_t(reinterpret_cast<const char*>(shadow_.next_stage) -
                                 reinterpret_cast<const char*>(shadow_buf_.ptr));
    device_.memset(shadow_buf_, at, static_cast<uint8_t>(Stage::SHADOW_TERMINATED), shadow_cap);
  }
  const uint32_t hw_shadow = uint32_t(shadow_cap);
  uint32_t hw_main = 0;
  uint64_t next_work = 0;
  device_.memset(step_buf_, 0, 0, sizeof(ar::StepTable));

  PassStatus status = PassStatus::COMPLETE;
  launched_.clear();
  for (;;) {
    if (at_boundary(&status)) return status;
    // Dev kernel reload (0002 D14, D15): a rebuilt module is swapped here, before the step's first launch.
    // A swap ends the pass like an epoch change, so nothing mixes two images; a failed load keeps the
    // previous image and leaves one warning entry per module until a good file loads.
    {
      const ModuleReload r = device_.poll_module_reload();
      if (config.report) {
        const double none[2] = {0.0, 0.0};
        // One entry per module (its subject), so a later good file removes it whatever the error text was.
        for (const auto& f : r.failed) {
          config.report->remove(AR_REPORT_KERNEL_MODULE_FALLBACK, f.subject);
          config.report->raise(AR_REPORT_KERNEL_MODULE_FALLBACK, f.subject,
                               f.module + ": " + f.error + "; previous image runs", 1, none, 0);
        }
        for (const auto& m : r.swapped) config.report->remove(AR_REPORT_KERNEL_MODULE_FALLBACK, m.subject);
        for (const auto& m : r.recovered) config.report->remove(AR_REPORT_KERNEL_MODULE_FALLBACK, m.subject);
      }
      if (!r.swapped.empty()) return PassStatus::EPOCH_PENDING;
    }
    const std::vector<uint32_t> prev_launched = std::move(launched_);
    launched_.clear();
    // 1. Partition both pools up to their high-water marks.
    const uint32_t hw[ar::kPoolCount] = {hw_main, hw_shadow};
    device_.upload(step_buf_, offsetof(ar::StepTable, high_water), hw, sizeof hw);
    const uint32_t grid = ar::queue_build_grid(hw_main, hw_shadow);
    launch_kernel(KernelId::QUEUE_BUILD_COUNT, grid);
    if (at_boundary(&status)) return status;
    launch_kernel(KernelId::QUEUE_BUILD_SCAN, ar::queue_scan_grid(hw_main, hw_shadow));  // never 0
    if (at_boundary(&status)) return status;
    launch_kernel(KernelId::QUEUE_BUILD_SCATTER, grid);

    // 2. One step-table read.
    ar::StepTable t;
    device_.read_step_table(t);
    ++counters_.steps;
    if (t.invalid_items != 0) {
      // The stage that wrote the bad value ran in the previous step: list every one of them.
      std::string stages;
      for (uint32_t s : prev_launched) stages += (stages.empty() ? "" : ", ") + std::string(kStageNames[s]);
      if (stages.empty()) stages = "none";
      throw std::runtime_error("Scheduler: queue_build found " + std::to_string(t.invalid_items) +
                               " item(s) whose next_stage is no stage of their pool at step " +
                               std::to_string(counters_.steps) + " (stages launched in the previous step: " +
                               stages + ")");
    }
    const uint32_t active_main = hw_main - t.refill_count;
    const uint32_t active_shadow = hw_shadow - t.stage[kShadowTerm].count;
    counters_.active_path_steps += active_main;
    counters_.peak_active_paths = std::max<uint64_t>(counters_.peak_active_paths, active_main);
    if (next_work == total && active_main == 0 && active_shadow == 0) break;  // the pass has drained

    bool progress = false;
    // 3. Sort the SHADE_SURFACE segment by material when worthwhile (sort_wanted).
    const uint32_t ss = static_cast<uint32_t>(Stage::SHADE_SURFACE);
    if (ar::sort_wanted(t.stage[ss].count, config.material_header_count)) {
      const uint32_t n = t.stage[ss].count;
      const uint32_t sort_launch = ar::sort_grid(n);
      if (at_boundary(&status)) return status;
      launch_kernel(KernelId::SORT_COUNT, sort_launch);
      if (at_boundary(&status)) return status;
      launch_kernel(KernelId::SORT_SCAN, ar::sort_scan_grid(n));
      if (at_boundary(&status)) return status;
      launch_kernel(KernelId::SORT_SCATTER, sort_launch);
      // The stage reads its segment of main_queue, so the sorted indices replace it.
      device_.copy(main_queue_buf_, uint64_t(t.stage[ss].start) * 4, sort_queue_buf_, 0, uint64_t(n) * 4);
      ++counters_.sorts;
    }

    // 4. Refill when fewer than a quarter of the slots are active and work remains. The free
    // slots are the TERMINATED segment, then the fresh tail of the identity queue.
    if (next_work < total && uint64_t(active_main) * 4 < cap) {
      const uint64_t free_total = uint64_t(t.refill_count) + (cap - hw_main);
      const uint64_t n = std::min(total - next_work, free_total);
      const uint64_t camera_left = next_work < work.camera_samples ? work.camera_samples - next_work : 0;
      const uint32_t nc = uint32_t(std::min(n, camera_left));
      const uint32_t nl = uint32_t(n) - nc;
      const uint32_t first = t.stage[kTerm].start;
      // The fresh entries follow the TERMINATED segment only when it ends at the high-water
      // mark, which holds with no invalid items (checked above).
      if (first + t.refill_count != hw_main)
        throw std::logic_error("Scheduler: the TERMINATED segment does not end at the high-water mark");
      // Describes one generate launch in the device's TERMINATED entry (header comment).
      auto refill = [&](KernelId kernel, uint32_t slot_offset, uint32_t count, uint32_t work_first) {
        if (count == 0) return true;
        if (at_boundary(&status)) return false;
        const ar::StepEntry e{first + slot_offset, count, work_first};
        device_.upload(step_buf_, offsetof(ar::StepTable, stage) + kTerm * sizeof(ar::StepEntry), &e, sizeof e);
        permute_range(false, e.start, count);
        launch_kernel(kernel, count);
        return true;
      };
      if (!refill(KernelId::GENERATE_CAMERA, 0, nc, uint32_t(next_work))) return status;
      if (!refill(KernelId::GENERATE_LIGHT, nc, nl, uint32_t(next_work + nc - work.camera_samples))) return status;
      hw_main += n > t.refill_count ? uint32_t(n - t.refill_count) : 0u;
      next_work += n;
      counters_.refills += 1;
      counters_.camera_paths += nc;
      counters_.light_paths += nl;
      counters_.state_bytes_moved += n * ar::path_state_bytes(features_);
      progress = true;
    }

    // 5. Every stage with a non-zero launch count, once, in Stage order.
    for (uint32_t s = 0; s < ar::kStageCount; ++s) {
      if (s == kTerm || s == kShadowTerm || t.stage[s].count == 0) continue;
      if (at_boundary(&status)) return status;
      launch_stage(static_cast<Stage>(s), t);
      progress = true;
    }
    counters_.rays_closest = counters_.items[static_cast<uint32_t>(Stage::INTERSECT_CLOSEST)];
    counters_.rays_shadow = counters_.items[static_cast<uint32_t>(Stage::INTERSECT_SHADOW)];
    // In M1a every emitted shadow item is traced exactly once (no re-queue, no drop), so the
    // items of INTERSECT_SHADOW are the items emitted. Once items can be dropped or re-queued
    // (M2: recorded hits, overflow), a device counter written by the producers replaces this.
    counters_.shadow_rays_emitted = counters_.rays_shadow;
    // Every SHADE_LIGHT item is one lamp crossing (0004 D8, D29): exact while light paths only pass
    // through lamps and nothing else routes there.
    counters_.lamp_crossings = counters_.items[static_cast<uint32_t>(Stage::SHADE_LIGHT)];
    if (!progress) throw std::runtime_error("Scheduler: step " + std::to_string(counters_.steps) + " made no progress");
  }
  return PassStatus::COMPLETE;
}

}  // namespace astroray::host

