// SPDX-License-Identifier: Apache-2.0
#include "host/scene/scene.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>

#include "host/data_resource.h"
#include "host/device.h"
#include "host/light/env_bake.h"
#include "host/light/lamps.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/accel.h"
#include "host/scene/edit.h"
#include "host/scene/media.h"
#include "host/scene/mesh.h"
#include "host/scene/world.h"
#include "host/scene/scene_tables.h"
#include "host/session/test_hooks.h"
#include "host/thread.h"
#include "host/thread_pool.h"

namespace astroray::host {

void apply_known(std::set<std::pair<uint32_t, uint64_t>>& known, const Pending& p) {
  if (p.known_reset) known.clear();
  for (const auto& r : p.known_removed) known.erase(r);
  for (const auto& a : p.known_added) known.insert(a);
}

Scene::Scene(ContextState& ctx, SceneConfig config)
    : ctx_(ctx), generations_(ctx, report_), device_of_(std::move(config.device_of)),
      device_ledger_(std::move(config.device_ledger)), accel_of_(std::move(config.accel_of)),
      tables_(std::make_unique<SceneTableCache>()) {
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
  // Read once, here: a malformed ASTRORAY_TEST_* value refuses the scene instead of failing the worker.
  const bool fail_init = read_test_hooks().fail_init;
  init_thread_ = start_engine_thread(ctx, [this, init = std::move(init), fail_init] {
    std::string err;
    bool device_error = false, device_sticky = false;
    {
      // Waiters are released however this scope ends.
      struct Done {
        Scene& s;
        std::string& err;
        bool& device_error;
        bool& device_sticky;
        ~Done() {
          {
            std::lock_guard<std::mutex> lock(s.init_mutex_);
            s.init_error_ = err;
            s.init_device_error_ = device_error;
            s.init_device_sticky_ = device_sticky;
            s.init_done_ = true;
          }
          s.init_cv_.notify_all();
        }
      } done{*this, err, device_error, device_sticky};
      try {
        if (init) {
          // Ref: 0002 D15 (failing init worker injection).
          if (fail_init)
            throw DeviceError("test hook ASTRORAY_TEST_FAIL_INIT: injected device init failure");
          init_result_ = init();
        }
      } catch (const DeviceError& e) {
        err = e.what()[0] ? e.what() : "device init failed";
        device_error = true;
        device_sticky = e.sticky();
      } catch (const std::exception& e) {
        err = e.what()[0] ? e.what() : "device init failed";
      } catch (...) {
        err = "device init failed: unknown exception";
      }
      // Posted when it happens (0002 D15), before the waiters are released, not when a session first
      // needs the device; the first upload re-raises it from wait_init and the host scene data is kept.
      if (!err.empty()) {
        try {
          const double value[2] = {0.0, 0.0};
          report_.raise(AR_REPORT_DEVICE_INIT_FAILED, 0, err, 1, value, 0);
        } catch (...) {
          // Out of memory while reporting: wait_init still throws the text.
        }
      }
    }
    if (!err.empty()) {
      ctx_.log.line("ERROR", "scene device init failed: " + err);
    }
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
  if (!init_error_.empty()) {
    if (init_device_error_) throw DeviceError(init_error_, init_device_sticky_);
    throw std::runtime_error(init_error_);
  }
}

Device* Scene::device() {
  wait_init();
  return device_of_ && init_result_ ? device_of_(init_result_.get()) : nullptr;
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

namespace {

// What plan_lights gathered for build_lights: one consistent copy, so the build reads nothing of the
// scene's live state but the cache it updates.
struct LightInputs {
  SceneLights in;
  std::vector<std::shared_ptr<const Lamp>> lamps;
  std::shared_ptr<const ObserverTables> observer;
  double gather_ms = 0.0;  // the full gather, which B-23's rebuild time includes
};

}  // namespace

std::shared_ptr<const LightTable> Scene::lights() {
  const LightPlan plan = plan_lights(false);
  return plan.table ? plan.table : build_lights(plan, nullptr, false);
}

LightPlan Scene::plan_lights(bool baking) {
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
  LightPlan plan;
  plan.world = materials->world();
  plan.images = materials->images().arrays();
  const WorldOutput* world = plan.world.get();
  plan.bake_key = EnvironmentBaker::world_key(world, plan.images.get());
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
    // The world program's identity: a table is built after its world's bake (0004 D11).
    in->key.insert(in->key.end(), plan.bake_key.begin(), plan.bake_key.end());
  };
  SceneLights in = gather_scene_lights(state_.instances.get(), *meshes, *mats, world, false);
  with_lamps(&in);
  const bool unbaked = baking && !plan.bake_key.empty() && state_.lights && !state_.lights->env_baked;
  if (state_.lights && in.key == state_.light_key && !unbaked) {
    plan.table = state_.lights;
    return plan;
  }
  auto inputs = std::make_shared<LightInputs>();
  const auto t_gather = std::chrono::steady_clock::now();
  inputs->in = gather_scene_lights(state_.instances.get(), *meshes, *mats, world, true);
  with_lamps(&inputs->in);
  inputs->gather_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_gather).count();
  inputs->lamps = std::move(lamps);
  inputs->observer = state_.observer;
  plan.inputs = std::move(inputs);
  return plan;
}

std::shared_ptr<const LightTable> Scene::build_lights(const LightPlan& plan, const BakedEnvironment* baked, bool world_refused) {
  if (!plan.inputs) throw std::logic_error("build_lights: the plan holds a cached table and no inputs");
  const LightInputs& src = *static_cast<const LightInputs*>(plan.inputs.get());
  std::lock_guard<std::mutex> lock(state_.mutex);
  // B-23 is the rebuild alone, not the key check: plan_lights' full gather plus everything from here.
  using Clock = std::chrono::steady_clock;
  const Clock::time_point t_start = Clock::now();
  SceneLights in = src.in;
  // A refused grid leaves no world light: misses render black, never the constant (rule 5).
  if (world_refused)
    in.lights.erase(std::remove_if(in.lights.begin(), in.lights.end(),
                                   [](const LightInput& li) { return li.record.type == uint8_t(ar::LightType::WORLD_CONST); }),
                    in.lights.end());
  const std::vector<std::shared_ptr<const Lamp>>& lamps = src.lamps;
  const uint32_t lamp_first = uint32_t(in.lights.size());
  for (const auto& l : lamps) {
    LightInput li;
    li.record = l->record;
    in.lights.push_back(std::move(li));
  }
  const ar::ObserverView obs{&src.observer->cmf[0][0], src.observer->d65};
  // 0003 d10: s = 2^k puts the largest emitter peak in [1, 2); it multiplies every emitter's strength
  // (record and estimate terms alike, so the importances and pmf do not move) and the film resolve
  // divides it out again. The key covers every emitter, so s changes only with a rebuild.
  // Per-light work runs on this thread and the scene workers (0007 d23); the bodies read only `in`
  // and `obs` and write their own indices, and the maximum stays in radiance_scale, serial.
  const ChunkRunner run = [this](uint32_t count, const std::function<void(uint32_t, uint32_t)>& body) {
    pool_->parallel_for(count, kDefaultChunk, body);  // 0002 D4 chunk size
  };
  const Clock::time_point t_gathered = Clock::now();
  std::vector<double> peaks(in.lights.size());
  run(uint32_t(in.lights.size()), [&](uint32_t first, uint32_t last) {
    for (uint32_t i = first; i < last; ++i)
      peaks[i] = (i != first && same_terms(in.lights[i - 1], in.lights[i])) ? peaks[i - 1] : emitter_peak(in.lights[i], obs);
  });
  const double s = host::radiance_scale(peaks);
  for (LightInput& li : in.lights) {
    li.record.strength = float(double(li.record.strength) * s);
    for (EmissionTerm& t : li.terms) t.strength = float(double(t.strength) * s);
  }
  const Clock::time_point t_scaled = Clock::now();
  // One immutable snapshot: the lights, s, and the lamps' instance records agree by construction.
  // The table is ours and unpublished until the assignment below, so the cast is safe.
  auto table = std::const_pointer_cast<LightTable>(build_light_table(in.lights, in.scene, obs, meshes->shared_ledger(), run, baked));
  {
    const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const Clock::time_point t_end = Clock::now();
    size_t triangles = 0;
    for (const ar::LightRecord& r : table->lights) triangles += r.type == uint8_t(ar::LightType::TRIANGLE);
    char line[240];
    std::snprintf(line, sizeof line,
                  "light table: %zu lights (%zu emissive triangles) built in %.1f ms (gather %.1f, peaks and scale %.1f, "
                  "importance and alias %.1f) on %u threads",
                  table->lights.size(), triangles, src.gather_ms + ms(t_start, t_end), src.gather_ms + ms(t_start, t_gathered),
                  ms(t_gathered, t_scaled),
                  ms(t_scaled, t_end), pool_->workers() + 1);
    ctx_.log.line("INFO", line);
  }
  table->radiance_scale = s;
  table->lamp_first = lamp_first;
  table->env_baked = baked != nullptr || world_refused;
  if (baked) table->env = baked->table, table->env_max = baked->max, table->env_mean = baked->mean;
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
  state_.light_lamps = lamps;
  state_.light_images = plan.images;
  ++state_.light_rebuilds;
  return state_.lights;
}

void Scene::publish_lamp(std::shared_ptr<const Lamp> lamp) {
  std::lock_guard<std::mutex> lock(state_.mutex);
  const ar_key k = lamp->key;
  state_.lamps[k] = std::move(lamp);
}

void Scene::publish_medium(std::shared_ptr<const Medium> medium) {
  std::lock_guard<std::mutex> lock(state_.mutex);
  const ar_key k = medium->key;
  state_.media[k] = std::move(medium);
}

void Scene::refresh_medium_reports() noexcept {
  try {
    std::set<ar_key> work, now;
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      if (state_.instances) now = state_.instances->medium_geoms;
      work = state_.medium_reported;
      work.insert(now.begin(), now.end());
      state_.medium_reported = now;
    }
    for (ar_key g : work) {
      try {
        // Dropped even when the mesh is replaced and the generation already cleared it: the entry is rebuilt
        // from the final state. The log line is for a condition that is new, not for every generation.
        const bool had = report_.remove(AR_REPORT_MEDIUM_MESH_OPEN, g) != 0;
        if (!now.count(g)) continue;
        const auto rec = meshes ? meshes->find(g) : nullptr;
        if (!rec || !rec->topo) {
          ctx_.log.line("ERROR", "geometry key " + std::to_string(g) +
                                     " bears a medium but has no installed mesh (a geometry kind this check does not know); "
                                     "its closedness was not checked");
          continue;
        }
        if (rec->topo->closed) continue;
        const std::string why = "mesh '" + rec->topo->name + "' (geometry key " + std::to_string(g) +
                                ") bears a medium but is not closed with consistent winding; the medium follows Cycles' convention";
        if (!had) ctx_.log.line("WARNING", why);
        report_.raise(AR_REPORT_MEDIUM_MESH_OPEN, g, why, 1, nullptr, 0);
      } catch (...) {
        try {
          ctx_.log.line("ERROR", "MEDIUM_MESH_OPEN for geometry key " + std::to_string(g) + " could not be recorded");
        } catch (...) {
          // the log itself failed: nothing is left to report through
        }
      }
    }
  } catch (...) {
    try {
      ctx_.log.line("ERROR", "MEDIUM_MESH_OPEN: the medium reports could not be refreshed (out of memory)");
    } catch (...) {
      // the log itself failed: nothing is left to report through
    }
  }
}

std::shared_ptr<const Medium> Scene::medium(ar_key key) const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  auto it = state_.media.find(key);
  if (it == state_.media.end() || !state_.known_active.count({AR_KIND_MEDIUM, key})) return nullptr;
  return it->second;
}

