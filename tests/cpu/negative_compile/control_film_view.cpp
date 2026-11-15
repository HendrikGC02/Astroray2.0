// expect: compile
// SPDX-License-Identifier: Apache-2.0
// Positive control: the same calls with film types compile, so each failing case fails because of
// DisplayImage and not because of a missing include or a changed signature.
#include "host/film/display_image.h"
#include "host/film/film_view.h"
#include "host/film/fits_writer.h"
#include "kernel/film/film_write.h"

void control(const astroray::host::FilmView& view, const ar::KernelGlobals& kg) {
  astroray::host::write_fits_film_dump("x.fits", view, astroray::host::FitsProvenance{});
  ar::film_add(kg, 0, 0, 1.0);
}
