// SPDX-License-Identifier: Apache-2.0
// Ref: 0004 D23 perspective entry; 0003 d26 (origin folded into the camera at sync).
#include "host/scene/camera.h"

#include <cmath>

namespace astroray::host {
namespace {

// projection_inverse (4x4 row-major, NDC to camera space) applied to (nx, ny, 0, 1), then the
// plane point on z = -1 along the ray through the camera origin. Any NDC z gives the same ray
// for a perspective projection.
bool film_point(const float* m, double nx, double ny, double* qx, double* qy) {
  double v[4];
  for (int i = 0; i < 4; ++i) v[i] = double(m[4 * i]) * nx + double(m[4 * i + 1]) * ny + double(m[4 * i + 3]);
  if (!(std::fabs(v[3]) > 0)) return false;
  const double z = v[2] / v[3];
  if (!(z < 0)) return false;
  *qx = (v[0] / v[3]) / -z;
  *qy = (v[1] / v[3]) / -z;
  return std::isfinite(*qx) && std::isfinite(*qy);
}

void unsupported(Report& r, uint64_t key, const char* feature, uint64_t generation) {
  const double value[2] = {0.0, 0.0};
  r.raise(AR_REPORT_CAMERA_UNSUPPORTED, key, feature, 1, value, generation);
}

}  // namespace

ar_status make_camera_view(const ar_camera_desc& d, uint32_t width, uint32_t height, const double origin[3],
                           uint64_t camera_key, Report& report, uint64_t generation, ar::CameraView* out) {
  if (!out || !origin || width == 0 || height == 0) return AR_ERR_INVALID_ARGUMENT;
  for (float f : d.cam_to_world.m)
    if (!std::isfinite(f)) return AR_ERR_INVALID_ARGUMENT;
  double q00x, q00y, q10x, q10y, q01x, q01y;
  if (!film_point(d.projection_inverse, -1, -1, &q00x, &q00y) || !film_point(d.projection_inverse, 1, -1, &q10x, &q10y) ||
      !film_point(d.projection_inverse, -1, 1, &q01x, &q01y))
    return AR_ERR_INVALID_ARGUMENT;
  const double ax = q10x - q00x, ay = q01y - q00y;
  if (!(ax != 0 && ay != 0)) return AR_ERR_INVALID_ARGUMENT;

  if (d.type == AR_CAMERA_ORTHO) unsupported(report, camera_key, "ORTHO", generation);
  if (d.type == AR_CAMERA_PANO) unsupported(report, camera_key, "PANO", generation);
  if (d.dof_enabled) unsupported(report, camera_key, "DOF", generation);
  // The film must map affinely onto the plane with no rotation of its axes. 1e-6 relative is
  // the float rounding of Blender's projection matrix; a real skew is orders larger.
  if (std::fabs(q10y - q00y) > 1e-6 * std::fabs(ay) || std::fabs(q01x - q00x) > 1e-6 * std::fabs(ax))
    unsupported(report, camera_key, "PROJECTION_SKEW", generation);

  const float* m = d.cam_to_world.m;
  ar::CameraView v{};
  v.type = ar::CameraType::PERSPECTIVE;
  v.origin = {ar::Real(double(m[3]) - origin[0]), ar::Real(double(m[7]) - origin[1]), ar::Real(double(m[11]) - origin[2])};
  // Scale is cleared from the columns as Cycles' Blender camera sync does (blender/camera.cpp,
  // transform_clear_scale on the camera matrix); a scaled object must not change the view.
  // Columns still non-orthogonal after that are sheared: reported, then Gram-Schmidt'd.
  double ax3[3][3];
  for (int i = 0; i < 3; ++i) {
    const double x = m[i], y = m[4 + i], z = m[8 + i];
    const double n = std::sqrt(x * x + y * y + z * z);
    if (!(n > 0)) return AR_ERR_INVALID_ARGUMENT;
    ax3[i][0] = x / n, ax3[i][1] = y / n, ax3[i][2] = z / n;
  }
  auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  // 1e-4 is well above float rounding of a rotation matrix and below any visible shear.
  if (std::fabs(dot3(ax3[0], ax3[1])) > 1e-4 || std::fabs(dot3(ax3[0], ax3[2])) > 1e-4 ||
      std::fabs(dot3(ax3[1], ax3[2])) > 1e-4) {
    unsupported(report, camera_key, "SHEAR", generation);
    for (int i = 1; i < 3; ++i) {
      for (int j = 0; j < i; ++j) {
        const double p = dot3(ax3[i], ax3[j]);
        for (int k = 0; k < 3; ++k) ax3[i][k] -= p * ax3[j][k];
      }
      const double n = std::sqrt(dot3(ax3[i], ax3[i]));
      if (!(n > 1e-9)) return AR_ERR_INVALID_ARGUMENT;
      for (int k = 0; k < 3; ++k) ax3[i][k] /= n;
    }
  }
  for (int i = 0; i < 3; ++i) v.axis[i] = {ar::Real(ax3[i][0]), ar::Real(ax3[i][1]), ar::Real(ax3[i][2])};
  v.film_origin = {ar::Real(q00x), ar::Real(q00y)};
  v.film_extent = {ar::Real(ax), ar::Real(ay)};
  v.film_area = ar::Real(std::fabs(ax * ay));
  v.width = ar::Real(width);
  v.height = ar::Real(height);
  *out = v;
  return AR_OK;
}

}  // namespace astroray::host
