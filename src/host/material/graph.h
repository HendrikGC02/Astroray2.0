// SPDX-License-Identifier: Apache-2.0
// The compiler's typed view of an ar_graph_desc: host-neutral op nodes (the ops of ops.def, which
// is also the node's `op` value), their parameter ranges and their input links. Parsing checks
// every range, infers the value type of each node and rejects type errors and cycles; errors are
// returned as text, never thrown across the ABI.
// Ref: docs/decisions/0005-material-model.md items 29-31; 0007 decision 8 (ar_graph_desc).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "astroray/astroray.h"
#include "../../kernel/vm/ops.h"

namespace astroray::host {

enum class VType : uint8_t { NONE, F32, F32X3, SPEC };

// Register width of a value of this type.
int vtype_width(VType t);

struct GraphNode {
  ar::Op op;
  uint32_t f32_first = 0, i32_first = 0;  // into Graph::f32 / Graph::i32, counts checked per op
  int input[2] = {-1, -1};                // producing node of each input socket
  int n_inputs = 0;
  VType out = VType::NONE;
};

struct Graph {
  std::vector<GraphNode> nodes;
  std::vector<float> f32;
  std::vector<int32_t> i32;
};

int op_input_count(ar::Op op);
VType op_result_type(ar::Op op);

// False with *err set on a bad range, an op a graph may not hold, a missing or doubled link, a
// type mismatch or a cycle. Nodes of *out are in the order of the description.
bool parse_graph(const ar_graph_desc& desc, Graph* out, std::string* err);

}  // namespace astroray::host
