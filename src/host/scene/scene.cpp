// SPDX-License-Identifier: Apache-2.0
#include "host/scene/scene.h"

#include <algorithm>
#include <stdexcept>

#include "host/scene/edit.h"
#include "host/thread.h"

namespace astroray::host {

Scene::Scene(ContextState& ctx, SceneConfig config) : ctx_(ctx), generations_(ctx, report_) {
  unsigned workers = config.workers;
  if (workers == 0) workers = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
  pool_ = std::make_unique<WorkerPool>(ctx, workers);
  arena_.set_prefault_request([this] { pool_->submit([this] { arena_.prefault_one(); }); });
  auto init = std::move(config.init);
  init_thread_ = start_engine_thread(ctx, [this, init = std::move(init)] {
    std::string err;
    try {
      if (init) init_result_ = init();
    } catch (const std::exception& e) {
      err = e.what()[0] ? e.what() : "device init failed";
    } catch (...) {
      err = "device init failed: unknown exception";
    }
    if (!err.empty()) ctx_.log.line("ERROR", "scene device init failed: " + err);
    {
      std::lock_guard<std::mutex> lock(init_mutex_);
      init_error_ = err;
      init_done_ = true;
    }
    init_cv_.notify_all();
  });
}

Scene::~Scene() {
  pool_.reset();  // runs queued jobs, which may wait for init
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
  init_cv_.wait(lock, [this] { return init_done_; });
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

}  // namespace astroray::host
