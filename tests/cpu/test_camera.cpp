// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Perspective camera: generate_ray, We, PDF_We, sample_wi (0004 D23 perspective entry).
// Ref: pbrt-v4 PerspectiveCamera::We, PDF_We, SampleWi; Veach 1997 ch. 10. The expected
// directions come from an independent tan(fov / 2) construction, not from the code under test.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "camera/camera.h"
#include "host/scene/camera.h"

namespace {

using ar::CameraSample;
using ar::CameraView;
using ar::Real;
using ar::Real2;
using ar::Real3;

constexpr bool kF32 = sizeof(Real) == 4;
constexpr double kW = 64, kH = 48;
constexpr double kTx = 0.5, kTy = 0.375;  // plane half extents (aspect 4:3)

// A camera rotated about the world axes so no axis is special: columns right, up, back.
struct Rig {
  double r[3][3];
  double pos[3] = {1.5, -2.0, 0.75};
  double sx, sy;
  Rig(double shift_x, double shift_y) : sx(shift_x), sy(shift_y) {
    const double a = 0.6, b = -0.4;  // yaw about z, pitch about x
    const double cz = std::cos(a), sz = std::sin(a), cx = std::cos(b), sxx = std::sin(b);
    const double rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    const double rx[3][3] = {{1, 0, 0}, {0, cx, -sxx}, {0, sxx, cx}};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) r[i][j] = rz[i][0] * rx[0][j] + rz[i][1] * rx[1][j] + rz[i][2] * rx[2][j];
  }
  // Camera-space film point on z = -1 for pixel (px, py): NDC then the half extents plus shift.
  void plane(double px, double py, double c[3]) const {
    c[0] = (2 * px / kW - 1) * kTx + sx * kTx * 2;
    c[1] = (2 * py / kH - 1) * kTy + sy * kTy * 2;
    c[2] = -1;
  }
  void to_world(const double c[3], double w[3]) const {
    for (int i = 0; i < 3; ++i) w[i] = r[i][0] * c[0] + r[i][1] * c[1] + r[i][2] * c[2];
  }
  ar_camera_desc desc() const {
    ar_camera_desc d{};
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) d.cam_to_world.m[4 * i + j] = float(r[i][j]);
      d.cam_to_world.m[4 * i + 3] = float(pos[i]);
    }
    // NDC (nx, ny, nz, 1) to camera space; row 3 makes w = 1 - 0.1 nz so the divide is exercised.
    const float p[16] = {float(kTx), 0, 0, float(sx * kTx * 2), 0, float(kTy), 0, float(sy * kTy * 2),
                         0, 0, 0, -1, 0, 0, -0.1f, 1};
    for (int i = 0; i < 16; ++i) d.projection_inverse[i] = p[i];
    d.type = AR_CAMERA_PERSP;
    return d;
  }
  // The same view assembled without the host: used for the f64 copy.
  CameraView view() const {
    CameraView v{};
    v.type = ar::CameraType::PERSPECTIVE;
    v.origin = {Real(pos[0]), Real(pos[1]), Real(pos[2])};
    for (int i = 0; i < 3; ++i) v.axis[i] = {Real(r[0][i]), Real(r[1][i]), Real(r[2][i])};
    v.film_origin = {Real(-kTx + sx * kTx * 2), Real(-kTy + sy * kTy * 2)};
    v.film_extent = {Real(2 * kTx), Real(2 * kTy)};
    v.film_area = Real(4 * kTx * kTy);
    v.width = Real(kW);
    v.height = Real(kH);
    return v;
  }
};

double len3(const double v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

CameraSample at(double x, double y) {
  CameraSample s;
  s.film = {Real(x), Real(y)};
  s.lens = {Real(0.5), Real(0.5)};
  s.time = Real(0);
  return s;
}

}  // namespace

