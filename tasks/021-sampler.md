# 021 Sampler: Owen-scrambled Sobol, independent mode and the dimension map
Goal: there is no random-number source -> 0003 decision 23's counter-based sampler exists as integer-only kernel code with its keys, full dimension map, variable streams and independent mode, bit-identical between the float and double builds and verified for stratification and independence.
Read first: 0003 d23 (definition, keys, seed, independent mode, event index, dimension map, streams), d6 (hero lane from its own dimension), 0003 Verification checks 14, 24, 25; R3, R26.
Scope: src/kernel/sample/sobol.h (direction numbers as constants), src/kernel/sample/hash.h, src/kernel/sample/dims.h, src/kernel/sample/keys.h, tests/cpu/test_sampler.cpp (+ f64 copy), third_party/deps.toml `[[borrowed]]` rows and the regenerated THIRD_PARTY.md.
Out of scope: the GPU part of check 24 (028); consumers (generate_camera 054, lobes, lights).
Reference: Burley 2020, "Practical Hash-based Owen Scrambling", JCGT 9(4); Joe and Kuo 2008, SIAM J. Sci. Comput. 30(5), direction numbers (first four dimensions of new-joe-kuo-6.21201, licence text recorded in THIRD_PARTY.md); Cycles `kernel/sample/sobol_burley.h` (Apache-2.0); pbrt-v4 `src/pbrt/util/lowdiscrepancy.h` `FastOwenScrambler` and `util/hash.h` `MixBits` (Apache-2.0); Jarzynski and Olano 2020, JCGT 9(3), `pcg4d`.
Do:
1. `sample(id, index, stream, d, seed) -> Real` exactly as 0003 d23: block b = d/4, lane j = d%4; s = hash(id, seed, stream, b); Owen-shuffle the index with s; 4-D Sobol lane j; Owen-scramble with hash(s, j); `u = (x >> 8) * 2^-24`. Integer arithmetic only; the final conversion is exact in float and double.
2. Independent mode: `sample_independent(...)` = `(pcg4d(id, index, stream * 2^24 + d, seed).x >> 8) * 2^-24`; mode is a per-session uniform passed in, never per path.
3. keys.h: camera key (id = pixel = y*W + x at allocated film size; index = n + sample_id % k; pixel from the active list entry sample_id / k); light key (id = 2^31 + sample_id, index = pass number in the epoch).
4. dims.h: named constants for every row of the fixed map (dims 0-7 per path kind and the 12-dimension per-event block at 8 + 12e) and stream ids 1-3; a function `event_dim(e, offset)`.
5. Tests (gate suite unless noted): f32 and f64 test objects produce bit-identical u over 10^6 (id, index, stream, d) draws written to a temp file and compared (fast suite, deterministic); the first 2^m samples of dims (0,1) for one pixel form a (0, m, 2)-net for m = 1..10 (fast); chi-squared uniformity of 16 cross-block dimension pairs through the harness (020); independent mode per-pixel lag-1 correlation within the harness bound of 0; check 14: chi-squared independence of (h, floor(4u_wavelength)) over 2^16 paths; seed 0 passes the same tests as seed 12345.
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="sampler/*"` passes; the reviewer can see the bitwise f32/f64 comparison count of 10^6 in the test output.
Look at: none.
Canary: tasks/021.canary.patch must turn red: astroray_tests_cpu "sampler/net property dims 0 1", "sampler/f32 f64 bitwise".
Budget: none.
Depends on: 019, 020.
Size: M (about 400 lines); stop and report at 2x.
Progress:
