// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0007-blender-integration.md d25, d12, d49; 0004 D25; RULINGS 69.
#include "host/session/latch.h"

#include <cstring>

namespace astroray::host {

StoredView StoredView::copy_of(const ar_view_desc& view) {
  StoredView s;
  std::memcpy(&s.v.camera, &view.camera, sizeof(ar_camera_desc));
  s.v.camera.motion_xf = nullptr;
  s.v.region_width = view.region_width;
  s.v.region_height = view.region_height;
  s.v.render_scale = view.render_scale;
  s.v.border_enabled = view.border_enabled;
  s.v.border_min_x = view.border_min_x;
  s.v.border_min_y = view.border_min_y;
  s.v.border_max_x = view.border_max_x;
  s.v.border_max_y = view.border_max_y;
  if (view.camera.motion_xf && view.camera.motion_steps)
    s.motion.assign(view.camera.motion_xf, view.camera.motion_xf + view.camera.motion_steps);
  return s;
}

bool StoredView::same_as(const StoredView& o) const {
  // ar_camera_desc has no padding (all 4-byte fields, then the pointer at an 8-byte offset), so
  // a byte compare is the field compare; ar_view_desc's tail padding is skipped by comparing its
  // own fields one by one.
  if (std::memcmp(&v.camera, &o.v.camera, sizeof(ar_camera_desc)) != 0) return false;
  if (v.region_width != o.v.region_width || v.region_height != o.v.region_height) return false;
  if (std::memcmp(&v.render_scale, &o.v.render_scale, sizeof(float)) != 0) return false;
  if (v.border_enabled != o.v.border_enabled) return false;
  const float a[4] = {v.border_min_x, v.border_min_y, v.border_max_x, v.border_max_y};
  const float b[4] = {o.v.border_min_x, o.v.border_min_y, o.v.border_max_x, o.v.border_max_y};
  if (std::memcmp(a, b, sizeof a) != 0) return false;
  if (motion.size() != o.motion.size()) return false;
  return motion.empty() || std::memcmp(motion.data(), o.motion.data(), motion.size() * sizeof(ar_xform)) == 0;
}

bool Latch::set_view(const ar_view_desc& view) {
  StoredView s = StoredView::copy_of(view);
  std::lock_guard<std::mutex> lock(mutex_);
  pending_view_ = std::move(s);
  has_pending_view_ = true;
  return !has_view_ || !pending_view_.same_as(view_);
}

bool Latch::set_display_mode(uint32_t mode) {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_mode_ = mode;
  return pending_mode_ != mode_;
}

bool Latch::epoch_pending_locked(uint64_t scene_active) const {
  if (first_) return true;
  // Every activated commit starts an epoch until commits carry a display-only flag (0007 d12;
  // docs/STATUS.md records the missing flag).
  if (scene_active != generation_) return true;
  return has_pending_view_ && (!has_view_ || !pending_view_.same_as(view_));
}

LatchResult Latch::latch(uint64_t scene_active, bool force_epoch) {
  std::lock_guard<std::mutex> lock(mutex_);
  LatchResult r;
  r.new_epoch = force_epoch || epoch_pending_locked(scene_active);
  r.changed = r.new_epoch || pending_mode_ != mode_;
  if (has_pending_view_) {
    view_ = std::move(pending_view_);
    has_view_ = true;
    has_pending_view_ = false;
    pending_view_ = StoredView();
  }
  mode_ = pending_mode_;
  // RULINGS 69: only ar_edit_commit allocates generation numbers, so a latch of views alone
  // keeps the active scene generation.
  generation_ = scene_active;
  first_ = false;
  if (r.new_epoch) {
    ++epoch_;
    presented_ = false;
  }
  if (r.changed) ++latches_;
  r.generation = generation_;
  r.epoch = epoch_;
  return r;
}

void Latch::mark_presented() {
  std::lock_guard<std::mutex> lock(mutex_);
  presented_ = true;
}

bool Latch::interrupt_due(uint64_t scene_active) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return presented_ && epoch_pending_locked(scene_active);
}

bool Latch::pending(uint64_t scene_active) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return epoch_pending_locked(scene_active) || pending_mode_ != mode_;
}

uint64_t Latch::generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}
uint64_t Latch::epoch() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return epoch_;
}
uint64_t Latch::latches() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latches_;
}
bool Latch::has_view() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_view_;
}
StoredView Latch::view() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return view_;
}
uint32_t Latch::display_mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mode_;
}

}  // namespace astroray::host
