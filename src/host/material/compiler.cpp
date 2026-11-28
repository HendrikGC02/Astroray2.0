// SPDX-License-Identifier: Apache-2.0
#include "compiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "../../kernel/material/lobes/delta.h"
#include "../../kernel/material/lobes/ggx.h"
#include "../../kernel/material/lobes/metal.h"
#include "../../kernel/material/material_header.h"
#include "../../kernel/material/slot_resolve.h"
#include "../../kernel/medium/phase.h"
#include "../../kernel/vm/ops/add_f32.h"
#include "../../kernel/vm/ops/add_f32x3.h"
#include "../../kernel/vm/ops/channel_f32x3.h"
#include "../../kernel/vm/ops/clamp_f32.h"
#include "../../kernel/vm/ops/color_to_f32.h"
#include "../../kernel/vm/ops/const_f32.h"
#include "../../kernel/vm/ops/div_f32.h"
#include "../../kernel/vm/ops/mul_f32.h"
#include "../../kernel/vm/ops/mul_f32x3.h"
#include "../../kernel/vm/ops/mul_f32x3_f32.h"
#include "../../kernel/vm/ops/store_slot.h"
#include "../../kernel/vm/ops/sub_f32.h"
#include "../../kernel/vm/ops/tex_image.h"
#include "../../kernel/vm/ops/volume_coeffs.h"
#include "../data_resource.h"
#include "../spectral/blackbody.h"
#include "graph.h"
#include "grating_authored.h"
#include "grating_table.h"
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
  std::vector<uint32_t> slot;  // TEX_IMAGE and TEX_ENVIRONMENT nodes: the image table slot, or kTexNoImage
  std::map<std::vector<float>, uint32_t> table_at;  // arena offset of each distinct SPECTRUM_TABLE content
  std::set<int> table_noted;                        // SPECTRUM_TABLE nodes already checked against the observer range
};

// The TABLE ref of SPECTRUM_TABLE node `src` for a slot of semantic `sem` (parse_graph checked the
// node): its wavelengths then values enter the material's arena once per distinct content, so two
// slots, or two nodes, with one table share it. Records, once per node, a support that does not
// cover the observer range (0003 d8: the edge values are used outside it, SPECTRUM_OUT_OF_RANGE)
// and, for a REFLECTANCE slot, a scaled value above 1 (the kernel clamps it, 0005 item 10).
// Ref: docs/decisions/0005-material-model.md item 9; 0003 d8.
ar::SpectrumRef table_ref(Work& w, int src, ar::SpectrumSemantic sem, CompileResult* res) {
  const GraphNode& nd = w.g.nodes[size_t(src)];
  const float* pf = w.g.f32.data() + nd.f32_first;
  const uint32_t n = (nd.f32_count - 1u) / 2u;
  std::vector<float> content(2u * n);
  float vmax = 0.0f;
  for (uint32_t k = 0; k < n; ++k) {
    content[k] = pf[1 + 2 * k];
    content[n + k] = pf[2 + 2 * k];
    vmax = std::max(vmax, pf[2 + 2 * k]);
  }
  const auto found = w.table_at.find(content);
  uint32_t at = uint32_t(res->spectra.size());
  if (found != w.table_at.end()) {
    at = found->second;
  } else {
    w.table_at.emplace(content, at);
    res->spectra.insert(res->spectra.end(), content.begin(), content.end());
  }
  if (w.table_noted.insert(src).second) {
    if (content[0] > float(ar::kObsLambdaMin) || content[n - 1] < float(ar::kObsLambdaMax)) {
      char buf[160];
      std::snprintf(buf, sizeof buf, "graph node %d (SPECTRUM_TABLE): support [%g, %g] nm does not cover [%d, %d]; end values used", src,
                    double(content[0]), double(content[n - 1]), ar::kObsLambdaMin, ar::kObsLambdaMax);
      if (!res->spectrum_range.empty()) res->spectrum_range += "; ";
      res->spectrum_range += buf;
    }
  }
  if (sem == ar::SpectrumSemantic::REFLECTANCE && double(pf[0]) * double(vmax) > 1.0) res->spectrum_clamped = true;
  return ar::make_table_ref(sem, at, n, pf[0]);
}

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
    case Op::DIV_F32: (*out)[0] = ar::op_div_f32(a[0], b[0]); return true;
    case Op::CLAMP_F32: (*out)[0] = ar::op_clamp_f32(a[0], f[0], f[1]); return true;
    case Op::MUL_F32X3: ar::op_mul_f32x3(a.data(), b.data(), out->data()); return true;
    case Op::MUL_F32X3_F32: ar::op_mul_f32x3_f32(a.data(), b[0], out->data()); return true;
    case Op::ADD_F32X3: ar::op_add_f32x3(a.data(), b.data(), out->data()); return true;
    case Op::CHANNEL_F32X3: (*out)[0] = ar::op_channel_f32x3(a.data(), uint32_t(w.g.i32[n.i32_first])); return true;
    case Op::COLOR_TO_F32: {
      const float* y = w.table->y_row;
      if (y[0] == 0.0f && y[1] == 0.0f && y[2] == 0.0f) {
        *err = "COLOR_TO_F32 needs the working space's luminance row (Rgb2SpecTable::y_row), which the host never filled";
        return false;
      }
      (*out)[0] = ar::op_color_to_f32(a.data(), y);
      return true;
    }
    default:
      *err = std::string("internal error: ops.def marks ") + op_meta(n.op).name +
             " foldable but compiler.cpp fold() has no case for it";
      return false;
  }
}

// The guide RGB (0008 d15) that a store into colour slot `idx` (Slot order) also sets: base_color
// and specular_color, the two colours the albedo guide reads; null for the other slots.
float* guide_rgb_of(ar::ConstantBlock* b, uint32_t idx) {
  if (idx == uint32_t(ar::Slot::BASE_COLOR)) return b->guide_base_color;
  if (idx == uint32_t(ar::Slot::SPECULAR_COLOR)) return b->guide_specular_color;
  return nullptr;
}

// True when a per-point store into record slot `slot` from a value of type `t` leaves the albedo
// guide without an RGB: base_color or specular_color fed a lifted spectrum. An RGB (or grey) source
// keeps its RGB in the record (store_slot.h), so only this case is magenta (0008 d15).
bool guide_rgb_lost(uint32_t slot, VType t) {
  return (slot == uint32_t(ar::RecordSlot::base_color) || slot == uint32_t(ar::RecordSlot::specular_color)) && t == VType::SPEC;
}