TEST_SUITE("fast") {

AR_RT_CASE("camera/corner rays") {
  const Rig rig(0, 0);
  const CameraView v = rig.view();
  const double xs[4] = {0, kW, 0, kW}, ys[4] = {0, 0, kH, kH};
  for (int k = 0; k < 4; ++k) {
    double c[3], w[3];
    rig.plane(xs[k], ys[k], c);
    rig.to_world(c, w);
    const double n = len3(w);
    const ar::CameraRay ray = ar::camera_generate_ray(v, at(xs[k], ys[k]));
    CHECK(double(ray.o.x) == doctest::Approx(rig.pos[0]).epsilon(1e-6));
    CHECK(double(ray.d.x) == doctest::Approx(w[0] / n).epsilon(1e-6));
    CHECK(double(ray.d.y) == doctest::Approx(w[1] / n).epsilon(1e-6));
    CHECK(double(ray.d.z) == doctest::Approx(w[2] / n).epsilon(1e-6));
  }
}

AR_RT_CASE("camera/host view matches projection_inverse") {
  if (!kF32) return;  // the host builder fills the float build's type
  astroray::host::Report report;
  for (double shift : {0.0, 0.3}) {
    const Rig rig(shift, -shift * 0.5);
    const double origin[3] = {0.5, 0.5, 0.25};
    ar::CameraView hv{};
    REQUIRE(astroray::host::make_camera_view(rig.desc(), 64, 48, origin, 7, report, 1, &hv) == AR_OK);
    const double xs[4] = {0, kW, 0, kW}, ys[4] = {0, 0, kH, kH};
    for (int k = 0; k < 4; ++k) {
      double c[3], w[3];
      rig.plane(xs[k], ys[k], c);
      rig.to_world(c, w);
      const double n = len3(w);
      const ar::CameraRay ray = ar::camera_generate_ray(hv, at(xs[k], ys[k]));
      CHECK(double(ray.d.x) == doctest::Approx(w[0] / n).epsilon(1e-6));
      CHECK(double(ray.d.y) == doctest::Approx(w[1] / n).epsilon(1e-6));
      CHECK(double(ray.d.z) == doctest::Approx(w[2] / n).epsilon(1e-6));
      CHECK(double(ray.o.x) == doctest::Approx(rig.pos[0] - origin[0]).epsilon(1e-6));
    }
  }
  // film_area and all three origin components against an independent tan(fov) construction.
  {
    const double fov = 50.0 * 3.14159265358979323846 / 180.0;
    const double tx = std::tan(fov / 2), ty = tx * kH / kW;
    ar_camera_desc d = Rig(0, 0).desc();
    const float p[16] = {float(tx), 0, 0, 0, 0, float(ty), 0, 0, 0, 0, 0, -1, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) d.projection_inverse[i] = p[i];
    const double o[3] = {0.25, -0.5, 2.0};
    ar::CameraView fv{};
    REQUIRE(astroray::host::make_camera_view(d, 64, 48, o, 8, report, 1, &fv) == AR_OK);
    CHECK(double(fv.film_area) == doctest::Approx(4 * tx * ty).epsilon(1e-6));
    CHECK(double(fv.origin.x) == doctest::Approx(Rig(0, 0).pos[0] - o[0]).epsilon(1e-6));
    CHECK(double(fv.origin.y) == doctest::Approx(Rig(0, 0).pos[1] - o[1]).epsilon(1e-6));
    CHECK(double(fv.origin.z) == doctest::Approx(Rig(0, 0).pos[2] - o[2]).epsilon(1e-6));
  }
  // A scaled camera object renders like the unscaled one, with no report; a shear and a skewed
  // projection are reported once each and still build a view.
  {
    const Rig rig(0, 0);
    const double zero[3] = {0, 0, 0};
    ar_camera_desc ds = rig.desc();
    const float sc[3] = {2.0f, 3.0f, 0.5f};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) ds.cam_to_world.m[4 * i + j] *= sc[j];
    ar::CameraView a{}, b{};
    REQUIRE(astroray::host::make_camera_view(rig.desc(), 64, 48, zero, 13, report, 1, &a) == AR_OK);
    REQUIRE(astroray::host::make_camera_view(ds, 64, 48, zero, 13, report, 1, &b) == AR_OK);
    for (int k = 0; k < 3; ++k) {
      const ar::CameraRay ra = ar::camera_generate_ray(a, at(10 + 20 * k, 7 + 15 * k));
      const ar::CameraRay rb = ar::camera_generate_ray(b, at(10 + 20 * k, 7 + 15 * k));
      CHECK(double(ra.d.x) == doctest::Approx(double(rb.d.x)).epsilon(1e-6));
      CHECK(double(ra.d.y) == doctest::Approx(double(rb.d.y)).epsilon(1e-6));
      CHECK(double(ra.d.z) == doctest::Approx(double(rb.d.z)).epsilon(1e-6));
    }
    ar_camera_desc dh = rig.desc();
    for (int i = 0; i < 3; ++i) dh.cam_to_world.m[4 * i + 1] += 0.2f * dh.cam_to_world.m[4 * i];  // up += 0.2 right
    ar::CameraView h{};
    REQUIRE(astroray::host::make_camera_view(dh, 64, 48, zero, 12, report, 1, &h) == AR_OK);
    ar_camera_desc dk = rig.desc();
    dk.projection_inverse[1] = 0.05f;  // x row picks up ndc_y: the film plane is rotated
    ar::CameraView k{};
    REQUIRE(astroray::host::make_camera_view(dk, 64, 48, zero, 11, report, 1, &k) == AR_OK);
    report.snapshot();
    ar_report_info ri{};
    REQUIRE(report.info(&ri) == AR_OK);
    uint32_t shear = 0, skew = 0, other13 = 0;
    for (uint32_t i = 0; i < ri.entry_count; ++i) {
      ar_report_entry e{};
      REQUIRE(report.entry(i, &e) == AR_OK);
      const std::string det(reinterpret_cast<const char*>(e.detail));
      if (e.subject == 12 && det == "SHEAR") ++shear;
      if (e.subject == 11 && det == "PROJECTION_SKEW") ++skew;
      if (e.subject == 13) ++other13;
    }
    CHECK(shear == 1);
    CHECK(skew == 1);
    CHECK(other13 == 0);
    // After orthonormalising, the axes are unit and orthogonal.
    CHECK(double(dot(h.axis[0], h.axis[1])) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(double(length(h.axis[1])) == doctest::Approx(1.0).epsilon(1e-6));
  }
  // Unsupported features are reported and still give a pinhole view.
  ar_camera_desc d = Rig(0, 0).desc();
  d.type = AR_CAMERA_PANO;
  d.dof_enabled = 1;
  const double origin[3] = {0, 0, 0};
  ar::CameraView hv{};
  CHECK(astroray::host::make_camera_view(d, 64, 48, origin, 9, report, 2, &hv) == AR_OK);
  report.snapshot();
  ar_report_info info{};
  REQUIRE(report.info(&info) == AR_OK);
  uint32_t n = 0;
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    REQUIRE(report.entry(i, &e) == AR_OK);
    if (e.code == AR_REPORT_CAMERA_UNSUPPORTED && e.subject == 9) ++n;
  }
  CHECK(n == 2);  // PANO and DOF
}

