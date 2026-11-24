// SPDX-License-Identifier: Apache-2.0
#include "compiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "../../kernel/material/lobes/delta.h"
#include "../../kernel/material/lobes/ggx.h"
#include "../../kernel/material/material_header.h"
#include "../../kernel/material/slot_resolve.h"
#include "../../kernel/medium/phase.h"
#include "../../kernel/vm/ops/add_f32.h"
#include "../../kernel/vm/ops/add_f32x3.h"
#include "../../kernel/vm/ops/channel_f32x3.h"
#include "../../kernel/vm/ops/const_f32.h"
#include "../../kernel/vm/ops/mul_f32.h"
#include "../../kernel/vm/ops/mul_f32x3.h"
#include "../../kernel/vm/ops/mul_f32x3_f32.h"
#include "../../kernel/vm/ops/store_slot.h"
#include "../../kernel/vm/ops/sub_f32.h"
#include "../../kernel/vm/ops/tex_image.h"
#include "../../kernel/vm/ops/volume_coeffs.h"
#include "../spectral/blackbody.h"
#include "graph.h"
#include "op_meta.h"
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
  std::vector<uint32_t> slot;  // TEX_IMAGE nodes: the image table slot, or kTexNoImage
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

// Folds node i when ops.def marks its op foldable (a pure function of constant inputs: no context,
// texture or wavelength read, 0005 item 35) and every input is constant. The arithmetic is the CPU
// build of the interpreter's own functions. A foldable op without a case below is a compiler bug:
// *err is set and the caller fails, instead of quietly leaving the op in the program.
bool fold(const Work& w, int i, Vec3* out, std::string* err) {
  const GraphNode& n = w.g.nodes[size_t(i)];
  *out = {0.0f, 0.0f, 0.0f};
  if (!op_meta(n.op).foldable) return false;
  for (int s = 0; s < n.n_inputs; ++s)
    if (!w.isc[size_t(n.input[s])]) return false;
  const float* f = w.g.f32.data() + n.f32_first;
  const Vec3& a = n.n_inputs > 0 ? w.cv[size_t(n.input[0])] : *out;
  const Vec3& b = n.n_inputs > 1 ? w.cv[size_t(n.input[1])] : *out;
  switch (n.op) {
    case Op::CONST_F32: (*out)[0] = f[0]; return true;
    case Op::CONST_F32X3: *out = {f[0], f[1], f[2]}; return true;
    case Op::MUL_F32: (*out)[0] = ar::op_mul_f32(a[0], b[0]); return true;
    case Op::ADD_F32: (*out)[0] = ar::op_add_f32(a[0], b[0]); return true;
    case Op::SUB_F32: (*out)[0] = ar::op_sub_f32(a[0], b[0]); return true;
    case Op::MUL_F32X3: ar::op_mul_f32x3(a.data(), b.data(), out->data()); return true;
    case Op::MUL_F32X3_F32: ar::op_mul_f32x3_f32(a.data(), b[0], out->data()); return true;
    case Op::ADD_F32X3: ar::op_add_f32x3(a.data(), b.data(), out->data()); return true;
    case Op::CHANNEL_F32X3: (*out)[0] = ar::op_channel_f32x3(a.data(), uint32_t(w.g.i32[n.i32_first])); return true;
    default:
      *err = std::string("internal error: ops.def marks ") + op_meta(n.op).name +
             " foldable but compiler.cpp fold() has no case for it";
      return false;
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

// One instruction slot of an entry's layout (0005 item 38). NODE evaluates `node` in instance
// `inst`: 0 is the run before the selection (the whole entry for a graph without a Mix Shader),
// k > 0 the BSDF block of the k-th child in layout order. A node evaluated in a block reads each
// input from that block when the input is evaluated there too, else from instance 0. BRANCH is
// the BRANCH_MIX of mix node `node`, whose second subtree starts at item `target`; JMP_END jumps
// to the entry's END; CONST_STORE is a STORE_SLOT whose source folded, written as one constant
// instruction and the store (a block holds its child's constants as code: the one constant block
// cannot hold two children's inputs).
struct Item {
  enum Kind : uint8_t { NODE, BRANCH, JMP_END, CONST_STORE } kind = NODE;
  int node = -1;
  int inst = 0;
  int target = -1;
};

bool spectral_slot(uint32_t slot) {
  return slot >= uint32_t(ar::RecordSlot::base_color) && slot < uint32_t(ar::RecordSlot::geometry_normal);
}

// The registers a CONST_STORE's constant takes: a colour slot takes RGB (a scalar source becomes
// grey), frames take three, scalars and specular_ior one.
int const_store_width(uint32_t slot) {
  if (slot == uint32_t(ar::RecordSlot::specular_ior)) return 1;
  return spectral_slot(slot) || slot >= uint32_t(ar::RecordSlot::geometry_normal) ? 3 : 1;
}

// The constant a CONST_STORE writes: a scalar into a colour slot is grey, clamped as store_const
// clamps it for the block, so the run-time lift sees the value the report describes.
Vec3 const_store_value(const Work& w, int store) {
  const GraphNode& st = w.g.nodes[size_t(store)];
  const uint32_t slot = uint32_t(w.g.i32[st.i32_first]);
  const int src = st.input[0];
  Vec3 v = w.cv[size_t(src)];
  if (spectral_slot(slot) && slot != uint32_t(ar::RecordSlot::specular_ior) && w.g.nodes[size_t(src)].out == VType::F32) {
    float x = v[0];
    if (!(x >= 0.0f)) x = 0.0f;
    if (ar::kSlotSemantic[slot - uint32_t(ar::RecordSlot::base_color)] == ar::SpectrumSemantic::REFLECTANCE && x > 1.0f) x = 1.0f;
    v = {x, x, x};
  }
  return v;
}

// One entry's code from its layout: values are allocated registers, END is appended and jumps
// are patched to word offsets from the jumping word.
bool build_code(const Work& w, const std::vector<Item>& items, std::vector<uint32_t>* code, std::string* err) {
  const int ni = int(items.size());
  std::map<std::pair<int, int>, int> value_of;  // (node, inst) -> value
  std::vector<int> item_value(size_t(ni), -1);  // the value an item defines
  std::vector<int> value_pos, value_last, value_width;
  for (int k = 0; k < ni; ++k) {
    const Item& it = items[size_t(k)];
    int width = 0;
    if (it.kind == Item::NODE) width = vtype_width(w.g.nodes[size_t(it.node)].out);
    if (it.kind == Item::CONST_STORE) width = const_store_width(uint32_t(w.g.i32[w.g.nodes[size_t(it.node)].i32_first]));
    if (width == 0) continue;
    item_value[size_t(k)] = int(value_pos.size());
    if (it.kind == Item::NODE) value_of[{it.node, it.inst}] = int(value_pos.size());
    value_pos.push_back(k), value_last.push_back(k), value_width.push_back(width);
  }
  auto read = [&](int node, int inst, int at) {
    auto f = value_of.find({node, inst});
    if (f == value_of.end()) f = value_of.find({node, 0});
    if (f == value_of.end()) return -1;
    value_last[size_t(f->second)] = std::max(value_last[size_t(f->second)], at);
    return f->second;
  };
  std::vector<std::array<int, 2>> item_in(size_t(ni), std::array<int, 2>{-1, -1});
  for (int k = 0; k < ni; ++k) {
    const Item& it = items[size_t(k)];
    if (it.kind == Item::JMP_END || it.kind == Item::CONST_STORE) continue;
    const GraphNode& nd = w.g.nodes[size_t(it.node)];
    if (it.kind == Item::NODE && w.isc[size_t(it.node)]) continue;  // materialised as a constant: reads nothing
    for (int s = 0; s < nd.n_inputs; ++s)
      if ((item_in[size_t(k)][size_t(s)] = read(nd.input[s], it.kind == Item::BRANCH ? 0 : it.inst, k)) < 0)
        return *err = "internal: an input is not evaluated before its reader", false;
  }
  // EMIT_LE reads colour in r[a..a+2] and strength in r[a+3]: the two values share one 4-wide
  // range, colour first, so no copy instructions are needed.
  std::vector<int> group(value_pos.size(), -1);
  std::vector<int> emits;
  for (int k = 0; k < ni; ++k) {
    if (items[size_t(k)].kind != Item::NODE || w.g.nodes[size_t(items[size_t(k)].node)].op != Op::EMIT_LE) continue;
    for (int s = 0; s < 2; ++s) {
      if (group[size_t(item_in[size_t(k)][size_t(s)])] >= 0)
        return *err = "an emission colour or strength feeds two EMIT_LE nodes; unsupported in M1a", false;
      group[size_t(item_in[size_t(k)][size_t(s)])] = k;
    }
    emits.push_back(k);
  }
  std::vector<LiveRange> ranges;
  std::vector<int> range_value, range_group_emit;  // value of a plain range, or -1 and the EMIT_LE item
  for (size_t v = 0; v < value_pos.size(); ++v) {
    if (group[v] >= 0) continue;
    ranges.push_back({value_pos[v], std::max(value_last[v], value_pos[v]), value_width[v]});
    range_value.push_back(int(v));
    range_group_emit.push_back(-1);
  }
  for (int e : emits) {
    const int c = item_in[size_t(e)][0], s = item_in[size_t(e)][1];
    ranges.push_back({std::min(value_pos[size_t(c)], value_pos[size_t(s)]),
                      std::max(value_last[size_t(c)], value_last[size_t(s)]), 4});
    range_value.push_back(-1);
    range_group_emit.push_back(e);
  }
  std::vector<int> regs;
  if (!allocate_registers(ranges, &regs, err)) return false;
  std::vector<int> reg(value_pos.size(), -1);
  for (size_t r = 0; r < ranges.size(); ++r) {
    if (range_value[r] >= 0) {
      reg[size_t(range_value[r])] = regs[r];
    } else {
      reg[size_t(item_in[size_t(range_group_emit[r])][0])] = regs[r];
      reg[size_t(item_in[size_t(range_group_emit[r])][1])] = regs[r] + 3;
    }
  }
  auto bits = [](float f) { return ar::bit_cast<uint32_t>(f); };
  auto emit_const = [&](uint32_t a, const Vec3& v, int width) {
    if (width == 1) {
      code->push_back(word(Op::CONST_F32, a));
      code->push_back(bits(v[0]));
    } else {
      code->push_back(word(Op::CONST_F32X3, a));
      for (float f : v) code->push_back(bits(f));
    }
  };
  std::vector<size_t> start(size_t(ni) + 1, 0);
  for (int k = 0; k < ni; ++k) {
    const Item& it = items[size_t(k)];
    start[size_t(k)] = code->size();
    if (it.kind == Item::JMP_END) {
      code->push_back(word(Op::JMP));
      code->push_back(0u);  // patched below
      continue;
    }
    const GraphNode& nd = w.g.nodes[size_t(it.node)];
    auto in = [&](int s) { return uint32_t(reg[size_t(item_in[size_t(k)][size_t(s)])]); };
    if (it.kind == Item::BRANCH) {
      code->push_back(word(Op::BRANCH_MIX, in(0)));
      code->push_back(0u);  // patched below
      continue;
    }
    const uint32_t a = item_value[size_t(k)] >= 0 ? uint32_t(reg[size_t(item_value[size_t(k)])]) : 0u;
    if (it.kind == Item::CONST_STORE) {
      const uint32_t slot = uint32_t(w.g.i32[nd.i32_first]);
      emit_const(a, const_store_value(w, it.node), const_store_width(slot));
      code->push_back(word(Op::STORE_SLOT, a));
      code->push_back(slot);
      continue;
    }
    if (w.isc[size_t(it.node)]) {
      emit_const(a, w.cv[size_t(it.node)], nd.out == VType::F32 ? 1 : 3);
      continue;
    }
    switch (nd.op) {
      case Op::LOAD_CTX_UV: code->push_back(word(nd.op, a, uint32_t(w.g.i32[nd.i32_first]))); break;
      case Op::LOAD_CTX_P: code->push_back(word(nd.op, a)); break;
      case Op::MUL_F32:
      case Op::ADD_F32:
      case Op::SUB_F32:
      case Op::MUL_F32X3:
      case Op::MUL_F32X3_F32:
      case Op::ADD_F32X3: code->push_back(word(nd.op, a, in(0), in(1))); break;
      case Op::TEX_IMAGE:
        code->push_back(word(nd.op, a, in(0)));
        code->push_back(w.slot[size_t(it.node)]);
        code->push_back(uint32_t(w.g.i32[nd.i32_first + 1]));
        break;
      case Op::RGB_LIFT: code->push_back(word(nd.op, a, in(0), uint32_t(w.g.i32[nd.i32_first]))); break;
      case Op::CHANNEL_F32X3:
        code->push_back(word(nd.op, a, in(0)));
        code->push_back(uint32_t(w.g.i32[nd.i32_first]));
        break;
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
  start[size_t(ni)] = code->size();
  code->push_back(word(Op::END));
  // Forward offsets in words from the jumping word: BRANCH_MIX to its second subtree (or END when
  // that subtree laid out no code), JMP to END.
  for (int k = 0; k < ni; ++k) {
    const Item& it = items[size_t(k)];
    if (it.kind == Item::BRANCH) (*code)[start[size_t(k)] + 1] = uint32_t(start[size_t(it.target)] - start[size_t(k)]);
    if (it.kind == Item::JMP_END) (*code)[start[size_t(k)] + 1] = uint32_t(start[size_t(ni)] - start[size_t(k)]);
  }
  return true;
}

// The layout of an entry without a Mix Shader: the marked nodes in topological order, those in
// `first` before the rest (the emission segment, item 38), all in instance 0.
std::vector<Item> plain_layout(const Work& w, const std::vector<char>& marked, const std::vector<char>& first) {
  std::vector<Item> items;
  for (int pass = 0; pass < 2; ++pass)
    for (int i : w.topo)
      if (marked[size_t(i)] && bool(first[size_t(i)]) == (pass == 0)) items.push_back({Item::NODE, i, 0, -1});
  return items;
}

// The mean of node i's value over a surface, for the emission estimate (0005 item 43), which
// also steers light sampling. `known` means the mean is exact for the clamped emission: a
// non-negative constant, a texture's own mean (non-negative), MUL or ADD of known operands (exact
// when at most one factor of a product varies) and one channel of a known colour. Everything else,
// including any negative constant, is not known and the caller bounds it by 1, so the estimate is
// never wrongly zero.
// Without an ImageMeanFn a texture is not known; every image read is added to *images.
struct Mean {
  Vec3 v = {1.0f, 1.0f, 1.0f};
  bool known = false;
};

struct MeanCtx {
  const ImageMeanFn* means;
  std::set<uint32_t>* images;
  std::vector<Mean> memo;  // shared sub-graphs are evaluated once
  std::vector<char> have;
};

Mean mean_of_node(const Work& w, int i, MeanCtx* x);

Mean mean_of(const Work& w, int i, MeanCtx* x) {
  if (x->have[size_t(i)]) return x->memo[size_t(i)];
  const Mean m = mean_of_node(w, i, x);
  x->have[size_t(i)] = 1;
  return x->memo[size_t(i)] = m;
}

Mean mean_of_node(const Work& w, int i, MeanCtx* x) {
  Mean r;
  if (w.isc[size_t(i)]) {
    r.v = w.cv[size_t(i)];
    r.known = r.v[0] >= 0.0f && r.v[1] >= 0.0f && r.v[2] >= 0.0f;
    return r;
  }
  const GraphNode& n = w.g.nodes[size_t(i)];
  if (n.op == Op::TEX_IMAGE) {
    const uint32_t image = uint32_t(w.g.i32[n.i32_first]);
    x->images->insert(image);
    if (x->means == nullptr) return r;
    std::array<float, 3> m = {};
    if (!(*x->means)(image, &m)) m = {1.0f, 0.0f, 1.0f};
    r.v = {m[0], m[1], m[2]};
    r.known = true;
    return r;
  }
  if (n.op == Op::CHANNEL_F32X3) {
    const Mean a = mean_of(w, n.input[0], x);
    if (!a.known) return r;
    r.v = {a.v[size_t(w.g.i32[n.i32_first])], 0.0f, 0.0f};
    r.known = true;
    return r;
  }
  const bool arith = n.op == Op::MUL_F32 || n.op == Op::MUL_F32X3 || n.op == Op::MUL_F32X3_F32 || n.op == Op::ADD_F32X3;
  if (!arith) return r;
  const Mean a = mean_of(w, n.input[0], x), b = mean_of(w, n.input[1], x);
  if (!a.known || !b.known) return r;
  r.known = true;
  switch (n.op) {
    case Op::MUL_F32: r.v = {a.v[0] * b.v[0], 0.0f, 0.0f}; break;
    case Op::MUL_F32X3: r.v = {a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2]}; break;
    case Op::MUL_F32X3_F32: r.v = {a.v[0] * b.v[0], a.v[1] * b.v[0], a.v[2] * b.v[0]}; break;
    default: r.v = {a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2]}; break;
  }
  return r;
}

// The estimate's value for input node i: the mean when it is known, the bound 1 otherwise. A
// result of 0 stands only when it is exact: a folded constant or a texture's own mean.
Vec3 estimate_input(const Work& w, int i, MeanCtx* x) {
  if (w.isc[size_t(i)]) return w.cv[size_t(i)];  // folded: the stored value, clamped by the colour packing
  const Mean m = mean_of(w, i, x);
  const bool direct = w.g.nodes[size_t(i)].op == Op::TEX_IMAGE;
  const bool zero = !(m.v[0] > 0.0f) && !(m.v[1] > 0.0f) && !(m.v[2] > 0.0f);
  return !m.known || (zero && !direct) ? Vec3{1.0f, 1.0f, 1.0f} : m.v;
}

// Resolves every TEX_IMAGE node in `used` to its image slot (0005 item 34: a missing image is
// magenta with a report entry); the program depends on the key either way, so setting it later
// recompiles.
void resolve_images(Work* w, const std::vector<char>& used, const ImageSlotFn* slots, std::set<uint32_t>* images,
                    std::set<uint32_t>* missing) {
  w->slot.assign(w->g.nodes.size(), ar::kTexNoImage);
  for (size_t i = 0; i < w->g.nodes.size(); ++i) {
    if (!used[i] || w->g.nodes[i].op != Op::TEX_IMAGE) continue;
    const uint32_t key = uint32_t(w->g.i32[w->g.nodes[i].i32_first]);
    images->insert(key);
    if (slots != nullptr && !(*slots)(key, &w->slot[i])) w->slot[i] = ar::kTexNoImage, missing->insert(key);
  }
}

// A graph with Mix Shaders (0005 item 38): every STORE_SLOT belongs to the child its `child`
// parameter names, and the BRANCH_MIX nodes form one tree over the children (graph.h). The surface
// entry is (a) the run before the selection: the mix factors and every value more than one child
// reads, computed once and kept live; (b) the selection, BRANCH_MIX nodes in preorder over the
// tree, each falling through to its first subtree and jumping to its second; (c) one block per
// child in tree order, its constants as CONST_STORE code, ending in JMP to END (the last block
// falls into END). The constant block keeps the OpenPBR defaults, so the record a block leaves is
// its child's alone. Emission and opacity under a Mix are not compiled yet and are rejected, as is
// a dispersive IOR, which is a block SpectrumRef no block code can store.
bool compile_mixed(Work& w, const ImageSlotFn* slots, CompileResult* res) {
  const size_t n = w.g.nodes.size();
  const auto& i32 = w.g.i32;
  std::map<int32_t, int> mix_of_tag;
  std::map<int, std::vector<int>> stores_of;  // child id -> its STORE_SLOT nodes, topological order
  for (int i : w.topo) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    const std::string who = "graph node " + std::to_string(i) + ": ";
    if (nd.op == Op::BRANCH_MIX && !mix_of_tag.emplace(i32[nd.i32_first], i).second)
      return res->error = who + "Mix Shader tag " + std::to_string(i32[nd.i32_first]) + " is used twice", false;
    if (nd.op == Op::EMIT_LE) return res->error = who + "an emission in a Mix Shader graph is not supported yet", false;
    if (nd.op != Op::STORE_SLOT) continue;
    const uint32_t slot = uint32_t(i32[nd.i32_first]);
    if (slot == uint32_t(ar::RecordSlot::geometry_opacity))
      return res->error = who + "geometry_opacity in a Mix Shader graph is not supported yet", false;
    if (slot == uint32_t(ar::RecordSlot::closure_weight))
      return res->error = who + "closure_weight is set by the selection, not stored by a graph", false;
    if (w.g.nodes[size_t(nd.input[0])].op == Op::CAUCHY_ABBE)
      return res->error = who + "a dispersive IOR cannot be stored per Mix Shader child", false;
    if (!w.isc[size_t(nd.input[0])] && w.g.nodes[size_t(nd.input[0])].out == VType::F32 && spectral_slot(slot) &&
        slot != uint32_t(ar::RecordSlot::specular_ior))
      return res->error = who + "a run-time scalar cannot be stored in a colour slot", false;
    std::vector<int>& list = stores_of[nd.child];
    for (int o : list)
      if (uint32_t(i32[w.g.nodes[size_t(o)].i32_first]) == slot)
        return res->error = who + "record slot " + std::to_string(slot) + " is stored twice in child " + std::to_string(nd.child), false;
    list.push_back(i);
  }
  // The tree: each child and each tag named once, one root, every mix reachable from it.
  std::map<int, int> child_refs, tag_refs;
  for (const auto& [tag, m] : mix_of_tag)
    for (int s = 1; s <= 2; ++s) {
      const int32_t ref = i32[w.g.nodes[size_t(m)].i32_first + uint32_t(s)];
      const std::string who = "Mix Shader tag " + std::to_string(tag) + ": ";
      if (ref >= 0) {
        if (++child_refs[ref] > 1) return res->error = who + "child " + std::to_string(ref) + " is named twice", false;
      } else {
        const int32_t t = -1 - ref;
        if (mix_of_tag.count(t) == 0) return res->error = who + "names Mix Shader tag " + std::to_string(t) + ", which the graph lacks", false;
        if (++tag_refs[t] > 1) return res->error = who + "Mix Shader tag " + std::to_string(t) + " is named twice", false;
      }
    }
  for (const auto& [child, list] : stores_of)
    if (child_refs.count(child) == 0)
      return res->error = "child " + std::to_string(child) + " stores record slots but no Mix Shader names it", false;
  std::vector<int> roots;
  for (const auto& [tag, m] : mix_of_tag)
    if (tag_refs.count(tag) == 0) roots.push_back(m);
  if (roots.size() != 1) return res->error = "a Mix Shader graph needs exactly one root Mix, found " + std::to_string(roots.size()), false;
  // Children in layout order (first subtree before second), and the mixes the walk reached.
  std::vector<int> order;
  size_t mixes_seen = 0;
  {
    std::vector<int32_t> walk{mix_ref(i32[w.g.nodes[size_t(roots[0])].i32_first])};
    while (!walk.empty()) {
      const int32_t ref = walk.back();
      walk.pop_back();
      if (ref >= 0) {
        order.push_back(ref);
        continue;
      }
      if (++mixes_seen > mix_of_tag.size()) break;
      const GraphNode& m = w.g.nodes[size_t(mix_of_tag[-1 - ref])];
      walk.push_back(i32[m.i32_first + 2]);  // popped after the first subtree
      walk.push_back(i32[m.i32_first + 1]);
    }
  }
  if (mixes_seen != mix_of_tag.size()) return res->error = "Mix Shader tags form a cycle or a second tree", false;
  std::map<int, int> inst_of;  // child id -> instance (1-based layout index)
  for (size_t k = 0; k < order.size(); ++k) inst_of[order[k]] = int(k) + 1;

  // Which instance evaluates each run-time node: one child's block, or the shared run when the
  // factors or two children read it. Constants are materialised where they are read.
  constexpr int kNone = -1, kShared = 0;
  std::vector<int> owner(n, kNone);
  auto claim = [&](int from, int inst) {
    std::vector<char> seen(n, 0);
    std::vector<int> stack{from};
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      if (seen[size_t(i)] || w.isc[size_t(i)]) continue;
      seen[size_t(i)] = 1;
      owner[size_t(i)] = owner[size_t(i)] == kNone || owner[size_t(i)] == inst ? inst : kShared;
      for (int s = 0; s < w.g.nodes[size_t(i)].n_inputs; ++s) stack.push_back(w.g.nodes[size_t(i)].input[s]);
    }
  };
  for (const auto& [child, list] : stores_of)
    for (int st : list)
      if (!w.isc[size_t(w.g.nodes[size_t(st)].input[0])]) claim(st, inst_of[child]);
  for (const auto& [tag, m] : mix_of_tag) claim(w.g.nodes[size_t(m)].input[0], kShared);
  // Constants each instance reads (a mix factor is read by the shared run).
  std::vector<std::set<int>> reads(order.size() + 1);
  for (size_t i = 0; i < n; ++i) {
    const GraphNode& nd = w.g.nodes[i];
    const int inst = nd.op == Op::BRANCH_MIX ? kShared : owner[i];
    if (inst == kNone) continue;
    for (int s = 0; s < nd.n_inputs; ++s)
      if (w.isc[size_t(nd.input[s])]) reads[size_t(inst)].insert(nd.input[s]);
  }
  auto block = [&](int inst, std::vector<Item>* items) {
    for (int i : w.topo) {
      const GraphNode& nd = w.g.nodes[size_t(i)];
      if (owner[size_t(i)] == inst || reads[size_t(inst)].count(i) != 0) items->push_back({Item::NODE, i, inst, -1});
      else if (inst != kShared && nd.op == Op::STORE_SLOT && inst_of[nd.child] == inst && w.isc[size_t(nd.input[0])])
        items->push_back({Item::CONST_STORE, i, inst, -1});
    }
  };
  std::vector<Item> items;
  block(kShared, &items);
  {
    // Preorder: BRANCH, its first subtree, then its second subtree, whose first item is the
    // BRANCH's target (`branch` names the BRANCH item a second subtree belongs to).
    struct Step {
      int32_t ref;
      long branch;
    };
    std::vector<Step> st{{mix_ref(i32[w.g.nodes[size_t(roots[0])].i32_first]), -1}};
    while (!st.empty()) {
      const Step s = st.back();
      st.pop_back();
      if (s.branch >= 0) items[size_t(s.branch)].target = int(items.size());
      if (s.ref >= 0) {
        block(inst_of[s.ref], &items);
        items.push_back({Item::JMP_END, -1, inst_of[s.ref], -1});
        continue;
      }
      const int m = mix_of_tag[-1 - s.ref];
      items.push_back({Item::BRANCH, m, kShared, -1});
      const long b = long(items.size()) - 1;
      st.push_back({i32[w.g.nodes[size_t(m)].i32_first + 2], b});
      st.push_back({i32[w.g.nodes[size_t(m)].i32_first + 1], -1});
    }
  }
  if (!items.empty() && items.back().kind == Item::JMP_END) items.pop_back();  // the last block falls into END

  std::set<uint32_t> images, missing;
  std::vector<char> used(n, 0);
  for (size_t i = 0; i < n; ++i) used[i] = owner[i] != kNone;
  resolve_images(&w, used, slots, &images, &missing);
  res->images.assign(images.begin(), images.end());
  res->missing_images.assign(missing.begin(), missing.end());
  std::vector<uint32_t> surface;
  if (!build_code(w, items, &surface, &res->error)) return false;
  res->program.assign(4, 0u);
  res->program[0] = 4;
  res->program.insert(res->program.end(), surface.begin(), surface.end());
  // Each child's constants over the defaults, for the material flags and the clamp report.
  for (int child : order) {
    ChildConstants c;
    c.id = child;
    c.block = res->block;
    for (int st : stores_of[child]) {
      const GraphNode& nd = w.g.nodes[size_t(st)];
      const uint32_t slot = uint32_t(i32[nd.i32_first]);
      if (!w.isc[size_t(nd.input[0])]) {
        c.runtime_slots |= uint64_t(1) << slot;
        continue;
      }
      if (!store_const(&c.block, *w.table, slot, w.g.nodes[size_t(nd.input[0])].out, w.cv[size_t(nd.input[0])],
                       &res->clamp_flags, &res->error))
        return false;
    }
    res->runtime_slots |= c.runtime_slots;
    res->children.push_back(c);
  }
  return true;
}

