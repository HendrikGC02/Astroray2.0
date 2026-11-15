// SPDX-License-Identifier: Apache-2.0
// 0003 V-checks 1 (Rec.709 white), 2 (Y_n, observer hash) and the K part of 11, on the
// committed data/spectral/observer.bin and cie1931_2deg.csv.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"

namespace {

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

astroray::ObserverTables load_observer() {
  auto b = read_data("spectral/observer.bin");
  astroray::ObserverTables t;
  std::string err = astroray::parse_observer(b.data(), b.size(), &t);
  REQUIRE_MESSAGE(err.empty(), err);
  return t;
}

}  // namespace

TEST_SUITE("fast") {
  TEST_CASE("observer/d65 white is 1 1 1") {
    auto t = load_observer();
    double xyz[3] = {0, 0, 0};
    for (uint32_t i = 0; i < astroray::kObserverSamples; ++i)
      for (int k = 0; k < 3; ++k) xyz[k] += double(t.d65[i]) * double(t.cmf[k][i]);
    for (double& v : xyz) v /= t.y_n;
    CHECK(xyz[1] == doctest::Approx(1.0).epsilon(1e-6));  // the SPD is scaled so Y = 1
    for (int r = 0; r < 3; ++r) {
      double rgb = t.m_ws[3 * r] * xyz[0] + t.m_ws[3 * r + 1] * xyz[1] + t.m_ws[3 * r + 2] * xyz[2];
      CHECK_MESSAGE(std::fabs(rgb - 1.0) < 5e-4, "channel " << r << " = " << rgb);
    }
  }

  // The white test cannot see wrong primaries (the matrix maps its own white to 1 for any
  // primaries), so each primary is pinned on its own.
  // Ref: ITU-R BT.709-6 Table 3: R (0.640, 0.330), G (0.300, 0.600), B (0.150, 0.060).
  TEST_CASE("observer/rec709 primaries map to pure channels") {
    auto t = load_observer();
    const double xy[3][2] = {{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}};
    for (int c = 0; c < 3; ++c) {
      const double x = xy[c][0], y = xy[c][1];
      const double xyz[3] = {x / y, 1.0, (1.0 - x - y) / y};
      for (int r = 0; r < 3; ++r) {
        double rgb = t.m_ws[3 * r] * xyz[0] + t.m_ws[3 * r + 1] * xyz[1] + t.m_ws[3 * r + 2] * xyz[2];
        if (r == c) CHECK_MESSAGE(rgb > 0.1, "primary " << c << " own channel = " << rgb);
        else CHECK_MESSAGE(std::fabs(rgb) < 1e-6, "primary " << c << " leaks " << rgb << " into channel " << r);
      }
    }
  }

  TEST_CASE("observer/Y_n value") {
    auto t = load_observer();  // also checks the stored observer_hash against the CMF floats
    CHECK(std::fabs(t.y_n / 106.856895 - 1.0) < 1e-6);
    double sum = 0;
    for (uint32_t i = 0; i < astroray::kObserverSamples; ++i) sum += double(t.cmf[1][i]);
    CHECK(std::fabs(sum / t.y_n - 1.0) < 1e-6);
    uint8_t h[8];
    astroray::observer_hash_of(t.cmf, h);
    CHECK(std::memcmp(h, t.observer_hash, 8) == 0);
  }

  TEST_CASE("observer/K is 683 Y_n") {
    auto t = load_observer();
    CHECK(std::fabs(t.k - 683.0 * t.y_n) <= 1e-9 * t.k);
  }

  TEST_CASE("observer/hash set refused when observer_hash differs") {
    auto make = [](uint8_t h) {
      std::vector<uint8_t> b(astroray::kDataHashOffset + 8 + 8, 0);
      std::memcpy(b.data(), astroray::kDataMagic, astroray::kDataMagicBytes);
      std::memset(b.data() + astroray::kDataHashOffset, h, 8);
      return b;
    };
    auto a = make(0x11), same = make(0x11), other = make(0x22);
    std::vector<uint8_t> plain = {'a', ',', 'b', '\n'};  // no common header: not compared
    auto blob = [](const std::vector<uint8_t>& v) {
      astroray::DataBlob d;
      d.data = v.data();
      d.size = v.size();
      return d;
    };
    using Set = std::vector<std::pair<std::string, astroray::DataBlob>>;
    CHECK(astroray::check_hash_set(Set{{"a.bin", blob(a)}, {"b.bin", blob(same)}, {"c.csv", blob(plain)}}).empty());
    std::string err = astroray::check_hash_set(Set{{"a.bin", blob(a)}, {"b.bin", blob(other)}});
    CHECK_MESSAGE(err.find("b.bin") != std::string::npos, err);
    CHECK_FALSE(astroray::check_hash_set(Set{{"c.csv", blob(plain)}}).empty());  // nothing to compare is not consistent
  }

  TEST_CASE("observer/embedded set matches the file and is consistent") {
    std::string err = astroray::check_observer_consistency();
    CHECK_MESSAGE(err.empty(), err);
    astroray::DataBlob blob;
    REQUIRE(astroray::find_data("spectral/observer.bin", &blob));
    auto file = read_data("spectral/observer.bin");
    REQUIRE(blob.size == file.size());
    CHECK(std::memcmp(blob.data, file.data(), file.size()) == 0);
  }
}
