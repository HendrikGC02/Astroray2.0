// SPDX-License-Identifier: Apache-2.0
// TEX_IMAGE a, b, imm slot, imm mode: r[a..a+2] = the colour of image `slot` at (u, v) =
// r[b], r[b+1]. `slot` indexes the scene's image table (the compiler resolved it from the image
// key); mode bit 0 selects LINEAR over CLOSEST and bit 1 EXTEND over REPEAT. Texels are filtered
// in their stored encoding and only then decoded (sRGB images by the sRGB function, linear and
// data images not), as Cycles does for 8-bit images. A slot outside the table is magenta; a
// freed slot holds a 1x1 magenta image and samples magenta by itself. Alpha has no output yet:
// its only consumer lands with alpha and NON_OPAQUE in M1b.
// Ref: docs/decisions/0005-material-model.md items 31, 34; 0002 D13 (software filtering);
// Cycles intern/cycles/kernel/util/image.h (Apache-2.0): ImageInterpolator::frac,
// wrap_periodic, wrap_clamp, interp_closest, interp_linear (the -0.5 texel-centre offset and
// the weight order), kernel_image_interp (xy = uv * size); kernel/svm/image.h
// svm_image_texture (sRGB decode after filtering).
#pragma once
#include "../../color/srgb.h"
#include "common.h"

AR_NS_BEGIN

inline constexpr uint32_t kTexLinear = 1u;    // mode bit: LINEAR, else CLOSEST
inline constexpr uint32_t kTexExtend = 2u;    // mode bit: EXTEND, else REPEAT
inline constexpr uint32_t kTexModeMask = 3u;  // every other bit is a bad operand
inline constexpr uint32_t kTexNoImage = 0xffffffffu;  // the slot of a key that is not set

// Cycles' frac, kept exactly so lookups match Blender: int(x) - 1 for every negative x (a
// negative integer therefore goes one lower than floor) and the fraction. The coordinate is first
// held inside +-2^30 so the float-to-int conversion is always defined (a NaN goes to the lower
// bound); every finite coordinate a texture can address in practice is unchanged by that.
AR_FN float tex_frac(float x, int* ix) {
  const float lim = 1073741824.0f;  // 2^30: int arithmetic below has headroom for ix + 1
  if (!(x >= -lim)) x = -lim;
  if (x > lim) x = lim;
  const int i = int(x) - (x < 0.0f ? 1 : 0);
  *ix = i;
  return x - float(i);
}

AR_FN int tex_wrap(int x, int size, bool extend) {
  if (extend) return x < 0 ? 0 : (x > size - 1 ? size - 1 : x);
  x %= size;
  return x < 0 ? x + size : x;
}

AR_FN Texel tex_read(const DeviceImageView& im, int x, int y) {
  return load_texel(image_texel(im, uint32_t(x), uint32_t(y)), im.channels);
}

// The filtered, still encoded RGBA value of `im` at texel coordinates (x, y) = uv * size.
AR_FN Texel tex_filter(const DeviceImageView& im, float x, float y, uint32_t mode) {
  const int w = int(im.width), h = int(im.height);
  const bool extend = (mode & kTexExtend) != 0u;
  int ix, iy;
  if ((mode & kTexLinear) == 0u) {
    tex_frac(x, &ix);
    tex_frac(y, &iy);
    return tex_read(im, tex_wrap(ix, w, extend), tex_wrap(iy, h, extend));
  }
  const float tx = tex_frac(x - 0.5f, &ix);
  const float ty = tex_frac(y - 0.5f, &iy);
  const int nix = tex_wrap(ix + 1, w, extend), niy = tex_wrap(iy + 1, h, extend);
  ix = tex_wrap(ix, w, extend);
  iy = tex_wrap(iy, h, extend);
  const Texel a = tex_read(im, ix, iy), b = tex_read(im, nix, iy), c = tex_read(im, ix, niy), d = tex_read(im, nix, niy);
  Texel r;
  for (int k = 0; k < 4; ++k)
    r.v[k] = (1.0f - ty) * (1.0f - tx) * a.v[k] + (1.0f - ty) * tx * b.v[k] + ty * (1.0f - tx) * c.v[k] + ty * tx * d.v[k];
  return r;
}

// The host constant folder never calls this (a texture read does not fold), but tests compare
// the interpreter against it directly.
AR_FN void op_tex_image(const SceneTables& t, uint32_t slot, uint32_t mode, const float* uv, float* out) {
  const float u = uv[0], v = uv[1];  // read before writing: out may alias uv
  if (slot >= t.image_count) {
    out[0] = 1.0f;
    out[1] = 0.0f;
    out[2] = 1.0f;
    return;
  }
  const DeviceImageView im = t.images[slot];
  const Texel f = tex_filter(im, u * float(im.width), v * float(im.height), mode);
  const bool srgb = im.encoding == uint32_t(TexEncoding::SRGB);
  for (int k = 0; k < 3; ++k) out[k] = srgb ? srgb_to_linear(f.v[k]) : f.v[k];
}

AR_FN void vm_op_tex_image(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 3) || !vm_regs_ok(vm, vm.b, 3)) return;
  const uint32_t mode = vm_imm(vm, 1);
  if ((mode & ~kTexModeMask) != 0u) return vm_fail(vm, VmStatus::BAD_OPERAND);
  // An empty table is a scene without images (every slot is magenta); a count without its
  // array is a wiring bug.
  if (vm.tables == nullptr || (vm.tables->images == nullptr && vm.tables->image_count != 0u))
    return vm_fail(vm, VmStatus::NULL_POINTER);
  op_tex_image(*vm.tables, vm_imm(vm, 0), mode, vm.r + vm.b, vm.r + vm.a);
  vm.pc += 3;
}
AR_NS_END
#define handle_TEX_IMAGE() ::ar::vm_op_tex_image(vm)
