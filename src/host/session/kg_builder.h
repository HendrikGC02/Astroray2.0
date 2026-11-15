// SPDX-License-Identifier: Apache-2.0
// The one KernelGlobals builder for both devices. At each latch the session pins the scene table
// set of its snapshot (host/scene/scene_tables.h) and, per launch group, builds the host
// KernelGlobals from that set, the session's own device tables, film, display target, active list,
// camera and filter, and SessionUniforms. The scheduler adds its pools, queues and step table and
// copies the struct to the device once per pass (host/scheduler.h); on the CPU device the same
// struct holds host pointers.
//
// Every pointer field is set, or optional for a reason stated in missing_pointers (SceneData::accel
// is always set: each snapshot, an empty one included, has its traversal structure, task 114):
//  - camera and filter: null until a view and the settings are latched; a pass then has no
//    camera work (session.cpp pass_camera_samples).
//  - scene arrays of an empty table (count 0), and SceneTables::rgb2spec of a scene without a
//    material store, which has nothing for the VM to run.
// Ref: docs/decisions/0002-kernel-architecture.md D4 (one KernelGlobals pointer), D13; 0007 d25
// (latch); RULINGS 70.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "host/device.h"
#include "host/film/response_table.h"
#include "host/scene/scene_tables.h"
#include "host/spectral/wavelength_table.h"
#include "kernel/film/film_write.h"
#include "kernel/kernel_globals.h"

namespace astroray::host {

// The session-owned tables the film reads (response channels 046, wavelength table 024), on the
// session's device. Camera and filter are uploaded by the session as views change.
struct SessionTables {
  std::shared_ptr<const DeviceTable> knot_nm, knot_value, channels, edges, cdf;
};

// Copies the tables to `device` and points `film`'s response and wavelength views at the copies.
SessionTables upload_session_tables(Device& device, Owner owner, const ResponseTable& response,
                                    const WavelengthTable& wavelengths, ar::FilmData& film);

// What the session supplies besides the scene set; addresses are the session device's.
struct KgSession {
  ar::FilmData* film = nullptr;
  ar::DisplayTarget* display = nullptr;
  uint32_t* active_pixels = nullptr;
  const ar::CameraView* camera = nullptr;  // null until a view is latched
  const ar::FilterTable* filter = nullptr;  // null until the settings are latched
  ar::SessionUniforms uniforms{};
};

// `scene` is null only before the session latched its first set: the scene fields stay null and
// the session launches no camera work (only the film kernels run).
ar::KernelGlobals build_kernel_globals(const SceneTableSet* scene, const KgSession& session);

// The pointer fields of `kg` (and of the set's SceneData) that are null without being optional
// above. `scheduler_filled` false skips the fields the scheduler fills per pass. Empty when
// every pointer is set.
std::vector<std::string> missing_pointers(const ar::KernelGlobals& kg, const SceneTableSet& scene,
                                          bool scheduler_filled);

}  // namespace astroray::host
