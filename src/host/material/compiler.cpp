// SPDX-License-Identifier: Apache-2.0
#include "compiler.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "../../kernel/material/slot_resolve.h"
#include "../../kernel/vm/ops/add_f32x3.h"
#include "../../kernel/vm/ops/const_f32.h"
#include "../../kernel/vm/ops/mul_f32.h"
#include "../../kernel/vm/ops/mul_f32x3.h"
#include "../../kernel/vm/ops/mul_f32x3_f32.h"
#include "../../kernel/vm/ops/store_slot.h"
#include "graph.h"
#include "regalloc.h"

namespace astroray::host {
namespace {

using ar::Op;
using Vec3 = std::array<float, 3>;

// The record's scalars that the constant block also holds, in record order. closure_weight is
// the selection's (item 38) and has no block slot.
#define AR_BLOCK_SCALARS(X)                                                                                   \
  X(base_weight) X(base_diffuse_roughness) X(base_metalness) X(specular_weight) X(specular_roughness)         \
  X(specular_roughness_anisotropy) X(transmission_weight) X(transmission_depth)                               \
  X(transmission_scatter_anisotropy) X(subsurface_weight) X(subsurface_radius)                                \
  X(subsurface_scatter_anisotropy) X(fuzz_weight) X(fuzz_roughness) X(coat_weight) X(coat_roughness)          \
  X(coat_roughness_anisotropy) X(coat_ior) X(coat_darkening) X(thin_film_weight) X(thin_film_thickness)       \
  X(thin_film_ior) X(geometry_opacity)

struct Work {
  Graph g;
  const ar::Rgb2SpecTable* table;
  std::vector<int> topo;  // stable topological order: smallest ready index first
  std::vector<char> isc;  // value is a compile-time constant
  std::vector<Vec3> cv;
};

std::vector<int> stable_topo(const Graph& g) {
  const size_t n = g.nodes.size();
  std::vector<char> done(n, 0);
  std::vector<int> order;
  while (order.size() < n)
    for (size_t i = 0; i < n; ++i) {
      if (done[i]) continue;
      bool ready = true;
      for (int s = 0; s < g.nodes[i].n_inputs; ++s) ready = ready && done[size_t(g.nodes[i].input[s])];
      if (!ready) continue;
      done[i] = 1;
      order.push_back(int(i));
      break;
    }
  return order;
}

// Folds node i when every input is constant and the op reads neither context nor texture
// (0005 item 35: LOAD_CTX_*, TEX_IMAGE and RGB_LIFT, which depends on the path's wavelengths, do
// not fold). The arithmetic is the CPU build of the interpreter's own functions.
bool fold(const Work& w, int i, Vec3* out) {
  const GraphNode& n = w.g.nodes[size_t(i)];
  *out = {0.0f, 0.0f, 0.0f};
  const float* f = w.g.f32.data() + n.f32_first;
  if (n.op == Op::CONST_F32) return (*out)[0] = f[0], true;
  if (n.op == Op::CONST_F32X3) return *out = {f[0], f[1], f[2]}, true;
  for (int s = 0; s < n.n_inputs; ++s)
    if (!w.isc[size_t(n.input[s])]) return false;
  const Vec3& a = n.n_inputs > 0 ? w.cv[size_t(n.input[0])] : *out;
  const Vec3& b = n.n_inputs > 1 ? w.cv[size_t(n.input[1])] : *out;
  switch (n.op) {
    case Op::MUL_F32: (*out)[0] = ar::op_mul_f32(a[0], b[0]); return true;
    case Op::MUL_F32X3: ar::op_mul_f32x3(a.data(), b.data(), out->data()); return true;
    case Op::MUL_F32X3_F32: ar::op_mul_f32x3_f32(a.data(), b[0], out->data()); return true;
    case Op::ADD_F32X3: ar::op_add_f32x3(a.data(), b.data(), out->data()); return true;
    default: return false;
  }
}

// Writes a constant into the block slot of RecordSlot `slot`, with the slot's semantic (item 10).
bool store_const(ar::ConstantBlock* b, const ar::Rgb2SpecTable& t, uint32_t slot, VType type, const Vec3& v,
                 uint32_t* flags, std::string* err) {
  using RS = ar::RecordSlot;
  if (slot < uint32_t(RS::base_color)) {
    switch (RS(slot)) {
#define AR_X(n) \
  case RS::n: b->n = v[0]; return true;
      AR_BLOCK_SCALARS(AR_X)
#undef AR_X
      default: return *err = "closure_weight is set by the selection, not stored by a graph", false;
    }
  }
  if (slot >= uint32_t(RS::geometry_normal)) {
    float* dst = nullptr;
    switch (RS(slot)) {
      case RS::geometry_normal: dst = b->geometry_normal; break;
      case RS::geometry_coat_normal: dst = b->geometry_coat_normal; break;
      case RS::geometry_tangent: dst = b->geometry_tangent; break;
      default: dst = b->geometry_coat_tangent; break;
    }
    for (int i = 0; i < 3; ++i) dst[i] = v[size_t(i)];
    return true;
  }
  const uint32_t idx = slot - uint32_t(RS::base_color);  // RecordSlot's spectral order is Slot's
  const ar::SpectrumSemantic sem = ar::kSlotSemantic[idx];
  ar::SpectrumRef& ref = ar::slot_ref(*b, ar::Slot(idx));
  if (type == VType::F32) {
    float x = v[0];
    if (sem != ar::SpectrumSemantic::IOR) {
      if (!(x >= 0.0f)) x = 0.0f, *flags |= ar::kRgbNegativeClamped;
      if (sem == ar::SpectrumSemantic::REFLECTANCE && x > 1.0f) x = 1.0f, *flags |= ar::kRgbAboveOneClamped;
    }
    ref = ar::make_const_ref(sem, ar::Real(x));
    return true;
  }
  const ar::Rgb rgb{{ar::Real(v[0]), ar::Real(v[1]), ar::Real(v[2])}};
  if (!ar::make_rgb_ref(t, rgb, sem, &ref, flags)) return *err = "specular_ior takes a scalar, not a colour (0005 item 10)", false;
  return true;
}

uint32_t word(Op op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
  return uint32_t(op) | (a << 8) | (b << 16) | (c << 24);
}

// One entry's code: the marked nodes in order, registers allocated, END appended. `first` lists
// the nodes that must come first (the emission segment, item 38) and stay live for what follows.
bool build_entry(const Work& w, const std::vector<char>& marked, const std::vector<char>& first,
                 std::vector<uint32_t>* code, std::string* err) {
  const size_t n = w.g.nodes.size();
  std::vector<int> seq;
  for (int pass = 0; pass < 2; ++pass)
    for (int i : w.topo)
      if (marked[size_t(i)] && bool(first[size_t(i)]) == (pass == 0)) seq.push_back(i);
  std::vector<int> pos(n, -1), last(n, -1), group(n, -1);
  for (size_t k = 0; k < seq.size(); ++k) pos[size_t(seq[k])] = int(k);
  for (int i : seq) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    if (w.isc[size_t(i)]) continue;  // materialised as a constant: reads nothing
    for (int s = 0; s < nd.n_inputs; ++s) last[size_t(nd.input[s])] = std::max(last[size_t(nd.input[s])], pos[size_t(i)]);
  }
  // EMIT_LE reads colour in r[a..a+2] and strength in r[a+3]: the two values share one 4-wide
  // range, colour first, so no copy instructions are needed.
  std::vector<LiveRange> ranges;
  std::vector<int> range_node, range_group_emit;  // node of a plain range, or -1 and the EMIT_LE
  std::vector<int> emits;
  for (int i : seq) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    if (nd.op != Op::EMIT_LE) continue;
    for (int s = 0; s < 2; ++s) {
      if (group[size_t(nd.input[s])] >= 0)
        return *err = "an emission colour or strength feeds two EMIT_LE nodes; unsupported in M1a", false;
      group[size_t(nd.input[s])] = i;
    }
    emits.push_back(i);
  }
  for (int i : seq) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    if (nd.out == VType::NONE || group[size_t(i)] >= 0) continue;
    ranges.push_back({pos[size_t(i)], std::max(last[size_t(i)], pos[size_t(i)]), vtype_width(nd.out)});
    range_node.push_back(i);
    range_group_emit.push_back(-1);
  }
  for (int e : emits) {
    const GraphNode& nd = w.g.nodes[size_t(e)];
    const int c = nd.input[0], s = nd.input[1];
    ranges.push_back({std::min(pos[size_t(c)], pos[size_t(s)]), std::max(last[size_t(c)], last[size_t(s)]), 4});
    range_node.push_back(-1);
    range_group_emit.push_back(e);
  }
  std::vector<int> regs;
  if (!allocate_registers(ranges, &regs, err)) return false;
  std::vector<int> reg(n, -1);
  for (size_t r = 0; r < ranges.size(); ++r) {
    if (range_node[r] >= 0) {
      reg[size_t(range_node[r])] = regs[r];
    } else {
      const GraphNode& nd = w.g.nodes[size_t(range_group_emit[r])];
      reg[size_t(nd.input[0])] = regs[r];
      reg[size_t(nd.input[1])] = regs[r] + 3;
    }
  }
  for (int i : seq) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    const uint32_t a = uint32_t(reg[size_t(i)]);
    auto in = [&](int s) { return uint32_t(reg[size_t(nd.input[s])]); };
    auto bits = [](float f) { return ar::bit_cast<uint32_t>(f); };
    if (w.isc[size_t(i)]) {
      const Vec3& v = w.cv[size_t(i)];
      if (nd.out == VType::F32) {
        code->push_back(word(Op::CONST_F32, a));
        code->push_back(bits(v[0]));
      } else {
        code->push_back(word(Op::CONST_F32X3, a));
        for (float f : v) code->push_back(bits(f));
      }
      continue;
    }
    switch (nd.op) {
      case Op::LOAD_CTX_UV: code->push_back(word(nd.op, a, uint32_t(w.g.i32[nd.i32_first]))); break;
      case Op::LOAD_CTX_P: code->push_back(word(nd.op, a)); break;
      case Op::MUL_F32:
      case Op::MUL_F32X3:
      case Op::MUL_F32X3_F32:
      case Op::ADD_F32X3: code->push_back(word(nd.op, a, in(0), in(1))); break;
      case Op::TEX_IMAGE:
        code->push_back(word(nd.op, a, in(0)));
        code->push_back(uint32_t(w.g.i32[nd.i32_first]));
        break;
      case Op::RGB_LIFT: code->push_back(word(nd.op, a, in(0), uint32_t(w.g.i32[nd.i32_first]))); break;
      case Op::STORE_SLOT: {
        const bool spec = w.g.nodes[size_t(nd.input[0])].out == VType::SPEC;
        code->push_back(word(nd.op, in(0)));
        code->push_back(uint32_t(w.g.i32[nd.i32_first]) | (spec ? ar::kStoreSlotSpecSource : 0u));
        break;
      }
      case Op::EMIT_LE: code->push_back(word(nd.op, in(0))); break;
      default: return *err = "internal: op has no encoding", false;
    }
  }
  code->push_back(word(Op::END));
  return true;
}

}  // namespace

