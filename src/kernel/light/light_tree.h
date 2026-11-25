// SPDX-License-Identifier: Apache-2.0
// The light tree of 0004 D10: the importance sampler behind the same {light, pmf} interface as the
// alias table (light_sampler.h), which is its oracle. Built on the host (host/light/light_tree.h);
// this file holds the node layout and the pure kernel functions: node importance, a stack-free
// descent that accumulates the pmf, and the pmf of a given light by replaying its bit trail.
//
// What follows Cycles, quoted from the files named under Ref:
//  - Importance (kernel/light/tree.h light_tree_importance): the maximum importance is
//    "fabsf(f_a * cos_min_incidence_angle * energy * cos_min_outgoing_angle / sqr(min_distance))"
//    with f_a = 1, the minimum importance the same with cos_max_incidence_angle,
//    cos_max_outgoing_angle and max_distance, and 0 when "bcone.theta_e - bcone.theta_o < 0 ||
//    cos_theta < 0 || cos_theta_u < 0 || cos_theta_plus_theta_u < fast_cosf(bcone.theta_e -
//    bcone.theta_o)". A node whose bounding box lies wholly behind the surface ("dot(N, centroid -
//    P) + dot(fabs(N), fabs(bbox_extent)) > 0" fails) or outside every emission cone ("Cluster is
//    invisible") has importance 0. The distance is clamped: "distance = fmaxf(0.5f *
//    len(centroid - bbox.max), distance)" ("Clamp distance to half the radius of the cluster when
//    splitting is disabled"), and min and max distance are the same for a node.
//  - Blend (get_left_probability): "probability_max = max_left_importance /
//    total_max_importance", probability_min the same with the minimum importances or, when their
//    total is 0, "0.5f * (float(max_left_importance > 0) + float(max_right_importance == 0.0f))",
//    and "left_probability = 0.5f * (probability_max + probability_min)"; a total maximum of 0
//    returns false ("Both child nodes have zero importance"), so the descent ends with pmf 0.
//  - Distant lights (light_tree_node_importance, LIGHT_TREE_DISTANT): "point_to_centroid =
//    -bcone.axis; cos_theta_u = fast_cosf(bcone.theta_o + bcone.theta_e); distance = 1.0f;" and
//    no bounding-box test. The builder puts every distant light under one child of the root, the
//    local lights under the other (scene/light_tree.cpp LightTree::build: "All distant lights are
//    grouped to the right child").
//  - Descent and pmf (light_tree_sample, light_tree_pdf): the branch is chosen by rescaling one
//    uniform (sample_reservoir), the pmf multiplies left_prob or 1 - left_prob per level, and the
//    pmf of a light replays its bit trail from the root, bit 0 first ("go_left = (bit_trail & 1)
//    == 0; bit_trail >>= 1").
// Differences, written into 0004 D10: a leaf holds one light and its importance is the node
// importance (Cycles evaluates per-type emitter parameters at leaves, M4); no transmission flag and
// no volume segment (the importance takes p and n only); the root is a leaf when the scene has one
// light, and that leaf's own importance decides whether it is drawn; a cone with theta_o + theta_e
// equal to pi emits everywhere (see light_tree_cone_importance); importances are clamped finite and
// each child's branch probability is computed from its own ratios with a positive floor, so a
// subtree holding a light with energy is never drawn with probability 0 (light_tree_split).
//
// Ref: docs/decisions/0004-integrator.md D10 (importance rules, alias table as oracle), D9 (pmf 0
// for estimate 0); Conty Estevez and Kulla 2018, "Importance Sampling of Many Lights with Adaptive
// Tree Splitting" (section 4, Eq. 3 importance, Fig. 6 bounding cone); Cycles
// third_party/_cache/refs/cycles-ed69ef2c6bd3/src/kernel/light/tree.h (light_tree_importance,
// light_tree_node_importance, light_tree_cos_bound_subtended_angle, get_left_probability,
// sample_reservoir, light_tree_sample, light_tree_pdf) and src/util/math_base.h sin_from_cos
// (Apache-2.0); cross-check pbrt-v4 third_party/_cache/refs/pbrt-v4-092b23a47ad4/src/pbrt/
// lightsamplers.h BVHLightSampler::Sample / PMF (bit trail, pmf product; pbrt samples infinite
// lights outside the tree, D10 follows Cycles and keeps them inside) (Apache-2.0).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/vec.h"
#include "../portability.h"
#include "light.h"

