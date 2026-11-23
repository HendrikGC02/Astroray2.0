// SPDX-License-Identifier: Apache-2.0
// TEX_IMAGE a, b, imm slot, imm mode: r[a..a+2] = the colour of image `slot` at (u, v) =
// r[b], r[b+1]. `slot` indexes the scene's image table (the compiler resolved it from the image
// key); mode bit 0 selects LINEAR over CLOSEST and bit 1 EXTEND over REPEAT. Texels are filtered
// in their stored encoding and only then decoded (sRGB images by the sRGB function, linear and
// data images not), as Cycles does for 8-bit images; a Closest sRGB texel is decoded through the
// host-built table SceneTables::srgb_decode (color/srgb.h). Half and float images are linear
// values: they are read as stored (a half widens exactly, half_to_float) and never decoded. A slot outside the table is magenta; a
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
  const uint8_t* p = image_texel(im, uint32_t(x), uint32_t(y));
  if (im.format != uint32_t(TexFormat::U8)) return load_texel_wide(p, im.channels, im.format);
  return load_texel(p, im.channels);
}

// Closest: the texel at texel coordinates (x, y) = uv * size (Cycles interp_closest). An sRGB
// texel is decoded through the host-built table, so CPU and GPU return the same bits.
AR_FN void tex_closest(const DeviceImageView& im, const float* srgb_decode, float x, float y, bool extend, float* out) {
  int ix, iy;
  tex_frac(x, &ix);
  tex_frac(y, &iy);
  ix = tex_wrap(ix, int(im.width), extend);
  iy = tex_wrap(iy, int(im.height), extend);
  const uint8_t* p = image_texel(im, uint32_t(ix), uint32_t(iy));
  if (im.format == uint32_t(TexFormat::U8) && im.encoding == uint32_t(TexEncoding::SRGB)) {
    for (uint32_t k = 0; k < 3u; ++k) out[k] = load_ro(srgb_decode + load_ro(p + (im.channels == 1u ? 0u : k)));
    return;
  }
  const Texel t = im.format == uint32_t(TexFormat::U8) ? load_texel(p, im.channels) : load_texel_wide(p, im.channels, im.format);
  for (int k = 0; k < 3; ++k) out[k] = t.v[k];
}

// Linear: bilinear on the stored values around (x, y) with the -0.5 centre offset (Cycles
// interp_linear, same weight order), decoded afterwards.
AR_FN void tex_linear(const DeviceImageView& im, float x, float y, bool extend, float* out) {
  const int w = int(im.width), h = int(im.height);
  int ix, iy;
  const float tx = tex_frac(x - 0.5f, &ix);
  const float ty = tex_frac(y - 0.5f, &iy);
  const int nix = tex_wrap(ix + 1, w, extend), niy = tex_wrap(iy + 1, h, extend);
  ix = tex_wrap(ix, w, extend);
  iy = tex_wrap(iy, h, extend);
  if (im.format != uint32_t(TexFormat::U8)) {
    // Half and float: the same weights and sum order, accumulated texel by texel so only one
    // widened texel is live at a time (the shade kernel is at its register cap).
    const float w00 = (1.0f - ty) * (1.0f - tx), w10 = (1.0f - ty) * tx, w01 = ty * (1.0f - tx), w11 = ty * tx;
    Texel t = tex_read(im, ix, iy);
    float acc[3] = {w00 * t.v[0], w00 * t.v[1], w00 * t.v[2]};
    t = tex_read(im, nix, iy);
    for (int k = 0; k < 3; ++k) acc[k] += w10 * t.v[k];
    t = tex_read(im, ix, niy);
    for (int k = 0; k < 3; ++k) acc[k] += w01 * t.v[k];
    t = tex_read(im, nix, niy);
    for (int k = 0; k < 3; ++k) out[k] = acc[k] + w11 * t.v[k];
    return;
  }
  const Texel a = tex_read(im, ix, iy), b = tex_read(im, nix, iy), c = tex_read(im, ix, niy), d = tex_read(im, nix, niy);
  const bool srgb = im.format == uint32_t(TexFormat::U8) && im.encoding == uint32_t(TexEncoding::SRGB);
  for (int k = 0; k < 3; ++k) {
    const float f = (1.0f - ty) * (1.0f - tx) * a.v[k] + (1.0f - ty) * tx * b.v[k] + ty * (1.0f - tx) * c.v[k] + ty * tx * d.v[k];
    out[k] = srgb ? srgb_to_linear(f) : f;
  }
}

// The host constant folder never calls this (a texture read does not fold), but tests compare
// the interpreter against it directly. A non-empty table needs t.srgb_decode.
AR_FN void op_tex_image(const SceneTables& t, uint32_t slot, uint32_t mode, const float* uv, float* out) {
  const float u = uv[0], v = uv[1];  // read before writing: out may alias uv
  if (slot >= t.image_count) {
    out[0] = 1.0f;
    out[1] = 0.0f;
    out[2] = 1.0f;
    return;
  }
  const DeviceImageView im = t.images[slot];
  const bool extend = (mode & kTexExtend) != 0u;
  const float x = u * float(im.width), y = v * float(im.height);
  if ((mode & kTexLinear) != 0u)
    tex_linear(im, x, y, extend, out);
  else
    tex_closest(im, t.srgb_decode, x, y, extend, out);
}

AR_FN void vm_op_tex_image(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 3) || !vm_regs_ok(vm, vm.b, 3)) return;
  const uint32_t mode = vm_imm(vm, 1);
  if ((mode & ~kTexModeMask) != 0u) return vm_fail(vm, VmStatus::BAD_OPERAND);
  // An empty table is a scene without images (every slot is magenta); a count without its
  // arrays is a wiring bug.
  if (vm.tables == nullptr ||
      (vm.tables->image_count != 0u && (vm.tables->images == nullptr || vm.tables->srgb_decode == nullptr)))
    return vm_fail(vm, VmStatus::NULL_POINTER);
  op_tex_image(*vm.tables, vm_imm(vm, 0), mode, vm.r + vm.b, vm.r + vm.a);
  vm.pc += 3;
}
AR_NS_END
#define handle_TEX_IMAGE() ::ar::vm_op_tex_image(vm)