AR_RT_CASE("camera/sample_wi round trip") {
  for (double shift : {0.0, 0.3}) {
    const Rig rig(shift, -shift * 0.5);
    const CameraView v = rig.view();
    const double px[4] = {5.25, 40.5, 63.0, 20.75}, py[4] = {44.0, 3.5, 24.0, 17.25};
    const double dist[4] = {2.0, 7.5, 0.3, 40.0};
    for (int k = 0; k < 4; ++k) {
      // The point seen through pixel (px, py) at a distance: independent of sample_wi.
      double c[3], w[3];
      rig.plane(px[k], py[k], c);
      rig.to_world(c, w);
      const double n = len3(w);
      const Real3 ref{Real(rig.pos[0] + dist[k] * w[0] / n), Real(rig.pos[1] + dist[k] * w[1] / n),
                      Real(rig.pos[2] + dist[k] * w[2] / n)};
      const ar::CameraWiSample s = ar::camera_sample_wi(v, ref, Real2{Real(0.5), Real(0.5)});
      REQUIRE(double(s.We) > 0);
      // Pixel back within 1e-5 NDC (NDC spans 2 per film).
      CHECK(2 * std::fabs(double(s.film.x) - px[k]) / kW < 1e-5);
      CHECK(2 * std::fabs(double(s.film.y) - py[k]) / kH < 1e-5);
      CHECK(double(s.dist) == doctest::Approx(dist[k]).epsilon(1e-5));
      // generate_ray at the returned pixel points at the reference point.
      const ar::CameraRay ray = ar::camera_generate_ray(v, at(double(s.film.x), double(s.film.y)));
      CHECK(double(ray.d.x) == doctest::Approx(-double(s.wi.x)).epsilon(1e-5));
      CHECK(double(ray.d.y) == doctest::Approx(-double(s.wi.y)).epsilon(1e-5));
      CHECK(double(ray.d.z) == doctest::Approx(-double(s.wi.z)).epsilon(1e-5));
      // pdf = position pdf x d^2 / cos at the lens, with We and PDF_We from the direction alone.
      Real2 film;
      const Real3 out = -s.wi;
      const double we = double(ar::camera_we(v, out, &film));
      const ar::CameraPdfWe pw = ar::camera_pdf_we(v, out);
      CHECK(we == doctest::Approx(double(s.We)).epsilon(1e-6));
      const double cos_t = double(pw.dir) > 0 ? 1.0 / std::cbrt(double(pw.dir) * double(v.film_area)) : 0.0;
      CHECK(double(pw.pos) == 1.0);
      CHECK(double(s.pdf) == doctest::Approx(double(pw.pos) * dist[k] * dist[k] / cos_t).epsilon(1e-5));
    }
  }
  // A point beyond a film edge and a point behind the camera (reference not at the camera):
  // no sample and We 0, never a NaN.
  const Rig rig(0, 0);
  const CameraView v = rig.view();
  const double out_px[3] = {-3.0, kW + 3.0, 10.0}, out_py[3] = {10.0, 20.0, kH + 3.0};
  for (int k = 0; k < 3; ++k) {
    double c[3], w[3];
    rig.plane(out_px[k], out_py[k], c);
    rig.to_world(c, w);
    const double n = len3(w);
    const Real3 ref{Real(rig.pos[0] + 4 * w[0] / n), Real(rig.pos[1] + 4 * w[1] / n), Real(rig.pos[2] + 4 * w[2] / n)};
    const ar::CameraWiSample s = ar::camera_sample_wi(v, ref, Real2{Real(0.5), Real(0.5)});
    CHECK(double(s.We) == 0.0);
    CHECK(double(s.pdf) == 0.0);
    CHECK(double(ar::camera_we(v, Real3{Real(w[0] / n), Real(w[1] / n), Real(w[2] / n)}, nullptr)) == 0.0);
    CHECK(double(ar::camera_pdf_we(v, Real3{Real(w[0] / n), Real(w[1] / n), Real(w[2] / n)}).dir) == 0.0);
  }
  const double cb[3] = {0, 0, 1};  // view axis direction in camera space is -z; behind is +z
  double wb[3];
  rig.to_world(cb, wb);
  const double nb = len3(wb);
  const Real3 behind{Real(rig.pos[0] + 3 * wb[0] / nb), Real(rig.pos[1] + 3 * wb[1] / nb), Real(rig.pos[2] + 3 * wb[2] / nb)};
  const ar::CameraWiSample sb = ar::camera_sample_wi(v, behind, Real2{Real(0.5), Real(0.5)});
  CHECK(double(sb.We) == 0.0);
  CHECK(double(sb.pdf) == 0.0);
  CHECK(double(ar::camera_we(v, Real3{Real(wb[0] / nb), Real(wb[1] / nb), Real(wb[2] / nb)}, nullptr)) == 0.0);
}

