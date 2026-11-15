// SPDX-License-Identifier: Apache-2.0
#include "triangulate.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace astroray::host {
namespace {

struct P3 {
  double x, y, z;
};

P3 load(const float* pos, uint32_t v) { return {pos[3 * v], pos[3 * v + 1], pos[3 * v + 2]}; }

P3 sub(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
P3 cross3(P3 a, P3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot3(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Quad rule, determined from golden data captured in Blender 5.3 (tests/data/triangulation,
// 0007 d10): every planar convex, non-planar and random quad is split on the 0-2 diagonal as
// (0,1,2),(0,2,3); a concave quad is split on the diagonal through its reflex corner
// (0-2 if corner 0 or 2 is reflex, else 1-3 as (0,1,3),(1,2,3)), which is the only diagonal
// inside the polygon. Reflex is judged against the Newell normal, so it also holds when the
// quad is not planar.
void quad_split(const float* pos, const uint32_t* cv, uint32_t s, std::vector<uint32_t>& out) {
  P3 p0 = load(pos, cv[s]), p1 = load(pos, cv[s + 1]), p2 = load(pos, cv[s + 2]), p3 = load(pos, cv[s + 3]);
  P3 n = cross3(sub(p2, p0), sub(p3, p1));  // diagonals' cross product: twice the Newell area vector
  // Triangles of the 0-2 split pointing against n mean corner 1 or 3 is reflex.
  bool flip = dot3(cross3(sub(p1, p0), sub(p2, p1)), n) < 0 || dot3(cross3(sub(p2, p0), sub(p3, p2)), n) < 0;
  if (flip) out.insert(out.end(), {s, s + 1, s + 3, s + 1, s + 2, s + 3});
  else out.insert(out.end(), {s, s + 1, s + 2, s, s + 2, s + 3});
}

struct P2 {
  double x, y;
};

double cross2(P2 o, P2 a, P2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Closed-triangle containment for a counter-clockwise triangle.
bool in_triangle(P2 a, P2 b, P2 c, P2 p) {
  return cross2(a, b, p) >= 0 && cross2(b, c, p) >= 0 && cross2(c, a, p) >= 0;
}

// Ref: D. Eberly, "Triangulation by Ear Clipping", Geometric Tools 2002 (ear test: convex
// vertex whose triangle holds no other non-convex vertex); Newell's normal (Sutherland,
// Sproull, Schumacker 1974) picks the projection. Ties are broken by always taking the
// first ear at or after the vertex following the last clip, so output is deterministic.
void ear_clip(const float* pos, const uint32_t* cv, uint32_t first, uint32_t n, size_t face,
              std::vector<uint32_t>& out) {
  P3 nw{0, 0, 0};
  for (uint32_t i = 0; i < n; ++i) {
    P3 a = load(pos, cv[first + i]), b = load(pos, cv[first + (i + 1) % n]);
    nw.x += (a.y - b.y) * (a.z + b.z);
    nw.y += (a.z - b.z) * (a.x + b.x);
    nw.z += (a.x - b.x) * (a.y + b.y);
  }
  double ax = std::fabs(nw.x), ay = std::fabs(nw.y), az = std::fabs(nw.z);
  if (!(ax + ay + az > 0))
    throw std::runtime_error("triangulate: face " + std::to_string(face) + " has zero area");
  // Drop the dominant axis; flip so the projected polygon is counter-clockwise.
  std::vector<P2> q(n);
  for (uint32_t i = 0; i < n; ++i) {
    P3 p = load(pos, cv[first + i]);
    if (az >= ax && az >= ay) q[i] = {nw.z > 0 ? p.x : p.y, nw.z > 0 ? p.y : p.x};
    else if (ax >= ay) q[i] = {nw.x > 0 ? p.y : p.z, nw.x > 0 ? p.z : p.y};
    else q[i] = {nw.y > 0 ? p.z : p.x, nw.y > 0 ? p.x : p.z};
  }
  double scale = 0;
  for (const P2& p : q) scale = std::fmax(scale, std::fmax(std::fabs(p.x), std::fabs(p.y)));
  const double eps = 1e-12 * scale * scale;  // a convex corner must beat rounding noise

  std::vector<uint32_t> prev(n), next(n);
  for (uint32_t i = 0; i < n; ++i) {
    prev[i] = (i + n - 1) % n;
    next[i] = (i + 1) % n;
  }
  uint32_t remaining = n, cur = 0;
  while (remaining > 3) {
    bool clipped = false;
    uint32_t v = cur;
    for (uint32_t tries = 0; tries < remaining; ++tries, v = next[v]) {
      uint32_t a = prev[v], b = next[v];
      if (cross2(q[a], q[v], q[b]) <= eps) continue;  // reflex or collinear: not an ear
      bool blocked = false;
      for (uint32_t w = next[b]; w != a; w = next[w]) {
        if (cross2(q[prev[w]], q[w], q[next[w]]) > eps) continue;  // strictly convex vertices cannot block
        if (in_triangle(q[a], q[v], q[b], q[w])) { blocked = true; break; }
      }
      if (blocked) continue;
      out.push_back(first + a);
      out.push_back(first + v);
      out.push_back(first + b);
      next[a] = b;
      prev[b] = a;
      --remaining;
      cur = b;
      clipped = true;
      break;
    }
    if (!clipped)
      throw std::runtime_error("triangulate: face " + std::to_string(face) +
                               " has no ear (self-intersecting or degenerate polygon)");
  }
  uint32_t a = cur, b = next[a], c = next[b];
  if (cross2(q[a], q[b], q[c]) <= eps)
    throw std::runtime_error("triangulate: face " + std::to_string(face) +
                             " ends in a degenerate triangle");
  out.push_back(first + a);
  out.push_back(first + b);
  out.push_back(first + c);
}

}  // namespace

std::vector<uint32_t> triangulate(const float* positions, const uint32_t* corner_vert,
                                  const uint32_t* face_start, size_t face_count) {
  std::vector<uint32_t> out;
  out.reserve(face_count * 6);
  for (size_t f = 0; f < face_count; ++f) {
    uint32_t s = face_start[f], n = face_start[f + 1] - s;
    if (n < 3) continue;
    if (n == 3) {
      out.insert(out.end(), {s, s + 1, s + 2});
    } else if (n == 4) {
      quad_split(positions, corner_vert, s, out);
    } else {
      ear_clip(positions, corner_vert, s, n, f, out);
    }
  }
  return out;
}

}  // namespace astroray::host