// The working-space RGB of a reflectance TABLE (scale times the n points w, v, clamped to [0, 1]
// as the kernel's resolve clamps a REFLECTANCE slot) under the working space's white, D65:
// XYZ_k = sum R(l) S(l) cmf_k(l) / sum S(l) ybar(l) at the observer's 1 nm samples, then M_ws.
// The table is read with the kernel's own lookup, so the edge values extend as in rendering.
// Ref: pbrt-v4 src/pbrt/film.h PixelSensor::ProjectReflectance (Apache-2.0), read in
// third_party/_cache/refs/pbrt-v4-092b23a47ad4; data_resource.h (observer.bin: CMFs, D65 with
// Y = 1, M_ws XYZ to linear Rec.709); 0008 d15 (the scene-build projection of a spectral table).
bool table_guide_rgb(const float* w, const float* v, uint32_t n, float scale, float out[3], std::string* err) {
  DataBlob blob;
  if (!find_data("spectral/observer.bin", &blob)) return *err = "spectral/observer.bin is not embedded", false;
  ObserverTables obs;
  if (const std::string e = parse_observer(blob.data, blob.size, &obs); !e.empty()) return *err = e, false;
  double xyz[3] = {0.0, 0.0, 0.0}, white_y = 0.0;
  for (uint32_t i = 0; i < kObserverSamples; ++i) {
    const double lam = double(kObserverLambdaMin) + double(i);
    double r = double(scale) * double(ar::table_lookup(w, v, n, ar::Real(lam)));
    r = !(r >= 0.0) ? 0.0 : std::min(r, 1.0);
    const double sw = double(obs.d65[i]);
    for (int k = 0; k < 3; ++k) xyz[k] += r * sw * double(obs.cmf[k][i]);
    white_y += sw * double(obs.cmf[1][i]);
  }
  for (int c = 0; c < 3; ++c) {
    double x = 0.0;
    for (int k = 0; k < 3; ++k) x += obs.m_ws[3 * c + k] * xyz[k] / white_y;
    out[c] = float(x);
  }
  return true;
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
  float* guide = guide_rgb_of(b, idx);
  if (type == VType::F32) {
    float x = v[0];
    if (sem != ar::SpectrumSemantic::IOR) {
      if (!(x >= 0.0f)) x = 0.0f, *flags |= ar::kRgbNegativeClamped;
      if (sem == ar::SpectrumSemantic::REFLECTANCE && x > 1.0f) x = 1.0f, *flags |= ar::kRgbAboveOneClamped;
    }
    ref = ar::make_const_ref(sem, ar::Real(x));
    if (guide) guide[0] = guide[1] = guide[2] = x;
    return true;
  }
  const ar::Rgb rgb{{ar::Real(v[0]), ar::Real(v[1]), ar::Real(v[2])}};
  if (!ar::make_rgb_ref(t, rgb, sem, &ref, flags)) return *err = "specular_ior takes a scalar, not a colour (0005 item 10)", false;
  // The RGB the lift used: a reflectance slot clamps each channel to [0, 1] (rgb2spec.h albedo_coeffs).
  if (guide)
    for (int c = 0; c < 3; ++c) guide[c] = !(v[size_t(c)] >= 0.0f) ? 0.0f : std::min(v[size_t(c)], 1.0f);
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
      case Op::DIV_F32:
      case Op::MUL_F32X3:
      case Op::MUL_F32X3_F32:
      case Op::ADD_F32X3: code->push_back(word(nd.op, a, in(0), in(1))); break;
      case Op::CLAMP_F32:
        code->push_back(word(nd.op, a, in(0)));
        code->push_back(bits(w.g.f32[nd.f32_first]));
        code->push_back(bits(w.g.f32[nd.f32_first + 1]));
        break;
      case Op::COLOR_TO_F32: code->push_back(word(nd.op, a, in(0))); break;
      case Op::TEX_IMAGE:
      case Op::TEX_ENVIRONMENT:
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
  if (n.op == Op::TEX_IMAGE || n.op == Op::TEX_ENVIRONMENT) {
    // The mean of an environment image is its plain texel mean, which the emission estimate of a
    // world never reads for importance (a program world has none).
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
  const bool direct = w.g.nodes[size_t(i)].op == Op::TEX_IMAGE || w.g.nodes[size_t(i)].op == Op::TEX_ENVIRONMENT;
  const bool zero = !(m.v[0] > 0.0f) && !(m.v[1] > 0.0f) && !(m.v[2] > 0.0f);
  return !m.known || (zero && !direct) ? Vec3{1.0f, 1.0f, 1.0f} : m.v;
}

// Resolves every TEX_IMAGE and TEX_ENVIRONMENT node in `used` to its image slot (0005 item 34: a
// missing image is magenta with a report entry); the program depends on the key either way, so
// setting it later recompiles.
void resolve_images(Work* w, const std::vector<char>& used, const ImageSlotFn* slots, std::set<uint32_t>* images,
                    std::set<uint32_t>* missing) {
  w->slot.assign(w->g.nodes.size(), ar::kTexNoImage);
  for (size_t i = 0; i < w->g.nodes.size(); ++i) {
    if (!used[i] || (w->g.nodes[i].op != Op::TEX_IMAGE && w->g.nodes[i].op != Op::TEX_ENVIRONMENT)) continue;
    const uint32_t key = uint32_t(w->g.i32[w->g.nodes[i].i32_first]);
    images->insert(key);
    if (slots != nullptr && !(*slots)(key, &w->slot[i])) w->slot[i] = ar::kTexNoImage, missing->insert(key);
  }
}

// The estimate term of one emission (0005 item 43): the colour's mean (a white spectrum for the
// per-point bound) and the strength's mean times `weight`, the estimate of the factors that scale
// the strength (1 when it is not known, the bound of item 43). Nothing is added for a zero term.
void add_emission_term(const Work& w, int colour, int strength, float weight, MeanCtx* mx, std::vector<EmissionTerm>* out) {
  EmissionTerm term;
  const ar::SpectrumSemantic esem = ar::kSlotSemantic[int(ar::Slot::EMISSION_COLOR)];
  const Vec3 mean = estimate_input(w, colour, mx);
  uint32_t ignored = 0;  // the clamp is reported from the stored colour, in the block or the program
  if (!w.isc[size_t(colour)] && mean[0] == 1.0f && mean[1] == 1.0f && mean[2] == 1.0f)
    term.color = ar::make_const_ref(esem, ar::Real(1));  // the per-point upper bound: a white spectrum
  else
    ar::make_rgb_ref(*w.table, ar::Rgb{{ar::Real(mean[0]), ar::Real(mean[1]), ar::Real(mean[2])}}, esem, &term.color, &ignored);
  const bool zero = !(mean[0] > 0.0f) && !(mean[1] > 0.0f) && !(mean[2] > 0.0f);
  term.strength = estimate_input(w, strength, mx)[0] * weight;
  if (!zero && term.strength > 0.0f) out->push_back(term);
}

// The nodes the roots need: a constant becomes one instruction, so its inputs are not needed.
std::vector<char> mark_from(const Work& w, const std::vector<int>& roots) {
  std::vector<char> m(w.g.nodes.size(), 0);
  std::vector<int> stack = roots;
  while (!stack.empty()) {
    const int i = stack.back();
    stack.pop_back();
    if (m[size_t(i)]) continue;
    m[size_t(i)] = 1;
    if (w.isc[size_t(i)]) continue;
    for (int s = 0; s < w.g.nodes[size_t(i)].n_inputs; ++s) stack.push_back(w.g.nodes[size_t(i)].input[s]);
  }
  return m;
}

// A graph with Mix Shaders (0005 item 38): every STORE_SLOT and EMIT_LE belongs to the child its
// `child` parameter names, and the BRANCH_MIX nodes form one tree over the children (graph.h).
// A child is one of three kinds: Emission (EMIT_LE and no store), Transparent (one constant
// geometry_opacity store and nothing else) or BSDF (a store to any other slot, or no node at all:
// the OpenPBR defaults); anything else is rejected. Emission and Transparent children carry no
// BSDF and are pruned from the selection: a mix whose two subtrees both hold a BSDF child stays a
// BRANCH_MIX, a mix with one such subtree collapses into it, a mix with none selects nothing.
// Weights are by socket (first input 1 - f, second f) and w_i is their product along the child's
// path. A BSDF child i is selected with p_i, the product over its branching mixes, and must give
// w_i a_i f_i in expectation while its null event estimates T = sum_j w_j (1 - a_j) over every
// child. Pruned children are opaque and black (Emission) or transmit 1 - a (Transparent), so the
// collapsed mixes k on i's path (outermost first, own weight s_k, pruned sibling transmitting
// T_k) give the survivor the opacity a'_i and the closure weight
//   1 - a'_i = tau_i + s_i (1 - a_i),  tau_i = sum_k (1 - s_k) T_k prod_{j outer than k} s_j,
//   s_i = prod_k s_k,  closure_weight_i = s_i a_i / a'_i = w_i a_i / (p_i a'_i),
// which is w_i / p_i = s_i with Emission siblings and a_i = 1, and a'_i = s_i a_i with closure
// weight 1 under white Transparent siblings (item 38's two rules). Constant cases fold on the
// host; any other per-point case needs a divide the VM lacks and is rejected.
// The surface entry is (a) the run before the selection: the branching factors, the emission
// segment Le = sum_i w_i a_i Le_i (a_i: emission sits inside the opacity mix, items 4 and 38) and
// every value more than one child reads, computed once and kept live; (b) the selection, BRANCH_MIX
// nodes in preorder; (c) one block per BSDF child with its constants as CONST_STORE code, ending in
// JMP to END (the last block falls into END). The constant block keeps the OpenPBR defaults but a
// constant effective opacity a_eff = 1 - T. The emission entry is (a)'s emission alone; the opacity
// entry stores a per-point a_eff. With no BSDF child the run stores closure_weight 0.
// A dispersive IOR is a block SpectrumRef no block code can store, and a Blackbody emission a block
// ref with a constant strength: both are rejected under a Mix.
// Ref: docs/decisions/0005-material-model.md items 4, 31, 32, 38, 43; pbrt-v4 src/pbrt/materials.h
// MixMaterial (selection by weight) and src/pbrt/util/sampling.h SampleDiscrete; Cycles
// src/kernel/svm/closure.h svm_node_mix_closure (weights by socket, Apache-2.0).
bool compile_mixed(Work& w, const ImageMeanFn* means, const ImageSlotFn* slots, CompileResult* res) {
  using RS = ar::RecordSlot;
  const auto& i32 = w.g.i32;
  const uint32_t opacity_slot = uint32_t(RS::geometry_opacity);
  auto slot_of = [&](int store) { return uint32_t(i32[w.g.nodes[size_t(store)].i32_first]); };
  std::map<int32_t, int> mix_of_tag;
  std::map<int, std::vector<int>> stores_of, emits_of;  // child id -> its nodes, topological order
  for (int i : w.topo) {
    const GraphNode& nd = w.g.nodes[size_t(i)];
    const std::string who = "graph node " + std::to_string(i) + ": ";
    if (nd.op == Op::BRANCH_MIX && !mix_of_tag.emplace(i32[nd.i32_first], i).second)
      return res->error = who + "Mix Shader tag " + std::to_string(i32[nd.i32_first]) + " is used twice", false;
    if (nd.op == Op::EMIT_LE) {
      if (w.g.nodes[size_t(nd.input[0])].op == Op::BLACKBODY)
        return res->error = who + "a Blackbody emission cannot sit under a Mix Shader (its strength must stay constant)", false;
      if (w.g.nodes[size_t(nd.input[0])].op == Op::SPECTRUM_TABLE)
        return res->error = who + "a TABLE emission cannot sit under a Mix Shader (a block ref with a constant strength)", false;
      if (w.isc[size_t(nd.input[1])] && !(w.cv[size_t(nd.input[1])][0] >= 0.0f))
        return res->error = "EMIT_LE strength must not be negative", false;
      emits_of[nd.child].push_back(i);
      continue;
    }
    if (nd.op != Op::STORE_SLOT) continue;
    const uint32_t slot = slot_of(i);
    if (slot == uint32_t(RS::closure_weight))
      return res->error = who + "closure_weight is set by the selection, not stored by a graph", false;
    if (w.g.nodes[size_t(nd.input[0])].op == Op::CAUCHY_ABBE)
      return res->error = who + "a dispersive IOR cannot be stored per Mix Shader child", false;
    if (w.g.nodes[size_t(nd.input[0])].op == Op::SPECTRUM_TABLE)
      return res->error = who + "a TABLE spectrum cannot be stored per Mix Shader child (a block ref no block code can store)", false;
    if (!w.isc[size_t(nd.input[0])] && w.g.nodes[size_t(nd.input[0])].out == VType::F32 && spectral_slot(slot) &&
        slot != uint32_t(RS::specular_ior))
      return res->error = who + "a run-time scalar cannot be stored in a colour slot", false;
    std::vector<int>& list = stores_of[nd.child];
    for (int o : list)
      if (slot_of(o) == slot)
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
  for (const auto* nodes_of : {&stores_of, &emits_of})
    for (const auto& [child, list] : *nodes_of)
      if (child_refs.count(child) == 0)
        return res->error = "child " + std::to_string(child) + (nodes_of == &stores_of ? " stores record slots" : " has an emission") +
                            " but no Mix Shader names it",
               false;
  std::vector<int> roots;
  for (const auto& [tag, m] : mix_of_tag)
    if (tag_refs.count(tag) == 0) roots.push_back(m);
  if (roots.size() != 1) return res->error = "a Mix Shader graph needs exactly one root Mix, found " + std::to_string(roots.size()), false;
  const int32_t root_ref = mix_ref(i32[w.g.nodes[size_t(roots[0])].i32_first]);
  auto mix_node = [&](int32_t ref) { return mix_of_tag[-1 - ref]; };
  auto sub_ref = [&](int32_t ref, int k) { return i32[w.g.nodes[size_t(mix_node(ref))].i32_first + 1u + uint32_t(k)]; };
  // Children in preorder (first subtree before second), and the mixes the walk reached.
  std::vector<int> order;
  size_t mixes_seen = 0;
  {
    std::vector<int32_t> walk{root_ref};
    while (!walk.empty()) {
      const int32_t ref = walk.back();
      walk.pop_back();
      if (ref >= 0) {
        order.push_back(ref);
        continue;
      }
      if (++mixes_seen > mix_of_tag.size()) break;
      walk.push_back(sub_ref(ref, 1));  // popped after the first subtree
      walk.push_back(sub_ref(ref, 0));
    }
  }
  if (mixes_seen != mix_of_tag.size()) return res->error = "Mix Shader tags form a cycle or a second tree", false;

  // New nodes: constants and arithmetic on constants fold at once (the interpreter's functions).
  auto push = [&](const GraphNode& nd) {
    w.g.nodes.push_back(nd);
    w.isc.push_back(0);
    w.cv.push_back(Vec3{0.0f, 0.0f, 0.0f});
    const int i = int(w.g.nodes.size()) - 1;
    w.isc[size_t(i)] = fold(w, i, &w.cv[size_t(i)], &res->error);
    return i;
  };
  auto konst = [&](float v) {
    GraphNode c;
    c.op = Op::CONST_F32;
    c.f32_first = uint32_t(w.g.f32.size());
    c.out = VType::F32;
    w.g.f32.push_back(v);
    return push(c);
  };
  auto is_const = [&](int i, float v) { return w.isc[size_t(i)] && w.cv[size_t(i)][0] == v; };
  auto arith = [&](Op op, int a, int b) {
    GraphNode m;
    m.op = op;
    m.input[0] = a, m.input[1] = b;
    m.n_inputs = 2;
    m.out = VType::F32;
    return push(m);
  };
  const int zero = konst(0.0f), one = konst(1.0f);
  // The shortcuts keep a pruned Emission sibling's T = 0 an exact constant, so a per-point weight
  // times it does not turn into code (or into a divide the VM lacks).
  auto mul = [&](int a, int b) {
    if (is_const(a, 1.0f)) return b;
    if (is_const(b, 1.0f)) return a;
    if (is_const(a, 0.0f) || is_const(b, 0.0f)) return zero;
    return arith(Op::MUL_F32, a, b);
  };
  auto add = [&](int a, int b) {
    if (is_const(a, 0.0f)) return b;
    if (is_const(b, 0.0f)) return a;
    return arith(Op::ADD_F32, a, b);
  };
  auto sub = [&](int a, int b) { return is_const(b, 0.0f) ? a : arith(Op::SUB_F32, a, b); };
  auto clamp01 = [](float v) { return std::min(std::max(v, 0.0f), 1.0f); };
  // A value clamped to [0, 1]: a constant on the host, as the block clamps one, a per-point one by
  // CLAMP_F32. The weights, the emission and the selection's factor then read one clamped value.
  auto clamped = [&](int v) {
    if (w.isc[size_t(v)]) return konst(clamp01(w.cv[size_t(v)][0]));
    GraphNode c;
    c.op = Op::CLAMP_F32;
    c.f32_first = uint32_t(w.g.f32.size());
    c.f32_count = 2;
    w.g.f32.push_back(0.0f);
    w.g.f32.push_back(1.0f);
    c.input[0] = v;
    c.n_inputs = 1;
    c.out = VType::F32;
    return push(c);
  };
  auto new_store = [&](int child, RS slot, int src) {
    GraphNode st;
    st.op = Op::STORE_SLOT;
    st.i32_first = uint32_t(w.g.i32.size());
    w.g.i32.push_back(int32_t(slot));
    w.g.i32.push_back(child);
    st.input[0] = src;
    st.n_inputs = 1;
    st.child = child;
    return push(st);
  };

  // Child kinds, and each child's own opacity a_i: a constant one clamped to [0, 1] and reported
  // as without a Mix, a per-point one through CLAMP_F32, as the opacity decision clamps it.
  enum class Kind { BSDF, EMISSION, TRANSPARENT };
  std::map<int, Kind> kind_of;
  std::map<int, int> opacity_store_of, opacity_of;  // child -> its geometry_opacity store, its a_i
  for (int c : order) {
    int op_store = -1;
    bool bsdf_store = false;
    for (int st : stores_of[c]) {
      if (slot_of(st) == opacity_slot) op_store = st;
      else bsdf_store = true;
    }
    const bool emits = !emits_of[c].empty();
    if (op_store >= 0) {
      const int v = w.g.nodes[size_t(op_store)].input[0];
      if (w.isc[size_t(v)] && clamp01(w.cv[size_t(v)][0]) != w.cv[size_t(v)][0]) {
        const int clamped = konst(clamp01(w.cv[size_t(v)][0]));  // konst may move the node array
        w.g.nodes[size_t(op_store)].input[0] = clamped;
        res->opacity_clamped = true;
      }
      opacity_store_of[c] = op_store;
    }
    const int src = op_store >= 0 ? w.g.nodes[size_t(op_store)].input[0] : one;
    opacity_of[c] = w.isc[size_t(src)] ? src : clamped(src);  // clamped() may move the node array
    const std::string who = "Mix Shader child " + std::to_string(c) + ": ";
    if (bsdf_store || (op_store < 0 && !emits)) {
      kind_of[c] = Kind::BSDF;
    } else if (op_store < 0) {
      kind_of[c] = Kind::EMISSION;
    } else if (!emits && w.isc[size_t(src)]) {
      kind_of[c] = Kind::TRANSPARENT;
      res->transparent_folded = res->transparent_folded || w.cv[size_t(src)][0] != 0.0f;
    } else {
      return res->error = who + (emits ? "an emission with an opacity and no BSDF store" : "a per-point opacity with no BSDF store") +
                          " is none of the child kinds (Emission, Transparent with a constant opacity, BSDF)", false;
    }
  }
  // Socket weights (1 - f, f) per mix, f clamped to [0, 1] as BRANCH_MIX clamps it for the
  // selection (CLAMP_F32 for a per-point factor).
  std::map<int, std::array<int, 2>> side_w;
  for (const auto& [tag, m] : mix_of_tag) {
    const int f = clamped(w.g.nodes[size_t(m)].input[0]);  // clamped() may move the node array
    side_w[m] = {sub(one, f), f};
  }
  std::map<int32_t, bool> has_bsdf_memo;
  std::function<bool(int32_t)> has_bsdf = [&](int32_t ref) -> bool {
    if (ref >= 0) return kind_of[ref] == Kind::BSDF;
    const auto f = has_bsdf_memo.find(ref);
    if (f != has_bsdf_memo.end()) return f->second;
    const bool a = has_bsdf(sub_ref(ref, 0)), b = has_bsdf(sub_ref(ref, 1));
    return has_bsdf_memo[ref] = a || b;
  };
  // What a pruned subtree transmits, and whether it is white Transparent children only (T = 1).
  std::function<std::pair<int, bool>(int32_t)> transmits = [&](int32_t ref) -> std::pair<int, bool> {
    if (ref >= 0) {
      if (kind_of[ref] == Kind::EMISSION) return {zero, false};
      return {sub(one, opacity_of[ref]), is_const(opacity_of[ref], 0.0f)};
    }
    const std::array<int, 2>& sw = side_w[mix_node(ref)];
    const auto a = transmits(sub_ref(ref, 0)), b = transmits(sub_ref(ref, 1));
    return {add(mul(sw[0], a.first), mul(sw[1], b.first)), a.second && b.second};
  };
  struct Path {
    int w, s, tau;   // full weight; product and transmittance of the collapsed mixes so far
    bool collapsed;  // a collapsed mix lies on the path
    bool white;      // every pruned sibling on the path is white Transparent
  };
  std::map<int, Path> path_of;
  std::function<void(int32_t, Path)> walk = [&](int32_t ref, Path p) {
    if (ref >= 0) {
      path_of[ref] = p;
      return;
    }
    const std::array<int, 2>& sw = side_w[mix_node(ref)];
    const bool hb[2] = {has_bsdf(sub_ref(ref, 0)), has_bsdf(sub_ref(ref, 1))};
    for (int k = 0; k < 2; ++k) {
      Path q = p;
      q.w = mul(p.w, sw[k]);
      if (hb[k] && !hb[1 - k]) {
        const auto t = transmits(sub_ref(ref, 1 - k));
        q.tau = add(p.tau, mul(p.s, mul(sw[1 - k], t.first)));
        q.s = mul(p.s, sw[k]);
        q.collapsed = true;
        q.white = p.white && t.second;
      }
      walk(sub_ref(ref, k), q);
    }
  };
  walk(root_ref, Path{one, one, zero, false, true});
  if (!res->error.empty()) return false;  // a fold failed

  // Emission segment: each EMIT_LE strength times w_c a_c. The estimate keeps the authored
  // strength and multiplies by the weight's estimate, in which only per-point factors are bounded.
  struct Emit {
    int colour, strength, weight;
  };
  std::vector<Emit> emitted;
  std::vector<int> emit_nodes;
  std::set<int> strength_nodes;
  for (int c : order)
    for (int e : emits_of[c]) {
      const int weight = mul(path_of[c].w, opacity_of[c]);
      const int strength = w.g.nodes[size_t(e)].input[1];
      emitted.push_back({w.g.nodes[size_t(e)].input[0], strength, weight});
      int scaled = mul(strength, weight);  // mul may move the node array
      // mul hands back a shared node (the zero of a weight of 0, a weight both children computed):
      // each EMIT_LE needs its own strength value, so a second reader gets a copy through x * 1.
      if (!strength_nodes.insert(scaled).second) scaled = arith(Op::MUL_F32, scaled, one);
      strength_nodes.insert(scaled);
      w.g.nodes[size_t(e)].input[1] = scaled;
      emit_nodes.push_back(e);
    }
  // The BSDF children's opacity and closure weight under collapsed mixes.
  std::map<int, int> inst_of;  // BSDF child id -> instance (1-based layout index)
  std::vector<int> border;     // BSDF children in layout order
  for (int c : order) {
    if (kind_of[c] != Kind::BSDF) continue;
    border.push_back(c);
    inst_of[c] = int(border.size());
    const Path& p = path_of[c];
    if (!p.collapsed) continue;
    const int a = opacity_of[c];
    int a2 = -1, cw = -1;  // new opacity source and closure weight; -1: unchanged
    if (w.isc[size_t(p.s)] && w.isc[size_t(p.tau)] && w.isc[size_t(a)]) {
      const double s = w.cv[size_t(p.s)][0], tau = w.cv[size_t(p.tau)][0], av = w.cv[size_t(a)][0];
      const double ap = 1.0 - tau - s * (1.0 - av);
      a2 = konst(float(ap));
      cw = konst(ap > 0.0 ? float(s * av / ap) : 0.0f);  // a' = 0: the surface is never taken
    } else if (p.white) {
      a2 = mul(p.s, a);
    } else if (is_const(p.tau, 0.0f) && is_const(a, 1.0f)) {
      cw = p.s;
    } else {
      // 1 - a' = tau + s (1 - a) and closure_weight = s a / a' (item 38). A constant a' of 0 never
      // takes the surface, so its weight is 0 as in the constant branch above.
      a2 = sub(one, add(p.tau, mul(p.s, sub(one, a))));
      cw = w.isc[size_t(a2)] && !(w.cv[size_t(a2)][0] > 0.0f) ? zero : arith(Op::DIV_F32, mul(p.s, a), a2);
    }
    if (a2 >= 0) {
      if (opacity_store_of.count(c) != 0) w.g.nodes[size_t(opacity_store_of[c])].input[0] = a2;
      else stores_of[c].push_back(opacity_store_of[c] = new_store(c, RS::geometry_opacity, a2));
    }
    if (cw >= 0 && !is_const(cw, 1.0f)) stores_of[c].push_back(new_store(c, RS::closure_weight, cw));
  }
  // T = sum_j w_j (1 - a_j) over every child, from the children's own opacities.
  int t = zero;
  for (int c : order)
    if (!is_const(opacity_of[c], 1.0f)) {
      t = add(t, mul(path_of[c].w, sub(one, opacity_of[c])));
    }
  const int a_eff = sub(one, t);
  int opacity_store = -1;  // the per-point a_eff store, the opacity entry's root
  if (w.isc[size_t(a_eff)]) {
    res->block.geometry_opacity = clamp01(w.cv[size_t(a_eff)][0]);
    // The block now holds a_eff, so each BSDF child states its own opacity.
    if (res->block.geometry_opacity != 1.0f)
      for (int c : border)
        if (opacity_store_of.count(c) == 0) stores_of[c].push_back(opacity_store_of[c] = new_store(c, RS::geometry_opacity, one));
  } else {
    opacity_store = new_store(-1, RS::geometry_opacity, a_eff);
  }
  const bool any_bsdf = !border.empty();
  const int no_bsdf_store = any_bsdf ? -1 : new_store(-1, RS::closure_weight, zero);
  if (!res->error.empty()) return false;
  w.topo = stable_topo(w.g);
  const size_t n = w.g.nodes.size();

  // Which instance evaluates each run-time node: one child's block, or the shared run when the
  // branching factors, the emission or two children read it. Constants are materialised where read.
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
  auto branching = [&](int m) {
    const int32_t ref = mix_ref(i32[w.g.nodes[size_t(m)].i32_first]);
    return has_bsdf(sub_ref(ref, 0)) && has_bsdf(sub_ref(ref, 1));
  };
  for (int c : border)
    for (int st : stores_of[c])
      if (!w.isc[size_t(w.g.nodes[size_t(st)].input[0])]) claim(st, inst_of[c]);
  for (const auto& [tag, m] : mix_of_tag)
    if (branching(m)) claim(w.g.nodes[size_t(m)].input[0], kShared);
  for (int e : emit_nodes) claim(e, kShared);
  if (!any_bsdf) {
    claim(no_bsdf_store, kShared);
    if (opacity_store >= 0) claim(opacity_store, kShared);
  }
  auto inst_of_child = [&](int child) {
    const auto f = inst_of.find(child);
    return f == inst_of.end() ? kNone : f->second;
  };
  // Constants each instance reads (a branching factor is read by the shared run).
  std::vector<std::set<int>> reads(border.size() + 1);
  for (size_t i = 0; i < n; ++i) {
    const GraphNode& nd = w.g.nodes[i];
    const int inst = nd.op == Op::BRANCH_MIX ? (branching(int(i)) ? kShared : kNone) : owner[i];
    if (inst == kNone) continue;
    for (int s = 0; s < nd.n_inputs; ++s)
      if (w.isc[size_t(nd.input[s])]) reads[size_t(inst)].insert(nd.input[s]);
  }
  auto block = [&](int inst, std::vector<Item>* items) {
    for (int i : w.topo) {
      const GraphNode& nd = w.g.nodes[size_t(i)];
      if (owner[size_t(i)] == inst || reads[size_t(inst)].count(i) != 0) items->push_back({Item::NODE, i, inst, -1});
      else if (inst != kShared && nd.op == Op::STORE_SLOT && inst_of_child(nd.child) == inst && w.isc[size_t(nd.input[0])])
        items->push_back({Item::CONST_STORE, i, inst, -1});
    }
  };
  std::vector<Item> items;
  block(kShared, &items);
  if (any_bsdf) {
    // Preorder over the branching mixes: BRANCH, its first subtree, then its second subtree, whose
    // first item is the BRANCH's target (`branch` names the BRANCH item a second subtree belongs
    // to). A collapsed mix hands its place to the subtree that holds the BSDF children.
    struct Step {
      int32_t ref;
      long branch;
    };
    std::vector<Step> st{{root_ref, -1}};
    while (!st.empty()) {
      const Step s = st.back();
      st.pop_back();
      if (s.ref < 0 && !(has_bsdf(sub_ref(s.ref, 0)) && has_bsdf(sub_ref(s.ref, 1)))) {
        st.push_back({sub_ref(s.ref, has_bsdf(sub_ref(s.ref, 0)) ? 0 : 1), s.branch});
        continue;
      }
      if (s.branch >= 0) items[size_t(s.branch)].target = int(items.size());
      if (s.ref >= 0) {
        block(inst_of[s.ref], &items);
        items.push_back({Item::JMP_END, -1, inst_of[s.ref], -1});
        continue;
      }
      const int m = mix_node(s.ref);
      items.push_back({Item::BRANCH, m, kShared, -1});
      const long b = long(items.size()) - 1;
      st.push_back({sub_ref(s.ref, 1), b});
      st.push_back({sub_ref(s.ref, 0), -1});
    }
  }
  if (!items.empty() && items.back().kind == Item::JMP_END) items.pop_back();  // the last block falls into END

  const std::vector<char> emit_set = mark_from(w, emit_nodes);
  const std::vector<char> opacity_set = opacity_store >= 0 ? mark_from(w, {opacity_store}) : std::vector<char>(n, 0);
  std::set<uint32_t> images, missing;
  std::vector<char> used(n, 0);
  for (size_t i = 0; i < n; ++i) used[i] = owner[i] != kNone || opacity_set[i];
  resolve_images(&w, used, slots, &images, &missing);
  std::vector<uint32_t> surface, emission, opacity;
  if (!build_code(w, items, &surface, &res->error)) return false;
  if (!emit_nodes.empty() && !build_code(w, plain_layout(w, emit_set, emit_set), &emission, &res->error)) return false;
  if (opacity_store >= 0 && !build_code(w, plain_layout(w, opacity_set, std::vector<char>(n, 0)), &opacity, &res->error))
    return false;
  res->program.assign(4, 0u);
  res->program[0] = 4;
  res->program.insert(res->program.end(), surface.begin(), surface.end());
  if (!emit_nodes.empty()) {
    res->program[2] = uint32_t(res->program.size());
    res->program.insert(res->program.end(), emission.begin(), emission.end());
  }
  if (opacity_store >= 0) {
    res->program[1] = uint32_t(res->program.size());
    res->program.insert(res->program.end(), opacity.begin(), opacity.end());
    res->runtime_slots |= uint64_t(1) << opacity_slot;
  }
  // The emission estimate (item 43): sum over the emitters of their weighted terms.
  MeanCtx mx{means, &images, std::vector<Mean>(n), std::vector<char>(n, 0)};
  // The weight w_i a_i is a product: its constant factors count at their value and only a
  // per-point factor without a known mean is bounded by 1 (item 43), so a small constant weight
  // keeps the estimate small.
  std::function<float(int)> weight_estimate = [&](int i) -> float {
    if (w.isc[size_t(i)]) return w.cv[size_t(i)][0];
    if (w.g.nodes[size_t(i)].op == Op::MUL_F32)
      return weight_estimate(w.g.nodes[size_t(i)].input[0]) * weight_estimate(w.g.nodes[size_t(i)].input[1]);
    const Mean m = mean_of(w, i, &mx);
    return m.known ? m.v[0] : 1.0f;
  };
  for (const Emit& e : emitted) add_emission_term(w, e.colour, e.strength, weight_estimate(e.weight), &mx, &res->emission);
  res->images.assign(images.begin(), images.end());
  res->missing_images.assign(missing.begin(), missing.end());
  // Each BSDF child's constants over the defaults, for the material flags and the clamp report.
  for (int child : border) {
    ChildConstants c;
    c.id = child;
    c.block = res->block;
    for (int st : stores_of[child]) {
      const GraphNode& nd = w.g.nodes[size_t(st)];
      const uint32_t slot = slot_of(st);
      // A child's colours are code: STORE_SLOT keeps an RGB source for the albedo guide, a lifted
      // spectrum has none (0008 d15).
      if (guide_rgb_lost(slot, w.g.nodes[size_t(nd.input[0])].out)) res->block.guide_flags |= ar::GUIDE_RGB_RUNTIME;
      if (!w.isc[size_t(nd.input[0])]) {
        c.runtime_slots |= uint64_t(1) << slot;
        continue;
      }
      if (slot == uint32_t(RS::closure_weight)) continue;  // the selection's, no block slot
      if (!store_const(&c.block, *w.table, slot, w.g.nodes[size_t(nd.input[0])].out, w.cv[size_t(nd.input[0])],
                       &res->clamp_flags, &res->error))
        return false;
    }
    // A selection weight that folded to 0 means no sample ever takes this child (item 43 "reachable").
    const int pw = path_of[child].w;
    c.reachable = !(w.isc[size_t(pw)] && !(w.cv[size_t(pw)][0] > 0.0f));
    res->runtime_slots |= c.runtime_slots;
    res->children.push_back(c);
  }
  return true;
}

// Emission inside the opacity mix (0005 item 4: top = mix(transparent, surface, opacity), the
// emission under the surface): every EMIT_LE strength becomes strength x a, a new MUL_F32 node, so
// the hit, the emission entry that NEE on an emissive triangle runs and the emission estimate all
// read a x Le from one program, and the kernel scales nothing again. A per-point opacity enters the
// product through CLAMP_F32, the clamp the opacity decision applies to the store's own source (item 38);
// a constant one is the clamped value the block holds, folded into the product (a lone constant
// emission then lands in the block's luminance as before). An opacity of exactly 1 changes nothing.
// A NaN constant never gets here: compile_surface refuses every non-finite constant first.
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
  // A Blackbody or TABLE emission is a block ref with a constant strength (items 9, 39): it cannot
  // take a per-point factor.
  if (!w.isc[size_t(src)])
    for (int e : emits) {
      const Op colour = w.g.nodes[size_t(w.g.nodes[size_t(e)].input[0])].op;
      if (colour == Op::BLACKBODY || colour == Op::SPECTRUM_TABLE)
        return *err = "graph node " + std::to_string(e) + " (EMIT_LE): a " + (colour == Op::BLACKBODY ? "Blackbody" : "TABLE") +
                      " emission needs a constant geometry_opacity", false;
    }
  int factor = src;
  if (w.isc[size_t(src)]) {
    const float v = w.cv[size_t(src)][0];
    if (v >= 1.0f) return true;  // above 1 clamps to 1; a NaN never gets here (compile_surface refuses it)
    if (v < 0.0f) {
      GraphNode c;
      c.op = Op::CONST_F32;
      c.f32_first = uint32_t(w.g.f32.size());
      c.out = VType::F32;
      w.g.f32.push_back(0.0f);
      w.g.nodes.push_back(c);
      factor = int(w.g.nodes.size()) - 1;
    }
  } else {
    // The decision clamps a per-point opacity to [0, 1]; the emission takes the same clamped value.
    GraphNode c;
    c.op = Op::CLAMP_F32;
    c.f32_first = uint32_t(w.g.f32.size());
    c.f32_count = 2;
    w.g.f32.push_back(0.0f);
    w.g.f32.push_back(1.0f);
    c.input[0] = src;
    c.n_inputs = 1;
    c.out = VType::F32;
    w.g.nodes.push_back(c);
    factor = int(w.g.nodes.size()) - 1;
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

// A constant that is NaN or Inf anywhere in the graph is refused, naming the node (and the record
// slot a STORE_SLOT gives it), even when it would fold into a finite value or sit in a branch no
// sample takes: a graph that carries one is a broken input, and the run-time check in STORE_SLOT
// would otherwise turn every sample INVALID (rule 5, ruling 91; 0005 item 32, "Non-finite input").
// Ref: docs/decisions/RULINGS.md 91; 0004 D17.
bool refuse_non_finite_constants(const Work& w, std::string* err) {
  for (size_t i = 0; i < w.g.nodes.size(); ++i) {
    if (!w.isc[i]) continue;
    const Vec3& v = w.cv[i];
    const int n = w.g.nodes[i].out == VType::F32 ? 1 : 3;
    bool finite = true;
    for (int k = 0; k < n; ++k) finite = finite && (v[size_t(k)] - v[size_t(k)] == 0.0f);
    if (finite) continue;
    *err = "graph node " + std::to_string(i) + " (" + op_meta(w.g.nodes[i].op).name +
           "): a non-finite constant, NaN or Inf";
    for (const GraphNode& c : w.g.nodes)
      for (int s = 0; s < c.n_inputs; ++s)
        if (c.op == Op::STORE_SLOT && c.input[s] == int(i)) *err += ", stored to record slot " + std::to_string(w.g.i32[c.i32_first]);
    return false;
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
  // A per-point COLOR_TO_F32 would fail at run time on a table without the luminance row.
  for (size_t i = 0; i < n; ++i)
    if (w.g.nodes[i].op == Op::COLOR_TO_F32 && table.y_row[0] == 0.0f && table.y_row[1] == 0.0f && table.y_row[2] == 0.0f)
      return res.error = "graph node " + std::to_string(i) + " (COLOR_TO_F32): the working-space table has no luminance row", res;
  w.topo = stable_topo(w.g);
  w.isc.assign(n, 0);
  w.cv.assign(n, Vec3{0.0f, 0.0f, 0.0f});
  for (int i : w.topo) {
    w.isc[size_t(i)] = fold(w, i, &w.cv[size_t(i)], &res.error);
    if (!res.error.empty()) return res;
  }

  if (!refuse_non_finite_constants(w, &res.error)) return res;
  res.block = ar::default_constant_block(table);
  if (std::any_of(w.g.nodes.begin(), w.g.nodes.end(), [](const GraphNode& nd) { return nd.op == Op::BRANCH_MIX; })) {
    res.ok = compile_mixed(w, means, slots, &res);
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
      } else if (w.g.nodes[size_t(src)].op == Op::SPECTRUM_TABLE) {
        // parse_graph admits this source only for a colour slot: one TABLE ref in the block, its
        // points in the arena.
        const uint32_t idx = slot - uint32_t(ar::RecordSlot::base_color);  // RecordSlot's spectral order is Slot's
        ar::slot_ref(res.block, ar::Slot(idx)) = table_ref(w, src, ar::kSlotSemantic[idx], &res);
        if (float* guide = guide_rgb_of(&res.block, idx)) {
          const GraphNode& tn = w.g.nodes[size_t(src)];
          const float* tf = w.g.f32.data() + tn.f32_first;
          const uint32_t np = (tn.f32_count - 1u) / 2u;
          std::vector<float> tw(np), tv(np);
          for (uint32_t k = 0; k < np; ++k) tw[k] = tf[1 + 2 * k], tv[k] = tf[2 + 2 * k];
          if (!table_guide_rgb(tw.data(), tv.data(), np, tf[0], guide, &res.error)) return res;
        }
      } else if (w.isc[size_t(src)]) {
        Vec3 v = w.cv[size_t(src)];
        if (slot == opacity_slot) {  // a fraction: clamped and reported like a colour (0005 item 32)
          if (v[0] < 0.0f || v[0] > 1.0f) v[0] = std::min(std::max(v[0], 0.0f), 1.0f), res.opacity_clamped = true;
        }
        if (!store_const(&res.block, table, slot, t, v, &res.clamp_flags, &res.error)) return res;
      } else {
        if (t == VType::F32 && slot >= uint32_t(ar::RecordSlot::base_color) && slot < uint32_t(ar::RecordSlot::geometry_normal))
          return res.error = "a run-time scalar cannot be stored in a colour slot", res;
        if (slot == uint32_t(ar::RecordSlot::closure_weight))
          return res.error = "closure_weight is set by the selection, not stored by a graph", res;
        if (guide_rgb_lost(slot, t)) res.block.guide_flags |= ar::GUIDE_RGB_RUNTIME;
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
        ar::SpectrumRef& r = res.block.emission_color;
        try {
          r = make_blackbody_ref(double(pf[0]), double(pf[1]), y_norm, double(res.block.emission_luminance));
        } catch (const std::exception& e) {
          return res.error = who + e.what(), res;
        }
        EmissionTerm bt;
        bt.color = r;
        bt.strength = res.block.emission_luminance;
        if (bt.strength > 0.0f && pf[1] > 0.0f) res.emission.push_back(bt);
        continue;
      }
      if (colour_node.op == Op::SPECTRUM_TABLE) {
        // As the Blackbody above: one constant-strength TABLE emission is a block ref, any other shape
        // would need the arena in the program, which no op reads.
        const std::string who = "graph node " + std::to_string(nd.input[0]) + " (SPECTRUM_TABLE): ";
        if (emit_count != 1) return res.error = who + "a TABLE emission must be the material's only emission", res;
        if (!w.isc[size_t(nd.input[1])]) return res.error = who + "the emission strength must be constant", res;
        const ar::SpectrumSemantic tsem = ar::kSlotSemantic[int(ar::Slot::EMISSION_COLOR)];
        res.block.emission_color = table_ref(w, nd.input[0], tsem, &res);
        res.block.emission_luminance = w.cv[size_t(nd.input[1])][0];
        {  // the strength multiplies the resolved spectrum in float, as the Blackbody check above does
          const float* tf = w.g.f32.data() + colour_node.f32_first;
          double vmax = 0.0;
          for (uint32_t k = 1; k < colour_node.f32_count; k += 2) vmax = std::max(vmax, double(tf[k + 1]));
          if (!(double(tf[0]) * vmax * double(res.block.emission_luminance) <= double(std::numeric_limits<float>::max())))
            return res.error = who + "the scale, the values and the strength overflow a float", res;
        }
        // The estimate (item 43) is the table's mean over the observer range as a CONSTANT term: the
        // light builder integrates terms without an arena, and the estimate only picks sidedness and
        // light importance, never a rendered value.
        const float* pf = w.g.f32.data() + colour_node.f32_first;
        const uint32_t np = (colour_node.f32_count - 1u) / 2u;
        const double lo = double(ar::kObsLambdaMin), hi = double(ar::kObsLambdaMax);
        double area = 0.0;
        for (uint32_t k = 0; k + 1 < np; ++k) {
          const double w0 = double(pf[1 + 2 * k]), w1 = double(pf[1 + 2 * (k + 1)]);
          const double v0 = double(pf[2 + 2 * k]), v1 = double(pf[2 + 2 * (k + 1)]);
          const double a = std::max(w0, lo), b = std::min(w1, hi);
          if (!(b > a)) continue;
          area += 0.5 * ((v0 + (v1 - v0) * (a - w0) / (w1 - w0)) + (v0 + (v1 - v0) * (b - w0) / (w1 - w0))) * (b - a);
        }
        // The edge values extend past the support.
        area += double(pf[2]) * std::max(0.0, std::min(double(pf[1]), hi) - lo);
        area += double(pf[2 * np]) * std::max(0.0, hi - std::max(double(pf[2 * np - 1]), lo));
        EmissionTerm tt;
        tt.color = ar::make_const_ref(tsem, ar::Real(double(pf[0]) * area / (hi - lo)));
        tt.strength = res.block.emission_luminance;
        if (tt.strength > 0.0f && tt.color.p[0] > 0.0f) res.emission.push_back(tt);
        continue;
      }
      add_emission_term(w, nd.input[0], nd.input[1], 1.0f, &mx, &res.emission);
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
// constants, since every child whose selection weight is not a folded 0 is reachable.
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
  for (const ChildConstants& c : r.children) delta = delta || (c.reachable && child_may_be_delta(c.block, c.runtime_slots));
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
  m.compiled.block.guide_base_color[0] = m.compiled.block.guide_base_color[2] = 1.0f;
  m.compiled.block.guide_base_color[1] = 0.0f;
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
  // compile_surface marked a per-point base_color or specular_color fed a lifted spectrum: the albedo
  // guide has no RGB for it and is magenta (0008 d15), said once per material.
  if ((m.compiled.block.guide_flags & ar::GUIDE_RGB_RUNTIME) != 0)
    add_entry(&m, AR_REPORT_NODE_APPROXIMATED,
              "per-point spectral base_color or specular_color: no RGB for the denoiser albedo guide, which is magenta");
  // The (kind, semantic) pairs resolve() evaluates (spectrum_ref.h, 0005 item 9): CONSTANT and RGB
  // anywhere, TABLE in every slot but an IOR one, BLACKBODY in an ILLUMINANT slot, CAUCHY_ABBE in
  // an IOR slot. Every other kind or pair is refused here, so it never reaches resolve().
  for (int s = 0; s < int(ar::Slot::COUNT_); ++s) {
    const ar::SpectrumRef& ref = ar::slot_ref(m.compiled.block, ar::Slot(s));
    const uint8_t k = ref.kind, sem = uint8_t(ar::kSlotSemantic[s]);
    const bool ok = k == uint8_t(ar::SpectrumKind::CONSTANT) || k == uint8_t(ar::SpectrumKind::RGB) ||
                    (k == uint8_t(ar::SpectrumKind::TABLE) && sem != uint8_t(ar::SpectrumSemantic::IOR)) ||
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
  if (m.compiled.transparent_folded)
    add_entry(&m, AR_REPORT_NODE_APPROXIMATED, "Transparent under a Mix Shader: colour taken as its mean (exact only for grey)");
  if (m.compiled.spectrum_clamped) add_entry(&m, AR_REPORT_NODE_APPROXIMATED, "TABLE reflectance above 1 clamped to 1");
  if (!m.compiled.spectrum_range.empty()) add_entry(&m, AR_REPORT_SPECTRUM_OUT_OF_RANGE, m.compiled.spectrum_range);
  // Camera and indirect rays pass through by the opacity decision in shade_surface; shadow rays
  // block on any hit (TERMINATE_ON_FIRST_HIT | DISABLE_ANYHIT, no Embree filter), so the material's
  // shadows are those of an opaque surface (0004 D4's pass-through needs the M2 shadow pass).
  if (!extension && (m.flags & ar::NON_OPAQUE) != 0u)
    add_entry(&m, AR_REPORT_NODE_APPROXIMATED, "opacity: shadow rays treat this material as opaque (transparent shadows are M2)");
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

}  // namespace

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

namespace {

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
  // The authored block (grating_authored.h) is generator input: ExtParams gets the device words,
  // the efficiency table stays on the host in the compile result (item 24).
  static_assert(sizeof(desc.ext_constants) == sizeof(GratingAuthored), "ext_constants is the authored block");
  GratingAuthored authored;
  GratingAuthoredError bad;
  if (!parse_grating_authored(desc.ext_constants, &authored, &bad))
    return magenta_material(table, AR_REPORT_PROGRAM_REJECTED, "extension kind " + std::string(name) + ": " + bad.why);
  // Coating (item 24, PROVISIONAL, CONST only in M1b): one wavelength-independent complex IOR
  // through the metal lobe's Fresnel, unpolarised mean. n = k = 0 divides by zero there; it means
  // no coating loss, reflectance 1.
  const double coat_n = authored.coating_n, coat_k = authored.coating_k;
  const bool no_coating = coat_n == 0.0 && coat_k == 0.0;
  GratingTableSpec spec;
  spec.pitch_nm = authored.pitch_nm;
  spec.depth_nm = authored.depth_nm;
  spec.m_min = authored.m_min;
  spec.m_max = authored.m_max;
  spec.coating = [=](double, double cos_i) {
    return no_coating ? 1.0 : double(ar::fresnel_conductor_scalar<double>(cos_i, coat_n, coat_k));
  };
  GratingTableResult built;
  try {
    built = build_grating_table(spec);
  } catch (const std::runtime_error& e) {
    const std::string what = e.what();
    const bool range = what.find("scalar theory out of range") != std::string::npos;
    return magenta_material(table, range ? AR_REPORT_GRATING_TABLE_LIMIT : AR_REPORT_PROGRAM_REJECTED, what);
  }
  ar::GratingParams gp = {};
  gp.pitch_nm = authored.pitch_nm;
  gp.mode = uint32_t(ar::GratingMode::Reflect);
  gp.m_min = authored.m_min;
  gp.m_max = authored.m_max;
  gp.table = 0;  // the TableArena offset is the routing task's
  const ar::ExtParams params = ar::grating_ext_params(gp);
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
  m.compiled.ext_table = std::move(built.eta);
  m.flags = material_flags(m.compiled);
  if (built.report_code != 0) add_entry(&m, built.report_code, built.report_detail);
  if (no_coating)
    add_entry(&m, AR_REPORT_INPUT_IGNORED, "coating_n and coating_k are 0: no coating loss (reflectance 1)");
  // One entry per dropped slot, however many stores named it.
  for (uint32_t s : pruned.ignored)
    add_entry(&m, AR_REPORT_INPUT_IGNORED, std::string(kRecordSlotName[s]) + " is ignored by extension kind " + name);
  return m;
}

}  // namespace astroray::host
