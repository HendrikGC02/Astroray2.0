// SPDX-License-Identifier: Apache-2.0
// specgen: generates the data files that depend on the CIE observer (0003 d14, d18).
// Not in the default build; run by hand: tools/build.ps1 -Target specgen, then
//   out/bin/specgen.exe observer [--out <file>]     (from the repository root)
//   out/bin/specgen.exe jh rec709 [--out <file>]
// `observer` reads data/spectral/cie1931_2deg.csv and writes data/spectral/observer.bin:
// Y_n, K = 683 Y_n, the D65 SPD scaled to Y = 1 and M_ws for lin_rec709_scene.
// `jh` fits the Jakob-Hanika coefficient table for that working space to the same observer and
// writes data/spectral/rec709.jh (layout: src/kernel/color/rgb2spec.h). The output is a pure
// function of the inputs: identical on every run and for every thread count.
// Ref: 0003 d11 (Y_n, K), d14 (one observer), d15 (working space), d16, d18 (jh); CIE 018:2019
// (K_m = 683 lm/W); ITU-R BT.709-6 Table 3 (primaries, D65 white); SMPTE RP 177-1993 (RGB-to-XYZ
// from primaries and white, no chromatic adaptation); Jakob and Hanika 2019, CGF 38(2) (jh).
// File layouts: src/host/data_resource.h, src/kernel/color/rgb2spec.h.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "host/data_resource.h"
#include "rgb2spec_opt_port.cpp"

namespace {

using astroray::kObserverLambdaMin;
using astroray::kObserverSamples;

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "specgen: %s\n", msg.c_str());
  std::exit(1);
}

struct Csv {
  double x[kObserverSamples], y[kObserverSamples], z[kObserverSamples], d65[kObserverSamples];
};

Csv read_csv(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) die("cannot open " + path);
  std::string line;
  std::getline(f, line);
  if (line.rfind("lambda_nm,xbar,ybar,zbar,d65", 0) != 0) die(path + ": unexpected header");
  Csv c;
  uint32_t n = 0;
  while (std::getline(f, line)) {
    if (line.empty() || line == "\r") continue;
    if (n >= kObserverSamples) die(path + ": more than 471 rows");
    double v[5];
    const char* s = line.c_str();
    for (int i = 0; i < 5; ++i) {
      char* e = nullptr;
      v[i] = std::strtod(s, &e);
      if (e == s) die(path + ": bad number in row " + std::to_string(n + 1));
      s = (*e == ',') ? e + 1 : e;
    }
    if (v[0] != double(kObserverLambdaMin + n)) die(path + ": wavelength grid is not 360 + i nm");
    c.x[n] = v[1];
    c.y[n] = v[2];
    c.z[n] = v[3];
    c.d65[n] = v[4];
    ++n;
  }
  if (n != kObserverSamples) die(path + ": " + std::to_string(n) + " rows, expected 471");
  return c;
}

using Mat3 = double[9];

void invert(const Mat3 m, Mat3 r) {
  double c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
  double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
  if (std::fabs(det) < 1e-12) die("singular primaries matrix");
  double id = 1.0 / det;
  r[0] = c0 * id;
  r[1] = (m[2] * m[7] - m[1] * m[8]) * id;
  r[2] = (m[1] * m[5] - m[2] * m[4]) * id;
  r[3] = c1 * id;
  r[4] = (m[0] * m[8] - m[2] * m[6]) * id;
  r[5] = (m[2] * m[3] - m[0] * m[5]) * id;
  r[6] = c2 * id;
  r[7] = (m[1] * m[6] - m[0] * m[7]) * id;
  r[8] = (m[0] * m[4] - m[1] * m[3]) * id;
}

// XYZ -> linear RGB for primaries (xr..yb) and white (xw, yw), white at Y = 1.
void xyz_to_rgb(const double xy[6], const double w[2], Mat3 out) {
  Mat3 p;  // columns: XYZ of each primary at Y = 1
  for (int i = 0; i < 3; ++i) {
    double x = xy[2 * i], y = xy[2 * i + 1];
    p[i] = x / y;
    p[3 + i] = 1.0;
    p[6 + i] = (1.0 - x - y) / y;
  }
  double wx = w[0] / w[1], wz = (1.0 - w[0] - w[1]) / w[1];
  Mat3 pi;
  invert(p, pi);
  double s[3];
  for (int i = 0; i < 3; ++i) s[i] = pi[3 * i] * wx + pi[3 * i + 1] * 1.0 + pi[3 * i + 2] * wz;
  Mat3 rgb_to_xyz;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) rgb_to_xyz[3 * r + c] = p[3 * r + c] * s[c];
  invert(rgb_to_xyz, out);
}