// The stored layout does not depend on Real, so it lives outside the Real namespace: the host
// builder (compiled once) and both kernel builds share one definition.
namespace ar {

// Node flags.
inline constexpr uint32_t kLightTreeLeaf = 1u << 0;     // `light` is valid, `child` is not
inline constexpr uint32_t kLightTreeDistant = 1u << 1;  // every light below is infinite: no bbox

// One node: the bounds of every light below it (box, orientation cone, summed importance energy).
// An inner node has two children; a leaf holds one light.
struct LightTreeNode {
  float bbox_lo[3], bbox_hi[3];  // unused (0) in distant nodes
  float axis[3];                 // unit cone axis
  float theta_o, theta_e;        // normal-cone half-angle and emission spread around it
  float energy;                  // sum of the importance energies below
  uint32_t child[2];             // inner: left and right node indices
  uint32_t light;                // leaf: light index
  uint32_t flags;                // kLightTree* bits
};
static_assert(sizeof(LightTreeNode) == 64, "light tree node layout (24 + 12 + 12 + 16 B)");

// Per light index: the path from the root to its leaf, bit d = 1 for the right child at depth d.
// depth is kLightTreeAbsent for a light that is not in the tree (importance 0).
struct LightTreeEmitter {
  uint32_t bit_trail;
  uint32_t depth;
};
inline constexpr uint32_t kLightTreeAbsent = ~0u;
inline constexpr uint32_t kLightTreeMaxDepth = 32;  // bits in bit_trail

// Node 0 is the root; node_count 0 means no light of positive importance.
struct LightTreeView {
  const LightTreeNode* nodes;
  const LightTreeEmitter* emitters;  // one per light index
  uint32_t node_count, emitter_count;
};

}  // namespace ar

AR_NS_BEGIN

inline constexpr uint32_t kNoLight = ~0u;

// A drawn light and its selection probability; pmf 0 (light kNoLight) when nothing was drawn. The
// result of both samplers, the tree here and the mixture of light_sampler.h.
struct SampledLight {
  uint32_t light;
  Real pmf;
};

struct LightTreeImportance {
  Real max, min;
};

AR_FN Real light_tree_sin_from_cos(Real c) { return light_safe_sqrt(Real(1) - c * c); }

// cos and sin of a cone angle through cospi / sinpi: sinf / cosf carry a double-precision slow
// path that a GPU shade kernel may not contain (portability.h sinpi).
inline constexpr Real kLightTreeInvPi = Real(0.318309886183790671537767526745);
AR_FN Real light_tree_cos(Real a) { return ar::cospi(a * kLightTreeInvPi); }
AR_FN Real light_tree_sin(Real a) { return ar::sinpi(a * kLightTreeInvPi); }

AR_FN Real3 light_tree_f3(const float* v) { return Real3{Real(v[0]), Real(v[1]), Real(v[2])}; }

// Cycles light_tree_importance for a surface point, has_transmission false, outside a volume.
// to_c is the unit direction from p toward the cluster, cos_u the cosine of the angle the cluster
// subtends, dist the (clamped) distance.
AR_FN LightTreeImportance light_tree_cone_importance(Real3 n, Real3 to_c, Real cos_u, Real3 axis, Real theta_o,
                                                     Real theta_e, Real dist, Real energy) {
  LightTreeImportance out{Real(0), Real(0)};
  const Real sin_u = light_tree_sin_from_cos(cos_u);
  const Real cos_i = dot(to_c, n);
  const Real sin_i = light_tree_sin_from_cos(cos_i);
  // cos(max(theta_i - theta_u, 0)) and cos(min(theta_i + theta_u, pi)), clamped at 0.
  const Real cos_min_inc = cos_i >= cos_u ? Real(1) : cos_i * cos_u + sin_i * sin_u;
  if (cos_min_inc < Real(0)) return out;  // wholly behind the surface
  const Real cos_max_inc = ar::max(cos_i * cos_u - sin_i * sin_u, Real(0));
  Real cos_t, sin_t;
  if (axis.x == -to_c.x && axis.y == -to_c.y && axis.z == -to_c.z) {
    cos_t = Real(1);  // Cycles: the dot product of two equal unit vectors is not always 1
    sin_t = Real(0);
  } else {
    cos_t = dot(axis, -to_c);
    sin_t = light_tree_sin_from_cos(cos_t);
  }
  const Real cos_t_minus_u = cos_t * cos_u + sin_t * sin_u;
  const Real cos_o = light_tree_cos(theta_o), sin_o = light_tree_sin(theta_o);
  // cos(theta'), theta' = max(theta - theta_o - theta_u, 0), when theta' < theta_e.
  Real cos_min_out;
  if (cos_t >= cos_u || cos_t_minus_u >= cos_o) {
    cos_min_out = Real(1);
  } else if (theta_o + theta_e >= kLightPi || cos_t_minus_u > light_tree_cos(theta_o + theta_e)) {
    // >= where Cycles has >: a two-sided emitter (theta_o 0, theta_e pi) seen from straight behind
    // has cos_t_minus_u = -1 = cos(pi) once cos_u rounds to 1, and > would call it invisible.
    cos_min_out = cos_t_minus_u * cos_o + light_tree_sin_from_cos(cos_t_minus_u) * sin_o;
  } else {
    return out;  // outside every emission cone
  }
  const Real inv_d2 = Real(1) / (dist * dist);
  out.max = ar::abs(cos_min_inc * energy * cos_min_out * inv_d2);
  const Real cos_t_plus_u = cos_t * cos_u - sin_t * sin_u;
  if (theta_e - theta_o < Real(0) || cos_t < Real(0) || cos_u < Real(0) || cos_t_plus_u < light_tree_cos(theta_e - theta_o))
    return out;
  const Real cos_max_out = cos_t_plus_u * cos_o - light_tree_sin_from_cos(cos_t_plus_u) * sin_o;
  out.min = ar::abs(cos_max_inc * energy * cos_max_out * inv_d2);
  return out;
}

