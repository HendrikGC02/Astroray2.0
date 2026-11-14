# 046 Response-channel conversion and the display channel
Goal: samples cannot become display values -> one kernel-library conversion turns a 4-lane spectral sample at its film wavelengths into any response channel (energy or photon unit), and the display channel maps through the CIE CMFs and the working-space matrix divided by Y_w, with film exposure.
Read first: 0008 d7 (response channels, display, film_exposure), d3 (where conversion runs), 0003 d2 (film wavelengths), d5 (`film_weights`), d11 (Y_w = Y_n, anchor), d13 (lambda_f, CODATA h and c), 0008 V4.
Scope: src/kernel/film/response.h, src/host/film/response_table.{h,cpp}, tests/cpu/test_response.cpp (+ f64).
Out of scope: band, cube and photon channels as user features (M8: only the mechanism and a test curve here); the resolve kernel (047).
Reference: pbrt-v4 `src/pbrt/film.cpp` `RGBFilm::AddSample` and `PixelSensor` (Apache-2.0) for conversion at sample time.
Do:
1. response_table (host): a channel = tabulated R(lambda) (piecewise linear, nm, vacuum) plus unit ENERGY or PHOTON; the display group = three channels xbar, ybar, zbar from observer.bin followed by the 3x3 M_ws of the working space (Rec.709 in M1a); host packs channels into one device table.
2. response.h (kernel): `convert(const Spec& L, const Lambda& lf, const Spec& w /*film_weights*/, channel) -> double` = sum_i L_i R(lf_i) w_u(lf_i) w_i, with w_u = 1 or lambda/(h c) (CODATA 2018 exact h, c folded on the host in double); `display_rgb(...)` = M_ws * XYZ / Y_w with Y_w = Y_n, times `film_exposure` (display and light-pass channels only).
3. Tests (fast): 0008 V4 for Rec.709 (a white reflector under the working-space illuminant, integrated by deterministic quadrature over lambda through `convert`, resolves to (1,1,1) within 1e-4; the CMFs integrate to Y_n); a constant-1 test channel in ENERGY returns the quadrature weight sum; the PHOTON unit scales by lambda/(h c) exactly at a fixed lambda.
Acceptance: `pwsh tools/check.ps1 fast` passes with `response/*` cases; float and double objects agree within 1e-6 relative.
Look at: none.
Canary: tasks/046.canary.patch must turn red: astroray_tests_cpu "response/white reflector is 1 1 1".
Budget: none.
Depends on: 023, 024.
Size: S (about 250 lines); stop and report at 2x.
Progress:
