// SPDX-License-Identifier: Apache-2.0
#include "host/light/light_tree.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <string>

namespace astroray::host {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Cone {
  double axis[3];
  double theta_o, theta_e;
};

// One subtree's measure, as Cycles' LightTreeMeasure: box, cone and energy. `has_box` is false for
// distant lights, which have no box (Cycles never grows a background or sun light's bbox).
struct Measure {
  double lo[3], hi[3];
  bool has_box;
  Cone cone;
  double energy;
};

double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Cycles util/math_float3.h make_orthonormals, first vector: (1, 1, 1) x N, or (-1, 1, 1) x N
// when N lies on the (1, 1, 1) diagonal, normalised.
void orthonormal(const double* n, double* out) {
  double a[3];
  if (n[0] != n[1] || n[0] != n[2]) a[0] = n[2] - n[1], a[1] = n[0] - n[2], a[2] = n[1] - n[0];
  else a[0] = n[2] - n[1], a[1] = n[0] + n[2], a[2] = -n[1] - n[0];
  const double l = std::sqrt(dot3(a, a));
  for (int k = 0; k < 3; ++k) out[k] = a[k] / l;
}

// Cycles scene/light_tree.cpp merge(OrientationBounds, OrientationBounds), in double: the cone
// with the larger theta_o is kept when it already contains the other (to Cycles' 5e-4 rad slack),
// else the union's theta_o = (theta_d + theta_o,a + theta_o,b) / 2 with the axis rotated from a
// toward b by theta_o - theta_o,a; theta_e is the larger of the two.
Cone merge(const Cone& ca, const Cone& cb) {
  const Cone* a = &ca;
  const Cone* b = &cb;
  if (cb.theta_o > ca.theta_o) std::swap(a, b);
  const double cos_ab = dot3(a->axis, b->axis);
  const double theta_d = std::acos(std::clamp(cos_ab, -1.0, 1.0));
  const double theta_e = std::max(a->theta_e, b->theta_e);
  if (a->theta_o + 5e-4 >= std::min(kPi, theta_d + b->theta_o)) return Cone{{a->axis[0], a->axis[1], a->axis[2]}, a->theta_o, theta_e};
  const double theta_o = (theta_d + a->theta_o + b->theta_o) * 0.5;
  if (theta_o >= kPi) return Cone{{a->axis[0], a->axis[1], a->axis[2]}, kPi, theta_e};
  Cone c;
  c.theta_o = theta_o;
  c.theta_e = theta_e;
  if (cos_ab < -0.9995) {
    orthonormal(a->axis, c.axis);  // opposite directions: any orthogonal vector is fine
  } else {
    const double theta_r = theta_o - a->theta_o;
    double ortho[3];
    for (int k = 0; k < 3; ++k) ortho[k] = b->axis[k] - a->axis[k] * cos_ab;
    const double ol = std::sqrt(dot3(ortho, ortho));  // > 0: cos_ab = 1 means a contains b
    for (int k = 0; k < 3; ++k) c.axis[k] = a->axis[k] * std::cos(theta_r) + ortho[k] / ol * std::sin(theta_r);
  }
  // Renormalise so the stored axis is unit to float precision, as the kernel assumes.
  const double l = std::sqrt(dot3(c.axis, c.axis));
  for (double& x : c.axis) x /= l;
  return c;
}

// Cycles LightTreeMeasure::add: grow the box, merge the cones, add the energies.
Measure add(const Measure& a, const Measure& b) {
  Measure m;
  m.has_box = a.has_box || b.has_box;
  for (int k = 0; k < 3; ++k) {
    m.lo[k] = !a.has_box ? b.lo[k] : !b.has_box ? a.lo[k] : std::min(a.lo[k], b.lo[k]);
    m.hi[k] = !a.has_box ? b.hi[k] : !b.has_box ? a.hi[k] : std::max(a.hi[k], b.hi[k]);
  }
  m.cone = merge(a.cone, b.cone);
  m.energy = a.energy + b.energy;
  return m;
}

struct Builder {
  const std::vector<LightTreeLight>& in;
  LightTree& out;
  std::vector<double> centre;  // 3 per input light

