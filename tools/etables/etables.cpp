// SPDX-License-Identifier: Apache-2.0
// etables: generates the directional-albedo tables of the lobe stack from the engine's own lobe
// code (0005 item 6). Not in the default build; run by hand from the repository root:
//   pwsh tools/build.ps1 -Target etables
//   out/bin/etables.exe e_diel_refl|e_spec [--out <file>]
// `e_diel_refl` writes data/tables/e_diel_refl.bin (layout: src/kernel/material/lobes/tables.h):
// E(mu, r, eta) = integral of f cos over the hemisphere for the GGX dielectric reflection lobe
// with specular_weight 1 and specular_color 1, 16^3 cells. `e_spec` writes data/tables/e_spec.bin:
// E(mu, r) of the metal lobe (lobes/metal.h) at F = 1 (base_color 1, specular_color 1, both weights
// 1), 32 x 32 cells. Each cell is estimated by importance sampling with the lobe's own VNDF sampler. The output is a pure function of the lobe source:
// each cell uses a fixed stratified grid with hashed jitter and a fixed summation order, so the
// bytes do not depend on the thread count.
// Ref: docs/decisions/0005-material-model.md item 6 (tables from our own lobe code, checked in
// with a hash); Kulla and Conty 2017 (SIGGRAPH course) for the table; Cycles
// src/app/cycles_precompute.cpp (Apache-2.0) for the pattern of a table generator.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "host/data_resource.h"
#include "material/lobes/ggx.h"
#include "material/lobes/metal.h"
#include "material/lobes/tables.h"

namespace {

constexpr uint32_t kGeneratorVersion = 1;
constexpr uint32_t kStrata = 256;  // 256 x 256 = 2^16 samples per cell
constexpr uint32_t kSamplesPerCell = kStrata * kStrata;
constexpr uint32_t kN = ar::kEDielN;
constexpr uint32_t kNs = ar::kESpecN;

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "etables: %s\n", msg.c_str());
  std::exit(1);
}

// splitmix64 (Steele, Lea, Flood 2014): a stateless hash for the sample jitter.
uint64_t mix64(uint64_t z) {
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
double unit(uint64_t h) { return double(h >> 11) * (1.0 / 9007199254740992.0); }

void put_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * i));
}
void put_f32(std::vector<uint8_t>& b, size_t off, float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  put_u32(b, off, u);
}

// SHA-256 of the lobe sources the table depends on, with CR removed so a CRLF checkout hashes
// like an LF one.
// names: up to 4 file names under src/kernel/material/lobes/, unused entries nullptr.
void lobe_hash(const char* const (&names)[4], uint8_t out[32]) {
  std::string all;
  for (const char* name : names) {
    if (name == nullptr) continue;
    const std::string path = std::string("src/kernel/material/lobes/") + name;
    std::ifstream f(path, std::ios::binary);
    if (!f) die("cannot open " + path + " (run from the repository root)");
    std::stringstream ss;
    ss << f.rdbuf();
    for (char c : ss.str())
      if (c != '\r') all.push_back(c);
  }
  astroray::sha256(all.data(), all.size(), out);
}

// Mean of f cos / pdf over the stratified grid of cell `cell` for a lobe sampler `sample(u)`. A delta
// lobe returns F exactly from any sample, so one sample is enough.
template <typename Sample>
double stratified_albedo(uint64_t cell, const Sample& sample) {
  double sum = 0.0;
  for (uint32_t i = 0; i < kStrata; ++i)
    for (uint32_t j = 0; j < kStrata; ++j) {
      const uint64_t h = mix64(cell * kSamplesPerCell + i * kStrata + j);
      const double u1 = (i + unit(h)) / kStrata, u2 = (j + unit(mix64(h))) / kStrata;
      const ar::LobeSample s = sample(ar::Real2{float(u1), float(u2)});
      if (!(s.pdf > 0.0f)) continue;  // reflected below the horizon: carries no energy
      const double w = double(s.f.v[0]) * double(s.wi.z) / double(s.pdf);
      if ((s.flags & ar::LOBE_DELTA) != 0) return w;
      sum += w;
    }
  return sum / double(kSamplesPerCell);
}

// E_diel at one node.
double cell_albedo(uint32_t imu, uint32_t ir, uint32_t ieta) {
  const double top = double(kN - 1);
  const double mu = std::fmax(double(imu) / top, double(ar::kEDielMuFloor));
  const double r = double(ir) / top;
  const double eta = double(ar::kEDielEtaMin) + (double(ar::kEDielEtaMax) - double(ar::kEDielEtaMin)) * double(ieta) / top;
  ar::GgxParams p;
  p.specular_color = ar::spec_fill(1.0f);
  p.specular_ior = ar::spec_fill(float(eta));
  p.specular_weight = 1.0f;
  p.roughness = float(r);
  const ar::Real3 wo{float(std::sqrt(1.0 - mu * mu)), 0.0f, float(mu)};
  const uint64_t cell = (uint64_t(ieta) * kN + ir) * kN + imu;
  return stratified_albedo(cell, [&](ar::Real2 u) { return ar::ggx_sample(p, wo, u, ar::TransportMode::Radiance); });
}

