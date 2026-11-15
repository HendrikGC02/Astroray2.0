// SPDX-License-Identifier: Apache-2.0
// FilmView readback, the denoiser interface and the FITS film dump on a synthetic 3x2 film. The
// film is filled directly (no resolve), so every expected value is a closed form:
//   display component c, pixel p: mean 10c + p + 1, variance of the mean 0.5 + p/8 (NaN at p = 5,
//   n = 1); n = 4 except pixel 5 (n = 1); "bounces" mean 2 (p + 1); "bounce_max" p + 1;
//   "bounce_paths" 4; "probe" (a sample0 ADD channel) stored p + 1 and read back undivided.
// tests/abi/fits_read.py repeats these formulas; keep the two in step.
// Ref: docs/decisions/0008-film-outputs-denoise.md d13, d13a, d16, d17; FITS Standard 4.0.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/build_info.h"
#include "host/denoise/denoiser.h"
#include "host/film/fits_writer.h"
#include "host/film/film_view.h"
#include "host/thread_pool.h"

using namespace astroray;
using namespace astroray::host;

namespace {

struct FitsRig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::unique_ptr<Film> film;

  FitsRig() : pool(ctx, 1), dev(ledger, pool) {
    FilmRequest req;
    req.max_width = 3;
    req.max_height = 2;
    req.extra.push_back({"probe", ChannelKind::Add, 1, 0, true});
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    ar::FilmData& d = film->data();
    const FilmLayout& l = film->layout();
    for (uint32_t p = 0; p < 6; ++p) {
      const uint32_t n = p == 5 ? 1u : 4u;
      d.n[p] = n;
      for (uint32_t c = 0; c < 3; ++c) {
        const double s1 = double(n) * double(10 * c + p + 1);
        d.s1[uint64_t(c) * d.plane_stride + p] = s1;
        d.s2[uint64_t(c) * d.plane_stride + p] = s1 * s1 / double(n) + 12.0 * (0.5 + p / 8.0);
      }
      d.add[uint64_t(l.offset("bounces")) * d.plane_stride + p] = float(2 * (p + 1) * n);
      d.add[uint64_t(l.offset("probe")) * d.plane_stride + p] = float(p + 1);
      d.count[uint64_t(l.offset("bounce_max")) * d.plane_stride + p] = p + 1;
      d.count[uint64_t(l.offset("bounce_paths")) * d.plane_stride + p] = 4;
    }
  }
};

FitsProvenance provenance() {
  FitsProvenance p;
  p.seed = 7;
  p.spp = 4;
  p.sampler_mode = "sobol";
  p.observer_hash = "0123456789abcdef";
  p.working_space = "Rec.709";
  p.filter = "gaussian";
  p.wavelength_pdf = "visible_default";
  return p;
}

// The card of an 80-byte-card header that starts with `key` ("" when absent before END).
std::string card_value(const std::vector<uint8_t>& f, size_t hdu_start, const std::string& key) {
  for (size_t at = hdu_start;; at += 80) {
    REQUIRE(at + 80 <= f.size());
    const std::string card(reinterpret_cast<const char*>(&f[at]), 80);
    if (card.compare(0, 3, "END") == 0 && card[3] == ' ') return "";
    if (card.compare(0, 8, (key + "        ").substr(0, 8)) == 0 && card.compare(8, 2, "= ") == 0) {
      std::string v = card.substr(10);
      const size_t b = v.find_first_not_of(' ');
      v = v.substr(b);
      if (v[0] == '\'') {  // trailing blanks of a string are not significant (FITS 4.0 4.2.1)
        std::string s = v.substr(1, v.find('\'', 1) - 1);
        return s.substr(0, s.find_last_not_of(' ') + 1);
      }
      return v.substr(0, v.find_first_of(' '));
    }
  }
}

uint32_t be32(const std::vector<uint8_t>& f, size_t at) {
  return uint32_t(f[at]) << 24 | uint32_t(f[at + 1]) << 16 | uint32_t(f[at + 2]) << 8 | f[at + 3];
}

