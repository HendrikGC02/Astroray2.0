// SPDX-License-Identifier: Apache-2.0
// The host scene: init thread, worker pool, staging arena, generations, stored edit state
// (0007 decisions 5, 6, 23-25). Creation returns at once; device init runs on its own
// thread and a failure is re-raised at the first job that needs the device (0002 D15).
// Ref: 0007 decisions 5, 23, 24, 25; 0002 D15.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../../include/astroray/astroray.h"
#include "host/fault.h"
#include "host/report.h"
#include "host/scene/arena.h"
#include "host/scene/generation.h"

struct ar_context_s;

namespace astroray {
// Defined in capi/context.cpp, where the context struct is private.
ContextState& context_state(ar_context_s* ctx);
void context_set_error(ar_context_s* ctx, const std::string& text);
void context_device(ar_context_s* ctx, uint32_t* kind, uint32_t* index);
}  // namespace astroray

namespace astroray::host {

class Edit;

// Applies one generation's key-set changes (reset, removes, then inserts).
void apply_known(std::set<std::pair<uint32_t, uint64_t>>& known, const Pending& p);

struct SceneConfig {
  unsigned workers = 0;  // 0: min(8, logical cores / 2), at least 1 (0007 decision 23)
  // Runs on the init thread (CUDA context and module loading); the returned object is kept
  // until the scene is destroyed. Empty on the CPU device. A throw is the init failure.
  std::function<std::shared_ptr<void>()> init;
};

// ar_settings and ar_camera_desc with their copy-in arrays owned.
struct StoredSettings {
  ar_settings s{};
  std::vector<ar_name> passes, aovs;
  std::vector<ar_response_channel> channels;
  std::vector<std::vector<float>> curve_nm, curve_value;
};
struct StoredCamera {
  ar_camera_desc c{};
  std::vector<ar_xform> motion;
};

class Scene {
 public:
  Scene(ContextState& ctx, SceneConfig config);
  ~Scene();
  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;

  // Null while another edit is open (one open edit per scene).
  std::unique_ptr<Edit> begin_edit();

  ContextState& ctx() { return ctx_; }
  Report& report() { return report_; }
  Arena& arena() { return arena_; }
  GenerationTracker& generations() { return generations_; }
  WorkerPool& pool() { return *pool_; }

  // Blocks until the init thread is done; throws std::runtime_error with its failure.
  void wait_init();
  bool init_done();

  // The last active generation's stored state (later tasks' jobs read it).
  std::shared_ptr<const StoredSettings> settings() const;
  std::shared_ptr<const StoredCamera> camera() const;
  bool astro(ar_key key, ar_astro_desc* out) const;
  bool known(uint32_t kind, ar_key key) const;

 private:
  friend class Edit;
  struct State {
    mutable std::mutex mutex;
    // Declared keys: optimistic (changes apply at commit so pipelined edits see them) and
    // rebuilt from known_active plus the still-queued generations when one is dropped.
    std::set<std::pair<uint32_t, uint64_t>> known;
    std::set<std::pair<uint32_t, uint64_t>> known_active;  // as of the active generation
    std::shared_ptr<const StoredSettings> settings;
    std::shared_ptr<const StoredCamera> camera;
    std::map<uint64_t, ar_astro_desc> astro;  // stored, no effect until M10
  };
  void close_edit();

  ContextState& ctx_;
  Report report_;
  GenerationTracker generations_;
  Arena arena_;
  State state_;
  std::mutex init_mutex_;
  std::condition_variable init_cv_;
  bool init_done_ = false;
  std::string init_error_;
  std::shared_ptr<void> init_result_;
  std::thread init_thread_;
  std::mutex edit_mutex_;
  bool edit_open_ = false;
  std::unique_ptr<WorkerPool> pool_;  // last: joined first
};

}  // namespace astroray::host