std::vector<std::shared_ptr<const Medium>> Scene::media() const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  std::vector<std::shared_ptr<const Medium>> out;
  for (const auto& kv : state_.media)
    if (state_.known_active.count({AR_KIND_MEDIUM, kv.first})) out.push_back(kv.second);
  return out;
}

std::shared_ptr<const InstanceTable> Scene::instances_with_lamps(const LightTable& table) const {
  return append_lamp_instances(instances().get(), table.lamp_instances);
}

SceneSnapshot Scene::snapshot(bool with_lights) {
  SceneSnapshot s;
  generations_.with_active([&](uint64_t generation) {
    s.generation = generation;
    s.instances = instances();
    s.media = media();
    if (materials) s.materials = materials->arrays(), s.images = materials->images().arrays();
    if (s.instances)
      for (const Instance& in : s.instances->instances)
        if (in.geom_key != 0 || in.light_index == ar::kNoLightIndex)
          s.meshes[in.geom_key] = meshes ? meshes->find(in.geom_key) : nullptr;
    if (with_lights && materials && meshes) {
      // A world that needs no bake is built here as before; a program world waits for its bake.
      s.light_plan = plan_lights(true);
      if (!s.light_plan.table && s.light_plan.bake_key.empty()) {
        s.light_plan.table = build_lights(s.light_plan, nullptr, false);
        s.light_plan.inputs.reset();  // built: the scene table build must not build it again
      }
      s.lights = s.light_plan.table;
    }
  });
  return s;
}

uint64_t Scene::light_rebuilds() const {
  std::lock_guard<std::mutex> lock(state_.mutex);
  return state_.light_rebuilds;
}

AccelBuilder& Scene::accel_builder() {
  wait_init();
  if (accel_of_ && init_result_) {
    AccelBuilder* b = accel_of_(init_result_.get());
    if (!b) throw std::runtime_error("the scene's device made no acceleration structure builder");
    return *b;
  }
  // A scene with a device must bring its own builder: Embree structures cannot serve a CUDA scene.
  if (device_of_) throw std::logic_error("the scene has a device but no acceleration structure builder (SceneConfig::accel_of)");
  std::lock_guard<std::mutex> lock(init_mutex_);
  if (!cpu_accel_) cpu_accel_ = make_cpu_accel_builder();
  return *cpu_accel_;
}

}  // namespace astroray::host