void put(std::vector<uint8_t>& b, const void* p, size_t n) {
  const uint8_t* q = static_cast<const uint8_t*>(p);
  b.insert(b.end(), q, q + n);
}

// Y_n (sum of ybar at 1 nm = 106.856895, 0003 d11) and the factor that scales the CSV's D65 to Y = 1.
void norm_factors(const Csv& c, double* y_n, double* d65_scale) {
  double yn = 0, y_d65 = 0;
  for (uint32_t i = 0; i < kObserverSamples; ++i) {
    yn += c.y[i];
    y_d65 += c.d65[i] * c.y[i];
  }
  // 0003 d11 records 106.856895; a table that disagrees is a different observer.
  if (std::fabs(yn - 106.856895) > 1e-6 * 106.856895) die("Y_n = " + std::to_string(yn) + ", expected 106.856895");
  *y_n = yn;
  *d65_scale = yn / y_d65;  // Y(D65) = sum(s ybar) / Y_n = 1
}

// XYZ -> linear Rec.709. BT.709-6 Table 3: R (0.640, 0.330), G (0.300, 0.600), B (0.150, 0.060),
// D65 white (0.3127, 0.3290).
void rec709_matrix(Mat3 m_ws) {
  const double prim[6] = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060};
  const double white[2] = {0.3127, 0.3290};
  xyz_to_rgb(prim, white, m_ws);
}

int observer(const std::string& root, const std::string& out_path) {
  auto t0 = std::chrono::steady_clock::now();
  Csv c = read_csv(root + "data/spectral/cie1931_2deg.csv");

  double y_n = 0, d65_scale = 0;
  norm_factors(c, &y_n, &d65_scale);
  const double k = 683.0 * y_n;

  astroray::ObserverTables t;
  for (uint32_t i = 0; i < kObserverSamples; ++i) {
    t.cmf[0][i] = float(c.x[i]);
    t.cmf[1][i] = float(c.y[i]);
    t.cmf[2][i] = float(c.z[i]);
    t.d65[i] = float(c.d65[i] * d65_scale);
  }
  uint8_t hash[8];
  astroray::observer_hash_of(t.cmf, hash);

  Mat3 m_ws;
  rec709_matrix(m_ws);

  std::vector<uint8_t> b;
  const uint32_t version = astroray::kObserverVersion, kind = astroray::kDataKindObserver;
  const uint32_t n = kObserverSamples, l0 = kObserverLambdaMin, step = 1;
  put(b, astroray::kDataMagic, astroray::kDataMagicBytes);
  put(b, &version, 4);
  put(b, &kind, 4);
  put(b, hash, 8);
  put(b, &n, 4);
  put(b, &l0, 4);
  put(b, &step, 4);
  put(b, t.cmf, sizeof(t.cmf));
  put(b, &y_n, 8);
  put(b, &k, 8);
  put(b, t.d65, sizeof(t.d65));
  put(b, m_ws, sizeof(m_ws));
  if (b.size() != astroray::kObserverBytes) die("internal: observer.bin size mismatch");

  std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
  if (!f) die("cannot write " + out_path);
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  f.close();
  if (!f) die("write failed: " + out_path);

  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("observer: Y_n = %.6f  K = %.3f  observer_hash = ", y_n, k);
  for (int i = 0; i < 8; ++i) std::printf("%02x", hash[i]);
  std::printf("\nobserver: wrote %s (%zu bytes) in %.1f ms\n", out_path.c_str(), b.size(), ms);
  return 0;
}

// Resolution of the table (0003 d16, PROVISIONAL) and the .jh header values. The layout lives in
// src/kernel/color/rgb2spec.h, which this tool cannot include (its headers need the kernel
// include path); tests/cpu/test_rgb2spec.cpp reads the committed file through that header's
// jh_view and checks these values against it.
constexpr int kJhRes = 64;
constexpr uint32_t kJhVersion = 1, kJhKind = 2, kJhIlluminantD65 = 1, kJhGeneratorVersion = 1, kJhHeaderBytes = 64;

