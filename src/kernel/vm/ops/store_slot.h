// SPDX-License-Identifier: Apache-2.0
// STORE_SLOT a, imm: writes register a into one slot of the OpenPBRParams record. imm bits 0-7
// are the RecordSlot, bit 8 says the source is an already lifted spectrum (4 registers, from
// RGB_LIFT) instead of the slot's natural form; other bits must be zero. Natural forms:
//   scalar slots      f32, one register
//   colour slots      RGB, three registers: lifted here with the slot's semantic (0005 item 10),
//                     so a colour stays RGB until it reaches its slot
//   specular_ior      f32, one register, flat over the wavelengths (IOR takes no RGB)
//   frame vectors     f32x3, three registers
// The record's field order is item 2(c)'s table; scalar slots are numbered in that order.
// Ref: docs/decisions/0005-material-model.md items 2(c), 10, 31; material/constant_block.h
// (kSlotSemantic).
#pragma once
#include "../../material/constant_block.h"
#include "common.h"
#include "rgb_lift.h"

// The 24 scalar fields of OpenPBRParams, in record order.
#define AR_RECORD_SCALARS(X)                                                                                  \
  X(base_weight) X(base_diffuse_roughness) X(base_metalness) X(specular_weight) X(specular_roughness)         \
  X(specular_roughness_anisotropy) X(transmission_weight) X(transmission_depth)                               \
  X(transmission_scatter_anisotropy) X(subsurface_weight) X(subsurface_radius)                                \
  X(subsurface_scatter_anisotropy) X(fuzz_weight) X(fuzz_roughness) X(coat_weight) X(coat_roughness)          \
  X(coat_roughness_anisotropy) X(coat_ior) X(coat_darkening) X(thin_film_weight) X(thin_film_thickness)       \
  X(thin_film_ior) X(geometry_opacity) X(closure_weight)
// The spectral fields and the constant-block slot whose semantic each uses.
#define AR_RECORD_SPECTRAL(X)                                                                                 \
  X(base_color, BASE_COLOR) X(specular_color, SPECULAR_COLOR) X(specular_ior, SPECULAR_IOR)                   \
  X(transmission_color, TRANSMISSION_COLOR) X(transmission_scatter, TRANSMISSION_SCATTER)                     \
  X(subsurface_color, SUBSURFACE_COLOR) X(subsurface_radius_scale, SUBSURFACE_RADIUS_SCALE)                   \
  X(fuzz_color, FUZZ_COLOR) X(coat_color, COAT_COLOR)
#define AR_RECORD_FRAMES(X) X(geometry_normal) X(geometry_coat_normal) X(geometry_tangent) X(geometry_coat_tangent)

AR_NS_BEGIN

enum class RecordSlot : uint32_t {
#define AR_X(n) n,
  AR_RECORD_SCALARS(AR_X)
#undef AR_X
#define AR_X(n, s) n,
  AR_RECORD_SPECTRAL(AR_X)
#undef AR_X
#define AR_X(n) n,
  AR_RECORD_FRAMES(AR_X)
#undef AR_X
  COUNT_
};

inline constexpr uint32_t kStoreSlotSpecSource = 1u << 8;

// One spectral slot: IOR is a flat scalar, other semantics lift an RGB colour, and a spectrum
// register group is stored as is.
AR_FN VmStatus store_spectral(Spec* dst, SpectrumSemantic sem, bool spec_src, const SceneTables& t,
                              const Lambda& lam, const float* src, uint32_t avail) {
  if (spec_src) {
    if (avail < 4u) return VmStatus::BAD_REGISTER;
    for (int i = 0; i < kLanes; ++i) dst->v[i] = Real(src[i]);
  } else if (sem == SpectrumSemantic::IOR) {
    if (avail < 1u) return VmStatus::BAD_REGISTER;
    *dst = spec_fill(Real(src[0]));
  } else {
    if (avail < 3u) return VmStatus::BAD_REGISTER;
    *dst = op_rgb_lift(t, src, sem, lam);
  }
  return VmStatus::OK;
}

// Writes src into the slot; BAD_OPERAND when the slot is unknown or the source form does not fit
// it, BAD_REGISTER when fewer registers than the form needs remain.
AR_FN VmStatus op_store_slot(OpenPBRParams* rec, const SceneTables& t, const Lambda& lam, uint32_t imm,
                             const float* src, uint32_t avail) {
  const uint32_t slot = imm & 0xffu;
  const bool spec_src = (imm & kStoreSlotSpecSource) != 0u;
  if ((imm >> 9) != 0u) return VmStatus::BAD_OPERAND;
  if (slot >= uint32_t(RecordSlot::COUNT_)) return VmStatus::BAD_OPERAND;
  // `avail` is how many registers remain from the source register on.
  switch (RecordSlot(slot)) {
#define AR_X(n)                                           \
  case RecordSlot::n:                                     \
    if (spec_src) return VmStatus::BAD_OPERAND;           \
    if (avail < 1u) return VmStatus::BAD_REGISTER;        \
    rec->n = Real(src[0]);                                \
    return VmStatus::OK;
    AR_RECORD_SCALARS(AR_X)
#undef AR_X
#define AR_X(n)                                           \
  case RecordSlot::n:                                     \
    if (spec_src) return VmStatus::BAD_OPERAND;           \
    if (avail < 3u) return VmStatus::BAD_REGISTER;        \
    rec->n = Real3{Real(src[0]), Real(src[1]), Real(src[2])}; \
    return VmStatus::OK;
    AR_RECORD_FRAMES(AR_X)
#undef AR_X
#define AR_X(n, s) \
  case RecordSlot::n: return store_spectral(&rec->n, kSlotSemantic[int(Slot::s)], spec_src, t, lam, src, avail);
    AR_RECORD_SPECTRAL(AR_X)
#undef AR_X
    default: return VmStatus::BAD_OPERAND;
  }
}

AR_FN void vm_op_store_slot(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 1)) return;
  if (vm.rec == nullptr || vm.tables == nullptr || vm.tables->rgb2spec == nullptr)
    return vm_fail(vm, VmStatus::NULL_POINTER);
  const VmStatus s = op_store_slot(vm.rec, *vm.tables, vm.ctx->lambda_t, vm_imm(vm, 0), vm.r + vm.a,
                                   kVmRegisterCount - vm.a);
  if (s != VmStatus::OK) return vm_fail(vm, s);
  vm.pc += 2;
}
AR_NS_END
#define handle_STORE_SLOT() ::ar::vm_op_store_slot(vm)