// Cycles light_tree_node_importance for a surface point (in_volume_segment false). p and n are
// passed explicitly, so a medium segment cannot be handed in by mistake.
AR_FN LightTreeImportance light_tree_importance(const LightTreeNode& node, Real3 p, Real3 n) {
  const Real3 axis = light_tree_f3(node.axis);
  const Real theta_o = Real(node.theta_o), theta_e = Real(node.theta_e);
  // Direction, subtended cosine and distance of the cluster, then one cone evaluation for both
  // kinds of node (one inlined copy in the kernels).
  Real3 to_c;
  Real cos_u, dist;
  if (node.flags & kLightTreeDistant) {
    to_c = -axis;
    cos_u = light_tree_cos(theta_o + theta_e);
    dist = Real(1);
  } else {
    const Real3 lo = light_tree_f3(node.bbox_lo), hi = light_tree_f3(node.bbox_hi);
    const Real3 c = (lo + hi) * Real(0.5);
    const Real3 ext = hi - c;
    const Real3 v = c - p;
    if (!(dot(n, v) + (ar::abs(n.x) * ar::abs(ext.x) + ar::abs(n.y) * ar::abs(ext.y) + ar::abs(n.z) * ar::abs(ext.z)) > Real(0)))
      return LightTreeImportance{Real(0), Real(0)};  // the box is wholly behind the surface
    const Real d = length(v);
    // At the centre itself any direction will do: p is inside the bounding sphere, so cos_u = -1
    // and the cone factors are 1 whatever the direction; dividing by d = 0 would give a NaN.
    to_c = d > Real(0) ? v / d : n;
    // light_tree_cos_bound_subtended_angle: -1 when p is inside the bounding sphere.
    const Real r2 = dot(ext, ext);
    cos_u = d * d <= r2 ? Real(-1) : light_safe_sqrt(Real(1) - r2 / (d * d));
    dist = ar::max(Real(0.5) * ar::sqrt(r2), d);
  }
  return light_tree_cone_importance(n, to_c, cos_u, axis, theta_o, theta_e, dist, Real(node.energy));
}

// Bounds of a branch probability (see light_tree_split): a quarter of the largest finite Real, so
// the sum of two importances stays finite, and the smallest normal Real, which a flush-to-zero GPU
// keeps.
inline constexpr Real kLightTreeImportanceCap =
    sizeof(Real) == 4 ? Real(float(0x1.fffffep+125)) : Real(double(0x1.fffffffffffffp+1021));
inline constexpr Real kLightTreeMinBranch = sizeof(Real) == 4 ? Real(float(0x1p-126)) : Real(double(0x1p-1022));

// A node importance made finite for the branch ratios. A point within float range of a point lamp's
// box (no extent) gets 1 / d^2 = inf, and an infinite sibling would leave every other subtree,
// the world under the root's right child included, with probability 0. A NaN is a malformed node:
// a debug build traps, a release build takes the cap, so its subtree keeps a positive probability.
AR_FN Real light_tree_finite(Real x) {
  AR_ASSERT(x == x);
  return x == x ? ar::min(x, kLightTreeImportanceCap) : kLightTreeImportanceCap;
}

// The two branch probabilities of an inner node.
struct LightTreeSplit {
  Real left, right;
};

