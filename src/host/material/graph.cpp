// SPDX-License-Identifier: Apache-2.0
#include "graph.h"

#include <cmath>
#include <limits>

#include "../../kernel/material/constant_block.h"
#include "../../kernel/material/slot_resolve.h"
#include "../../kernel/vm/context.h"
#include "../../kernel/vm/ops/store_slot.h"
#include "../../kernel/vm/ops/tex_image.h"
#include "../../kernel/vm/ops/volume_coeffs.h"

namespace astroray::host {
namespace {

using ar::Op;

std::string at(size_t i, const char* what) { return "graph node " + std::to_string(i) + ": " + what; }

}  // namespace

// Fixed parameter counts per op: f32 pool count, i32 pool count. False for ops a graph may not
// hold (END, JMP, UNSUPPORTED and the test hooks are the compiler's, never the addon's) and for
// SPECTRUM_TABLE, whose f32 count varies with its point count (parse_graph checks it).
bool op_param_counts(Op op, uint32_t* nf, uint32_t* ni) {
  *nf = *ni = 0;
  switch (op) {
    case Op::CONST_F32: *nf = 1; return true;
    case Op::CONST_F32X3:
    case Op::CAUCHY_ABBE: *nf = 3; return true;  // CAUCHY_ABBE: n_d, V_d, dispersion scale
    case Op::BLACKBODY: *nf = 2; return true;     // temperature in K, scale
    case Op::LOAD_CTX_UV:
    case Op::RGB_LIFT:
    case Op::STORE_SLOT:
    case Op::CHANNEL_F32X3: *ni = 1; return true;
    case Op::TEX_IMAGE: *ni = 2; return true;  // image ar_key (low 32 bits), mode bits (tex_image.h)
    case Op::VOLUME_COEFFS: *nf = 1; *ni = 1; return true;  // HG anisotropy g; VolumeTerm (volume_coeffs.h)
    case Op::BRANCH_MIX: *ni = 3; return true;  // tag, first subtree, second subtree (graph.h)
    case Op::LOAD_CTX_P:
    case Op::MUL_F32:
    case Op::ADD_F32:
    case Op::SUB_F32:
    case Op::MUL_F32X3:
    case Op::MUL_F32X3_F32:
    case Op::ADD_F32X3:
    case Op::COLOR_TO_F32:
    case Op::EMIT_LE: return true;
    default: return false;
  }
}

uint32_t op_optional_i32(Op op) { return op == Op::STORE_SLOT || op == Op::EMIT_LE ? 1u : 0u; }

VType op_input_type(Op op, int i) {
  switch (op) {
    case Op::MUL_F32:
    case Op::ADD_F32:
    case Op::SUB_F32:
    case Op::BRANCH_MIX: return VType::F32;
    case Op::MUL_F32X3:
    case Op::ADD_F32X3: return VType::F32X3;
    case Op::MUL_F32X3_F32: return i == 0 ? VType::F32X3 : VType::F32;
    case Op::TEX_IMAGE:
    case Op::RGB_LIFT:
    case Op::COLOR_TO_F32:
    case Op::CHANNEL_F32X3: return VType::F32X3;
    case Op::EMIT_LE:
    case Op::VOLUME_COEFFS: return i == 0 ? VType::F32X3 : VType::F32;  // colour, density
    default: return VType::NONE;
  }
}

namespace {

// What a STORE_SLOT of this slot accepts: its natural form (one f32 for scalars and IOR, RGB for
// colours, f32x3 for frames) or an already lifted spectrum for the nine spectral slots. A scalar
// into a colour slot is a grey constant; the compiler rejects it when it is not constant.
bool store_accepts(uint32_t slot, VType t) {
  const uint32_t first_spec = uint32_t(ar::RecordSlot::base_color);
  const uint32_t first_frame = uint32_t(ar::RecordSlot::geometry_normal);
  if (slot < first_spec) return t == VType::F32;
  if (slot == uint32_t(ar::RecordSlot::specular_ior)) return t == VType::F32;  // IOR takes no RGB or spectrum (0005 item 10)
  if (slot < first_frame) return t == VType::F32X3 || t == VType::SPEC || t == VType::F32;
  return t == VType::F32X3;
}

}  // namespace

int vtype_width(VType t) {
  switch (t) {
    case VType::F32: return 1;
    case VType::F32X3: return 3;
    case VType::SPEC: return 4;
    default: return 0;
  }
}

int op_input_count(Op op) {
  switch (op) {
    case Op::MUL_F32:
    case Op::ADD_F32:
    case Op::SUB_F32:
    case Op::MUL_F32X3:
    case Op::MUL_F32X3_F32:
    case Op::ADD_F32X3:
    case Op::EMIT_LE:
    case Op::VOLUME_COEFFS: return 2;
    case Op::TEX_IMAGE:
    case Op::RGB_LIFT:
    case Op::STORE_SLOT:
    case Op::CHANNEL_F32X3:
    case Op::COLOR_TO_F32:
    case Op::BRANCH_MIX: return 1;
    default: return 0;
  }
}

VType op_result_type(Op op) {
  switch (op) {
    case Op::CONST_F32:
    case Op::MUL_F32:
    case Op::ADD_F32:
    case Op::SUB_F32:
    case Op::COLOR_TO_F32:
    case Op::CHANNEL_F32X3: return VType::F32;
    case Op::CONST_F32X3:
    case Op::LOAD_CTX_UV:
    case Op::LOAD_CTX_P:
    case Op::MUL_F32X3:
    case Op::MUL_F32X3_F32:
    case Op::ADD_F32X3:
    case Op::TEX_IMAGE: return VType::F32X3;
    case Op::RGB_LIFT:
    case Op::BLACKBODY:
    case Op::CAUCHY_ABBE:
    case Op::SPECTRUM_TABLE: return VType::SPEC;
    default: return VType::NONE;
  }
}

bool parse_graph(const ar_graph_desc& d, Graph* g, std::string* err) {
  g->nodes.assign(d.node_count, GraphNode{});
  g->f32.assign(d.params_f32, d.params_f32 + d.f32_count);
  g->i32.assign(d.params_i32, d.params_i32 + d.i32_count);
  for (uint32_t i = 0; i < d.node_count; ++i) {
    const ar_graph_node& n = d.nodes[i];
    GraphNode& o = g->nodes[i];
    if (n.op >= uint32_t(Op::COUNT_)) return *err = at(i, "unknown op"), false;
    o.op = Op(n.op);
    uint32_t nf, ni;
    if (o.op == Op::SPECTRUM_TABLE) {
      // A scale, then n (wavelength, value) pairs: the point count is the pool count, bounded here.
      nf = n.f32_count, ni = 0;
      if (nf < 5u || nf % 2u == 0u || (nf - 1u) / 2u > ar::kTableMaxPoints)
        return *err = at(i, "SPECTRUM_TABLE needs a scale and 2 to 4096 wavelength and value pairs"), false;
    } else if (!op_param_counts(o.op, &nf, &ni)) {
      return *err = at(i, "op is not allowed in a graph"), false;
    }
    const bool with_child = op_optional_i32(o.op) != 0u && n.i32_count == ni + op_optional_i32(o.op);
    if (n.f32_count != nf || (n.i32_count != ni && !with_child)) return *err = at(i, "wrong parameter count for its op"), false;
    if (uint64_t(n.f32_first) + nf > d.f32_count || uint64_t(n.i32_first) + n.i32_count > d.i32_count)
      return *err = at(i, "parameter range outside its pool"), false;
    if (n.string_count != 0) return *err = at(i, "string parameters are not used by M1a ops"), false;
    o.f32_first = n.f32_first;
    o.f32_count = n.f32_count;
    o.i32_first = n.i32_first;
    o.n_inputs = op_input_count(o.op);
    o.out = op_result_type(o.op);
    if (with_child && (o.child = g->i32[o.i32_first + ni]) < 0) return *err = at(i, "child id must not be negative"), false;
    if (o.op == Op::BRANCH_MIX && g->i32[o.i32_first] < 0) return *err = at(i, "Mix Shader tag must not be negative"), false;
    if (o.op == Op::LOAD_CTX_UV && uint32_t(g->i32[o.i32_first]) >= ar::kMaxUvSets)
      return *err = at(i, "UV set index out of range"), false;
    if (o.op == Op::RGB_LIFT && uint32_t(g->i32[o.i32_first]) > uint32_t(ar::SpectrumSemantic::ILLUMINANT))
      return *err = at(i, "RGB_LIFT semantic must be REFLECTANCE, UNBOUNDED or ILLUMINANT"), false;
    if (o.op == Op::TEX_IMAGE && (uint32_t(g->i32[o.i32_first + 1]) & ~ar::kTexModeMask) != 0u)
      return *err = at(i, "TEX_IMAGE mode must be CLOSEST (0) or LINEAR (1) plus REPEAT (0) or EXTEND (2)"), false;
    // Constants only: a per-point temperature or n_d is M2 dispersion work and has no socket here.
    const float* pf = g->f32.data() + o.f32_first;
    auto finite_positive = [](float v) { return std::isfinite(v) && v > 0.0f; };
    // planck.h documents float-safe Planck only for T in [3 K, 1e9 K]; outside it the radiance underflows
    // to 0 or the host's 1 / Y(Planck) overflows, and the kernel would return NaN or inf silently.
    constexpr float kPlanckTMin = 3.0f, kPlanckTMax = 1.0e9f;
    if (o.op == Op::BLACKBODY && !(finite_positive(pf[0]) && pf[0] >= kPlanckTMin && pf[0] <= kPlanckTMax &&
                                    std::isfinite(pf[1]) && pf[1] >= 0.0f))
      return *err = at(i, "BLACKBODY needs a temperature in [3, 1e9] K (planck.h float domain) and a finite non-negative scale"), false;
    if (o.op == Op::CAUCHY_ABBE &&
        !(finite_positive(pf[0]) && finite_positive(pf[1]) && std::isfinite(pf[2]) && pf[2] >= 0.0f))
      return *err = at(i, "CAUCHY_ABBE needs a finite positive n_d and Abbe number and a finite non-negative dispersion scale"), false;
    if (o.op == Op::SPECTRUM_TABLE) {
      // Ref: 0005 item 9 (a piecewise-linear table times a scale). Strictly increasing wavelengths keep
      // the lookup's search and slopes well defined; a negative value has no reflectance or emission meaning.
      const uint32_t np = (n.f32_count - 1u) / 2u;
      if (!(std::isfinite(pf[0]) && pf[0] >= 0.0f))
        return *err = at(i, "SPECTRUM_TABLE needs a finite non-negative scale"), false;
      for (uint32_t k = 0; k < np; ++k) {
        const float wl = pf[1 + 2 * k], val = pf[2 + 2 * k];
        if (!std::isfinite(wl) || (k > 0 && !(wl > pf[1 + 2 * (k - 1)])))
          return *err = at(i, "SPECTRUM_TABLE wavelengths must be finite and strictly increasing"), false;
        if (!(std::isfinite(val) && val >= 0.0f))
          return *err = at(i, "SPECTRUM_TABLE values must be finite and not negative"), false;
        // The kernel multiplies in float: a product past FLT_MAX would resolve to inf (as the Blackbody check).
        if (!(double(pf[0]) * double(val) <= double(std::numeric_limits<float>::max())))
          return *err = at(i, "SPECTRUM_TABLE scale times a value overflows a float"), false;
      }
    }
    if (o.op == Op::CHANNEL_F32X3 && uint32_t(g->i32[o.i32_first]) > 2u)
      return *err = at(i, "CHANNEL_F32X3 channel must be 0, 1 or 2"), false;
    // VOLUME_COEFFS: one Volume Absorption or Volume Scatter term (its colour and density are the
    // inputs); the anisotropy only means something for a scatter term.
    if (o.op == Op::VOLUME_COEFFS && uint32_t(g->i32[o.i32_first]) > uint32_t(ar::VolumeTerm::SCATTER))
      return *err = at(i, "VOLUME_COEFFS term must be ABSORPTION (0) or SCATTER (1)"), false;
    if (o.op == Op::VOLUME_COEFFS && !std::isfinite(pf[0]))
      return *err = at(i, "VOLUME_COEFFS anisotropy must be finite"), false;
    if (o.op == Op::STORE_SLOT && uint32_t(g->i32[o.i32_first]) >= uint32_t(ar::RecordSlot::COUNT_))
      return *err = at(i, "unknown record slot"), false;
  }
  for (uint32_t k = 0; k < d.link_count; ++k) {
    const ar_graph_link& l = d.links[k];
    if (l.from_node >= d.node_count || l.to_node >= d.node_count) return *err = "link names a missing node", false;
    GraphNode& to = g->nodes[l.to_node];
    const GraphNode& from = g->nodes[l.from_node];
    if (l.from_socket != 0 || from.out == VType::NONE) return *err = at(l.from_node, "has no such output socket"), false;
    if (l.to_socket >= uint32_t(to.n_inputs)) return *err = at(l.to_node, "has no such input socket"), false;
    if (to.input[l.to_socket] >= 0) return *err = at(l.to_node, "input linked twice"), false;
    // A BLACKBODY value is an emission colour and a CAUCHY_ABBE value is an IOR (0005 item 9): each
    // has exactly one consumer shape, everything else is refused here with both nodes named.
    const std::string sink = to.op == Op::STORE_SLOT ? " (it feeds record slot " + std::to_string(g->i32[to.i32_first]) + ")" : "";
    if (from.op == Op::BLACKBODY && !(to.op == Op::EMIT_LE && l.to_socket == 0))
      return *err = at(l.from_node, "BLACKBODY only feeds the colour of an emission (EMIT_LE)") + sink, false;
    if (from.op == Op::CAUCHY_ABBE &&
        !(to.op == Op::STORE_SLOT && uint32_t(g->i32[to.i32_first]) == uint32_t(ar::RecordSlot::specular_ior)))
      return *err = at(l.from_node, "CAUCHY_ABBE only feeds the specular_ior slot") + sink, false;
    if (from.op == Op::SPECTRUM_TABLE) {
      // A table is a reflectance or unbounded colour (every colour slot but the IOR) or the colour of an emission.
      const uint32_t slot = to.op == Op::STORE_SLOT ? uint32_t(g->i32[to.i32_first]) : 0u;
      const bool colour_slot = to.op == Op::STORE_SLOT && slot >= uint32_t(ar::RecordSlot::base_color) &&
                               slot < uint32_t(ar::RecordSlot::geometry_normal) && slot != uint32_t(ar::RecordSlot::specular_ior);
      if (!colour_slot && !(to.op == Op::EMIT_LE && l.to_socket == 0))
        return *err = at(l.from_node, "SPECTRUM_TABLE only feeds a colour slot or the colour of an emission (EMIT_LE)") + sink, false;
    }
    if (from.op == Op::BLACKBODY || from.op == Op::CAUCHY_ABBE || from.op == Op::SPECTRUM_TABLE) {
      // accepted above; the usual type rules do not apply to these spec values
    } else if (to.op == Op::STORE_SLOT) {
      const uint32_t slot = uint32_t(g->i32[to.i32_first]);
      if (slot == uint32_t(ar::RecordSlot::specular_ior) && from.out != VType::F32)
        return *err = at(l.to_node, "specular_ior takes a scalar, not a colour (0005 item 10)"), false;
      if (!store_accepts(slot, from.out))
        return *err = at(l.to_node, "type error: this value cannot be stored in that slot"), false;
    } else if (from.out != op_input_type(to.op, int(l.to_socket))) {
      return *err = at(l.to_node, "type error: input socket wants another type"), false;
    }
    to.input[l.to_socket] = int(l.from_node);
  }
  std::vector<uint32_t> indeg(d.node_count, 0);
  std::vector<std::vector<uint32_t>> users(d.node_count);
  for (uint32_t i = 0; i < d.node_count; ++i)
    for (int s = 0; s < g->nodes[i].n_inputs; ++s) {
      if (g->nodes[i].input[s] < 0) return *err = at(i, "unconnected input"), false;
      users[uint32_t(g->nodes[i].input[s])].push_back(i);
      ++indeg[i];
    }
  // Kahn: a node left with unmet inputs after the walk lies on or behind a cycle.
  std::vector<uint32_t> ready;
  for (uint32_t i = 0; i < d.node_count; ++i)
    if (indeg[i] == 0) ready.push_back(i);
  uint32_t seen = 0;
  while (!ready.empty()) {
    const uint32_t n = ready.back();
    ready.pop_back();
    ++seen;
    for (uint32_t u : users[n])
      if (--indeg[u] == 0) ready.push_back(u);
  }
  if (seen != d.node_count) return *err = "graph has a cycle", false;
  return true;
}

}  // namespace astroray::host
