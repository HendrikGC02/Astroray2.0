// SPDX-License-Identifier: Apache-2.0
// The scene's images: 8-bit texels as stored (with their encoding tag), the linear-RGB mean the
// emission estimate reads, and the arrays the kernel's image table is built from (host memory,
// ledger category TEXTURES; the device upload is 102). An image is one keyed edit item whose job
// decodes on a scene worker; activation installs it and the material pass raises its report
// entry. An unreadable image or an unsupported colour space becomes a 1x1 magenta texture with
// exactly one entry and the generation still activates.
// Ref: docs/decisions/0007-blender-integration.md d9, d13, d21; 0005-material-model.md items 11,
// 34; 0001 D2.11 (IMAGE_UNREADABLE, COLORSPACE_UNSUPPORTED).
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "astroray/astroray.h"
#include "../../kernel/scene/texture_view.h"
#include "host/ledger.h"

namespace astroray::host {

class Edit;
class Scene;

// ar_image_desc::color_interop. astroray.h defines no numbering yet, so this enum is the one:
// the addon maps Blender 5.3's Image.colorspace_settings.interop_id strings to these values.
// The first four are decoded; the rest are ids 0007 d21 lists that the 8-bit path does not
// convert yet (they render magenta with COLORSPACE_UNSUPPORTED); any other value is unknown.
enum class ColorInterop : uint32_t {
  SRGB_REC709_DISPLAY = 0,  // "srgb_rec709_display": Blender 5.3's id of its "sRGB" colour space
  LIN_REC709_SCENE = 1,     // "lin_rec709_scene"
  DATA = 2,                 // "data" (Non-Color)
  SRGB_REC709_SCENE = 3,    // "srgb_rec709_scene"
  LIN_REC2020_SCENE = 4,
  LIN_AP1_SCENE = 5,
  LIN_AP0_SCENE = 6,
  SRGB_P3D65_DISPLAY = 7,
  LIN_P3D65_SCENE = 8,
};
// The interop id string of a value, or "unknown interop id N".
std::string interop_name(uint32_t color_interop);
// True and the encoding for the decoded values.
bool interop_encoding(uint32_t color_interop, ar::TexEncoding* out);

struct Image {
  uint32_t width = 1, height = 1, channels = 3;
  ar::TexEncoding encoding = ar::TexEncoding::LINEAR;
  uint32_t alpha_mode = AR_ALPHA_STRAIGHT;
  std::vector<uint8_t> texels;       // as decoded, rows bottom-up
  std::array<float, 3> mean = {};    // linear RGB, transfer function decoded before averaging
  bool magenta = false;              // the 1x1 stand-in for an unreadable image
  uint64_t bytes() const { return texels.size(); }
};

// What a decode job produced: the image (never null) and at most one report entry.
struct ImageResult {
  std::shared_ptr<const Image> image;
  uint32_t code = 0;  // 0 or AR_REPORT_IMAGE_UNREADABLE / AR_REPORT_COLORSPACE_UNSUPPORTED
  std::string detail;
};

// The arrays of the active images. views[i] belongs to keys[i]; its texel address points into
// images[i], which the arrays keep alive.
struct ImageArrays {
  std::vector<ar_key> keys;
  std::vector<ar::DeviceImageView> views;
  std::vector<std::shared_ptr<const Image>> images;
};

class ImageStore : public std::enable_shared_from_this<ImageStore> {
 public:
  explicit ImageStore(std::shared_ptr<Ledger> ledger);
  ~ImageStore();
  ImageStore(const ImageStore&) = delete;
  ImageStore& operator=(const ImageStore&) = delete;

  // Stages and commits one keyed item. Throws std::invalid_argument for an enum out of range or
  // a missing path or bytes; PIXELS sources are refused by the caller (M1b). Registers the key
  // with the edit; the caller (MaterialStore) owns the pass that activates it.
  ar_status set_image(Edit& edit, const ar_image_desc* desc);

  // Decode one image from its descriptor. Used by the job and directly by tests; never throws
  // for bad input, only std::bad_alloc.
  ImageResult load(const ar_image_desc& desc, const std::vector<uint8_t>& packed);

  // The active image under `key`, or null.
  std::shared_ptr<const Image> find(ar_key key) const;
  bool mean(ar_key key, std::array<float, 3>* rgb) const;
  std::shared_ptr<const ImageArrays> arrays() const;

  // Activation of an edit's removals (after its sets were installed, which `keep` names).
  void remove(bool reset, const std::set<ar_key>& removed, const std::set<ar_key>& keep);
  // Raises the entries the generation's image items left, subject = the image key.
  void raise_entries(Scene& scene, uint64_t generation);

 private:
  struct Pending {
    ar_key key;
    uint32_t code;
    std::string detail;
  };
  void install(ar_key key, const ImageResult& r);
  void rebuild_locked();

  std::shared_ptr<Ledger> ledger_;
  mutable std::mutex mutex_;
  std::map<ar_key, std::shared_ptr<const Image>> active_;
  std::shared_ptr<const ImageArrays> arrays_;
  std::vector<Pending> pending_;
  uint64_t ledger_id_ = 0;
  // Decoded images by (source, interop id, alpha mode, tile, size, mtime, reload); weak so an
  // image nobody holds is freed.
  std::mutex cache_mutex_;
  std::map<std::string, std::weak_ptr<const Image>> cache_;
};

}  // namespace astroray::host
