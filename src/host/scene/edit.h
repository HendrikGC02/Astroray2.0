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
#include <string>
#include <utility>

#include "host/scene/scene.h"

namespace astroray::host {

// What a begin call (035 and later) registers for one key. `job` validates and converts the
// staged arrays on a worker and throws on failure (its text is the report detail).
struct EditItem {
  ar_key key = 0;
  uint32_t kind = AR_KIND_GEOMETRY;
  std::function<void()> job;
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

  // For the instance entry points (036): the scene, a reasoned refusal, whether a key is
  // declared as seen by this edit (earlier generations plus its own committed items), and
  // what this edit's set_instances established.
  Scene& scene() { return scene_; }
  ar_status fail(const std::string& why) { return bad(why); }
  bool declares(uint32_t kind, ar_key key) const;
  struct InstanceStage {
    uint64_t next = 0;  // reserved item key counter
    bool has_set = false;
    uint32_t count = 0, steps = 1;
  };
  InstanceStage& instance_stage() { return instances_; }

 private:
  friend class Scene;
  explicit Edit(Scene& scene);
  ar_status bad(const std::string& why);
  ar_status touch();  // common precondition: not finished; counts as a call

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