// Cycles get_left_probability, false when both children have maximum importance 0. Each side's
// probability comes from its own ratios rather than as 1 minus the other's, so a child whose
// importance is far below its sibling's (the world beside a nearby point lamp) keeps a representable
// probability; the pair is normalised, and a child of positive maximum importance gets at least
// kLightTreeMinBranch, so every subtree holding a light with energy can be drawn.
AR_FN bool light_tree_split(const LightTreeView& v, const LightTreeNode& node, Real3 p, Real3 n, LightTreeSplit* out) {
  // One importance evaluation in a loop over the two children rather than two inlined copies: each
  // walk is inlined into the shade kernels, where the copies cost registers and compile time.
  LightTreeImportance l{Real(0), Real(0)}, r{Real(0), Real(0)};
#if defined(__CUDACC__)
#pragma unroll 1
#endif
  for (uint32_t c = 0; c < 2u; ++c) {
    const LightTreeImportance imp = light_tree_importance(v.nodes[node.child[c]], p, n);
    const LightTreeImportance f{light_tree_finite(imp.max), light_tree_finite(imp.min)};
    if (c == 0u) l = f;
    else r = f;
  }
  const Real max_sum = l.max + r.max;
  if (max_sum == Real(0)) return false;
  const Real min_sum = l.min + r.min;
  const Real l_min = min_sum > Real(0) ? l.min / min_sum : Real(0.5) * (Real(l.max > Real(0) ? 1 : 0) + Real(r.max == Real(0) ? 1 : 0));
  const Real r_min = min_sum > Real(0) ? r.min / min_sum : Real(0.5) * (Real(r.max > Real(0) ? 1 : 0) + Real(l.max == Real(0) ? 1 : 0));
  Real pl = Real(0.5) * (l.max / max_sum + l_min), pr = Real(0.5) * (r.max / max_sum + r_min);
  const Real sum = pl + pr;
  pl = pl / sum, pr = pr / sum;
  if (l.max > Real(0)) pl = ar::max(pl, kLightTreeMinBranch);
  if (r.max > Real(0)) pr = ar::max(pr, kLightTreeMinBranch);
  AR_ASSERT(pl >= Real(0) && pr >= Real(0) && pl + pr > Real(0));
  out->left = pl, out->right = pr;
  return true;
}

// The one descent behind light_tree_sample and light_tree_pmf (and the mixture of light_sampler.h),
// so a drawn light's pmf and the pmf of that light are one computation (0004 D7). From the root, an
// inner node splits by light_tree_split; the branch is the uniform's (Cycles sample_reservoir with
// the left child selected: the right child replaces it when r <= its probability, and r is rescaled
// to stay uniform) or, with `replay`, bit `depth` of `trail` (Cycles light_tree_pdf). The pmf is the
// product of the branch probabilities taken. No light (kNoLight, pmf 0) when the tree is empty, a
// node on the way has both children at importance 0, or a replayed branch has probability 0. A
// loop with one node index: no recursion, no stack.
AR_FN SampledLight light_tree_walk(const LightTreeView& v, Real3 p, Real3 n, Real u, bool replay, uint32_t trail) {
  SampledLight s{kNoLight, Real(0)};
  if (v.node_count == 0u) return s;
  Real pmf = Real(1);
  uint32_t idx = 0u;
  Real r = ar::min(ar::max(u, Real(0)), Real(1));
  for (uint32_t depth = 0u; depth <= kLightTreeMaxDepth; ++depth) {
    const LightTreeNode& node = v.nodes[idx];
    if (node.flags & kLightTreeLeaf) {
      // A lone root leaf was never weighed against a sibling: its own importance decides.
      if (idx == 0u && !(light_tree_importance(node, p, n).max > Real(0))) return s;
      s.light = node.light;
      s.pmf = pmf;
      return s;
    }
    LightTreeSplit sp;
    if (!light_tree_split(v, node, p, n, &sp)) return s;
    bool right;
    if (replay) {
      right = depth < 32u && ((trail >> depth) & 1u) != 0u;
    } else {
      right = sp.right > Real(0) && r <= sp.right;
      if (sp.right > Real(0)) r = right ? r / sp.right : (r - sp.right) / (Real(1) - sp.right);
      r = ar::min(ar::max(r, Real(0)), Real(1));
    }
    pmf *= right ? sp.right : sp.left;
    if (!(pmf > Real(0))) return s;
    idx = node.child[right ? 1 : 0];
  }
  AR_ASSERT(false);  // deeper than the bit trail: the builder refuses such a tree
  return s;
}

// One light drawn by descending from the root with the uniform u in [0, 1); pmf accumulated on the
// way.
AR_FN SampledLight light_tree_sample(const LightTreeView& v, Real3 p, Real3 n, Real u) {
  return light_tree_walk(v, p, n, u, false, 0u);
}

// The probability that light_tree_sample(v, p, n, .) returns `light`: its bit trail replayed
// from the root. 0 for a light not in the tree, and as soon as a node on the way gives 0.
AR_FN Real light_tree_pmf(const LightTreeView& v, Real3 p, Real3 n, uint32_t light) {
  if (light >= v.emitter_count || v.node_count == 0u) return Real(0);
  const LightTreeEmitter e = v.emitters[light];
  if (e.depth == kLightTreeAbsent || e.depth > kLightTreeMaxDepth) return Real(0);
  const SampledLight s = light_tree_walk(v, p, n, Real(0), true, e.bit_trail);
  AR_ASSERT(s.light == kNoLight || s.light == light);
  return s.light == light ? s.pmf : Real(0);
}

AR_NS_END
