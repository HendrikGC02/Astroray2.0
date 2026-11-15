// expect: fail DisplayImage
// SPDX-License-Identifier: Apache-2.0
// A denoised image must not reach the FITS writer (0008 d17).
#include "host/film/display_image.h"
#include "host/film/film_view.h"
#include "host/film/fits_writer.h"
#include "kernel/film/film_write.h"

void case_fits(const astroray::host::DisplayImage& image) {
  astroray::host::write_fits_film_dump("x.fits", image, astroray::host::FitsProvenance{});
}
