// SPDX-License-Identifier: Apache-2.0
// One open scene edit: stages items, hands each to the workers at its item commit, stores
// the settings, camera and astro descriptors, and turns into one generation at commit
// (0007 decisions 5, 9, 12, 24, 41). Every call returns a status and leaves its reason in
// last_error(). The edit is dead after commit or abort.
// Ref: 0007 decisions 5, 9, 24, 41; 0001 D2.3 (staging).
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "host/scene/scene.h"

namespace astroray::host {

// Thrown by a job to name the report subject of its rejection (default: the item's key).
struct ItemFailure : std::runtime_error {
  uint64_t subject;
  ItemFailure(uint64_t s, const std::string& what) : std::runtime_error(what), subject(s) {}
};

// What a begin call (035 and later) registers for one key. `job` validates and converts the
// staged arrays on a worker and throws on failure (its text is the report detail).
struct EditItem {
  ar_key key = 0;
  uint32_t kind = AR_KIND_GEOMETRY;
  std::function<void()> job;
  // Internal items only (Edit::stage_internal): called at commit with the geometry and the medium
  // predicate of the edit's final key set, before the job is handed to a worker.
  std::function<void(const std::function<bool(ar_key)>& geometry, const std::function<bool(ar_key)>& medium)> bind;
  bool replaces_subject = true;     // false: leaves the key's scene report entries alone
  // If set, item_commit hands it a function that queues the job instead of queueing at
  // once; the item calls it when its job may run (a dependency finished). It must call it
  // exactly once, on any thread, and never block a worker waiting for another job.
  std::function<void(std::function<void()>)> schedule;
  bool needs_device = true;         // waits for init and re-raises its failure
  std::function<void()> release;    // after the job finished: free arena blocks
  std::function<void()> on_active;  // generation became active: free replaced objects
  std::function<void()> on_dropped;  // generation dropped: free the new objects
  // Staged arrays use the T** form: the host reads `*field`, so each field points at a
  // slot here whose value is the array pointer; slots keep their address.
  std::deque<void*> slots;
  void** new_slot(void* array) {
    slots.push_back(array);
    return &slots.back();
  }
};

class Edit {
 public:
  ~Edit();  // an edit neither committed nor aborted is aborted
  Edit(const Edit&) = delete;
  Edit& operator=(const Edit&) = delete;

  ar_status reset();  // first call only
  // Registers a staged item; item_commit hands it to the workers.
  ar_status stage(std::shared_ptr<EditItem> item);
  ar_status item_commit(ar_key key);
  ar_status remove(uint32_t kind, ar_key key);
  ar_status set_settings(const ar_settings* s);
  ar_status set_camera(const ar_camera_desc* c);
  ar_status set_astro(ar_key key, const ar_astro_desc* a);
  ar_status commit(uint64_t* generation);
  ar_status abort();

  const std::string& last_error() const { return last_error_; }

  // For the instance entry points (036). An internal item is not a scene key: it never
  // enters the staged map or the known-key set; it starts at commit, after `bind`, and
  // its callbacks run after the generation's own state is applied.
  Scene& scene() { return scene_; }
  ar_status fail(const std::string& why) { return bad(why); }
  ar_status stage_internal(std::shared_ptr<EditItem> item);
  bool reset_requested() const { return reset_; }
  std::set<uint64_t> removed_geometry() const;  // removed here and not re-staged
  std::set<uint64_t> removed_media() const;     // likewise
  struct InstanceStage {
    bool has_set = false;
    uint32_t count = 0, steps = 1;
    std::shared_ptr<const InstanceSummary> summary;  // of this edit's last set_instances
  };
  InstanceStage& instance_stage() { return instances_; }

 private:
  friend class Scene;
  explicit Edit(Scene& scene);
  ar_status bad(const std::string& why);
  ar_status touch();  // common precondition: not finished; counts as a call
  void submit(const std::shared_ptr<EditItem>& item);
  // Geometry key as it stands once this edit is applied (its own staged geometry, its
  // removes, the scene's declared keys including earlier pipelined generations).
  bool final_geometry(ar_key key) const;
  bool final_medium(ar_key key) const;  // the same for a medium key
  std::set<uint64_t> removed_of(uint32_t kind) const;

  struct Staged {
    std::shared_ptr<EditItem> item;
    bool committed = false;
  };

  Scene& scene_;
  std::shared_ptr<Pending> pending_;
  std::string last_error_;
  bool finished_ = false;
  bool any_call_ = false;
  bool reset_ = false;
  std::map<ar_key, Staged> staged_;
  std::vector<std::shared_ptr<EditItem>> internal_;
  std::set<std::pair<uint32_t, uint64_t>> removed_;
  std::shared_ptr<StoredSettings> settings_;
  std::shared_ptr<StoredCamera> camera_;
  std::map<uint64_t, ar_astro_desc> astro_;
  InstanceStage instances_;
};

}  // namespace astroray::host

// The C handles. last_error is the first member so that the unsupported-entry stubs
// (capi/unimplemented.cpp) can store their message through any of them.
struct ar_edit_s;
struct ar_scene_s {
  std::string last_error;
  std::unique_ptr<astroray::host::Scene> scene;
  std::unique_ptr<ar_edit_s> edit;  // the one open edit; destroyed first
};
struct ar_edit_s {
  std::string last_error;
  ar_scene_s* owner = nullptr;
  std::unique_ptr<astroray::host::Edit> edit;
};
