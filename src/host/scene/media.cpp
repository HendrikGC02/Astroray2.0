// SPDX-License-Identifier: Apache-2.0
#include "host/scene/media.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "host/material/compiler.h"
#include "host/scene/edit.h"
#include "host/scene/scene.h"
#include "../../kernel/vm/ops.h"
#include "../../kernel/vm/ops/volume_coeffs.h"

namespace astroray::host {

namespace {

// The volume graph of a medium with constant coefficients: one VOLUME_COEFFS node per term, fed by a
// constant colour and a constant density. Cycles' mapping (volume_coeffs.h) is
//   Volume Scatter    sigma = colour * density
//   Volume Absorption sigma = (1 - colour) * density
// so a coefficient triple s in 1/m is density D = max(s) with colour s / D (scatter) or 1 - s / D
// (absorption), which keeps every colour inside [0, 1]: nothing is clamped and nothing reported.
struct VolumeGraph {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f32;
  std::vector<int32_t> i32;

  int add(ar::Op op, std::vector<float> fp = {}, std::vector<int32_t> ip = {}) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f32.size());
    n.f32_count = uint32_t(fp.size());
    n.i32_first = uint32_t(i32.size());
    n.i32_count = uint32_t(ip.size());
    f32.insert(f32.end(), fp.begin(), fp.end());
    i32.insert(i32.end(), ip.begin(), ip.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  void term(ar::VolumeTerm kind, const float s[3], float g) {
    const float dmax = std::max(s[0], std::max(s[1], s[2]));
    float c[3] = {0.0f, 0.0f, 0.0f};
    if (dmax > 0.0f)
      for (int k = 0; k < 3; ++k) c[k] = kind == ar::VolumeTerm::SCATTER ? s[k] / dmax : 1.0f - s[k] / dmax;
    const int col = add(ar::Op::CONST_F32X3, {c[0], c[1], c[2]});
    const int dens = add(ar::Op::CONST_F32, {dmax});
    const int n = add(ar::Op::VOLUME_COEFFS, {g}, {int32_t(kind)});
    links.push_back({uint32_t(col), 0u, uint32_t(n), 0u});
    links.push_back({uint32_t(dens), 0u, uint32_t(n), 1u});
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size());
    d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f32.size());
    d.i32_count = uint32_t(i32.size());
    d.nodes = nodes.data();
    d.links = links.data();
    d.params_f32 = f32.data();
    d.params_i32 = i32.data();
    return d;
  }
};

void require_coefficients(const char* name, const float s[3]) {
  for (int k = 0; k < 3; ++k)
    if (!(std::isfinite(s[k]) && s[k] >= 0.0f))
      throw std::invalid_argument(std::string("ar_edit_set_medium: ") + name + " must be finite and >= 0 in every channel");
}

}  // namespace

std::shared_ptr<const Medium> build_medium(ar_key key, const ar_medium_desc& d, const ar::Rgb2SpecTable& table) {
  require_coefficients("sigma_a", d.sigma_a);
  require_coefficients("sigma_s", d.sigma_s);
  if (!std::isfinite(d.g)) throw std::invalid_argument("ar_edit_set_medium: g must be finite");
  const bool scatters = d.sigma_s[0] > 0.0f || d.sigma_s[1] > 0.0f || d.sigma_s[2] > 0.0f;
  VolumeGraph g;
  // The absorption term is always present, with density 0 for a purely scattering medium, so that a
  // vacuum medium still compiles to a block with a volume entry.
  g.term(ar::VolumeTerm::ABSORPTION, d.sigma_a, 0.0f);
  if (scatters) g.term(ar::VolumeTerm::SCATTER, d.sigma_s, d.g);
  const ar_graph_desc gd = g.desc();
  VolumeOutput v = compile_volume(gd, table);
  // Every input was checked above, so a refusal here is an engine fault, not the caller's.
  if (!v.has_medium) throw std::logic_error("medium " + std::to_string(key) + " did not compile to a block");

  auto m = std::make_shared<Medium>();
  m->key = key;
  m->block = v.medium.block;
  m->block.entry = 0;
  m->program = std::move(v.medium.program);
  for (const MaterialEntry& e : v.entries) {
    if (e.code != AR_REPORT_NODE_APPROXIMATED) throw std::logic_error("medium " + std::to_string(key) + ": " + e.detail);
    m->approximations.push_back(e.detail);
  }
  // Estimator and flags as 0006 D9 derives them: scattering selects delta tracking.
  m->desc.model = uint8_t(ar::MediumModel::BLENDER_VOLUME);
  m->desc.estimator = uint8_t(scatters ? ar::MediumEstimator::DELTA_TRACK : ar::MediumEstimator::MARCH_EMISSION_ABSORPTION);
  m->desc.flags = scatters ? ar::MEDIUM_HAS_SCATTERING : uint16_t(0);
  m->desc.program = Medium::kNoMediumProgram;
  return m;
}

ar_status set_medium(Edit& edit, ar_key key, const ar_medium_desc* desc, const ar::Rgb2SpecTable& table) {
  if (desc == nullptr) return edit.fail("invalid argument: ar_edit_set_medium desc is null");
  if (key == 0) return edit.fail("invalid argument: ar_edit_set_medium key 0 means no medium");
  const std::shared_ptr<const Medium> medium = build_medium(key, *desc, table);  // throws invalid_argument
  auto item = std::make_shared<EditItem>();
  item->key = key;
  item->kind = AR_KIND_MEDIUM;
  item->needs_device = false;
  item->job = [] {};
  Scene* scene = &edit.scene();
  // Raised from the activation callback, after the replaced subject's old entries are cleared.
  item->on_active = [scene, medium] {
    scene->publish_medium(medium);
    for (const std::string& a : medium->approximations) {
      scene->ctx().log.line("WARNING", "medium " + std::to_string(medium->key) + ": " + a);
      scene->report().raise(AR_REPORT_NODE_APPROXIMATED, medium->key, a, 1, nullptr, 0);
    }
  };
  if (ar_status st = edit.stage(item)) return st;
  return edit.item_commit(key);
}

}  // namespace astroray::host
