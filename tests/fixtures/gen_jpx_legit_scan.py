# -*- coding: utf-8 -*-
"""Generate the F39-c "legitimate large scan" JPEG2000 fixture.

Meta-audit follow-up F39-c (phase-47) adds a decode-time pixel-count bound to
MuPDF's JPX path (FZ_PDFE_JPX_MAX_PIXELS = 600,000,000 px; see
vcpkg-overlay-ports/libmupdf/0001-pdfe-jpx-decode-gate.patch).

A bound that refuses everything is not a bound, it is a ban. So the guard test
needs a NEGATIVE control: a JPX that is unambiguously a real scan and that must
still decode. This produces it.

Geometry: 2480 x 3508 = 8,699,840 px -- ISO A4 at 300 dpi, the single most
common archival document-scan geometry. That is 1.45% of the bound, i.e. well
under it, while the F39-c attack fixture (65535 x 65535 = 4,294,836,225 px) is
7.16x OVER it. One file on each side of the threshold, from the same decode
entry point.

Content is a smooth diagonal gradient with a dark bar, which is cheap to encode
(the committed file is tens of KB) but still exercises a genuine multi-level
wavelet decode rather than a degenerate constant image.

Usage (regenerates the committed fixture in place):
    python tests/fixtures/gen_jpx_legit_scan.py
"""

from __future__ import annotations

import hashlib
import io
import os

import numpy as np
from PIL import Image

WIDTH = 2480   # ISO A4 short edge (210 mm) at 300 dpi
HEIGHT = 3508  # ISO A4 long edge  (297 mm) at 300 dpi
OUT_NAME = "jpx_legit_a4_300dpi.jp2"


def build_image() -> Image.Image:
    x = np.linspace(0.0, 255.0, WIDTH, dtype=np.float32)[None, :]
    y = np.linspace(0.0, 255.0, HEIGHT, dtype=np.float32)[:, None]
    samples = (x * 0.5 + y * 0.5).astype("uint8")
    # A dark horizontal bar, so the image has real high-frequency content and a
    # decode failure that returned a flat pixmap could not masquerade as success.
    samples[200:400, 200:WIDTH - 200] = 16
    return Image.fromarray(samples, "L")


def main() -> None:
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), OUT_NAME)
    buf = io.BytesIO()
    build_image().save(buf, format="JPEG2000", irreversible=True,
                       quality_layers=[40])
    data = buf.getvalue()

    with open(out_path, "wb") as fh:
        fh.write(data)

    with io.open(out_path, "rb") as fh:
        digest = hashlib.sha256(fh.read()).hexdigest()
    print("wrote %s: %d x %d = %d px, %d bytes, sha256=%s"
          % (out_path, WIDTH, HEIGHT, WIDTH * HEIGHT, len(data), digest))


if __name__ == "__main__":
    main()