CompileResult compile_surface(const ar_graph_desc& desc, const ar::Rgb2SpecTable& table) {
  CompileResult res;
  Work w;
  w.table = &table;
  if (!parse_graph(desc, &w.g, &res.error)) return res;
  const size_t n = w.g.nodes.size();
  w.topo = stable_topo(w.g);
  w.isc.assign(n, 0);
  w.cv.assign(n, Vec3{0.0f, 0.0f, 0.0f});
  for (int i : w.topo) w.isc[size_t(i)] = fold(w, i, &w.cv[size_t(i)]);

  res.block = ar::default_constant_block(table);
  int emit_count = 0;
  for (const GraphNode& nd : w.g.nodes) emit_count += nd.op == Op::EMIT_LE;
  std::vector<char> written(size_t(ar::RecordSlot::COUNT_), 0), is_root(n, 0), is_emit_root(n, 0);
  for (int i : w.topo) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    if (nd.op == Op::STORE_SLOT) {
      const uint32_t slot = uint32_t(w.g.i32[nd.i32_first]);
      if (written[slot]) return res.error = "record slot " + std::to_string(slot) + " is stored twice", res;
      written[slot] = 1;
      const int src = nd.input[0];
      const VType t = w.g.nodes[size_t(src)].out;
      if (w.isc[size_t(src)]) {
        if (!store_const(&res.block, table, slot, t, w.cv[size_t(src)], &res.clamp_flags, &res.error)) return res;
      } else {
        if (t == VType::F32 && slot >= uint32_t(ar::RecordSlot::base_color) && slot < uint32_t(ar::RecordSlot::geometry_normal))
          return res.error = "a run-time scalar cannot be stored in a colour slot", res;
        if (slot == uint32_t(ar::RecordSlot::closure_weight))
          return res.error = "closure_weight is set by the selection, not stored by a graph", res;
        is_root[size_t(i)] = 1;
      }
    } else if (nd.op == Op::EMIT_LE) {
      // A lone constant emission goes to the block; several accumulate in the program.
      if (emit_count == 1 && w.isc[size_t(nd.input[0])] && w.isc[size_t(nd.input[1])]) {
        const Vec3& c = w.cv[size_t(nd.input[0])];
        const ar::Rgb rgb{{ar::Real(c[0]), ar::Real(c[1]), ar::Real(c[2])}};
        ar::make_rgb_ref(table, rgb, ar::kSlotSemantic[int(ar::Slot::EMISSION_COLOR)], &res.block.emission_color,
                         &res.clamp_flags);
        res.block.emission_luminance = w.cv[size_t(nd.input[1])][0];
      } else {
        is_root[size_t(i)] = is_emit_root[size_t(i)] = 1;
      }
    }
  }
  if (std::none_of(is_root.begin(), is_root.end(), [](char c) { return c != 0; })) {
    res.constant_surface = true;
    res.ok = true;
    return res;
  }
  auto mark = [&](const std::vector<char>& roots) {
    std::vector<char> m(n, 0);
    std::vector<int> stack;
    for (size_t i = 0; i < n; ++i)
      if (roots[i]) stack.push_back(int(i));
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      if (m[size_t(i)]) continue;
      m[size_t(i)] = 1;
      if (w.isc[size_t(i)]) continue;  // becomes one constant instruction; its inputs are not needed
      for (int s = 0; s < w.g.nodes[size_t(i)].n_inputs; ++s) stack.push_back(w.g.nodes[size_t(i)].input[s]);
    }
    return m;
  };
  const std::vector<char> emit_set = mark(is_emit_root), all_set = mark(is_root);
  std::vector<uint32_t> surface, emission;
  if (!build_entry(w, all_set, emit_set, &surface, &res.error)) return res;
  const bool has_emission = std::any_of(is_emit_root.begin(), is_emit_root.end(), [](char c) { return c != 0; });
  if (has_emission && !build_entry(w, emit_set, emit_set, &emission, &res.error)) return res;
  res.program.assign(4, 0u);
  res.program[0] = 4;
  res.program.insert(res.program.end(), surface.begin(), surface.end());
  if (has_emission) {
    res.program[2] = uint32_t(res.program.size());
    res.program.insert(res.program.end(), emission.begin(), emission.end());
  }
  res.ok = true;
  return res;
}

}  // namespace astroray::host