  // Builds the subtree over ids (light indices) at `depth` with the given bit trail and returns its
  // measure; the node is out.nodes[node].
  Measure build(uint32_t node, std::vector<uint32_t>& ids, size_t first, size_t last, uint32_t trail, uint32_t depth,
                bool distant) {
    if (last - first == 1) {
      const uint32_t i = ids[first];
      if (depth > ar::kLightTreeMaxDepth)
        throw std::runtime_error("light tree: depth " + std::to_string(depth) + " exceeds the 32-bit trail");
      out.emitters[i] = ar::LightTreeEmitter{trail, depth};
      const LightTreeLight& l = in[i];
      Measure m;
      m.has_box = !distant;
      for (int k = 0; k < 3; ++k) m.lo[k] = l.lo[k], m.hi[k] = l.hi[k], m.cone.axis[k] = l.w[k];
      m.cone.theta_o = l.theta_o;
      m.cone.theta_e = l.theta_e;
      m.energy = l.energy;
      store(node, m, distant, ar::kLightTreeLeaf, {0u, 0u}, i);
      return m;
    }
    // Median split: local lights on the largest axis of their centres' bounds (the lowest axis on
    // a tie), distant lights (no centre) by index; ties by light index, so the tree is unique.
    int axis = -1;
    if (!distant) {
      double lo[3] = {HUGE_VAL, HUGE_VAL, HUGE_VAL}, hi[3] = {-HUGE_VAL, -HUGE_VAL, -HUGE_VAL};
      for (size_t k = first; k < last; ++k)
        for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], centre[3 * ids[k] + a]), hi[a] = std::max(hi[a], centre[3 * ids[k] + a]);
      axis = 0;
      for (int a = 1; a < 3; ++a)
        if (hi[a] - lo[a] > hi[axis] - lo[axis]) axis = a;
    }
    const size_t mid = first + (last - first) / 2;
    std::nth_element(ids.begin() + std::ptrdiff_t(first), ids.begin() + std::ptrdiff_t(mid), ids.begin() + std::ptrdiff_t(last),
                     [&](uint32_t x, uint32_t y) {
                       if (axis >= 0 && centre[3 * x + axis] != centre[3 * y + axis]) return centre[3 * x + axis] < centre[3 * y + axis];
                       return x < y;
                     });
    const uint32_t left = uint32_t(out.nodes.size());
    out.nodes.emplace_back();
    const Measure ml = build(left, ids, first, mid, trail, depth + 1, distant);
    const uint32_t right = uint32_t(out.nodes.size());
    out.nodes.emplace_back();
    const Measure mr = build(right, ids, mid, last, depth < 32 ? trail | (1u << depth) : trail, depth + 1, distant);
    const Measure m = add(ml, mr);
    store(node, m, distant, 0u, {left, right}, 0u);
    return m;
  }

  void store(uint32_t node, const Measure& m, bool distant, uint32_t flags, std::array<uint32_t, 2> child, uint32_t light) {
    ar::LightTreeNode& n = out.nodes[node];
    for (int k = 0; k < 3; ++k) {
      n.bbox_lo[k] = m.has_box ? float(m.lo[k]) : 0.0f;
      n.bbox_hi[k] = m.has_box ? float(m.hi[k]) : 0.0f;
      n.axis[k] = float(m.cone.axis[k]);
    }
    n.theta_o = float(m.cone.theta_o);
    n.theta_e = float(m.cone.theta_e);
    n.energy = float(m.energy);
    n.child[0] = child[0];
    n.child[1] = child[1];
    n.light = light;
    n.flags = flags | (distant ? ar::kLightTreeDistant : 0u);
  }
};

}  // namespace

LightTree build_light_tree(const std::vector<LightTreeLight>& lights) {
  if (lights.empty()) throw std::runtime_error("light tree: no lights to build from");
  if (lights.size() >= ar::kNoLight) throw std::runtime_error("light tree: too many lights for 32-bit indices");
  LightTree t;
  t.emitters.assign(lights.size(), ar::LightTreeEmitter{0u, ar::kLightTreeAbsent});
  Builder b{lights, t, std::vector<double>(3 * lights.size(), 0.0)};
  std::vector<uint32_t> local, distant;
  double total = 0.0;
  for (uint32_t i = 0; i < uint32_t(lights.size()); ++i) {
    const LightTreeLight& l = lights[i];
    if (!std::isfinite(l.energy) || l.energy < 0.0)
      throw std::runtime_error("light tree: light " + std::to_string(i) + " has a non-finite or negative energy");
    bool finite = std::isfinite(l.theta_o) && std::isfinite(l.theta_e);
    for (int k = 0; k < 3; ++k) finite = finite && std::isfinite(l.lo[k]) && std::isfinite(l.hi[k]) && std::isfinite(l.w[k]);
    if (!finite) throw std::runtime_error("light tree: light " + std::to_string(i) + " has non-finite bounds");
    if (!(l.energy > 0.0)) continue;  // no leaf: pmf 0 (D9)
    total += l.energy;
    for (int k = 0; k < 3; ++k) b.centre[3 * i + k] = 0.5 * (l.lo[k] + l.hi[k]);
    (l.infinite ? distant : local).push_back(i);
  }
  if (!(total <= double(FLT_MAX))) throw std::runtime_error("light tree: the total importance energy overflows float");
  if (local.empty() && distant.empty()) return t;
  t.nodes.emplace_back();
  if (local.empty() || distant.empty()) {
    std::vector<uint32_t>& ids = local.empty() ? distant : local;
    b.build(0u, ids, 0, ids.size(), 0u, 0u, local.empty());
    return t;
  }
  // Cycles LightTree::build: the local lights under the root's left child, the distant ones under
  // its right child (bit 0 of their trails is 1).
  const uint32_t left = 1u;
  t.nodes.emplace_back();
  const Measure ml = b.build(left, local, 0, local.size(), 0u, 1u, false);
  const uint32_t right = uint32_t(t.nodes.size());
  t.nodes.emplace_back();
  const Measure mr = b.build(right, distant, 0, distant.size(), 1u, 1u, true);
  b.store(0u, add(ml, mr), false, 0u, {left, right}, 0u);
  return t;
}

}  // namespace astroray::host
