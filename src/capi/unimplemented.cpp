// SPDX-License-Identifier: Apache-2.0
// Declared functions that have no implementation yet: each refuses with AR_ERR_UNSUPPORTED
// and, when it has a live handle, leaves "<function>: not implemented in this build" as that
// handle's last error (0001 D2.5, D2.8; fail loudly, never a silent no-op). The task that
// implements a function deletes its entry here.
// The scene and edit handle structs live in host/scene/edit.h (032); the session struct
// below stays here until 052 creates the first live session handle and moves it.
#include <string>

#include "astroray/astroray.h"
#include "host/scene/edit.h"

struct ar_session_s {
  std::string last_error;
};

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

// Session: one render of one scene.
AR_API ar_status ar_session_create(ar_scene h, const ar_session_desc*, ar_session*) { return unsupported(h, "ar_session_create"); }
AR_API ar_status ar_session_close(ar_session h) { return unsupported(h, "ar_session_close"); }
AR_API ar_status ar_session_destroy(ar_session h) { return unsupported(h, "ar_session_destroy"); }
AR_API ar_status ar_session_start(ar_session h) { return unsupported(h, "ar_session_start"); }
AR_API ar_status ar_session_pause(ar_session h) { return unsupported(h, "ar_session_pause"); }
AR_API ar_status ar_session_resume(ar_session h) { return unsupported(h, "ar_session_resume"); }
AR_API ar_status ar_session_cancel(ar_session h) { return unsupported(h, "ar_session_cancel"); }
AR_API ar_status ar_session_set_view(ar_session h, const ar_view_desc*) { return unsupported(h, "ar_session_set_view"); }
AR_API ar_status ar_session_set_display_mode(ar_session h, uint32_t) { return unsupported(h, "ar_session_set_display_mode"); }
AR_API ar_status ar_session_get_progress(ar_session h, ar_progress*) { return unsupported(h, "ar_session_get_progress"); }
AR_API ar_status ar_session_wait(ar_session h, uint32_t) { return unsupported(h, "ar_session_wait"); }
AR_API ar_status ar_session_last_error(ar_session, uint32_t, uint8_t*) { return AR_ERR_UNSUPPORTED; }
AR_API ar_status ar_session_report_info(ar_session h, ar_report_info*) { return unsupported(h, "ar_session_report_info"); }
AR_API ar_status ar_session_report_entry(ar_session h, uint32_t, ar_report_entry*) { return unsupported(h, "ar_session_report_entry"); }

// Scene edit transaction: one open edit per scene.
AR_API ar_status ar_edit_mesh_begin(ar_edit h, const ar_mesh_desc*, ar_mesh_buffers*) { return unsupported(h, "ar_edit_mesh_begin"); }
AR_API ar_status ar_edit_positions_begin(ar_edit h, ar_key, ar_positions_buffers*) { return unsupported(h, "ar_edit_positions_begin"); }
AR_API ar_status ar_edit_curves_begin(ar_edit h, const ar_curves_desc*, ar_curves_buffers*) { return unsupported(h, "ar_edit_curves_begin"); }
AR_API ar_status ar_edit_points_begin(ar_edit h, const ar_points_desc*, ar_points_buffers*) { return unsupported(h, "ar_edit_points_begin"); }
AR_API ar_status ar_edit_grid_leaves_begin(ar_edit h, const ar_grid_desc*, ar_grid_leaf_buffers*) { return unsupported(h, "ar_edit_grid_leaves_begin"); }
AR_API ar_status ar_edit_image_begin(ar_edit h, const ar_image_desc*, ar_image_buffers*) { return unsupported(h, "ar_edit_image_begin"); }
AR_API ar_status ar_edit_set_image(ar_edit h, const ar_image_desc*) { return unsupported(h, "ar_edit_set_image"); }
AR_API ar_status ar_edit_set_instances(ar_edit h, const ar_instances_desc*) { return unsupported(h, "ar_edit_set_instances"); }
AR_API ar_status ar_edit_update_transforms(ar_edit h, uint32_t, const uint32_t*, const ar_xform*) { return unsupported(h, "ar_edit_update_transforms"); }
AR_API ar_status ar_edit_set_graph(ar_edit h, ar_key, uint32_t, const ar_graph_desc*) { return unsupported(h, "ar_edit_set_graph"); }
AR_API ar_status ar_edit_set_material(ar_edit h, ar_key, const ar_material_desc*) { return unsupported(h, "ar_edit_set_material"); }
AR_API ar_status ar_edit_set_light(ar_edit h, ar_key, const ar_light_desc*) { return unsupported(h, "ar_edit_set_light"); }
AR_API ar_status ar_edit_set_world(ar_edit h, const ar_world_desc*) { return unsupported(h, "ar_edit_set_world"); }

// Display and readback: all AR_QUERY, never recorded (0008 decision 13a).
AR_API ar_status ar_display_acquire(ar_session h, uint64_t, ar_display_frame*) { return unsupported(h, "ar_display_acquire"); }
AR_API ar_status ar_display_release(ar_session h, uint64_t) { return unsupported(h, "ar_display_release"); }
AR_API ar_status ar_session_pass_count(ar_session h, uint32_t*) { return unsupported(h, "ar_session_pass_count"); }
AR_API ar_status ar_session_pass_info(ar_session h, uint32_t, ar_pass_info*) { return unsupported(h, "ar_session_pass_info"); }
AR_API ar_status ar_session_read_pass(ar_session h, uint32_t, uint32_t, uint32_t, float*) { return unsupported(h, "ar_session_read_pass"); }
AR_API ar_status ar_session_read_counters(ar_session h, uint32_t, uint64_t*) { return unsupported(h, "ar_session_read_counters"); }
AR_API ar_status ar_session_write_fits(ar_session h, const char*, uint32_t) { return unsupported(h, "ar_session_write_fits"); }

}  // extern "C"
