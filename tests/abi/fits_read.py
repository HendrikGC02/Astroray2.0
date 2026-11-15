# SPDX-License-Identifier: Apache-2.0
"""Parses a film-dump FITS file with the standard library and NumPy only and checks it against
the synthetic 3x2 film of tests/cpu/test_fits.cpp (keep the formulas in step).

usage: fits_read.py FILE BITPIX   (BITPIX is -32 or -64, the precision the dump was written at)
Exit status 0 and one "fits_read: OK" line when every check passes, 1 with the first failure.

Ref: FITS Standard 4.0 (IAU FITS Working Group, 2016): 2880-byte blocks of 80-byte cards,
big-endian data, row 1 is the bottom row of the image.
"""
import sys

import numpy as np

BLOCK = 2880


def read_hdus(raw):
    """[(header dict, ndarray or None)] for a primary HDU and IMAGE extensions."""
    if len(raw) % BLOCK:
        raise ValueError("file size %d is not a multiple of %d" % (len(raw), BLOCK))
    hdus, at = [], 0
    while at < len(raw):
        header, done = {}, False
        while not done:
            if at + BLOCK > len(raw):
                raise ValueError("header has no END card")
            for i in range(0, BLOCK, 80):
                card = raw[at + i:at + i + 80].decode("ascii")
                if card[:3] == "END" and card[3:].strip() == "":
                    done = True
                    break
                if card[8:10] == "= ":
                    value = card[10:].strip()
                    if value.startswith("'"):
                        value = value[1:value.index("'", 1)].rstrip()
                    elif value in ("T", "F"):
                        value = value == "T"
                    else:
                        value = int(value)
                    header[card[:8].strip()] = value
            at += BLOCK
        naxis = header["NAXIS"]
        data = None
        if naxis:
            shape = [header["NAXIS%d" % (a + 1)] for a in range(naxis)][::-1]
            bitpix = header["BITPIX"]
            dtype = {-32: ">f4", -64: ">f8", 32: ">i4"}[bitpix]
            nbytes = int(np.prod(shape)) * abs(bitpix) // 8
            data = np.frombuffer(raw[at:at + nbytes], dtype=dtype).reshape(shape)
            at += (nbytes + BLOCK - 1) // BLOCK * BLOCK
        hdus.append((header, data))
    return hdus


def expected_planes():
    """EXTNAME -> expected array, shape (2, 3), row 0 = bottom row; pixel p = y * 3 + x."""
    p = np.arange(6, dtype=np.float64).reshape(2, 3)
    n = np.full((2, 3), 4, dtype=np.int64)
    n[1, 2] = 1
    var = 0.5 + p / 8.0
    var[1, 2] = np.nan
    out = {}
    for c in range(3):
        out["display.%d" % c] = 10 * c + p + 1
        out["display.%d.VAR" % c] = var
        out["display.%d.NSAMP" % c] = n
    out["probe"] = p + 1
    out["bounces"] = 2 * (p + 1)
    out["bounce_max"] = p + 1
    out["bounce_paths"] = np.full((2, 3), 4)
    return out


def main(path, bitpix):
    with open(path, "rb") as f:
        hdus = read_hdus(f.read())
    primary = hdus[0][0]
    assert primary["SIMPLE"] is True and primary["NAXIS"] == 0, "primary HDU is not empty"
    assert primary["DENOISED"] is False, "DENOISED is not F"
    for key, want in (("SEED", 7), ("SPP", 4), ("SAMPLER", "sobol"), ("OBSHASH", "0123456789abcdef"),
                      ("WORKSPC", "Rec.709"), ("FILTER", "gaussian"), ("WLPDF", "visible_default")):
        assert primary[key] == want, "%s = %r, expected %r" % (key, primary[key], want)
    assert primary["ENGVER"] and primary["COMMIT"], "engine version or commit missing"

    want = expected_planes()
    got = {h["EXTNAME"]: (h, d) for h, d in hdus[1:]}
    assert set(got) == set(want), "extensions %s, expected %s" % (sorted(got), sorted(want))
    rtol = 1e-6 if bitpix == -32 else 1e-12
    for name, expect in want.items():
        header, data = got[name]
        assert header["XTENSION"] == "IMAGE", name
        is_int = name.endswith(".NSAMP") or name.startswith("bounce_")
        assert header["BITPIX"] == (32 if is_int else bitpix), "%s BITPIX %d" % (name, header["BITPIX"])
        assert data.shape == (2, 3), "%s shape %s" % (name, data.shape)
        # Row 0 of the array is FITS row 1, the film's bottom row: no flip anywhere.
        np.testing.assert_allclose(data, expect, rtol=rtol, atol=0, err_msg=name)
    print("fits_read: OK %d extensions, BITPIX %d" % (len(got), bitpix))


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[2] not in ("-32", "-64"):
        sys.exit("usage: fits_read.py FILE -32|-64")
    try:
        main(sys.argv[1], int(sys.argv[2]))
    except (AssertionError, ValueError, KeyError) as e:
        print("fits_read: FAIL %s" % e)
        sys.exit(1)
