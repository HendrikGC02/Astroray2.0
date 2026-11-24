// SPDX-License-Identifier: Apache-2.0
#include "host/scene/world.h"

#include <string>

#include "../../kernel/material/material_header.h"
#include "../../kernel/material/slot_resolve.h"
#include "host/material/graph.h"
#include "host/material/material_store.h"

namespace astroray::host {
namespace {

// The first node (lowest index) feeding an EMIT_LE that reads the UV context: a world has no
// surface, so LOAD_CTX_UV would read zeros and render a constant without any report. The direction
// (LOAD_CTX_P) is the one context a world program has. Empty when there is none.
std::string first_uv_read(const ar_graph_desc& d) {
  Graph g;
  std::string err;
  if (!parse_graph(d, &g, &err)) return {};
  const size_t n = g.nodes.size();
  std::vector<char> feeds(n, 0);
  for (bool changed = true; changed;) {  // parse_graph rejects cycles, so this ends
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      const GraphNode& nd = g.nodes[i];
      if (nd.op != ar::Op::EMIT_LE && !feeds[i]) continue;
      for (int s = 0; s < nd.n_inputs; ++s)
        if (!feeds[size_t(nd.input[s])]) feeds[size_t(nd.input[s])] = 1, changed = true;
    }
  }
  for (size_t i = 0; i < n; ++i)
    if (feeds[i] && g.nodes[i].op == ar::Op::LOAD_CTX_UV) return "node " + std::to_string(i) + " (LOAD_CTX_UV)";
  return {};
}

void make_magenta(const ar::Rgb2SpecTable& t, WorldOutput* w) {
  uint32_t flags = 0;
  ar::make_rgb_ref(t, ar::Rgb{{ar::Real(1), ar::Real(0), ar::Real(1)}}, ar::SpectrumSemantic::ILLUMINANT, &w->color, &flags);
  w->strength = 1.0f;
  w->magenta = true;
}

}  // namespace

WorldOutput compile_world(const ar_world_desc& desc, const StoredGraph* graph, const ar::Rgb2SpecTable& t,
                          const ImageMeanFn* means, const ImageSlotFn* slots) {
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
  const MaterialOutput m = compile_material(gd, t, means, slots);
  if (m.magenta) {  // unsupported nodes or a rejected program: the material policy's entries
    make_magenta(t, &w);
    w.entries.insert(w.entries.end(), m.entries.begin(), m.entries.end());
    return w;
  }
  // The keys of the images the program samples, kept so the world recompiles when one changes.
  w.images = m.images;
  if (!(m.flags & ar::CONSTANT_EMISSION)) {
    // The emission varies with the direction (an Environment Texture) or sums several emissions: a
    // program that shade_background runs per escaped ray. A node that would read the UV context
    // has no value in a world, so it is reported like any other unsupported node.
    const std::string uv = first_uv_read(gd);
    if (!uv.empty()) {
      make_magenta(t, &w);
      w.entries.push_back({AR_REPORT_NODE_UNSUPPORTED, uv});
      return w;
    }
    w.entries.insert(w.entries.end(), m.entries.begin(), m.entries.end());
    w.program = m.compiled.program;
    w.block = m.compiled.block;
    // The light record's strength carries the radiance scale s only (the program's own strength is
    // in its code), so the table sees 1 and a lit world.
    w.color = ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(1));
    w.strength = 1.0f;
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
