// SPDX-License-Identifier: Apache-2.0
// The scene's images: texels as stored (8-bit with an encoding tag, or half and float, which are
// linear), the linear-RGB mean the
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
#include "../../kernel/color/srgb.h"
#include "../../kernel/scene/texture_view.h"
#include "host/ledger.h"

namespace astroray::host {

class Edit;
class Scene;

// ar_image_desc::color_interop is one of the AR_COLOR_INTEROP_* values of astroray.h. 0, 1 and 6
// (sRGB, linear Rec.709, data) decode; the other listed ids (0007 d21) are not converted by the
// decode path yet and render magenta with COLORSPACE_UNSUPPORTED; any other value is unknown. On a
// half or float image an sRGB id is read as linear (0005 item 34).
// The interop id string of a value, or "unknown interop id N".
std::string interop_name(uint32_t color_interop);
// True and the encoding for the decoded values.
bool interop_encoding(uint32_t color_interop, ar::TexEncoding* out);

struct Image {
  uint32_t width = 1, height = 1, channels = 3;
  ar::TexEncoding encoding = ar::TexEncoding::LINEAR;
  ar::TexFormat format = ar::TexFormat::U8;
  uint32_t alpha_mode = AR_ALPHA_STRAIGHT;
  std::vector<uint8_t> texels;       // as decoded (components of `format`), rows bottom-up
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

// The kernel's image table, indexed by slot. views[i] belongs to keys[i]; its texel address
// points into images[i], which the arrays keep alive. A slot whose key is free or not yet active
// holds the 1x1 magenta image (keys[i] is then 0 for a free slot).
struct ImageArrays {
  std::vector<ar_key> keys;
  std::vector<ar::DeviceImageView> views;
  std::vector<std::shared_ptr<const Image>> images;
  std::array<float, ar::kSrgbTableSize> srgb_decode = {};  // SceneTables::srgb_decode
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
  // The table slot of the active image under `key`; false when no image is active under it.
  // Slots are dense and stable: set_image assigns one in the API call, and it is freed when the
  // key is removed (or its only pending set dropped) and is not active.
  bool slot(ar_key key, uint32_t* out) const;
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
  void dropped(ar_key key);
  void rebuild_locked();
  uint32_t assign_slot_locked(ar_key key);
  void free_slot_locked(ar_key key);

  std::shared_ptr<Ledger> ledger_;
  mutable std::mutex mutex_;
  std::map<ar_key, std::shared_ptr<const Image>> active_;
  std::shared_ptr<const ImageArrays> arrays_;
  std::map<ar_key, uint32_t> slots_;
  std::vector<ar_key> slot_keys_;      // per slot; free slots are in free_slots_
  std::vector<uint32_t> free_slots_;
  std::shared_ptr<const Image> magenta_;  // what a free or not yet active slot holds
  std::vector<Pending> pending_;
  uint64_t ledger_id_ = 0;
  // Decoded images by (source, interop id, alpha mode, tile, size, mtime, reload); weak so an
  // image nobody holds is freed.
  std::mutex cache_mutex_;
  struct CacheEntry {
    std::weak_ptr<const Image> image;
    std::vector<uint8_t> packed;  // MEMORY sources: compared on a hit
  };
  std::map<std::string, CacheEntry> cache_;
};

}  // namespace astroray::host
