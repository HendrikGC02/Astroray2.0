# 050 FilmView, DisplayImage, the denoiser interface and FITS film dumps
Goal: film data has no read-only view, no science isolation and no file format -> planes are reachable only through `FilmView`, denoisers write only `DisplayImage`, the denoiser interface exists with no backend, a negative-compile test proves the separation, and a hand-written FITS 4.0 writer produces film dumps.
Read first: 0008 d13 (FITS only, film dump), d13a (what readback returns), d16 (denoiser interface, asynchronous creation), d17 (science isolation by type), V9; 0001 D9.1 (no FITS library).
Scope: src/host/film/film_view.h, src/host/film/display_image.h, src/host/denoise/denoiser.{h,cpp}, src/host/film/fits_writer.{h,cpp}, tests/cpu/test_fits.cpp, tests/cpu/negative_compile/ (cases), CMakeLists.txt (custom target `astroray_negative_compile`: compiles each case and fails when an outcome differs from its expectation), tests/abi/fits_read.py.
Out of scope: OIDN (M1b); the science FITS file with bands and cubes (M8); the ABI `ar_session_write_fits` wiring (104).
Reference: FITS Standard 4.0 (IAU FITS Working Group, 2016) for headers, IMAGE extensions and BITPIX -32/-64.
Do:
1. FilmView: read-only access to a channel's mean, variance of the mean and counts (host copies from the device). DisplayImage: an RGBA float image with a `denoised` flag; no conversion from DisplayImage to any film type exists.
2. denoiser.h: `denoise(DenoiserState&, const DenoiseInputs&, DisplayImage& out, DenoiseMode)`; `DenoiserState` created on a worker thread at session start and reporting "ready"; backend `NONE` copies colour to `out` with `denoised = false`. OIDN slots in later without a signature change.
3. fits_writer: primary HDU with provenance keywords (engine version, commit, seed, spp, sampler mode, observer hash, working space, filter, wavelength-pdf name, `DENOISED = F`), then one IMAGE extension per channel component with `EXTNAME`, and `VAR`/`NSAMP` companions for MOMENT channels; BITPIX -32 default, -64 on request; rows bottom-up (no flip); 2880-byte blocks. It accepts only `FilmView`.
4. Negative compile: try_compile cases that pass a DisplayImage to the FITS writer and to a `film_*` write function must fail to compile; a positive control compiles (0008 V9 compile part).
5. test_fits.cpp (fast): a synthetic 3x2 film written at -32 and -64; tests/abi/fits_read.py (stdlib + NumPy) parses headers and data, checks keywords, `DENOISED = F`, values and bottom-up order; test_fits runs the reader through `Get-ArPython` only in `gate` (fast stays C++).
Acceptance: `pwsh tools/check.ps1 fast` passes; `pwsh tools/check.ps1 gate` runs fits_read on the dump and passes; `cmake --build out/build --target astroray_negative_compile` passes and lists each case with its expected outcome.
Look at: none.
Canary: tasks/050.canary.patch must turn red: astroray_tests_cpu "fits/header keywords", and `cmake --build out/build --target astroray_negative_compile` (the patch adds a DisplayImage overload to the writer).
Budget: none.
Depends on: 047, 049.
Size: M (about 400 lines); stop and report at 2x.
Progress:
