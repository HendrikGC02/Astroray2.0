// SPDX-License-Identifier: Apache-2.0
// The generation latch of one session: `set_view` and `set_display_mode` only record pending
// state; at a pass boundary the render thread folds every pending view and every scene commit
// that became active since the last latch into one latched state. A latch whose change alters
// the image starts a new epoch. Thread safe: the API thread records, the render thread latches,
// a scene worker may ask whether an interrupt is due.
// Ref: docs/decisions/0007-blender-integration.md d25 (generations coalesced at pass boundaries,
// interrupt rule), d12 (which edits start an epoch); 0004 D25 (epoch resets); RULINGS 69
// (a latch with no newly active commit keeps the active scene generation number).
#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

#include "../../../include/astroray/astroray.h"

namespace astroray::host {

// ar_view_desc with its copy-in motion array owned. Equality is the view hash of 0007 d49:
// every field, the motion transforms included.
struct StoredView {
  ar_view_desc v{};  // v.camera.motion_xf is null; the transforms are in `motion`
  std::vector<ar_xform> motion;
  static StoredView copy_of(const ar_view_desc& view);
  bool same_as(const StoredView& o) const;
};

struct LatchResult {
  bool changed = false;    // anything was folded in
  bool new_epoch = false;  // the image changed: film, statistics and sample indices reset
  uint64_t generation = 0;
  uint64_t epoch = 0;
};

class Latch {
 public:
  // API thread: record pending state. Returns true when the pending state now differs from
  // what is latched, i.e. the next latch will change something.
  bool set_view(const ar_view_desc& view);
  bool set_display_mode(uint32_t mode);

  // Render thread, at a pass boundary. `scene_active` is the scene's active generation.
  LatchResult latch(uint64_t scene_active);
  // The latched generation has presented a frame (the interrupt rule's condition).
  void mark_presented();
  // True when pending state would start an epoch and the latched generation has already
  // presented, so the pass in flight must stop at its next launch (0007 d25).
  bool interrupt_due(uint64_t scene_active) const;
  // True when the next latch would change anything.
  bool pending(uint64_t scene_active) const;

  uint64_t generation() const;
  uint64_t epoch() const;
  uint64_t latches() const;  // latches that changed something
  bool has_view() const;
  StoredView view() const;
  uint32_t display_mode() const;

 private:
  bool epoch_pending_locked(uint64_t scene_active) const;
  mutable std::mutex mutex_;
  bool first_ = true;  // nothing latched yet: the first latch always starts epoch 1
  bool has_pending_view_ = false;
  StoredView pending_view_;
  bool has_view_ = false;
  StoredView view_;
  uint32_t pending_mode_ = AR_DISPLAY_BEAUTY;
  uint32_t mode_ = AR_DISPLAY_BEAUTY;
  uint64_t generation_ = 0;  // the latched scene generation (RULINGS 69)
  uint64_t epoch_ = 0;
  uint64_t latches_ = 0;
  bool presented_ = false;
};

}  // namespace astroray::host
