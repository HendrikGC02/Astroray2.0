// SPDX-License-Identifier: Apache-2.0
// Hand-written FITS 4.0 film dump: an empty primary HDU with provenance keywords, then one IMAGE
// extension per channel component (mean for MOMENT and ADD, raw counters for COUNT) with VAR and
// NSAMP companions for MOMENT channels. Rows are written as the film holds them, bottom row
// first, which is FITS row order, so there is no flip. It accepts only a FilmView: there is no
// overload for a DisplayImage, so a denoised image cannot be written as science data and every
// file says DENOISED = F.
// Ref: FITS Standard 4.0 (IAU FITS Working Group, 2016) sections 3 (2880-byte blocks, 80-byte
// cards), 4 (keyword formats), 7.1 (IMAGE extension), 5 (big-endian BITPIX -32/-64/32);
// docs/decisions/0008-film-outputs-denoise.md d13, d17; 0001 D9.1 (no FITS library).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "host/film/film_view.h"

namespace astroray::host {

enum class FitsPrecision : uint8_t { F32 = 0, F64 };

// What the primary HDU records besides the engine version and commit, which the writer takes from
// the build. Every text field must be non-empty printable ASCII of at most 68 characters; the
// writer throws std::invalid_argument otherwise (no silent truncation).
struct FitsProvenance {
  uint32_t seed = 0;
  uint32_t spp = 0;
  std::string sampler_mode;
  std::string observer_hash;
  std::string working_space;
  std::string filter;
  std::string wavelength_pdf;
};

// The file's bytes. Throws std::invalid_argument for bad provenance and std::length_error for a
// counter above INT32_MAX (BITPIX 32).
std::vector<uint8_t> fits_film_dump_bytes(const FilmView& film, const FitsProvenance& provenance,
                                          FitsPrecision precision = FitsPrecision::F32);

// Writes fits_film_dump_bytes to `path` (UTF-8); throws std::runtime_error when it cannot.
void write_fits_film_dump(const std::string& path, const FilmView& film, const FitsProvenance& provenance,
                          FitsPrecision precision = FitsPrecision::F32);

}  // namespace astroray::host
