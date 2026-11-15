// SPDX-License-Identifier: Apache-2.0
#include "compiler.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

#include "../../kernel/material/lobes/delta.h"
#include "../../kernel/material/lobes/ggx.h"
#include "../../kernel/material/material_header.h"
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

// The mean of node i's value over a surface, for the emission estimate, which only steers light
// sampling. A constant is itself; TEX_IMAGE is its image's linear mean (magenta when the scene has
// no such image, which is what the lookup returns); the arithmetic ops combine their inputs'
// means, exact when at most one factor of a product varies; any other per-point value is bounded
// by 1. Without an ImageMeanFn images count as 1; every image read is added to *images.
struct MeanCtx {
  const ImageMeanFn* means;
  std::set<uint32_t>* images;
  std::vector<Vec3> memo;  // shared sub-graphs are evaluated once
  std::vector<char> have;
};

Vec3 mean_of_node(const Work& w, int i, MeanCtx* x);

Vec3 mean_of(const Work& w, int i, MeanCtx* x) {
  if (x->have[size_t(i)]) return x->memo[size_t(i)];
  const Vec3 v = mean_of_node(w, i, x);
  x->have[size_t(i)] = 1;
  return x->memo[size_t(i)] = v;
}

Vec3 mean_of_node(const Work& w, int i, MeanCtx* x) {
  if (w.isc[size_t(i)]) return w.cv[size_t(i)];
  const GraphNode& n = w.g.nodes[size_t(i)];
  auto in = [&](int s) { return mean_of(w, n.input[s], x); };
  switch (n.op) {
    case Op::TEX_IMAGE: {
      const uint32_t image = uint32_t(w.g.i32[n.i32_first]);
      x->images->insert(image);
      if (x->means == nullptr) return {1.0f, 1.0f, 1.0f};
      std::array<float, 3> m = {};
      if (!(*x->means)(image, &m)) m = {1.0f, 0.0f, 1.0f};
      return {m[0], m[1], m[2]};
    }
    case Op::MUL_F32: return {in(0)[0] * in(1)[0], 0.0f, 0.0f};
    case Op::MUL_F32X3: {
      const Vec3 a = in(0), b = in(1);
      return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
    }
    case Op::MUL_F32X3_F32: {
      const Vec3 a = in(0);
      const float s = in(1)[0];
      return {a[0] * s, a[1] * s, a[2] * s};
    }
    case Op::ADD_F32X3: {
      const Vec3 a = in(0), b = in(1);
      return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
    }
    default: return {1.0f, 1.0f, 1.0f};
  }
}

}  // namespace

