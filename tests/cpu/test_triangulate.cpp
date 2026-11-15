// SPDX-License-Identifier: Apache-2.0
// 0007 V2: triangles and quads index-identical to Blender's loop_triangles,
// n-gons cover the same area with no degenerate triangle.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/host/geometry/triangulate.h"

namespace {

// Minimal JSON reader, enough for golden.json (objects, arrays, strings, numbers).
struct Json {
  enum Kind { Num, Str, Arr, Obj } kind = Num;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::map<std::string, Json> obj;
};

struct Parser {
  const std::string& s;
  size_t i = 0;
  void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
  Json value() {
    ws();
    Json j;
    if (s[i] == '{') {
      j.kind = Json::Obj;
      ++i;
      for (;;) {
        ws();
        if (s[i] == '}') { ++i; break; }
        if (s[i] == ',') { ++i; continue; }
        std::string k = value().str;
        ws();
        ++i;  // ':'
        j.obj[k] = value();
      }
    } else if (s[i] == '[') {
      j.kind = Json::Arr;
      ++i;
      for (;;) {
        ws();
        if (s[i] == ']') { ++i; break; }
        if (s[i] == ',') { ++i; continue; }
        j.arr.push_back(value());
      }
    } else if (s[i] == '"') {
      j.kind = Json::Str;
      ++i;
      while (s[i] != '"') j.str += s[i++];
      ++i;
    } else {
      char* end = nullptr;
      j.num = std::strtod(s.c_str() + i, &end);
      i = static_cast<size_t>(end - s.c_str());
    }
    return j;
  }
};

struct Case {
  std::string name, kind;
  std::vector<float> pos;
  std::vector<uint32_t> tris;
};

std::vector<Case> load_golden() {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../data/triangulation/golden.json", std::ios::binary);
  if (!f) f.open("tests/data/triangulation/golden.json", std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "tests/data/triangulation/golden.json not found");
  std::stringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  Parser p{text};
  Json root = p.value();
  std::vector<Case> out;
  for (const Json& c : root.obj["cases"].arr) {
    Case k;
    k.name = c.obj.at("name").str;
    k.kind = c.obj.at("kind").str;
    for (const Json& v : c.obj.at("positions").arr)
      for (const Json& x : v.arr) k.pos.push_back(static_cast<float>(x.num));
    for (const Json& t : c.obj.at("tris").arr)
      for (const Json& x : t.arr) k.tris.push_back(static_cast<uint32_t>(x.num));
    out.push_back(std::move(k));
  }
  return out;
}

std::vector<uint32_t> run(const Case& c) {
  uint32_t n = static_cast<uint32_t>(c.pos.size() / 3);
  std::vector<uint32_t> cv(n);
  for (uint32_t i = 0; i < n; ++i) cv[i] = i;
  uint32_t fs[2] = {0, n};
  return astroray::host::triangulate(c.pos.data(), cv.data(), fs, 1).corners;
}

struct V3 { double x, y, z; };
V3 at(const Case& c, uint32_t i) { return {c.pos[3 * i], c.pos[3 * i + 1], c.pos[3 * i + 2]}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

}  // namespace

TEST_SUITE("fast") {
  TEST_CASE("triangulate/quads match blender") {
    auto cases = load_golden();
    int checked = 0;
    for (const Case& c : cases) {
      if (c.kind == "ngon") continue;
      INFO(c.name);
      CHECK(run(c) == c.tris);
      ++checked;
    }
    CHECK(checked >= 40);
  }

  TEST_CASE("triangulate/ngon area") {
    auto cases = load_golden();
    int checked = 0;
    for (const Case& c : cases) {
      if (c.kind != "ngon") continue;
      INFO(c.name);
      uint32_t n = static_cast<uint32_t>(c.pos.size() / 3);
      // Newell normal; its length is twice the polygon area.
      V3 nw{0, 0, 0};
      for (uint32_t i = 0; i < n; ++i) {
        V3 a = at(c, i), b = at(c, (i + 1) % n);
        nw.x += (a.y - b.y) * (a.z + b.z);
        nw.y += (a.z - b.z) * (a.x + b.x);
        nw.z += (a.x - b.x) * (a.y + b.y);
      }
      double area2 = std::sqrt(dot(nw, nw));
      V3 nh{nw.x / area2, nw.y / area2, nw.z / area2};
      auto tris = run(c);
      REQUIRE(tris.size() == 3 * (n - 2));
      double sum2 = 0;
      for (size_t t = 0; t < tris.size(); t += 3) {
        V3 a = at(c, tris[t]), b = at(c, tris[t + 1]), d = at(c, tris[t + 2]);
        double a2 = dot(cross(sub(b, a), sub(d, a)), nh);  // signed along the polygon normal
        CHECK(a2 > 1e-6 * area2);                          // no degenerate or flipped triangle
        sum2 += a2;
      }
      CHECK(std::fabs(sum2 - area2) <= 1e-6 * area2);
      ++checked;
    }
    CHECK(checked >= 15);
  }
}
