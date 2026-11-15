// SPDX-License-Identifier: Apache-2.0
#include "host/scene/world.h"

#include <string>

#include "../../kernel/material/material_header.h"
#include "../../kernel/material/slot_resolve.h"
#include "host/material/graph.h"
#include "host/material/material_store.h"

namespace astroray::host {
namespace {

const char* const kOpNames[] = {
#define AR_OP(NAME, ...) #NAME,
#include "../../kernel/vm/ops.def"
#undef AR_OP
};

// The first node (lowest index) feeding an EMIT_LE that is not constant while all its inputs are:
// by 0005 item 35 a node folds only when every input is constant and it reads nothing from the
// context, so this is a context, texture or wavelength read. Empty when there is none.
std::string first_non_constant(const ar_graph_desc& d) {
  Graph g;
  std::string err;
  if (!parse_graph(d, &g, &err)) return {};
  const size_t n = g.nodes.size();
  auto reads_context = [](ar::Op op, int inputs) {
    if (op == ar::Op::TEX_IMAGE || op == ar::Op::RGB_LIFT) return true;
    return inputs == 0 && op != ar::Op::CONST_F32 && op != ar::Op::CONST_F32X3;
  };
  std::vector<char> varies(n, 0), feeds(n, 0);
  for (bool changed = true; changed;) {  // parse_graph rejects cycles, so this ends
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      const GraphNode& nd = g.nodes[i];
      bool v = reads_context(nd.op, nd.n_inputs);
      for (int s = 0; s < nd.n_inputs; ++s) v = v || varies[size_t(nd.input[s])];
      if (v && !varies[i]) varies[i] = 1, changed = true;
      bool f = nd.op == ar::Op::EMIT_LE;
      if (f || feeds[i])
        for (int s = 0; s < nd.n_inputs; ++s)
          if (!feeds[size_t(nd.input[s])]) feeds[size_t(nd.input[s])] = 1, changed = true;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    const GraphNode& nd = g.nodes[i];
    if (!feeds[i] || !varies[i]) continue;
    bool inputs_vary = false;
    for (int s = 0; s < nd.n_inputs; ++s) inputs_vary = inputs_vary || varies[size_t(nd.input[s])];
    if (!inputs_vary) return "node " + std::to_string(i) + " (" + kOpNames[size_t(nd.op)] + ")";
  }
  return {};
}

void make_magenta(const ar::Rgb2SpecTable& t, WorldOutput* w) {
  uint32_t flags = 0;
  ar::make_rgb_ref(t, ar::Rgb{{ar::Real(1), ar::Real(0), ar::Real(1)}}, ar::SpectrumSemantic::ILLUMINANT, &w->color, &flags);
  w->strength = 1.0f;
  w->magenta = true;
}

}  // namespace

WorldOutput compile_world(const ar_world_desc& desc, const StoredGraph* graph, const ar::Rgb2SpecTable& t) {
  WorldOutput w;
  w.desc = desc;
  w.subject = desc.surface_graph != 0 ? desc.surface_graph : desc.volume_graph;
  w.color = ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(0));
  if (desc.volume_graph != 0) w.entries.push_back({AR_REPORT_NODE_UNSUPPORTED, "world volume graph"});
  if (desc.surface_graph == 0) return w;  // no world: black
  if (graph == nullptr) {
    make_magenta(t, &w);
    w.entries.push_back({AR_REPORT_PROGRAM_REJECTED, "world graph " + std::to_string(desc.surface_graph) + " is not set"});
    return w;
  }
  const ar_graph_desc gd = graph->desc();
  const MaterialOutput m = compile_material(gd, t);
  if (m.magenta) {  // unsupported nodes or a rejected program: the material policy's entries
    make_magenta(t, &w);
    w.entries.insert(w.entries.end(), m.entries.begin(), m.entries.end());
    return w;
  }
  if (!(m.flags & ar::CONSTANT_EMISSION)) {
    make_magenta(t, &w);
    std::string node = first_non_constant(gd);
    // No varying node means several constant emissions, which accumulate in a program (059).
    if (node.empty()) node = "more than one Background emission";
    w.entries.push_back({AR_REPORT_NODE_UNSUPPORTED, node});
    return w;
  }
  w.entries.insert(w.entries.end(), m.entries.begin(), m.entries.end());
  if (!m.emission_estimate.empty()) {  // one constant EMIT_LE, stored in the block (059)
    w.color = m.compiled.block.emission_color;
    w.strength = m.compiled.block.emission_luminance;
  }
  return w;
}

}  // namespace astroray::host