AR_RT_CASE("camera/We normalised") {
  // Midpoint rule over the film plane: dw = cos^3 dA on z = -1, so integral We cos dw = integral
  // dA / A = 1 for the true We = 1 / (A cos^4).
  for (double shift : {0.0, 0.3}) {
    const Rig rig(shift, 0.2);
    const CameraView v = rig.view();
    double fwd[3] = {-rig.r[0][2], -rig.r[1][2], -rig.r[2][2]};
    const int n = 64;
    double sum = 0;
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) {
        const double x = (i + 0.5) / n * kW, y = (j + 0.5) / n * kH;
        const ar::CameraRay ray = ar::camera_generate_ray(v, at(x, y));
        Real2 film;
        const double we = double(ar::camera_we(v, ray.d, &film));
        const double cos_t = double(ray.d.x) * fwd[0] + double(ray.d.y) * fwd[1] + double(ray.d.z) * fwd[2];
        const double da = (2 * kTx / n) * (2 * kTy / n);
        sum += we * cos_t * cos_t * cos_t * cos_t * da;
      }
    CHECK(sum == doctest::Approx(1.0).epsilon(1e-4));
  }
  // Zero outside the film and behind the camera.
  const CameraView v = Rig(0, 0).view();
  const ar::CameraRay mid = ar::camera_generate_ray(v, at(kW / 2, kH / 2));
  CHECK(double(ar::camera_we(v, -mid.d, nullptr)) == 0.0);
}

}  // TEST_SUITE
