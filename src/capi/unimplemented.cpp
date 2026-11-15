// SPDX-License-Identifier: Apache-2.0
// Declared functions that have no implementation yet: each refuses with AR_ERR_UNSUPPORTED
// and, when it has a live handle, leaves "<function>: not implemented in this build" as that
// handle's last error (0001 D2.5, D2.8; fail loudly, never a silent no-op). The task that
// implements a function deletes its entry here.
// The scene and edit handle structs live in host/scene/edit.h, the session one in
// host/session/session.h.
#include <string>

#include "astroray/astroray.h"
#include "host/scene/edit.h"
#include "host/session/session.h"

namespace {

template <class Handle>
ar_status unsupported(Handle* h, const char* function) {
  if (h) {
    try {
      h->last_error = std::string(function) + ": not implemented in this build";
    } catch (...) {
      // Out of memory while storing the text: the status is still the refusal.
    }
  }
  return AR_ERR_UNSUPPORTED;
}

}  // namespace

extern "C" {


// Scene: host scene, its device copy, worker pool and generations (0007 decision 5).
AR_API ar_status ar_scene_trim(ar_scene h) { return unsupported(h, "ar_scene_trim"); }

// Scene edit transaction: one open edit per scene.
AR_API ar_status ar_edit_curves_begin(ar_edit h, const ar_curves_desc*, ar_curves_buffers*) { return unsupported(h, "ar_edit_curves_begin"); }
AR_API ar_status ar_edit_points_begin(ar_edit h, const ar_points_desc*, ar_points_buffers*) { return unsupported(h, "ar_edit_points_begin"); }
AR_API ar_status ar_edit_grid_leaves_begin(ar_edit h, const ar_grid_desc*, ar_grid_leaf_buffers*) { return unsupported(h, "ar_edit_grid_leaves_begin"); }
AR_API ar_status ar_edit_image_begin(ar_edit h, const ar_image_desc*, ar_image_buffers*) { return unsupported(h, "ar_edit_image_begin"); }
AR_API ar_status ar_edit_set_light(ar_edit h, ar_key, const ar_light_desc*) { return unsupported(h, "ar_edit_set_light"); }

}  // extern "C"