// Emission inside the opacity mix (0005 item 4: top = mix(transparent, surface, opacity), the
// emission under the surface): every EMIT_LE strength becomes strength x a, a new MUL_F32 node, so
// the hit, the emission entry that NEE on an emissive triangle runs and the emission estimate all
// read a x Le from one program, and the kernel scales nothing again. A per-point opacity is the
// store's own source, computed once in the emission segment and kept live for the store (item 38);
// a constant one is the clamped value the block holds, folded into the product (a lone constant
// emission then lands in the block's luminance as before). An opacity of exactly 1 changes nothing.
// A NaN constant is left to the store's own rejection.
// Ref: docs/decisions/0005-material-model.md items 4, 38, 43; third_party/_cache/refs/
// openpbr-781573a80cb2 index.html "Opacity / Transparency" (M_PBR = mix(S_ambient, M_surface,
// geometry_opacity)) and reference/open_pbr_surface.mtlx (the surface node takes the emission EDF
// and opacity = geometry_opacity together).
bool scale_emission_by_opacity(Work& w, std::string* err) {
  const uint32_t opacity_slot = uint32_t(ar::RecordSlot::geometry_opacity);
  int src = -1;
  std::vector<int> emits;
  for (int i : w.topo) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    if (nd.op == Op::EMIT_LE) emits.push_back(i);
    if (nd.op == Op::STORE_SLOT && src < 0 && uint32_t(w.g.i32[nd.i32_first]) == opacity_slot) src = nd.input[0];
  }
  if (src < 0 || emits.empty()) return true;
  // A Blackbody emission is a block ref with a constant strength (items 9, 39): it cannot take a
  // per-point factor.
  if (!w.isc[size_t(src)])
    for (int e : emits)
      if (w.g.nodes[size_t(w.g.nodes[size_t(e)].input[0])].op == Op::BLACKBODY)
        return *err = "graph node " + std::to_string(e) + " (EMIT_LE): a Blackbody emission needs a constant geometry_opacity", false;
  int factor = src;
  if (w.isc[size_t(src)]) {
    const float v = w.cv[size_t(src)][0];
    if (std::isnan(v) || v >= 1.0f) return true;  // NaN is rejected by the store; above 1 clamps to 1
    if (v < 0.0f) {
      GraphNode c;
      c.op = Op::CONST_F32;
      c.f32_first = uint32_t(w.g.f32.size());
      c.out = VType::F32;
      w.g.f32.push_back(0.0f);
      w.g.nodes.push_back(c);
      factor = int(w.g.nodes.size()) - 1;
    }
  }
  for (int e : emits) {
    GraphNode m;
    m.op = Op::MUL_F32;
    m.input[0] = w.g.nodes[size_t(e)].input[1];
    m.input[1] = factor;
    m.n_inputs = 2;
    m.out = VType::F32;
    w.g.nodes.push_back(m);
    w.g.nodes[size_t(e)].input[1] = int(w.g.nodes.size()) - 1;
  }
  // The new nodes come last in index order, so the stable order of the old ones is kept where it
  // can be; each is folded from its inputs, which are evaluated before it.
  const size_t old_n = w.isc.size();
  w.topo = stable_topo(w.g);
  w.isc.resize(w.g.nodes.size(), 0);
  w.cv.resize(w.g.nodes.size(), Vec3{0.0f, 0.0f, 0.0f});
  for (int i : w.topo) {
    if (size_t(i) < old_n) continue;
    w.isc[size_t(i)] = fold(w, i, &w.cv[size_t(i)], err);
    if (!err->empty()) return false;
  }
  return true;
}

}  // namespace

