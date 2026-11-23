// SPDX-License-Identifier: Apache-2.0
// The minimal host builder of 0004 D10's light tree (kernel/light/light_tree.h): one leaf per
// light of positive importance, median splits, and Cycles' bounds merge. Built in double from
// each light's kernel bounds (light_bounds) and its importance energy (LightTable::importance).
// Not uploaded yet: the tree's kernel binding and its ledger record come with traversal.
// Ref: docs/decisions/0004-integrator.md D10 (the M1b builder and the node layout); Cycles
// third_party/_cache/refs/cycles-ed69ef2c6bd3/src/scene/light_tree.cpp (merge, LightTree::build,
// recursive_build bit trails) and light_tree.h (OrientationBounds, LightTreeMeasure::add)
// (Apache-2.0).
#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "../../kernel/light/light_tree.h"

namespace astroray::host {

// One light as the builder sees it, in double: its LightBounds, whether it is infinite, and its
// importance energy. Real-independent, so both Real builds of a test reach the same builder.
struct LightTreeLight {
  double lo[3], hi[3], w[3];
  double theta_o, theta_e;
  double energy;
  bool infinite;
};

struct LightTree {
  std::vector<ar::LightTreeNode> nodes;        // node 0 is the root; empty without lit lights
  std::vector<ar::LightTreeEmitter> emitters;  // one per input light, in input order
  ar::LightTreeView view() const {
    return ar::LightTreeView{nodes.data(), emitters.data(), uint32_t(nodes.size()), uint32_t(emitters.size())};
  }
};

// Lights of energy 0 get no leaf (depth kLightTreeAbsent), so their pmf is 0 (D9). Local lights
// split at the median of their box centres on the largest axis of the centre bounds, ties by
// light index; infinite lights form the root's right subtree, split at the median index. With no
// light of positive energy the tree has no node. Throws std::runtime_error for an empty input, a
// non-finite or negative energy, a total energy that overflows float, non-finite bounds, or a
// leaf deeper than kLightTreeMaxDepth.
LightTree build_light_tree(const std::vector<LightTreeLight>& lights);

// The input of one light record: light_bounds and light_is_infinite of this Real build.
inline LightTreeLight light_tree_light(const ar::LightRecord& r, double energy) {
  const ar::LightBounds b = ar::light_bounds(r);
  LightTreeLight l;
  const ar::Real3 lo = b.bbox.lo, hi = b.bbox.hi, w = b.w;
  const double vlo[3] = {double(lo.x), double(lo.y), double(lo.z)}, vhi[3] = {double(hi.x), double(hi.y), double(hi.z)};
  const double vw[3] = {double(w.x), double(w.y), double(w.z)};
  for (int a = 0; a < 3; ++a) l.lo[a] = vlo[a], l.hi[a] = vhi[a], l.w[a] = vw[a];
  l.theta_o = double(b.theta_o);
  l.theta_e = double(b.theta_e);
  l.energy = energy;
  l.infinite = ar::light_is_infinite(r);
  return l;
}

// The tree over `lights` (LightTable::lights) with their importance energies (LightTable::importance).
inline LightTree build_light_tree(const std::vector<ar::LightRecord>& lights, const std::vector<double>& importance) {
  if (lights.size() != importance.size()) throw std::runtime_error("light tree: one importance per light is required");
  std::vector<LightTreeLight> in;
  in.reserve(lights.size());
  for (size_t i = 0; i < lights.size(); ++i) in.push_back(light_tree_light(lights[i], importance[i]));
  return build_light_tree(in);
}

}  // namespace astroray::host
