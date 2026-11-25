// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0002-kernel-architecture.md D4, D13; RULINGS 70. See kg_builder.h.
#include "host/session/kg_builder.h"

#include "host/light/light_builder.h"
#include "kernel/film/film_write.h"

namespace astroray::host {

SessionTables upload_session_tables(Device& device, Owner owner, const ResponseTable& response,
                                    const WavelengthTable& wavelengths, ar::FilmData& film) {
  const UploadTarget t{&device, nullptr, owner};
  const auto put = [&](const void* data, uint64_t bytes) {
    return DeviceTable::put(t, LedgerCategory::TABLES, data, bytes);
  };
  SessionTables s;
  s.knot_nm = put(response.knot_nm.data(), response.knot_nm.size() * 4);
  s.knot_value = put(response.knot_value.data(), response.knot_value.size() * 4);
  s.channels = put(response.channels.data(), response.channels.size() * sizeof(ar::ResponseChannel));
  s.edges = put(wavelengths.edges.data(), wavelengths.edges.size() * 4);
  s.cdf = put(wavelengths.cdf.data(), wavelengths.cdf.size() * 4);
  film.response = response_view(response);
  film.response.knot_nm = static_cast<const float*>(s.knot_nm->ptr());
  film.response.knot_value = static_cast<const float*>(s.knot_value->ptr());
  film.response.channels = static_cast<const ar::ResponseChannel*>(s.channels->ptr());
  film.wavelengths = {static_cast<const float*>(s.edges->ptr()), static_cast<const float*>(s.cdf->ptr()),
                      wavelengths.bins()};
  return s;
}

ar::KernelGlobals build_kernel_globals(const SceneTableSet* set, const KgSession& session) {
  ar::KernelGlobals kg{};
  kg.session = session.uniforms;
  kg.film = session.film;
  kg.display = session.display;
  kg.active_pixels = session.active_pixels;
  kg.camera = session.camera;
  kg.filter = session.filter;
  if (!set) return kg;
  const SceneTableSet& scene = *set;
  kg.scene = scene.scene;
  kg.tables = scene.vm;
  kg.lights = static_cast<const ar::LightRecord*>(scene.tables[static_cast<uint32_t>(TableId::LIGHTS)]->ptr());
  kg.light_sampler = scene.light_sampler;
  kg.world_light = scene.lights ? scene.lights->world_light : ar::kNoLight;
  kg.light_index = static_cast<const uint32_t*>(scene.tables[static_cast<uint32_t>(TableId::LIGHT_INDEX)]->ptr());
  kg.light_index_words = scene.counts[static_cast<uint32_t>(TableId::LIGHT_INDEX)];
  kg.e_diel = scene.e_diel;
  kg.e_spec = scene.e_spec;
  kg.material_slot_count = scene.counts[static_cast<uint32_t>(TableId::MATERIAL_SLOTS)];
  kg.material_program_words = scene.counts[static_cast<uint32_t>(TableId::MATERIAL_PROGRAMS)];
  kg.material_constant_count = scene.counts[static_cast<uint32_t>(TableId::MATERIAL_CONSTANTS)];
  // The environment table (0004 D11): both distributions are views into ENV_TABLE, whose floats are
  // the plain distribution's four arrays and then the compensated one's (scene_tables.h
  // env_table_floats); the integrals and n come from the light table's host copy.
  const EnvTable* env = scene.lights ? scene.lights->env.get() : nullptr;
  const auto& env_block = scene.tables[static_cast<uint32_t>(TableId::ENV_TABLE)];
  if (env && env_block && env_block->ptr()) {
    const float* p = static_cast<const float*>(env_block->ptr());
    const EnvDistribution* d[2] = {&env->plain, &env->compensated};
    for (int k = 0; k < 2; ++k) {
      ar::EnvTableView& v = kg.env[k];
      v.func = p, p += d[k]->func.size();
      v.cdf_u = p, p += d[k]->cdf_u.size();
      v.marginal_func = p, p += d[k]->marginal_func.size();
      v.marginal_cdf = p, p += d[k]->marginal_cdf.size();
      v.func_integral = d[k]->func_integral;
      v.n = d[k]->n;
    }
  }
  // A WORLD_ENV world's NEE items carry their lanes and item_flags (0003 d20): the scheduler
  // allocates them from this feature.
  const uint32_t w = kg.world_light;
  kg.session.features.env_world = scene.lights && w != ar::kNoLight && w < scene.lights->lights.size() &&
                                  scene.lights->lights[w].type == uint8_t(ar::LightType::WORLD_ENV);
  return kg;
}

std::vector<std::string> missing_pointers(const ar::KernelGlobals& kg, const SceneTableSet& scene,
                                          bool scheduler_filled) {
  std::vector<std::string> out;
  const auto need = [&](const void* p, const char* name) {
    if (!p) out.push_back(name);
  };
  // Null exactly when its table is empty.
  const auto table = [&](const void* p, TableId id, const char* name) {
    if (!p && scene.counts[static_cast<uint32_t>(id)] != 0) out.push_back(name);
  };
  if (scheduler_filled) {
    need(kg.path, "path"), need(kg.shadow, "shadow"), need(kg.step_table, "step_table");
    need(kg.main_queue, "main_queue"), need(kg.shadow_queue, "shadow_queue"), need(kg.queue_counts, "queue_counts");
    need(kg.sort_queue, "sort_queue"), need(kg.sort_counts, "sort_counts");
  }
  need(kg.active_pixels, "active_pixels"), need(kg.film, "film"), need(kg.display, "display");
  need(kg.scene, "scene"), need(kg.tables, "tables"), need(kg.light_sampler, "light_sampler");
  need(kg.e_diel, "e_diel"), need(kg.e_spec, "e_spec");
  table(kg.lights, TableId::LIGHTS, "lights");
  table(kg.light_index, TableId::LIGHT_INDEX, "light_index");
  const ar::SceneData& s = scene.scene_host;
  need(s.accel, "scene.accel");
  table(s.instances, TableId::INSTANCES, "scene.instances");
  table(s.meshes, TableId::MESHES, "scene.meshes");
  table(s.material_slots, TableId::MATERIAL_SLOTS, "scene.material_slots");
  table(s.material_headers, TableId::MATERIAL_HEADERS, "scene.material_headers");
  table(s.material_programs, TableId::MATERIAL_PROGRAMS, "scene.material_programs");
  table(s.material_constants, TableId::MATERIAL_CONSTANTS, "scene.material_constants");
  table(s.material_pass_index, TableId::MATERIAL_PASS_INDEX, "scene.material_pass_index");
  table(s.material_spectra, TableId::MATERIAL_SPECTRA, "scene.material_spectra");
  table(s.medium_descs, TableId::MEDIA, "scene.medium_descs");
  table(s.world_program, TableId::WORLD_PROGRAM, "scene.world_program");
  table(s.world_constants, TableId::WORLD_CONSTANTS, "scene.world_constants");
  table(kg.env[0].func, TableId::ENV_TABLE, "env");
  // A WORLD_ENV world is sampled from both views: a world light of that type without them is a bug.
  if (kg.session.features.env_world && (kg.env[0].func == nullptr || kg.env[1].func == nullptr || kg.env[1].n == 0u))
    out.push_back("env (WORLD_ENV world light)");
  table(s.medium_blocks, TableId::MEDIUM_BLOCKS, "scene.medium_blocks");
  table(s.medium_programs, TableId::MEDIUM_PROGRAMS, "scene.medium_programs");
  for (uint32_t i = 0; i < kTableCount; ++i)
    if (scene.counts[i] != 0 && (!scene.tables[i] || !scene.tables[i]->ptr())) out.push_back(table_name(TableId(i)));
  return out;
}

}  // namespace astroray::host