CompileResult compile_surface(const ar_graph_desc& desc, const ar::Rgb2SpecTable& table, const ImageMeanFn* means,
                              const ImageSlotFn* slots) {
  CompileResult res;
  Work w;
  w.table = &table;
  if (!parse_graph(desc, &w.g, &res.error)) return res;
  size_t n = w.g.nodes.size();
  for (size_t i = 0; i < n; ++i)
    if (w.g.nodes[i].op == Op::VOLUME_COEFFS)
      return res.error = "graph node " + std::to_string(i) + " (VOLUME_COEFFS): a volume term has no surface meaning", res;
  w.topo = stable_topo(w.g);
  w.isc.assign(n, 0);
  w.cv.assign(n, Vec3{0.0f, 0.0f, 0.0f});
  for (int i : w.topo) {
    w.isc[size_t(i)] = fold(w, i, &w.cv[size_t(i)], &res.error);
    if (!res.error.empty()) return res;
  }

  res.block = ar::default_constant_block(table);
  if (std::any_of(w.g.nodes.begin(), w.g.nodes.end(), [](const GraphNode& nd) { return nd.op == Op::BRANCH_MIX; })) {
    res.ok = compile_mixed(w, slots, &res);
    return res;
  }
  for (size_t i = 0; i < n; ++i)
    if (w.g.nodes[i].child != 0)
      return res.error = "graph node " + std::to_string(i) + " names child " + std::to_string(w.g.nodes[i].child) +
                         " but the graph has no Mix Shader", res;
  // Every strength is checked as the graph gives it, before scale_emission_by_opacity makes it
  // strength x a: a per-point product does not fold, and a negative strength times a clamped
  // opacity of 0 folds to -0, so a check of the product would pass both.
  for (int i : w.topo) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    if (nd.op == Op::EMIT_LE && w.isc[size_t(nd.input[1])] && !(w.cv[size_t(nd.input[1])][0] >= 0.0f))
      return res.error = "EMIT_LE strength must not be negative", res;
  }
  if (!scale_emission_by_opacity(w, &res.error)) return res;
  n = w.g.nodes.size();
  int emit_count = 0;
  for (const GraphNode& nd : w.g.nodes) emit_count += nd.op == Op::EMIT_LE;
  std::vector<char> written(size_t(ar::RecordSlot::COUNT_), 0), is_root(n, 0), is_emit_root(n, 0), is_opacity_root(n, 0);
  const uint32_t opacity_slot = uint32_t(ar::RecordSlot::geometry_opacity);
  std::set<uint32_t> images;
  MeanCtx mx{means, &images, std::vector<Mean>(n), std::vector<char>(n, 0)};
  for (int i : w.topo) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    if (nd.op == Op::STORE_SLOT) {
      const uint32_t slot = uint32_t(w.g.i32[nd.i32_first]);
      if (written[slot]) return res.error = "record slot " + std::to_string(slot) + " is stored twice", res;
      written[slot] = 1;
      const int src = nd.input[0];
      const VType t = w.g.nodes[size_t(src)].out;
      if (w.g.nodes[size_t(src)].op == Op::CAUCHY_ABBE) {
        // parse_graph admits this source only for specular_ior, from constant parameters: the
        // dispersive IOR is one ref in the block, evaluated per wavelength by resolve().
        const float* pf = w.g.f32.data() + w.g.nodes[size_t(src)].f32_first;
        ar::SpectrumRef& r = res.block.specular_ior;
        r = {};
        r.kind = uint8_t(ar::SpectrumKind::CAUCHY_ABBE);
        r.semantic = uint8_t(ar::SpectrumSemantic::IOR);
        for (int k = 0; k < 3; ++k) r.p[k] = pf[k];
      } else if (w.isc[size_t(src)]) {
        Vec3 v = w.cv[size_t(src)];
        if (slot == opacity_slot) {  // a fraction: clamped and reported like a colour (0005 item 32)
          if (std::isnan(v[0])) return res.error = "geometry_opacity is NaN", res;
          if (v[0] < 0.0f || v[0] > 1.0f) v[0] = std::min(std::max(v[0], 0.0f), 1.0f), res.opacity_clamped = true;
        }
        if (!store_const(&res.block, table, slot, t, v, &res.clamp_flags, &res.error)) return res;
      } else {
        if (t == VType::F32 && slot >= uint32_t(ar::RecordSlot::base_color) && slot < uint32_t(ar::RecordSlot::geometry_normal))
          return res.error = "a run-time scalar cannot be stored in a colour slot", res;
        if (slot == uint32_t(ar::RecordSlot::closure_weight))
          return res.error = "closure_weight is set by the selection, not stored by a graph", res;
        is_root[size_t(i)] = 1;
        is_opacity_root[size_t(i)] = slot == opacity_slot;
        res.runtime_slots |= uint64_t(1) << slot;
      }
    } else if (nd.op == Op::EMIT_LE) {
      // Emission estimate (0005 item 43): the input's mean (a constant exactly, an image by its
      // mean, any other per-point input by 1).
      const GraphNode& colour_node = w.g.nodes[size_t(nd.input[0])];
      if (colour_node.op == Op::BLACKBODY) {
        // Analytic emission (0005 items 9, 39): one constant Blackbody feeding the material's only
        // emission is a block ref; any other shape would need a spectrum in the program, which
        // M1 has no slot-resolve for.
        const std::string who = "graph node " + std::to_string(nd.input[0]) + " (BLACKBODY): ";
        if (emit_count != 1) return res.error = who + "a Blackbody emission must be the material's only emission", res;
        if (!w.isc[size_t(nd.input[1])]) return res.error = who + "the emission strength must be constant", res;
        const float* pf = w.g.f32.data() + colour_node.f32_first;
        double y_norm;
        try {
          y_norm = blackbody_y_normalised(double(pf[0]));
        } catch (const std::exception& e) {
          return res.error = who + e.what(), res;
        }
        res.block.emission_luminance = w.cv[size_t(nd.input[1])][0];
        // The stored scale and the brightest band value must fit a float, else the kernel would
        // emit inf or NaN (a cold T makes 1 / Y huge; a huge scale does the same).
        const double p1 = double(pf[1]) * y_norm;
        // Wien displacement, b = 2.897771955e6 nm K (CODATA 2018): where the band is brightest.
        const double wien_nm =std::min(830.0, std::max(360.0, 2897771.955 / double(pf[0])));
        const double peak = p1 * ar::planck_radiance_t<double>(wien_nm, double(pf[0])) * double(res.block.emission_luminance);
        if (!(p1 <= double(std::numeric_limits<float>::max())) || !(peak <= double(std::numeric_limits<float>::max())))
          return res.error = who + "the scale and 1 / Y(Planck(T)) overflow a float at this temperature", res;
        ar::SpectrumRef& r = res.block.emission_color;
        r = {};
        r.kind = uint8_t(ar::SpectrumKind::BLACKBODY);
        r.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
        r.p[0] = pf[0];
        r.p[1] = float(double(pf[1]) * y_norm);
        EmissionTerm bt;
        bt.color = r;
        bt.strength = res.block.emission_luminance;
        if (bt.strength > 0.0f && pf[1] > 0.0f) res.emission.push_back(bt);
        continue;
      }
      EmissionTerm term;
      const ar::SpectrumSemantic esem = ar::kSlotSemantic[int(ar::Slot::EMISSION_COLOR)];
      const Vec3 mean = estimate_input(w, nd.input[0], &mx);
      uint32_t ignored = 0;  // the clamp is reported from the stored colour, below or in the block
      if (!w.isc[size_t(nd.input[0])] && mean[0] == 1.0f && mean[1] == 1.0f && mean[2] == 1.0f)
        term.color = ar::make_const_ref(esem, ar::Real(1));  // the per-point upper bound: a white spectrum
      else
        ar::make_rgb_ref(table, ar::Rgb{{ar::Real(mean[0]), ar::Real(mean[1]), ar::Real(mean[2])}}, esem, &term.color, &ignored);
      const bool zero = !(mean[0] > 0.0f) && !(mean[1] > 0.0f) && !(mean[2] > 0.0f);
      term.strength = estimate_input(w, nd.input[1], &mx)[0];
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
  if (std::none_of(is_root.begin(), is_root.end(), [](char c) { return c != 0; })) {
    res.images.assign(images.begin(), images.end());
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
  const std::vector<char> emit_set = mark(is_emit_root), all_set = mark(is_root), opacity_set = mark(is_opacity_root);
  std::set<uint32_t> missing;
  resolve_images(&w, all_set, slots, &images, &missing);
  res.images.assign(images.begin(), images.end());
  res.missing_images.assign(missing.begin(), missing.end());
  std::vector<uint32_t> surface, emission, opacity;
  if (!build_code(w, plain_layout(w, all_set, emit_set), &surface, &res.error)) return res;
  const bool has_emission = std::any_of(is_emit_root.begin(), is_emit_root.end(), [](char c) { return c != 0; });
  if (has_emission && !build_code(w, plain_layout(w, emit_set, emit_set), &emission, &res.error)) return res;
  // Item 38: the opacity entry is the per-point geometry_opacity store and what feeds it, nothing
  // else; a constant opacity is read from the block and needs none.
  const bool has_opacity = std::any_of(is_opacity_root.begin(), is_opacity_root.end(), [](char c) { return c != 0; });
  if (has_opacity && !build_code(w, plain_layout(w, opacity_set, std::vector<char>(n, 0)), &opacity, &res.error)) return res;
  res.program.assign(4, 0u);
  res.program[0] = 4;
  res.program.insert(res.program.end(), surface.begin(), surface.end());
  if (has_emission) {
    res.program[2] = uint32_t(res.program.size());
    res.program.insert(res.program.end(), emission.begin(), emission.end());
  }
  if (has_opacity) {
    res.program[1] = uint32_t(res.program.size());
    res.program.insert(res.program.end(), opacity.begin(), opacity.end());
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

void add_entry(std::vector<MaterialEntry>* entries, uint32_t code, const std::string& detail) {
  for (const MaterialEntry& e : *entries)
    if (e.code == code && e.detail == detail) return;  // one entry per (material, node type)
  entries->push_back({code, detail});
}

void add_entry(MaterialOutput* m, uint32_t code, const std::string& detail) { add_entry(&m->entries, code, detail); }

// An unknown op or an UNSUPPORTED node, one entry per node type (0001 V15).
void note_unsupported(const ar_graph_desc& d, std::vector<MaterialEntry>* entries) {
  for (uint32_t i = 0; d.nodes != nullptr && i < d.node_count; ++i) {
    const ar_graph_node& n = d.nodes[i];
    if (n.op >= uint32_t(Op::COUNT_)) {
      add_entry(entries, AR_REPORT_NODE_UNSUPPORTED, "unknown op " + std::to_string(n.op));
    } else if (Op(n.op) == Op::UNSUPPORTED) {
      std::string type;
      if (n.string_count != 1 || !pool_string(d, n.string_first, &type) || type.empty()) type = "unnamed node";
      add_entry(entries, AR_REPORT_NODE_UNSUPPORTED, type);
    }
  }
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

bool per_point(uint64_t runtime_slots, ar::RecordSlot s) { return ((runtime_slots >> uint32_t(s)) & 1u) != 0; }
bool per_point(const CompileResult& r, ar::RecordSlot s) { return per_point(r.runtime_slots, s); }

// MAY_BE_DELTA of one BSDF child from its own constants and per-point slots.
bool child_may_be_delta(const ar::ConstantBlock& b, uint64_t runtime_slots) {
  using RS = ar::RecordSlot;
  bool delta = lobe_may_be_delta(per_point(runtime_slots, RS::specular_roughness), b.specular_roughness);
  if (per_point(runtime_slots, RS::coat_weight) || b.coat_weight != 0.0f)
    delta = delta || lobe_may_be_delta(per_point(runtime_slots, RS::coat_roughness), b.coat_roughness);
  return delta;
}

void finish(MaterialOutput* m) {
  m->flags = material_flags(m->compiled);
  m->emission_estimate = m->compiled.emission;
  m->images = m->compiled.images;
}

}  // namespace

VolumeOutput compile_volume(const ar_graph_desc& d, const ar::Rgb2SpecTable& table) {
  VolumeOutput out;
  note_unsupported(d, &out.entries);
  if (!out.entries.empty()) return out;
  auto reject = [&](const std::string& why) {
    add_entry(&out.entries, AR_REPORT_PROGRAM_REJECTED, why);
    return out;
  };
  Work w;
  w.table = &table;
  std::string err;
  if (!parse_graph(d, &w.g, &err)) return reject(err);
  const size_t n = w.g.nodes.size();
  // Every op of a volume program must be volume reachable (0006 D7: it has an interval rule).
  for (size_t i = 0; i < n; ++i) {
    const OpMeta& m = op_meta(w.g.nodes[i].op);
    if (!m.volume_reachable)
      return reject("graph node " + std::to_string(i) + " (" + m.name + "): the op has no volume meaning");
  }
  w.topo = stable_topo(w.g);
  w.isc.assign(n, 0);
  w.cv.assign(n, Vec3{0.0f, 0.0f, 0.0f});
  for (int i : w.topo) {
    w.isc[size_t(i)] = fold(w, i, &w.cv[size_t(i)], &err);
    if (!err.empty()) return reject(err);
  }
  ar::MediumBlock block = ar::medium_block_vacuum();
  bool have_absorber = false, density_clamped = false, g_clamped = false;
  uint32_t clamp_flags = 0;
  const float g_max = float(ar::kHgMaxAbsG);
  for (size_t i = 0; i < n; ++i) {
    const GraphNode& nd = w.g.nodes[i];
    if (nd.op != Op::VOLUME_COEFFS) continue;
    const std::string who = "graph node " + std::to_string(i) + " (VOLUME_COEFFS): ";
    // Spatially varying coefficients need grids and the majorant builder, which do not exist yet
    // (0006 A.6); nothing is guessed from the position.
    for (int s = 0; s < 2; ++s)
      if (!w.isc[size_t(nd.input[s])])
        return reject(who + (s == 0 ? "the colour" : "the density") + " is not constant (node " + std::to_string(nd.input[s]) +
                      "); spatially varying media are not built");
    const Vec3& c = w.cv[size_t(nd.input[0])];
    const float density = w.cv[size_t(nd.input[1])][0];
    const float g = w.g.f32[nd.f32_first];
    if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2]) || !std::isfinite(density))
      return reject(who + "the colour and the density must be finite");
    const auto kind = ar::VolumeTerm(w.g.i32[nd.i32_first]);
    const ar::SpectrumRef ref =
        ar::volume_term_ref(table, ar::Rgb{{ar::Real(c[0]), ar::Real(c[1]), ar::Real(c[2])}}, density, kind, &clamp_flags);
    density_clamped = density_clamped || density < 0.0f;
    if (kind == ar::VolumeTerm::ABSORPTION) {
      if (have_absorber) {
        add_entry(&out.entries, AR_REPORT_NODE_UNSUPPORTED, "Volume Absorption: a second absorber (one absorption term per medium)");
        return out;
      }
      have_absorber = true;
      block.sigma_a = ref;
      continue;
    }
    if (block.lobe_count == ar::kMediumMaxLobes) {
      add_entry(&out.entries, AR_REPORT_NODE_UNSUPPORTED, "Volume Scatter: a third phase lobe (at most two per medium)");
      return out;
    }
    // Stored within +-kHgMaxAbsG, as phase.h evaluates it, and reported rather than clamped quietly.
    const float gc = g > g_max ? g_max : (g < -g_max ? -g_max : g);
    g_clamped = g_clamped || gc != g;
    block.sigma_s[block.lobe_count] = ref;
    block.g[block.lobe_count] = gc;
    ++block.lobe_count;
  }
  if (!have_absorber && block.lobe_count == 0) return out;  // no volume node: no medium
  if (clamp_flags != 0) add_entry(&out.entries, AR_REPORT_NODE_APPROXIMATED, "volume RGB clamped");
  if (density_clamped) add_entry(&out.entries, AR_REPORT_NODE_APPROXIMATED, "volume density clamped to 0");
  if (g_clamped)
    add_entry(&out.entries, AR_REPORT_NODE_APPROXIMATED, "volume anisotropy clamped to +-" + std::to_string(g_max).substr(0, 5));
  out.has_medium = true;
  out.medium.block = block;
  out.medium.program = {word(Op::VOLUME_COEFFS), word(Op::END)};
  return out;
}

