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
#include "host/ledger.h"
#include "host/report.h"
#include "host/scene/arena.h"
#include "host/scene/generation.h"
#include "host/scene/instances.h"

struct ar_context_s;
struct ar_scene_s;

namespace astroray {
// Defined in capi/context.cpp, where the context struct is private.
ContextState& context_state(ar_context_s* ctx);
void context_set_error(ar_context_s* ctx, const std::string& text);
void context_device(ar_context_s* ctx, uint32_t* kind, uint32_t* index);
// ar_scene_destroy's deferred part (0007 d23): the context keeps the emptied handle until it is
// destroyed and its reaper frees the edit and scene. `state` is the scene's context.
void context_retire_scene(ContextState& state, ar_scene_s* h);
struct ObserverTables;  // host/data_resource.h
}  // namespace astroray

namespace astroray::host {

class Device;           // host/device.h
class AccelBuilder;     // host/scene/accel.h
struct SceneTableCache;  // host/scene/scene_tables.h
class Edit;
class MeshStore;  // host/scene/mesh.h
struct MeshRecord;
class MaterialStore;  // host/material/material_store.h
struct LightTable;    // host/light/light_builder.h
struct Lamp;          // host/light/lamps.h
struct Medium;        // host/scene/media.h

// Applies one generation's key-set changes (reset, removes, then inserts).
void apply_known(std::set<std::pair<uint32_t, uint64_t>>& known, const Pending& p);

struct SceneConfig {
  unsigned workers = 0;  // 0: min(8, logical cores / 2), at least 1 (0007 decision 23)
  // Runs on the init thread (CUDA context and module loading); the returned object is kept
  // until the scene is destroyed. Empty on the CPU device. A throw is the init failure.
  std::function<std::shared_ptr<void>()> init;
  // The scene's device inside init's result: it uploads scene tables and charges the scene's
  // device ledger (RULINGS 70). Empty on the CPU device.
  std::function<Device*(void*)> device_of;
  // That device ledger: device memory only, apart from the mesh store's host ledger, so sessions
  // can report it beside their own before init has finished (0007 d14). Null on the CPU device.
  std::shared_ptr<Ledger> device_ledger;
  // The scene's traversal-structure builder inside init's result (the OptiX one, created on the init
  // thread with the device). Empty on the CPU device: the scene then owns an Embree builder.
  std::function<AccelBuilder*(void*)> accel_of;
};

// ar_settings and ar_camera_desc with their copy-in arrays owned.
struct ImageArrays;     // host/image/image_store.h
struct MaterialArrays;  // host/material/material_store.h

// One immutable view of the active state, captured under the generation tracker's lock so every
// part belongs to `generation` (RULINGS 70): the instances, the material and image arrays, the
// active mesh record of every key the instances reference (null when none is active), and, when
// asked for, the light table built from that same state.
struct SceneSnapshot {
  uint64_t generation = 0;
  std::shared_ptr<const InstanceTable> instances;
  std::shared_ptr<const MaterialArrays> materials;
  std::shared_ptr<const ImageArrays> images;
  std::map<ar_key, std::shared_ptr<const MeshRecord>> meshes;
  std::shared_ptr<const LightTable> lights;
};

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
  // Host mesh records (035); created by ar_scene_create.
  std::shared_ptr<MeshStore> meshes;
  // Graphs, materials and their compiled arrays (059); created by ar_scene_create.
  std::shared_ptr<MaterialStore> materials;

  // Blocks until the init thread is done; throws its failure: a DeviceError (AR_ERR_DEVICE) when the init
  // failed in the device, else std::runtime_error.
  void wait_init();
  bool init_done();
  // The device init made (wait_init first, so it throws the init failure); null on a CPU context.
  Device* device();
  // SceneConfig::device_ledger; never waits for the init.
  std::shared_ptr<Ledger> device_ledger() const { return device_ledger_; }
  // The builder of this scene's traversal structures (waits for device init like device()).
  AccelBuilder& accel_builder();
  // The scene's table sets (host/scene/scene_tables.h): the one cache every session shares.
  SceneTableCache& table_cache() { return *tables_; }

