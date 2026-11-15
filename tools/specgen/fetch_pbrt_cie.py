# SPDX-License-Identifier: Apache-2.0
"""Fetch the CIE 1931 2-degree tables from pbrt-v4 at a pinned commit.

Run by hand: <python> -I tools/specgen/fetch_pbrt_cie.py
Writes data/spectral/cie1931_2deg.csv (360-830 nm, 1 nm, 471 rows: xbar, ybar, zbar and
the CIE D65 illuminant resampled to the same grid) and prints its SHA-256 for
data/MANIFEST.toml.
Ref: pbrt-v4 src/pbrt/util/spectrum.cpp (Apache-2.0), arrays CIE_X, CIE_Y, CIE_Z and
CIE_Illum_D6500; CIE dataset DOI 10.25039/CIE.DS.xvudnb9b; 0003 d14, 0001 D8.2.
The CIE values carry the CIE's own terms, which pbrt's licence does not cover (0001 D8.2).
"""
import hashlib
import os
import re
import sys
import urllib.request

COMMIT = "b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f"
URL = f"https://raw.githubusercontent.com/mmp/pbrt-v4/{COMMIT}/src/pbrt/util/spectrum.cpp"
SOURCE_SHA256 = "4c907c7655ad5431bd2797692b963ac34bc6a533c7a053a16e6988aea03c750b"
N, LO, HI = 471, 360, 830


def array(src, name):
    m = re.search(r"const Float " + name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit(f"array {name} not found in spectrum.cpp")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [t for t in re.split(r"[\s,]+", body) if t]


def main():
    with urllib.request.urlopen(URL, timeout=60) as r:
        raw = r.read()
    got = hashlib.sha256(raw).hexdigest()
    if got != SOURCE_SHA256:
        sys.exit(f"spectrum.cpp sha256 {got} differs from the pinned {SOURCE_SHA256}")
    src = raw.decode("utf-8")
    cols = {k: array(src, "CIE_" + k) for k in "XYZ"}
    for k, v in cols.items():
        if len(v) != N:
            sys.exit(f"CIE_{k} has {len(v)} samples, expected {N}")
    d = [float(t) for t in array(src, "CIE_Illum_D6500")]
    wl, val = d[0::2], d[1::2]
    if wl[0] > LO or wl[-1] < HI:
        sys.exit("CIE_Illum_D6500 does not cover 360-830 nm")

    def d65(lam):  # linear interpolation, as pbrt's PiecewiseLinearSpectrum
        i = max(k for k in range(len(wl)) if wl[k] <= lam)
        if wl[i] == lam:
            return val[i]
        t = (lam - wl[i]) / (wl[i + 1] - wl[i])
        return val[i] + t * (val[i + 1] - val[i])

    out = ["lambda_nm,xbar,ybar,zbar,d65"]
    for i in range(N):
        lam = LO + i
        out.append(f"{lam},{cols['X'][i]},{cols['Y'][i]},{cols['Z'][i]},{d65(lam):.9g}")
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    path = os.path.join(root, "data", "spectral", "cie1931_2deg.csv")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = ("\n".join(out) + "\n").encode("ascii")
    with open(path, "wb") as f:
        f.write(data)
    print(f"wrote {path}\nsha256 {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
