// expect: fail DisplayImage
// No conversion turns a DisplayImage into a film or a FilmView (0008 d17).
#include "host/film/display_image.h"
#include "host/film/film_view.h"
#include "host/film/fits_writer.h"
#include "kernel/film/film_write.h"

void case_convert(astroray::host::Device& device, const astroray::host::DisplayImage& image) {
  astroray::host::FilmView view(device, image);
}
