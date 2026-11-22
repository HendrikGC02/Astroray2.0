// SPDX-License-Identifier: Apache-2.0
#include "op_meta.h"

#include <cstdio>
#include <cstdlib>

namespace astroray::host {
namespace {

constexpr bool is_none(const char* s) {
  const char* k = "NONE_SURFACE_ONLY";
  while (*k && *s == *k) { ++s; ++k; }
  return *k == '\0' && *s == '\0';
}
constexpr const char* rule_or_null(const char* s) { return is_none(s) ? nullptr : s; }

// Same rows as the enum, in the same order.
constexpr OpMeta kMeta[] = {
#define AR_OP(NAME, ARITY, OPERANDS, RESULT, COST, RULE, VOL, TEST, FOLD) \
  {ar::Op::NAME, #NAME, ARITY, OPERANDS, RESULT, CostClass::COST, rule_or_null(#RULE), VOL, TEST, FOLD},
#include "../../kernel/vm/ops.def"
#undef AR_OP
};

}  // namespace

const OpMeta* op_meta_table() { return kMeta; }
int op_meta_count() { return static_cast<int>(sizeof(kMeta) / sizeof(kMeta[0])); }

const OpMeta& op_meta(ar::Op op) {
  const unsigned i = static_cast<unsigned>(op);
  if (i >= static_cast<unsigned>(op_meta_count())) {
    std::fprintf(stderr, "op_meta: opcode %u is not in ops.def\n", i);
    std::abort();
  }
  return kMeta[i];
}

}  // namespace astroray::host
