// expect: fail DisplayImage
// A denoised image must not stand in for the film in a film_* write function (0008 d17).
#include "host/film/display_image.h"
#include "host/film/film_view.h"
#include "host/film/fits_writer.h"
#include "kernel/film/film_write.h"

void case_film_write(const astroray::host::DisplayImage& image) {
  ar::film_add(image, 0, 0, 1.0);
}