// In M1a the specular lobe (which the transmission lobe shares) always counts; the coat counts
// unless coat_weight folds to 0, where no coat lobe can be sampled. NON_OPAQUE is clear exactly
// when geometry_opacity is the constant 1, so T folds to 0 (0005 item 43). Cycles agrees on the
// linked case: Ref: third_party/_cache/refs/cycles-ed69ef2c6bd3 src/scene/shader_nodes.cpp
// PrincipledBsdfNode::has_surface_transparent calls a linked Alpha transparent, and
// src/scene/shader.cpp Shader::has_surface_shadow_transparency ORs that over the graph's nodes;
// Cycles also calls alpha < 1 - CLOSURE_WEIGHT_CUTOFF (1e-5f, src/kernel/svm/types.h) transparent,
// a cutoff item 43 does not have;
// EXTENSION is ext_kind != 0 (item 43) and then the OpenPBR lobes do not exist, so the kind alone
// decides MAY_BE_DELTA; READS_RAY_TYPE is clear because no Light Path op exists. A Mix Shader graph ORs MAY_BE_DELTA over its BSDF children, each from its own
// constants, since every child is reachable by the selection.
uint32_t material_flags(const CompileResult& r) {
  using RS = ar::RecordSlot;
  if (r.ext_kind != 0) {
    uint32_t f = ar::EXTENSION;
    if (r.ext_kind == uint32_t(ar::ExtKind::GRATING)) f |= ar::MAY_BE_DELTA;
    if (r.constant_surface) f |= ar::CONSTANT_SURFACE;
    if (!(r.program.size() > 2 && r.program[2] != 0)) f |= ar::CONSTANT_EMISSION;
    return f;
  }
  bool delta = r.children.empty() && child_may_be_delta(r.block, r.runtime_slots);
  for (const ChildConstants& c : r.children) delta = delta || child_may_be_delta(c.block, c.runtime_slots);
  uint32_t flags = delta ? ar::MAY_BE_DELTA : 0u;
  if (per_point(r, RS::geometry_opacity) || r.block.geometry_opacity < 1.0f) flags |= ar::NON_OPAQUE;
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

void set_medium_fields(MaterialOutput* m, uint32_t medium_priority) {
  ar::ConstantBlock& b = m->compiled.block;
  if ((m->compiled.runtime_slots >> uint32_t(ar::RecordSlot::specular_ior) & 1u) != 0) {
    b.interior_ior = ar::make_const_ref(ar::SpectrumSemantic::IOR, ar::Real(1));
    add_entry(m, AR_REPORT_NODE_APPROXIMATED, "textured specular_ior: interior_ior is 1");
  } else if (!m->compiled.children.empty()) {
    // A Mix material's block keeps the defaults; its children's constant IORs are the medium's
    // only when they all agree.
    const ar::SpectrumRef& first = m->compiled.children[0].block.specular_ior;
    bool agree = true;
    for (const ChildConstants& c : m->compiled.children)
      agree = agree && std::memcmp(&c.block.specular_ior, &first, sizeof first) == 0;
    b.interior_ior = agree ? first : ar::make_const_ref(ar::SpectrumSemantic::IOR, ar::Real(1));
    if (!agree) add_entry(m, AR_REPORT_NODE_APPROXIMATED, "Mix Shader children differ in specular_ior: interior_ior is 1");
  } else {
    b.interior_ior = b.specular_ior;
  }
  b.medium_priority = uint8_t(std::min(medium_priority, 255u));
  if (medium_priority > 255u) add_entry(m, AR_REPORT_NODE_APPROXIMATED, "medium_priority clamped to 255");
}

// `extension`: compile_extension's call. It then replaces the flags by the kind's (item 43: no
// NON_OPAQUE), so the entry that follows NON_OPAQUE is for OpenPBR materials only.
static MaterialOutput compile_material_impl(const ar_graph_desc& d, const ar::Rgb2SpecTable& table, const ImageMeanFn* means,
                                            const ImageSlotFn* slots, uint32_t medium_priority, bool extension) {
  // Unsupported nodes first, every one of them, not only the first (0001 V15).
  MaterialOutput unsupported;
  note_unsupported(d, &unsupported.entries);
  if (!unsupported.entries.empty()) {
    MaterialOutput m = magenta_material(table, unsupported.entries[0].code, unsupported.entries[0].detail);
    for (const MaterialEntry& e : unsupported.entries) add_entry(&m, e.code, e.detail);
    return m;
  }
  MaterialOutput m;
  m.compiled = compile_surface(d, table, means, slots);
  if (!m.compiled.ok) return magenta_material(table, AR_REPORT_PROGRAM_REJECTED, m.compiled.error);
  // The (kind, semantic) pairs resolve() evaluates (spectrum_ref.h, 0005 item 9): CONSTANT and RGB
  // anywhere, BLACKBODY in an ILLUMINANT slot, CAUCHY_ABBE in an IOR slot. Every other kind or
  // pair is refused here, so it never reaches resolve().
  for (int s = 0; s < int(ar::Slot::COUNT_); ++s) {
    const ar::SpectrumRef& ref = ar::slot_ref(m.compiled.block, ar::Slot(s));
    const uint8_t k = ref.kind, sem = uint8_t(ar::kSlotSemantic[s]);
    const bool ok = k == uint8_t(ar::SpectrumKind::CONSTANT) || k == uint8_t(ar::SpectrumKind::RGB) ||
                    (k == uint8_t(ar::SpectrumKind::BLACKBODY) && sem == uint8_t(ar::SpectrumSemantic::ILLUMINANT)) ||
                    (k == uint8_t(ar::SpectrumKind::CAUCHY_ABBE) && sem == uint8_t(ar::SpectrumSemantic::IOR));
    if (!ok)
      return magenta_material(table, AR_REPORT_PROGRAM_REJECTED,
                              "SpectrumRef kind " + std::to_string(k) + " is not allowed in slot " + std::to_string(s) +
                                  " (semantic " + std::to_string(sem) + ")");
  }
  finish(&m);
  if (m.compiled.clamp_flags != 0) add_entry(&m, AR_REPORT_NODE_APPROXIMATED, "RGB clamped");
  if (m.compiled.opacity_clamped) add_entry(&m, AR_REPORT_NODE_APPROXIMATED, "opacity clamped");
  // Camera and indirect rays pass through by the opacity decision in shade_surface; shadow rays
  // block on any hit (TERMINATE_ON_FIRST_HIT | DISABLE_ANYHIT, no Embree filter), so the material's
  // shadows are those of an opaque surface (0004 D4's pass-through needs the M2 shadow pass).
  if (!extension && (m.flags & ar::NON_OPAQUE) != 0u)
    add_entry(&m, AR_REPORT_NODE_APPROXIMATED, "opacity: shadow rays treat this material as opaque (transparent shadows are M2)");
  // A per-point opacity multiplies Le as the graph computes it, while the pass decision clamps it
  // to [0, 1]: a value outside that range scales the emission by itself, not by the clamped
  // fraction. A Mix Shader graph does not scale its emission here (no rewire in compile_mixed).
  const uint64_t opacity_bit = uint64_t(1) << uint32_t(ar::RecordSlot::geometry_opacity);
  if (m.compiled.children.empty() && (m.compiled.runtime_slots & opacity_bit) != 0u && !m.compiled.emission.empty())
    add_entry(&m, AR_REPORT_NODE_APPROXIMATED, "opacity: a per-point geometry_opacity scales emission unclamped");
  set_medium_fields(&m, medium_priority);
  for (uint32_t key : m.compiled.missing_images)
    add_entry(&m, AR_REPORT_IMAGE_UNREADABLE, "image key " + std::to_string(key) + " is not set");
  return m;
}

MaterialOutput compile_material(const ar_graph_desc& d, const ar::Rgb2SpecTable& table, const ImageMeanFn* means,
                                const ImageSlotFn* slots, uint32_t medium_priority) {
  return compile_material_impl(d, table, means, slots, medium_priority, false);
}

namespace {

const char* const kRecordSlotName[] = {
#define AR_X(n) #n,
    AR_RECORD_SCALARS(AR_X)
#undef AR_X
#define AR_X(n, s, sem) #n,
    AR_RECORD_SPECTRAL(AR_X)
#undef AR_X
#define AR_X(n) #n,
    AR_RECORD_FRAMES(AR_X)
#undef AR_X
};
static_assert(sizeof(kRecordSlotName) / sizeof(kRecordSlotName[0]) == size_t(ar::RecordSlot::COUNT_), "one name per RecordSlot");

const char* ext_kind_name(uint32_t kind) {
  switch (kind) {
#define AR_EXT_KIND(name, id) \
  case id: return #name;
#include "../../kernel/material/ext_fields.def"
    default: return nullptr;
  }
}

// What an extension keeps of the OpenPBR record: opacity and the frame (item 15; closure weight is
// the selection's and no graph stores it), plus the kind's rows in ext_fields.def (item 16).
bool ext_keeps_slot(uint32_t kind, ar::RecordSlot s) {
  using RS = ar::RecordSlot;
  (void)kind;  // unused while no kind has a field row
  if (s == RS::geometry_opacity || s == RS::geometry_normal || s == RS::geometry_tangent) return true;
#define AR_EXT_FIELD(k, slot) \
  if (kind == uint32_t(ar::ExtKind::k) && s == RS::slot) return true;
#include "../../kernel/material/ext_fields.def"
  return false;
}

uint32_t ext_max_kind() {
  uint32_t m = 0;
#define AR_EXT_KIND(name, id) m = std::max(m, uint32_t(id));
#include "../../kernel/material/ext_fields.def"
  return m;
}

// The kind's check of its constants. GRATING (item 23, layout in ext_params.h): a finite positive
// pitch, |m| <= 8 with m_min <= m_max (at most 17 orders, the table's limit), mode reflect or
// transmit, and in transmission a substrate index the kernel can resolve (CONSTANT or CAUCHY_ABBE,
// IOR semantic). The table handle is the TableArena's to check.
bool ext_params_valid(uint32_t kind, const ar::ExtParams& e, std::string* why) {
  if (kind != uint32_t(ar::ExtKind::GRATING)) return true;
  const ar::GratingParams g = ar::grating_params(e);
  if (!(std::isfinite(g.pitch_nm) && g.pitch_nm > 0.0f)) {
    *why = "grating pitch_nm " + std::to_string(g.pitch_nm) + " is not finite and positive";
    return false;
  }
  if (g.m_min < -8 || g.m_min > 8 || g.m_max < -8 || g.m_max > 8 || g.m_min > g.m_max) {
    *why = "grating orders [" + std::to_string(g.m_min) + ", " + std::to_string(g.m_max) +
           "] need -8 <= m_min <= m_max <= 8";
    return false;
  }
  if (g.mode != uint32_t(ar::GratingMode::Reflect) && g.mode != uint32_t(ar::GratingMode::Transmit)) {
    *why = "grating mode " + std::to_string(g.mode) + " is neither 0 (reflect) nor 1 (transmit)";
    return false;
  }
  if (g.mode == uint32_t(ar::GratingMode::Transmit)) {
    const ar::SpectrumRef& r = g.substrate_ior;
    const bool kind_ok = r.kind == uint8_t(ar::SpectrumKind::CONSTANT) || r.kind == uint8_t(ar::SpectrumKind::CAUCHY_ABBE);
    if (!kind_ok || r.semantic != uint8_t(ar::SpectrumSemantic::IOR) || !(std::isfinite(r.p[0]) && r.p[0] > 0.0f)) {
      *why = "grating substrate IOR must be a CONSTANT or CAUCHY_ABBE IOR ref with a finite positive index";
      return false;
    }
    // The CAUCHY_ABBE domain of graph.cpp's op check: outside it cauchy_abbe_ior gives wrong indices silently.
    if (r.kind == uint8_t(ar::SpectrumKind::CAUCHY_ABBE) &&
        !(std::isfinite(r.p[1]) && r.p[1] > 0.0f && std::isfinite(r.p[2]) && r.p[2] >= 0.0f)) {
      *why = "grating substrate CAUCHY_ABBE needs a finite positive n_d and Abbe number and a finite non-negative dispersion scale";
      return false;
    }
  }
  return true;
}

// `graph` without its STORE_SLOT nodes to slots `kind` does not keep (item 15: those inputs do not
// exist for the material). Roots only they fed drop out of compile_surface's dead-code pass, so a
// refused SpectrumRef or an image on an ignored slot cannot reach the program or the image list.
// A STORE_SLOT whose slot operand is unreadable stays for compile_material to reject.
struct PrunedGraph {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::set<uint32_t> ignored;  // RecordSlot ids
  ar_graph_desc desc;
};

void prune_ignored_stores(const ar_graph_desc& g, uint32_t kind, PrunedGraph* out) {
  std::vector<uint32_t> remap(g.node_count, UINT32_MAX);
  for (uint32_t i = 0; i < g.node_count; ++i) {
    const ar_graph_node& n = g.nodes[i];
    if (n.op == uint32_t(Op::STORE_SLOT) && n.i32_count >= 1 && uint64_t(n.i32_first) < g.i32_count && g.params_i32 != nullptr) {
      const uint32_t s = uint32_t(g.params_i32[n.i32_first]);
      if (s < uint32_t(ar::RecordSlot::COUNT_) && !ext_keeps_slot(kind, ar::RecordSlot(s))) {
        out->ignored.insert(s);
        continue;
      }
    }
    remap[i] = uint32_t(out->nodes.size());
    out->nodes.push_back(n);
  }
  for (uint32_t k = 0; k < g.link_count; ++k) {
    ar_graph_link l = g.links[k];
    // A link naming a missing node is left for compile_material to reject.
    if (l.from_node < g.node_count && remap[l.from_node] == UINT32_MAX) continue;
    if (l.to_node < g.node_count && remap[l.to_node] == UINT32_MAX) continue;
    if (l.from_node < g.node_count) l.from_node = remap[l.from_node];
    if (l.to_node < g.node_count) l.to_node = remap[l.to_node];
    out->links.push_back(l);
  }
  out->desc = g;
  out->desc.node_count = uint32_t(out->nodes.size());
  out->desc.link_count = uint32_t(out->links.size());
  out->desc.nodes = out->nodes.data();
  out->desc.links = out->links.data();
}

}  // namespace

MaterialOutput compile_extension(const ar_material_desc& desc, const ar_graph_desc& graph, const ar::Rgb2SpecTable& table,
                                 const ImageMeanFn* means, const ImageSlotFn* slots) {
  const uint32_t kind = desc.ext_kind;
  const char* name = ext_kind_name(kind);
  if (kind == 0) return magenta_material(table, AR_REPORT_PROGRAM_REJECTED, "internal: compile_extension called with ext_kind 0");
  if (name == nullptr)
    return magenta_material(table, AR_REPORT_PROGRAM_REJECTED,
                            "extension kind " + std::to_string(kind) + " is not in the list (0.." + std::to_string(ext_max_kind()) + ")");
  if (kind != uint32_t(ar::ExtKind::GRATING))
    return magenta_material(table, AR_REPORT_PROGRAM_REJECTED,
                            "extension kind " + std::string(name) + " (" + std::to_string(kind) + ") is not built");
  ar::ExtParams params = {};
  static_assert(sizeof(params) == sizeof(desc.ext_constants), "ext_constants is the ExtParams block");
  std::memcpy(&params, desc.ext_constants, sizeof(params));
  std::string why;
  if (!ext_params_valid(kind, params, &why))
    return magenta_material(table, AR_REPORT_PROGRAM_REJECTED, "extension kind " + std::string(name) + ": " + why);
  // Item 19: no masking. A Mix Shader would select between an extension and an OpenPBR branch.
  for (uint32_t i = 0; i < graph.node_count; ++i)
    if (graph.nodes[i].op == uint32_t(Op::BRANCH_MIX))
      return magenta_material(table, AR_REPORT_PROGRAM_REJECTED,
                              "node " + std::to_string(i) + " (Mix Shader) under extension kind " + std::string(name) +
                                  ": an extension cannot be mixed with another branch (use material slots)");
  PrunedGraph pruned;
  prune_ignored_stores(graph, kind, &pruned);
  MaterialOutput m = compile_material_impl(pruned.desc, table, means, slots, desc.medium_priority, true);
  if (m.magenta) return m;
  m.compiled.ext_kind = kind;
  m.compiled.ext = params;
  m.flags = material_flags(m.compiled);
  // One entry per dropped slot, however many stores named it.
  for (uint32_t s : pruned.ignored)
    add_entry(&m, AR_REPORT_INPUT_IGNORED, std::string(kRecordSlotName[s]) + " is ignored by extension kind " + name);
  return m;
}

}  // namespace astroray::host
