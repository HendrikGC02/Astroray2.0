// SPDX-License-Identifier: Apache-2.0
// The camera interface every camera type shares: generate_ray, importance We, PDF_We and
// sample_wi (0004 D7, D23). Types are a closed enum in a switch; only PERSPECTIVE exists so far.
// Include this file, not perspective.h.
//
// Film coordinates: pixels, x to the right, y UP from the bottom row (Blender's buffer order),
// origin at the film corner, so NDC = 2 (x / W, y / H) - 1.
//
// Ref: docs/decisions/0004-integrator.md D23 (perspective entry); pbrt-v4 src/pbrt/cameras.cpp
// PerspectiveCamera::We, PDF_We, SampleWi; second sources pbrt-v3 section 16.1.1 and Veach 1997
// ch. 10 (We).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/vec.h"
#include "../portability.h"

#define AR_CAMERA_TYPES_DEFINED 1

AR_NS_BEGIN

enum class CameraType : uint8_t { PERSPECTIVE = 0 };

// Filled on the host from ar_camera_desc and the view (src/host/scene/camera.cpp), scene origin
// already folded into `origin` (0003 d26). The film plane is z = -1 in camera space (the camera
// looks down -z); camera-space film points are film_origin + (u ax, v ay) with (u, v) in [0, 1]
// across the film, which holds for any shift because projection_inverse maps NDC affinely onto
// that plane.
struct CameraView {
  CameraType type;
  Real3 origin;
  Real3 axis[3];       // camera-to-world columns: right, up, back (-forward)
  Real2 film_origin;   // plane point of the film's (0, 0) corner
  Real2 film_extent;   // plane size (ax, ay) of the whole film
  Real film_area;      // ax * ay: A of We = 1 / (A cos^4)
  Real width, height;  // film size in pixels
};

struct CameraSample {
  Real2 film;  // pixels
  Real2 lens;  // unused by the pinhole
  Real time;   // unused until motion blur
};

struct CameraRay {
  Real3 o, d;  // d unit
};

struct CameraWiSample {
  Real3 wi;    // unit, from the reference point toward the camera
  Real dist;
  Real2 film;  // pixels
  Real We;     // importance along -wi; 0 means no sample
  Real pdf;    // solid angle at the reference point (area pdf x d^2 / cos at the lens)
  Real3 p_lens;
};

// pbrt's Pdf_We: the position pdf (a delta lens point counts as 1) and the direction pdf.
struct CameraPdfWe {
  Real pos, dir;
};

AR_FN Real3 camera_to_world_dir(const CameraView& c, Real3 v) {
  return c.axis[0] * v.x + c.axis[1] * v.y + c.axis[2] * v.z;
}
AR_FN Real3 camera_from_world_dir(const CameraView& c, Real3 w) {
  return Real3{dot(c.axis[0], w), dot(c.axis[1], w), dot(c.axis[2], w)};
}

AR_NS_END

// perspective.h needs the types above and opens its own namespace block.
#include "perspective.h"

AR_NS_BEGIN

AR_FN CameraRay camera_generate_ray(const CameraView& c, const CameraSample& s) {
  return persp_generate_ray(c, s);
}
// Importance of a ray leaving the camera along world direction w; film receives the pixel it
// crosses. 0 outside the frustum or behind the camera.
AR_FN Real camera_we(const CameraView& c, Real3 w, Real2* film) { return persp_we(c, w, film); }
AR_FN CameraPdfWe camera_pdf_we(const CameraView& c, Real3 w) { return persp_pdf_we(c, w); }
AR_FN CameraWiSample camera_sample_wi(const CameraView& c, Real3 ref, Real2 u_lens) {
  return persp_sample_wi(c, ref, u_lens);
}

AR_NS_END