// E_spec at one node: the metal lobe with F = 1 (F82 at F0 = 1 and tint 1 is 1 at every angle).
double cell_albedo_spec(uint32_t imu, uint32_t ir) {
  const double top = double(kNs - 1);
  const double mu = std::fmax(double(imu) / top, double(ar::kEDielMuFloor));
  ar::MetalParams p;
  p.base_color = ar::spec_fill(1.0f);
  p.specular_color = ar::spec_fill(1.0f);
  p.base_weight = 1.0f;
  p.specular_weight = 1.0f;
  p.roughness = float(double(ir) / top);
  const ar::Real3 wo{float(std::sqrt(1.0 - mu * mu)), 0.0f, float(mu)};
  const uint64_t cell = uint64_t(ir) * kNs + imu;
  return stratified_albedo(cell, [&](ar::Real2 u) { return ar::metal_sample(p, wo, u, ar::TransportMode::Radiance); });
}

// What distinguishes the two tables; the header layout and the writing are shared.
struct TableSpec {
  const char* name;
  uint32_t kind, n_mu, n_r, n_eta;
  float axes[6];
  const char* sources[4];  // the lobe files the values are a function of, nullptr-padded
  double (*cell)(uint32_t c);
  const char* (*validate)(const uint8_t* d, uint64_t size);
  bool positive;  // every cell must be > 0 (E_spec divides by it, metal.h metal_compensation)
};

double diel_cell(uint32_t c) { return cell_albedo(c % kN, (c / kN) % kN, c / (kN * kN)); }
double spec_cell(uint32_t c) { return cell_albedo_spec(c % kNs, c / kNs); }
const char* diel_validate(const uint8_t* d, uint64_t size) {
  ar::EDielTable v;
  return ar::e_diel_view(d, size, &v);
}
const char* spec_validate(const uint8_t* d, uint64_t size) {
  ar::ESpecTable v;
  return ar::e_spec_view(d, size, &v);
}

int run_table(const TableSpec& t, const std::string& out_path) {
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t cells = t.n_mu * t.n_r * t.n_eta;
  std::vector<float> e(cells);
  std::atomic<uint32_t> next{0};
  auto worker = [&] {
    for (uint32_t c = next.fetch_add(1); c < cells; c = next.fetch_add(1)) e[c] = float(t.cell(c));
  };
  std::vector<std::thread> pool;
  const unsigned n_threads = std::max(1u, std::thread::hardware_concurrency());
  for (unsigned i = 0; i < n_threads; ++i) pool.emplace_back(worker);
  for (auto& th : pool) th.join();

  std::vector<uint8_t> b(ar::kETableHeaderBytes + sizeof(float) * e.size(), 0);
  const uint8_t magic[8] = {'A', 'R', 'E', 'T', 'B', 'L', 0, 0};
  std::memcpy(b.data(), magic, 8);
  put_u32(b, 8, ar::kETableVersion);
  put_u32(b, 12, t.kind);
  put_u32(b, 16, kGeneratorVersion);
  put_u32(b, ar::kETableSamplesOffset, kSamplesPerCell);
  uint8_t h[32];
  lobe_hash(t.sources, h);
  std::memcpy(b.data() + ar::kETableLobeHashOffset, h, 32);
  put_u32(b, 56, t.n_mu);
  put_u32(b, 60, t.n_r);
  put_u32(b, 64, t.n_eta);
  for (int i = 0; i < 6; ++i) put_f32(b, 68 + 4 * i, t.axes[i]);
  for (size_t i = 0; i < e.size(); ++i) put_f32(b, ar::kETableHeaderBytes + 4 * i, e[i]);

  // A table the kernel would refuse must never be written.
  if (const char* err = t.validate(b.data(), b.size())) die(err);
  for (float v : e) {
    if (!(v >= 0.0f && v <= 1.0f + 1e-3f)) die("a cell is outside [0, 1]: the lobe code is broken");
    if (t.positive && !(v > 0.0f)) die("a cell is 0: the compensation would divide by it");
  }

  const std::filesystem::path p(out_path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
  std::ofstream f(out_path, std::ios::binary);
  if (!f) die("cannot write " + out_path);
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  f.close();
  uint8_t d[32];
  astroray::sha256(b.data(), b.size(), d);
  char hex[65];
  for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("etables %s: wrote %s (%zu bytes) sha256 %s\n", t.name, out_path.c_str(), b.size(), hex);
  std::printf("etables %s: %u cells x %u samples, %u threads, run time %.2f s\n", t.name, cells, kSamplesPerCell, n_threads, secs);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) die("usage: etables e_diel_refl|e_spec [--out <file>]");
  const TableSpec diel{"e_diel_refl", ar::kETableKindDielRefl, kN, kN, kN,
                       {0.0f, 1.0f, 0.0f, 1.0f, float(ar::kEDielEtaMin), float(ar::kEDielEtaMax)},
                       {"ggx.h", "fresnel.h", "delta.h", nullptr}, diel_cell, diel_validate, false};
  const TableSpec spec{"e_spec", ar::kETableKindSpec, kNs, kNs, 1u, {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
                       {"metal.h", "ggx.h", "fresnel.h", "delta.h"}, spec_cell, spec_validate, true};
  const TableSpec* t = std::strcmp(argv[1], "e_diel_refl") == 0 ? &diel : std::strcmp(argv[1], "e_spec") == 0 ? &spec : nullptr;
  if (t == nullptr) die(std::string("unknown table ") + argv[1]);
  std::string out = std::string("data/tables/") + t->name + ".bin";
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
    else die(std::string("unknown argument ") + argv[i]);
  }
  return run_table(*t, out);
}