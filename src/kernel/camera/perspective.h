// SPDX-License-Identifier: Apache-2.0
// Pinhole perspective camera (lens area 1, a delta position). Include camera.h, not this file.
//
// Film area A is that of the whole film on the plane z = -1 in camera space; for a ray leaving
// at angle theta from the view axis, We = 1 / (A cos^4 theta) and the direction pdf is
// 1 / (A cos^3 theta), so that the integral of We cos theta dw over the film's solid angle is 1
// (pbrt's normalisation). Ref: pbrt-v3 PerspectiveCamera::We, Pdf_We, Sample_Wi; Veach 1997 ch. 10.

#pragma once
#ifndef AR_CAMERA_TYPES_DEFINED
#error "include camera/camera.h, not perspective.h"
#endif

AR_NS_BEGIN

AR_FN CameraRay persp_generate_ray(const CameraView& c, const CameraSample& s) {
  const Real u = s.film.x / c.width, v = s.film.y / c.height;
  const Real3 pc{c.film_origin.x + u * c.film_extent.x, c.film_origin.y + v * c.film_extent.y, Real(-1)};
  CameraRay r;
  r.o = c.origin;
  r.d = normalize(camera_to_world_dir(c, pc));
  return r;
}

// cos theta against the view axis (-z in camera space) of world direction w, and the pixel it
// crosses; 0 for directions behind the camera or outside the film.
AR_FN Real persp_cos_film(const CameraView& c, Real3 w, Real2* film) {
  const Real3 v = camera_from_world_dir(c, w);
  const Real cos_t = -v.z / length(v);
  if (!(cos_t > Real(0))) return Real(0);
  const Real qx = v.x / -v.z, qy = v.y / -v.z;
  const Real nx = (qx - c.film_origin.x) / c.film_extent.x;
  const Real ny = (qy - c.film_origin.y) / c.film_extent.y;
  if (!(nx >= Real(0) && nx <= Real(1) && ny >= Real(0) && ny <= Real(1))) return Real(0);
  if (film) *film = Real2{nx * c.width, ny * c.height};
  return cos_t;
}

AR_FN Real persp_we(const CameraView& c, Real3 w, Real2* film) {
  const Real cos_t = persp_cos_film(c, w, film);
  if (!(cos_t > Real(0))) return Real(0);
  const Real c2 = cos_t * cos_t;
  return Real(1) / (c.film_area * c2 * c2);
}

AR_FN CameraPdfWe persp_pdf_we(const CameraView& c, Real3 w) {
  const Real cos_t = persp_cos_film(c, w, nullptr);
  CameraPdfWe p{Real(0), Real(0)};
  if (!(cos_t > Real(0))) return p;
  p.pos = Real(1);
  p.dir = Real(1) / (c.film_area * cos_t * cos_t * cos_t);
  return p;
}

AR_FN CameraWiSample persp_sample_wi(const CameraView& c, Real3 ref, Real2 /*u_lens*/) {
  CameraWiSample s;
  s.p_lens = c.origin;
  s.film = Real2{Real(0), Real(0)};
  s.We = Real(0);
  s.pdf = Real(0);
  s.wi = Real3{Real(0), Real(0), Real(0)};
  s.dist = Real(0);
  const Real3 d = c.origin - ref;
  const Real d2 = dot(d, d);
  if (!(d2 > Real(0))) return s;
  s.dist = ar::sqrt(d2);
  s.wi = d / s.dist;
  // The ray from the lens toward the reference point leaves along -wi.
  const Real cos_t = persp_cos_film(c, -s.wi, &s.film);
  if (!(cos_t > Real(0))) return s;
  const Real c2 = cos_t * cos_t;
  s.We = Real(1) / (c.film_area * c2 * c2);
  // Area-to-solid-angle at the reference point; the lens normal is the view axis.
  s.pdf = d2 / cos_t;
  return s;
}

AR_NS_END