std::filesystem::path repo_root() {
  std::string here = __FILE__;  // <root>/tests/cpu/test_fits.cpp
  return std::filesystem::path(here).parent_path().parent_path().parent_path();
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("film_view/mean variance counts") {
  FitsRig rig;
  FilmView v(rig.dev, *rig.film);
  CHECK(v.width() == 3);
  CHECK(v.height() == 2);
  const std::vector<double> m = v.mean("display", 2), var = v.variance_of_mean("display", 1);
  const std::vector<double> bounces = v.mean("bounces", 0), probe = v.mean("probe", 0);
  const std::vector<uint32_t> bmax = v.counts("bounce_max", 0), n = v.sample_counts();
  for (uint32_t p = 0; p < 6; ++p) {
    CHECK(m[p] == doctest::Approx(20 + p + 1));
    if (p == 5) CHECK(std::isnan(var[p]));
    else CHECK(var[p] == doctest::Approx(0.5 + p / 8.0));
    CHECK(bounces[p] == doctest::Approx(2 * (p + 1)));
    CHECK(probe[p] == doctest::Approx(p + 1));  // sample0: not divided by n = 4
    CHECK(bmax[p] == p + 1);
    CHECK(n[p] == (p == 5 ? 1u : 4u));
  }
  CHECK_THROWS_AS(v.variance_of_mean("bounces", 0), std::invalid_argument);
  CHECK_THROWS_AS(v.counts("display", 0), std::invalid_argument);
  CHECK_THROWS_AS(v.mean("display", 3), std::invalid_argument);
  CHECK_THROWS_AS(v.mean("nope", 0), std::invalid_argument);
}

TEST_CASE("fits/header keywords") {
  FitsRig rig;
  FilmView v(rig.dev, *rig.film);
  const std::vector<uint8_t> f = fits_film_dump_bytes(v, provenance());
  CHECK(f.size() % 2880 == 0);
  CHECK(card_value(f, 0, "SIMPLE") == "T");
  CHECK(card_value(f, 0, "BITPIX") == "8");
  CHECK(card_value(f, 0, "NAXIS") == "0");
  CHECK(card_value(f, 0, "DENOISED") == "F");
  CHECK(card_value(f, 0, "ENGVER") == std::string(kBuildInfo.version));
  CHECK(card_value(f, 0, "COMMIT").compare(0, std::strlen(kBuildInfo.git_sha), kBuildInfo.git_sha) == 0);
  CHECK(card_value(f, 0, "SEED") == "7");
  CHECK(card_value(f, 0, "SPP") == "4");
  CHECK(card_value(f, 0, "SAMPLER") == "sobol");
  CHECK(card_value(f, 0, "OBSHASH") == "0123456789abcdef");
  CHECK(card_value(f, 0, "WORKSPC") == "Rec.709");
  CHECK(card_value(f, 0, "FILTER") == "gaussian");
  CHECK(card_value(f, 0, "WLPDF") == "visible_default");
  // The first extension: an IMAGE of display.0's mean, 3 x 2, BITPIX -32.
  CHECK(card_value(f, 2880, "XTENSION") == "IMAGE");
  CHECK(card_value(f, 2880, "BITPIX") == "-32");
  CHECK(card_value(f, 2880, "NAXIS1") == "3");
  CHECK(card_value(f, 2880, "NAXIS2") == "2");
  CHECK(card_value(f, 2880, "EXTNAME") == "display.0");
}

TEST_CASE("fits/rows bottom-up at both precisions") {
  FitsRig rig;
  FilmView v(rig.dev, *rig.film);
  // display.0's mean is 1 .. 6 with pixel 0 the bottom-left; the data follows the 2880-byte header.
  const std::vector<uint8_t> f32 = fits_film_dump_bytes(v, provenance());
  for (uint32_t p = 0; p < 6; ++p) {
    const uint32_t bits = be32(f32, 2 * 2880 + 4 * p);
    float x;
    std::memcpy(&x, &bits, 4);
    CHECK(x == float(p + 1));
  }
  const std::vector<uint8_t> f64 = fits_film_dump_bytes(v, provenance(), FitsPrecision::F64);
  CHECK(card_value(f64, 2880, "BITPIX") == "-64");
  for (uint32_t p = 0; p < 6; ++p) {
    const uint64_t bits = uint64_t(be32(f64, 2 * 2880 + 8 * p)) << 32 | be32(f64, 2 * 2880 + 8 * p + 4);
    double x;
    std::memcpy(&x, &bits, 8);
    CHECK(x == double(p + 1));
  }
}

TEST_CASE("fits/rejects unusable provenance") {
  FitsRig rig;
  FilmView v(rig.dev, *rig.film);
  FitsProvenance p = provenance();
  p.filter.clear();
  CHECK_THROWS_AS(fits_film_dump_bytes(v, p), std::invalid_argument);
  p = provenance();
  p.working_space = std::string(69, 'x');
  CHECK_THROWS_AS(fits_film_dump_bytes(v, p), std::invalid_argument);
  p = provenance();
  p.sampler_mode = "so\xC3\xA9";
  CHECK_THROWS_AS(fits_film_dump_bytes(v, p), std::invalid_argument);
}

TEST_CASE("denoiser/none copies colour undenoised") {
  ContextState ctx;
  auto state = DenoiserState::create(ctx, DenoiserBackend::NONE);
  state->wait();
  REQUIRE(state->ready());
  const float rgb[6] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
  DenoiseInputs in;
  in.color = {rgb, 2, 1, 3};
  DisplayImage out;
  out.denoised = true;
  denoise(*state, in, out, DenoiseMode::FINAL);
  CHECK(out.width == 2);
  CHECK(out.height == 1);
  CHECK_FALSE(out.denoised);
  const float want[8] = {0.1f, 0.2f, 0.3f, 1.0f, 0.4f, 0.5f, 0.6f, 1.0f};
  for (int i = 0; i < 8; ++i) CHECK(out.rgba[i] == want[i]);
}

TEST_CASE("denoiser/unbuilt backend fails loudly") {
  ContextState ctx;
  auto state = DenoiserState::create(ctx, DenoiserBackend::OIDN);
  state->wait();
  CHECK(state->failed());
  CHECK_FALSE(state->ready());
  CHECK(state->error().find("OIDN") != std::string::npos);
  const float rgb[3] = {0, 0, 0};
  DenoiseInputs in;
  in.color = {rgb, 1, 1, 3};
  DisplayImage out;
  CHECK_THROWS_AS(denoise(*state, in, out, DenoiseMode::VIEWPORT), std::runtime_error);
}

}  // TEST_SUITE