  // The last active generation's stored state (later tasks' jobs read it).
  std::shared_ptr<const StoredSettings> settings() const;
  std::shared_ptr<const StoredCamera> camera() const;
  bool astro(ar_key key, ar_astro_desc* out) const;
  bool known(uint32_t kind, ar_key key) const;
  // The active generation's instances (null before the first set_instances), and a counter
  // that moves whenever they change, which tells the device tasks the TLAS is dirty.
  std::shared_ptr<const InstanceTable> instances() const;
  uint64_t tlas_epoch() const;
  // The instance summary as of the latest committed generation, pending ones included
  // (optimistic like `known`, rebuilt when a generation is dropped).
  std::shared_ptr<const InstanceSummary> latest_instances() const;
  void publish_instances(std::shared_ptr<const InstanceTable> table);
  // The light table of the active state (lamps join in 068). Rebuilt on the call only when its
  // inputs changed (gather_scene_lights' key), under the state lock; call it at the session's
  // latch, never per pass or launch. Throws std::runtime_error when observer.bin is unusable or
  // a material's emission sampling is out of range.
  std::shared_ptr<const LightTable> lights();
  uint64_t light_rebuilds() const;  // how many times lights() rebuilt the table
  // The active state as one snapshot; `with_lights` also builds (or reuses) the light table of it,
  // when the scene has material and mesh stores. Lock order: tracker, then state and stores.
  SceneSnapshot snapshot(bool with_lights);
  // A lamp the activating generation set (068). Lamps count only while their key is in the active
  // generation's key set, so a removed or reset-away lamp drops out without a callback.
  void publish_lamp(std::shared_ptr<const Lamp> lamp);
  // A medium the activating generation set (181). Like lamps, media count only while their key is in the
  // active generation's key set. `medium` is null for an unknown, removed or not yet active key; `media`
  // lists the live ones in key order, the order the medium table indexes them in.
  void publish_medium(std::shared_ptr<const Medium> medium);
  std::shared_ptr<const Medium> medium(ar_key key) const;
  std::vector<std::shared_ptr<const Medium>> media() const;
  // The active instances plus one record per lamp of `table`, which carries each lamp's light
  // index. Never null (an empty scene gives an empty table). It reads only `table` and the active
  // instances, so the light indices are those of the snapshot the caller holds.
  std::shared_ptr<const InstanceTable> instances_with_lamps(const LightTable& table) const;

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
    std::shared_ptr<const InstanceTable> instances;
    uint64_t tlas_epoch = 0;
    std::shared_ptr<const InstanceSummary> inst_latest, inst_active;
    std::shared_ptr<const ObserverTables> observer;  // loaded by the first lights()
    std::vector<uint8_t> light_key;
    std::vector<std::shared_ptr<const MeshRecord>> light_meshes;  // pins the records light_key names
    std::map<ar_key, std::shared_ptr<const Lamp>> lamps;          // as set; live ones are in known_active
    std::map<ar_key, std::shared_ptr<const Medium>> media;        // as set; live ones are in known_active
    std::vector<std::shared_ptr<const Lamp>> light_lamps;         // the last build's lamps, pinned for light_key
    std::shared_ptr<const LightTable> lights;
    uint64_t light_rebuilds = 0;
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
  bool init_device_error_ = false;  // the init failure was a DeviceError: wait_init keeps its type
  bool init_device_sticky_ = false;  // ... and its sticky flag
  std::shared_ptr<void> init_result_;
  std::function<Device*(void*)> device_of_;
  std::shared_ptr<Ledger> device_ledger_;
  std::function<AccelBuilder*(void*)> accel_of_;
  std::unique_ptr<AccelBuilder> cpu_accel_;  // declared before tables_, so it outlives their snapshots
  // After init_result_, so the table copies on the scene's device go first.
  std::unique_ptr<SceneTableCache> tables_;
  std::thread init_thread_;
  std::mutex edit_mutex_;
  bool edit_open_ = false;
  std::unique_ptr<WorkerPool> pool_;  // last: joined first
};

}  // namespace astroray::host
