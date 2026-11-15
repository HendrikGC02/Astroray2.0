// SPDX-License-Identifier: Apache-2.0
// Camera descriptor to kernel CameraView (0004 D23 perspective entry, 0003 d26 scene origin).
// Ref: 0004 D23; 0007 appendix (ar_camera_desc); 0001 D2.11 (CAMERA_UNSUPPORTED).
#pragma once
#include <cstdint>

#include "../../../include/astroray/astroray.h"
#include "host/report.h"
#include "kernel/camera/camera.h"

namespace astroray::host {

// Builds the view for a film of width x height pixels. `origin` is the scene origin in the
// descriptor's world units; it is subtracted from the camera position. An ORTHO or PANO type,
// enabled depth of field or a skewed projection raises CAMERA_UNSUPPORTED (subject = camera_key)
// and the result is still the pinhole perspective. Returns AR_ERR_INVALID_ARGUMENT, with `out`
// untouched, for a non-finite or degenerate descriptor or a zero-sized film.
ar_status make_camera_view(const ar_camera_desc& desc, uint32_t width, uint32_t height, const double origin[3],
                           uint64_t camera_key, Report& report, uint64_t generation, ar::CameraView* out);

}  // namespace astroray::host