CompileResult compile_surface(const ar_graph_desc& desc, const ar::Rgb2SpecTable& table, const ImageMeanFn* means) {
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
  std::set<uint32_t> images;
  MeanCtx mx{means, &images, std::vector<Vec3>(n), std::vector<char>(n, 0)};
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
        res.runtime_slots |= uint64_t(1) << slot;
      }
    } else if (nd.op == Op::EMIT_LE) {
      if (w.isc[size_t(nd.input[1])] && !(w.cv[size_t(nd.input[1])][0] >= 0.0f))
        return res.error = "EMIT_LE strength must not be negative", res;
      // Emission estimate (0005 item 43): the input's mean (a constant exactly, an image by its
      // mean, any other per-point input by 1).
      EmissionTerm term;
      const ar::SpectrumSemantic esem = ar::kSlotSemantic[int(ar::Slot::EMISSION_COLOR)];
      const Vec3 c = mean_of(w, nd.input[0], &mx);
      uint32_t ignored = 0;  // the clamp is reported from the stored colour, below or in the block
      if (!w.isc[size_t(nd.input[0])] && c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f)
        term.color = ar::make_const_ref(esem, ar::Real(1));  // the per-point upper bound: a white spectrum
      else
        ar::make_rgb_ref(table, ar::Rgb{{ar::Real(c[0]), ar::Real(c[1]), ar::Real(c[2])}}, esem, &term.color, &ignored);
      const bool zero = !(c[0] > 0.0f) && !(c[1] > 0.0f) && !(c[2] > 0.0f);
      term.strength = mean_of(w, nd.input[1], &mx)[0];
      if (!zero && term.strength > 0.0f) res.emission.push_back(term);
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
  res.images.assign(images.begin(), images.end());
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

namespace {

// The string of index k in the NUL-separated pool, or false when the pool has no such string.
bool pool_string(const ar_graph_desc& d, uint32_t k, std::string* out) {
  if (d.string_bytes_count != 0 && d.string_bytes == nullptr) return false;
  uint32_t at = 0, begin = 0;
  for (uint32_t i = 0; i < d.string_bytes_count; ++i) {
    if (d.string_bytes[i] != 0) continue;
    if (at++ == k) return out->assign(reinterpret_cast<const char*>(d.string_bytes + begin), i - begin), true;
    begin = i + 1;
  }
  return false;
}

void add_entry(MaterialOutput* m, uint32_t code, const std::string& detail) {
  for (const MaterialEntry& e : m->entries)
    if (e.code == code && e.detail == detail) return;  // one entry per (material, node type)
  m->entries.push_back({code, detail});
}

// MAY_BE_DELTA for one microfacet lobe: a per-point roughness may reach 0; a constant one is
// tested with the lobe library's own alpha and predicate. Ref: 0005 item 43; item 4
// (lobe_is_delta, alpha < 1e-3 on both axes); ggx.h ggx_alpha (alpha = roughness^2).
bool lobe_may_be_delta(bool per_point, float roughness) {
  if (per_point) return true;
  ar::GgxParams g = {};
  g.roughness = ar::Real(roughness);
  const ar::Real a = ar::ggx_alpha(g);
  return ar::lobe_is_delta(a, a);
}

bool per_point(const CompileResult& r, ar::RecordSlot s) { return ((r.runtime_slots >> uint32_t(s)) & 1u) != 0; }

void finish(MaterialOutput* m) {
  m->flags = material_flags(m->compiled);
  m->emission_estimate = m->compiled.emission;
  m->images = m->compiled.images;
}

}  // namespace

// In M1a the specular lobe (which the transmission lobe shares) always counts; the coat counts
// unless coat_weight folds to 0, where no coat lobe can be sampled. NON_OPAQUE is clear because
// geometry_opacity is fixed at 1 until alpha lands (M1b); EXTENSION is clear because no extension
// kind compiles; READS_RAY_TYPE is clear because no Light Path op exists.
uint32_t material_flags(const CompileResult& r) {
  using RS = ar::RecordSlot;
  bool delta = lobe_may_be_delta(per_point(r, RS::specular_roughness), r.block.specular_roughness);
  if (per_point(r, RS::coat_weight) || r.block.coat_weight != 0.0f)
    delta = delta || lobe_may_be_delta(per_point(r, RS::coat_roughness), r.block.coat_roughness);
  uint32_t flags = delta ? ar::MAY_BE_DELTA : 0u;
  const bool emission_entry = r.program.size() > 2 && r.program[2] != 0;
  if (r.constant_surface) flags |= ar::CONSTANT_SURFACE;
  if (!emission_entry) flags |= ar::CONSTANT_EMISSION;
  return flags;
}

MaterialOutput magenta_material(const ar::Rgb2SpecTable& table, uint32_t code, const std::string& detail) {
  MaterialOutput m;
  m.magenta = true;
  m.compiled.ok = true;
  m.compiled.constant_surface = true;
  m.compiled.block = ar::default_constant_block(table);
  // Diffuse magenta (0005 item 40): base_color (1,0,1) as reflectance and no specular layer.
  uint32_t flags = 0;
  ar::make_rgb_ref(table, ar::Rgb{{ar::Real(1), ar::Real(0), ar::Real(1)}}, ar::SpectrumSemantic::REFLECTANCE,
                   &m.compiled.block.base_color, &flags);
  m.compiled.block.specular_weight = 0.0f;
  finish(&m);
  add_entry(&m, code, detail);
  return m;
}

MaterialOutput compile_material(const ar_graph_desc& d, const ar::Rgb2SpecTable& table, const ImageMeanFn* means) {
  // Unsupported nodes first, every one of them, not only the first (0001 V15).
  MaterialOutput unsupported;
  for (uint32_t i = 0; d.nodes != nullptr && i < d.node_count; ++i) {
    const ar_graph_node& n = d.nodes[i];
    if (n.op >= uint32_t(Op::COUNT_)) {
      add_entry(&unsupported, AR_REPORT_NODE_UNSUPPORTED, "unknown op " + std::to_string(n.op));
    } else if (Op(n.op) == Op::UNSUPPORTED) {
      std::string type;
      if (n.string_count != 1 || !pool_string(d, n.string_first, &type) || type.empty()) type = "unnamed node";
      add_entry(&unsupported, AR_REPORT_NODE_UNSUPPORTED, type);
    }
  }
  if (!unsupported.entries.empty()) {
    MaterialOutput m = magenta_material(table, unsupported.entries[0].code, unsupported.entries[0].detail);
    for (const MaterialEntry& e : unsupported.entries) add_entry(&m, e.code, e.detail);
    return m;
  }
  MaterialOutput m;
  m.compiled = compile_surface(d, table, means);
  if (!m.compiled.ok) return magenta_material(table, AR_REPORT_PROGRAM_REJECTED, m.compiled.error);
  // Only CONSTANT and RGB resolve in M1a (spectrum_ref.h); TABLE, BLACKBODY and CAUCHY_ABBE land in M1b.
  for (int s = 0; s < int(ar::Slot::COUNT_); ++s) {
    const uint8_t k = ar::slot_ref(m.compiled.block, ar::Slot(s)).kind;
    if (k != uint8_t(ar::SpectrumKind::CONSTANT) && k != uint8_t(ar::SpectrumKind::RGB))
      return magenta_material(table, AR_REPORT_PROGRAM_REJECTED, "SpectrumRef kind " + std::to_string(k) + " lands in M1b");
  }
  finish(&m);
  if (m.compiled.clamp_flags != 0) add_entry(&m, AR_REPORT_NODE_APPROXIMATED, "RGB clamped");
  return m;
}

}  // namespace astroray::host
