// SPDX-License-Identifier: Apache-2.0
#include "graph.h"

#include "../../kernel/material/constant_block.h"
#include "../../kernel/vm/context.h"
#include "../../kernel/vm/ops/store_slot.h"

namespace astroray::host {
namespace {

using ar::Op;

std::string at(size_t i, const char* what) { return "graph node " + std::to_string(i) + ": " + what; }

// Fixed parameter counts per op: f32 pool count, i32 pool count. False for ops a graph may not
// hold (END, JMP, UNSUPPORTED and the test hooks are the compiler's, never the addon's).
bool param_counts(Op op, uint32_t* nf, uint32_t* ni) {
  *nf = *ni = 0;
  switch (op) {
    case Op::CONST_F32: *nf = 1; return true;
    case Op::CONST_F32X3: *nf = 3; return true;
    case Op::LOAD_CTX_UV:
    case Op::TEX_IMAGE:
    case Op::RGB_LIFT:
    case Op::STORE_SLOT: *ni = 1; return true;
    case Op::LOAD_CTX_P:
    case Op::MUL_F32:
    case Op::MUL_F32X3:
    case Op::MUL_F32X3_F32:
    case Op::ADD_F32X3:
    case Op::EMIT_LE: return true;
    default: return false;
  }
}

VType input_type(Op op, int i) {
  switch (op) {
    case Op::MUL_F32: return VType::F32;
    case Op::MUL_F32X3:
    case Op::ADD_F32X3: return VType::F32X3;
    case Op::MUL_F32X3_F32: return i == 0 ? VType::F32X3 : VType::F32;
    case Op::TEX_IMAGE:
    case Op::RGB_LIFT: return VType::F32X3;
    case Op::EMIT_LE: return i == 0 ? VType::F32X3 : VType::F32;
    default: return VType::NONE;
  }
}

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
    case Op::MUL_F32X3:
    case Op::MUL_F32X3_F32:
    case Op::ADD_F32X3:
    case Op::EMIT_LE: return 2;
    case Op::TEX_IMAGE:
    case Op::RGB_LIFT:
    case Op::STORE_SLOT: return 1;
    default: return 0;
  }
}

VType op_result_type(Op op) {
  switch (op) {
    case Op::CONST_F32:
    case Op::MUL_F32: return VType::F32;
    case Op::CONST_F32X3:
    case Op::LOAD_CTX_UV:
    case Op::LOAD_CTX_P:
    case Op::MUL_F32X3:
    case Op::MUL_F32X3_F32:
    case Op::ADD_F32X3:
    case Op::TEX_IMAGE: return VType::F32X3;
    case Op::RGB_LIFT: return VType::SPEC;
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
    if (!param_counts(o.op, &nf, &ni)) return *err = at(i, "op is not allowed in a graph"), false;
    if (n.f32_count != nf || n.i32_count != ni) return *err = at(i, "wrong parameter count for its op"), false;
    if (uint64_t(n.f32_first) + nf > d.f32_count || uint64_t(n.i32_first) + ni > d.i32_count)
      return *err = at(i, "parameter range outside its pool"), false;
    if (n.string_count != 0) return *err = at(i, "string parameters are not used by M1a ops"), false;
    o.f32_first = n.f32_first;
    o.i32_first = n.i32_first;
    o.n_inputs = op_input_count(o.op);
    o.out = op_result_type(o.op);
    if (o.op == Op::LOAD_CTX_UV && uint32_t(g->i32[o.i32_first]) >= ar::kMaxUvSets)
      return *err = at(i, "UV set index out of range"), false;
    if (o.op == Op::RGB_LIFT && uint32_t(g->i32[o.i32_first]) > uint32_t(ar::SpectrumSemantic::ILLUMINANT))
      return *err = at(i, "RGB_LIFT semantic must be REFLECTANCE, UNBOUNDED or ILLUMINANT"), false;
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
    if (to.op == Op::STORE_SLOT) {
      const uint32_t slot = uint32_t(g->i32[to.i32_first]);
      if (slot == uint32_t(ar::RecordSlot::specular_ior) && from.out != VType::F32)
        return *err = at(l.to_node, "specular_ior takes a scalar, not a colour (0005 item 10)"), false;
      if (!store_accepts(slot, from.out))
        return *err = at(l.to_node, "type error: this value cannot be stored in that slot"), false;
    } else if (from.out != input_type(to.op, int(l.to_socket))) {
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