TEST_SUITE("gate") {

// Writes both dumps, then runs tests/abi/fits_read.py on each through tools/lib/python.ps1's
// Get-ArPython, so the interpreter is the one every tool uses.
TEST_CASE("fits/reader parses both dumps") {
  FitsRig rig;
  FilmView v(rig.dev, *rig.film);
  const std::filesystem::path root = repo_root();
  const std::filesystem::path dir = root / "out" / "check" / "fits";
  std::filesystem::create_directories(dir);
  for (const FitsPrecision prec : {FitsPrecision::F32, FitsPrecision::F64}) {
    const bool f32 = prec == FitsPrecision::F32;
    const std::string file = (dir / (f32 ? "dump_f32.fits" : "dump_f64.fits")).generic_string();
    write_fits_film_dump(file, v, provenance(), prec);
    const std::string cmd = "pwsh -NoProfile -Command \". '" + (root / "tools/lib/python.ps1").generic_string() +
                            "'; & (Get-ArPython) -I '" + (root / "tests/abi/fits_read.py").generic_string() + "' '" +
                            file + "' " + (f32 ? "-32" : "-64") + " 2>&1; exit $LASTEXITCODE\"";
    std::string output;
    FILE* pipe = _popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe) != nullptr) output += buf;
    const int rc = _pclose(pipe);
    INFO(output);
    CHECK(rc == 0);
  }
}

}  // TEST_SUITE
