// SPDX-License-Identifier: Apache-2.0
#include "host/scene/scene.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "host/data_resource.h"
#include "host/light/lamps.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/mesh.h"
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
    state_.inst_latest = state_.inst_active;
    for (const auto& q : queued) apply_instance_summary(state_.inst_latest, *q);
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
std::shared_ptr<const InstanceSummary> Scene::latest_instances() const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  return state_.inst_latest;
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

std::shared_ptr<const LightTable> Scene::lights() {
  std::lock_guard<std::mutex> lock(state_.mutex);
  if (!state_.observer) {
    DataBlob blob;
    if (!find_data("spectral/observer.bin", &blob)) throw std::runtime_error("light table: spectral/observer.bin is not embedded");
    auto t = std::make_shared<ObserverTables>();
    const std::string err = parse_observer(blob.data, blob.size, t.get());
    if (!err.empty()) throw std::runtime_error("light table: " + err);
    state_.observer = std::move(t);
  }
  const auto mats = materials->arrays();
  const auto world = materials->world();
  // Live lamps: set, and still in the active generation's key set (a remove or reset drops them).
  // Stale map entries stay, since one may be between its publish and its key-set update.
  std::vector<std::shared_ptr<const Lamp>> lamps;
  for (const auto& kv : state_.lamps)
    if (state_.known_active.count({AR_KIND_LIGHT, kv.first}) != 0) lamps.push_back(kv.second);
  const bool world_lit = world && world->strength > 0.0f;
  auto with_lamps = [&](SceneLights* in) {
    // Lamp bounds join the scene sphere, so a scene of lamps alone still gives the world power.
    for (const auto& l : lamps) in->scene = enclose(in->scene, *l);
    for (const auto& l : lamps) {
      const Lamp* p = l.get();  // pinned by light_lamps while the table lives, so it names this lamp only
      const uint8_t* k = reinterpret_cast<const uint8_t*>(&l->key);
      in->key.insert(in->key.end(), k, k + sizeof(l->key));
      const uint8_t* a = reinterpret_cast<const uint8_t*>(&p);
      in->key.insert(in->key.end(), a, a + sizeof(p));
    }
    if (world_lit && !lamps.empty()) {
      const uint8_t* b = reinterpret_cast<const uint8_t*>(&in->scene);
      in->key.insert(in->key.end(), b, b + sizeof(in->scene));
    }
  };
  SceneLights in = gather_scene_lights(state_.instances.get(), *meshes, *mats, world.get(), false);
  with_lamps(&in);
  if (state_.lights && in.key == state_.light_key) return state_.lights;
  in = gather_scene_lights(state_.instances.get(), *meshes, *mats, world.get(), true);
  with_lamps(&in);
  const uint32_t lamp_first = uint32_t(in.lights.size());
  for (const auto& l : lamps) {
    LightInput li;
    li.record = l->record;
    in.lights.push_back(std::move(li));
  }
  const ar::ObserverView obs{&state_.observer->cmf[0][0], state_.observer->d65};
  // 0003 d10: s = 2^k puts the largest emitter peak in [1, 2); it multiplies every emitter's strength
  // (record and estimate terms alike, so the importances and pmf do not move) and the film resolve
  // divides it out again. The key covers every emitter, so s changes only with a rebuild.
  std::vector<double> peaks;
  for (const LightInput& li : in.lights) peaks.push_back(emitter_peak(li, obs));
  const double s = host::radiance_scale(peaks);
  for (LightInput& li : in.lights) {
    li.record.strength = float(double(li.record.strength) * s);
    for (EmissionTerm& t : li.terms) t.strength = float(double(t.strength) * s);
  }
  // One immutable snapshot: the lights, s, and the lamps' instance records agree by construction.
  // The table is ours and unpublished until the assignment below, so the cast is safe.
  auto table = std::const_pointer_cast<LightTable>(build_light_table(in.lights, in.scene, obs, meshes->shared_ledger()));
  table->radiance_scale = s;
  table->lamp_first = lamp_first;
  for (size_t i = 0; i < lamps.size(); ++i) {
    const Lamp& l = *lamps[i];
    table->lamp_instances.push_back({l.key, l.xf, l.visibility, l.record.light_group, lamp_first + uint32_t(i)});
  }
  // Entries whose key is in neither the active nor the latest key set are gone for good; one between
  // its publish and its key-set update is in the latest set, so it stays.
  for (auto it = state_.lamps.begin(); it != state_.lamps.end();)
    it = (state_.known_active.count({AR_KIND_LIGHT, it->first}) == 0 && state_.known.count({AR_KIND_LIGHT, it->first}) == 0)
             ? state_.lamps.erase(it)
             : std::next(it);
  state_.lights = std::move(table);
  state_.light_key = std::move(in.key);
  state_.light_meshes = std::move(in.meshes);
  state_.light_lamps = std::move(lamps);
  ++state_.light_rebuilds;
  return state_.lights;
}

void Scene::publish_lamp(std::shared_ptr<const Lamp> lamp) {
  std::lock_guard<std::mutex> lock(state_.mutex);
  const ar_key k = lamp->key;
  state_.lamps[k] = std::move(lamp);
}

std::shared_ptr<const InstanceTable> Scene::instances_with_lamps(const LightTable& table) const {
  return append_lamp_instances(instances().get(), table.lamp_instances);
}

uint64_t Scene::light_rebuilds() const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  return state_.light_rebuilds;
}

}  // namespace astroray::host
