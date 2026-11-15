// SPDX-License-Identifier: Apache-2.0
#include "host/scene/scene.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "host/scene/edit.h"
#include "host/thread.h"

namespace astroray::host {

void apply_known(std::set<std::pair<uint32_t, uint64_t>>& known, const Pending& p) {
  if (p.known_reset) known.clear();
  for (const auto& r : p.known_removed) known.erase(r);
  for (const auto& a : p.known_added) known.insert(a);
}

Scene::Scene(ContextState& ctx, SceneConfig config) : ctx_(ctx), generations_(ctx, report_) {
  unsigned workers = config.workers;
  if (workers == 0) workers = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
  pool_ = std::make_unique<WorkerPool>(ctx, workers);
  generations_.set_drop_hook([this](const std::deque<std::shared_ptr<Pending>>& queued) {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.known = state_.known_active;
    for (const auto& q : queued) apply_known(state_.known, *q);
  });
  arena_.set_prefault_request([this] { pool_->submit([this] { arena_.prefault_one(); }); });
  auto init = std::move(config.init);
  init_thread_ = start_engine_thread(ctx, [this, init = std::move(init)] {
    std::string err;
    {
      // Waiters are released however this scope ends.
      struct Done {
        Scene& s;
        std::string& err;
        ~Done() {
          {
            std::lock_guard<std::mutex> lock(s.init_mutex_);
            s.init_error_ = err;
            s.init_done_ = true;
          }
          s.init_cv_.notify_all();
        }
      } done{*this, err};
      try {
        if (init) init_result_ = init();
      } catch (const std::exception& e) {
        err = e.what()[0] ? e.what() : "device init failed";
      } catch (...) {
        err = "device init failed: unknown exception";
      }
    }
    if (!err.empty()) ctx_.log.line("ERROR", "scene device init failed: " + err);
  });
}

Scene::~Scene() {
  pool_->shutdown();  // runs queued jobs, which may wait for init; pool_ stays valid meanwhile
  init_thread_.join();
}

std::unique_ptr<Edit> Scene::begin_edit() {
  {
    std::lock_guard<std::mutex> lock(edit_mutex_);
    if (edit_open_) return nullptr;
    edit_open_ = true;
  }
  return std::unique_ptr<Edit>(new Edit(*this));
}

void Scene::close_edit() {
  std::lock_guard<std::mutex> lock(edit_mutex_);
  edit_open_ = false;
}

void Scene::wait_init() {
  std::unique_lock<std::mutex> lock(init_mutex_);
  // A fault on the init thread poisons the context and never sets init_done_.
  while (!init_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return init_done_; }))
    if (ctx_.poisoned.load(std::memory_order_acquire))
      throw std::runtime_error("device init did not finish: " + ctx_.poison_message);
  if (!init_error_.empty()) throw std::runtime_error(init_error_);
}

bool Scene::init_done() {
  std::lock_guard<std::mutex> lock(init_mutex_);
  return init_done_;
}

std::shared_ptr<const StoredSettings> Scene::settings() const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  return state_.settings;
}
std::shared_ptr<const StoredCamera> Scene::camera() const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  return state_.camera;
}
bool Scene::astro(ar_key key, ar_astro_desc* out) const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  auto it = state_.astro.find(key);
  if (it == state_.astro.end()) return false;
  *out = it->second;
  return true;
}
bool Scene::known(uint32_t kind, ar_key key) const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  return state_.known.count({kind, key}) != 0;
}

std::shared_ptr<const InstanceTable> Scene::instances() const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  return state_.instances;
}
uint64_t Scene::tlas_epoch() const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  return state_.tlas_epoch;
}
void Scene::publish_instances(std::shared_ptr<const InstanceTable> table) {
  std::lock_guard<std::mutex> lock(state_.mutex);
  state_.instances = std::move(table);
  ++state_.tlas_epoch;
}

}  // namespace astroray::host