int jh(const std::string& root, const std::string& space, const std::string& out_path) {
  if (space != "rec709") die("jh: only rec709 exists yet (Rec.2020 and AP1 come with M4)");
  auto t0 = std::chrono::steady_clock::now();
  Csv c = read_csv(root + "data/spectral/cie1931_2deg.csv");
  double y_n = 0, d65_scale = 0;
  norm_factors(c, &y_n, &d65_scale);

  // The observer hash is that of observer.bin: SHA-256 of the CMF as floats.
  float cmf[3][kObserverSamples];
  for (uint32_t i = 0; i < kObserverSamples; ++i) {
    cmf[0][i] = float(c.x[i]);
    cmf[1][i] = float(c.y[i]);
    cmf[2][i] = float(c.z[i]);
  }
  uint8_t hash[8];
  astroray::observer_hash_of(cmf, hash);

  // Forward model: working-space RGB of a spectrum s is sum_i s_i * rgb_tbl[.][i], the 1 nm sum
  // of 0003 d11 with the D65 illuminant (Y = 1) folded in.
  Mat3 m_ws, rgb_to_xyz;
  rec709_matrix(m_ws);
  invert(m_ws, rgb_to_xyz);
  rgb2spec_port::Model model;
  model.n = int(kObserverSamples);
  for (int k = 0; k < 3; ++k) model.rgb_tbl[k].resize(kObserverSamples);
  model.lambda01.resize(kObserverSamples);
  const double* cmf_d[3] = {c.x, c.y, c.z};
  for (uint32_t i = 0; i < kObserverSamples; ++i) {
    model.lambda01[i] = double(i) / double(kObserverSamples - 1);  // (lambda - 360) / (830 - 360)
    const double ill = c.d65[i] * d65_scale / y_n;
    for (int k = 0; k < 3; ++k) {
      double acc = 0;
      for (int j = 0; j < 3; ++j) acc += m_ws[3 * k + j] * cmf_d[j][i] * ill;
      model.rgb_tbl[k][i] = acc;
    }
    for (int j = 0; j < 3; ++j) model.xyz_whitepoint[j] += cmf_d[j][i] * ill;
  }
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) model.rgb_to_xyz[a][b] = rgb_to_xyz[3 * a + b];

  std::vector<float> scale, coeffs;
  rgb2spec_port::optimise_table(model, kJhRes, scale, coeffs, std::thread::hardware_concurrency());

  std::vector<uint8_t> b;
  const uint32_t version = kJhVersion, kind = kJhKind, res = kJhRes;
  const uint32_t illum = kJhIlluminantD65, gen = kJhGeneratorVersion;
  put(b, astroray::kDataMagic, astroray::kDataMagicBytes);
  put(b, &version, 4);
  put(b, &kind, 4);
  put(b, hash, 8);
  put(b, &res, 4);
  put(b, &illum, 4);
  put(b, &gen, 4);
  b.resize(kJhHeaderBytes, 0);
  put(b, scale.data(), scale.size() * 4);
  put(b, coeffs.data(), coeffs.size() * 4);
  if (b.size() != kJhHeaderBytes + 4u * res + 4u * 9u * res * res * res) die("internal: jh size mismatch");

  std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
  if (!f) die("cannot write " + out_path);
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  f.close();
  if (!f) die("write failed: " + out_path);

  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("jh %s: res %d, observer_hash = ", space.c_str(), kJhRes);
  for (int i = 0; i < 8; ++i) std::printf("%02x", hash[i]);
  std::printf("\njh %s: wrote %s (%zu bytes) in %.2f s (0009 B-37 reports specgen total, limit 20 s)\n", space.c_str(),
              out_path.c_str(), b.size(), s);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const char* usage = "usage: specgen observer [--out <file>] | specgen jh rec709 [--out <file>]";
  if (argc < 2) die(usage);
  const bool is_jh = std::strcmp(argv[1], "jh") == 0;
  if (!is_jh && std::strcmp(argv[1], "observer") != 0) die(usage);
  int first = 2;
  std::string space;
  if (is_jh) {
    if (argc < 3) die(usage);
    space = argv[2];
    first = 3;
  }
  std::string out = is_jh ? "data/spectral/" + space + ".jh" : "data/spectral/observer.bin";
  for (int i = first; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
    else die(std::string("unknown argument ") + argv[i]);
  }
  return is_jh ? jh("", space, out) : observer("", out);
}
