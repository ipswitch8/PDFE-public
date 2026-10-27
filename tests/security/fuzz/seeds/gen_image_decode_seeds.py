#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_image_decode_seeds.py -- seed corpus for the fuzz_image_decode harness.

Meta-audit follow-up F4 / gap SEC-06.

The harness (tests/security/fuzz/fuzz_image_decode.cpp) takes the fuzz input
as the RAW IMAGE XOBJECT STREAM and wraps it, unchanged, three times: once as
/Filter /JPXDecode, once as /DCTDecode and once as /FlateDecode. A seed file is
therefore just a codestream/blob -- no PDF scaffolding, the harness supplies it.

Seeds are minimal *valid-ish* images of each codec so the fuzzer starts from
reachable decoder states instead of spending its first hours rediscovering the
JPEG SOI marker. Truncated/corrupt variants are included because the classic
decoder overflow lives just past a header the decoder already trusted.

Every seed is DETERMINISTIC -- no RNG, no timestamps, no PIL -- so the bytes are
byte-stable across machines, matching the convention of the sibling generators
gen_adversarial_seeds.py and gen_encryption_dict_seeds.py.

Run:
    python tests/security/fuzz/seeds/gen_image_decode_seeds.py
"""
from __future__ import annotations

import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = os.path.join(HERE, "fuzz_image_decode")


# ---------------------------------------------------------------------------
# JPEG2000 (JPXDecode -> MuPDF fz_load_jpx -> OpenJPEG)
#   This is the #50 / DCA-0011 surface: malformed JPX heap-overflowed
#   OpenJPEG 2.5.4 (0xC0000374). The harness enables the JPX decode gate so
#   these bytes actually reach openjpeg.
# ---------------------------------------------------------------------------

def _j2k_codestream(width: int = 8, height: int = 8, ncomp: int = 1) -> bytes:
    """A minimal raw J2K codestream: SOC SIZ COD QCD SOT SOD <data> EOC."""
    soc = b"\xff\x4f"

    # SIZ: Lsiz = 38 + 3*Csiz
    siz_body = struct.pack(
        ">HIIIIIIIIH",
        0,            # Rsiz  (capabilities: baseline)
        width,        # Xsiz
        height,       # Ysiz
        0,            # XOsiz
        0,            # YOsiz
        width,        # XTsiz (single tile covering the image)
        height,       # YTsiz
        0,            # XTOsiz
        0,            # YTOsiz
        ncomp,        # Csiz
    )
    # per-component: Ssiz (8-bit unsigned = 7), XRsiz, YRsiz
    siz_body += b"\x07\x01\x01" * ncomp
    siz = b"\xff\x51" + struct.pack(">H", 2 + len(siz_body)) + siz_body

    # COD: Scod=0, SGcod = (prog order 0 LRCP, 1 layer, no MCT),
    #      SPcod = (0 decomposition levels, 64x64 codeblocks, style 0, 5/3 wavelet)
    cod_body = b"\x00" + b"\x00" + struct.pack(">H", 1) + b"\x00" \
        + b"\x00" + b"\x04" + b"\x04" + b"\x00" + b"\x01"
    cod = b"\xff\x52" + struct.pack(">H", 2 + len(cod_body)) + cod_body

    # QCD: Sqcd = 0x40 (no quantization, 2 guard bits); 3*NL+1 = 1 exponent byte
    qcd_body = b"\x40" + b"\x40"
    qcd = b"\xff\x5c" + struct.pack(">H", 2 + len(qcd_body)) + qcd_body

    data = b"\x00" * 16                      # tile-part entropy payload
    sod = b"\xff\x93" + data
    psot = 12 + len(sod)                     # SOT segment + SOD + data
    sot = b"\xff\x90" + struct.pack(">HHIBB", 10, 0, psot, 0, 1)

    eoc = b"\xff\xd9"
    return soc + siz + cod + qcd + sot + sod + eoc


def _jp2_container(codestream: bytes, width: int = 8, height: int = 8,
                   ncomp: int = 1) -> bytes:
    """Wrap a J2K codestream in the JP2 box container (signature/ftyp/jp2h/jp2c)."""
    def box(btype: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", 8 + len(payload)) + btype + payload

    sig = box(b"jP  ", b"\x0d\x0a\x87\x0a")
    ftyp = box(b"ftyp", b"jp2 " + struct.pack(">I", 0) + b"jp2 ")
    ihdr = box(b"ihdr", struct.pack(">IIHBBBB", height, width, ncomp, 7, 7, 0, 0))
    colr = box(b"colr", b"\x01\x00\x00" + struct.pack(">I", 17))  # greyscale
    jp2h = box(b"jp2h", ihdr + colr)
    jp2c = box(b"jp2c", codestream)
    return sig + ftyp + jp2h + jp2c


# ---------------------------------------------------------------------------
# JPEG (DCTDecode -> MuPDF fz_load_jpeg -> libjpeg-turbo)
# ---------------------------------------------------------------------------

# Annex-K standard luminance Huffman tables (the tables every baseline encoder
# emits). Kept literal so the seed is reproducible without a JPEG library.
_DC_LUM_BITS = bytes([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0])
_DC_LUM_VALS = bytes(range(12))
_AC_LUM_BITS = bytes([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d])
_AC_LUM_VALS = bytes([
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
])


def _jpeg_gray_8x8() -> bytes:
    """A minimal baseline grayscale 8x8 JPEG: one MCU, DC=0 + EOB.

    Entropy-coded bits with the standard luminance tables:
        DC category 0 -> '00'   (2 bits)
        AC EOB        -> '1010' (4 bits)
    -> 001010, padded with 1-bits to a byte -> 0b00101011 = 0x2b.
    """
    def seg(marker: int, payload: bytes) -> bytes:
        return bytes([0xff, marker]) + struct.pack(">H", 2 + len(payload)) + payload

    soi = b"\xff\xd8"
    # DQT: table 0, 8-bit precision, all-ones quant table (no scaling surprises)
    dqt = seg(0xdb, b"\x00" + b"\x01" * 64)
    # SOF0: precision 8, 8x8, 1 component (id 1, 1x1 sampling, quant table 0)
    sof0 = seg(0xc0, b"\x08" + struct.pack(">HH", 8, 8) + b"\x01"
               + b"\x01\x11\x00")
    dht_dc = seg(0xc4, b"\x00" + _DC_LUM_BITS + _DC_LUM_VALS)
    dht_ac = seg(0xc4, b"\x10" + _AC_LUM_BITS + _AC_LUM_VALS)
    # SOS: 1 component (id 1, DC table 0 / AC table 0), Ss=0 Se=63 Ah/Al=0
    sos = seg(0xda, b"\x01" + b"\x01\x00" + b"\x00\x3f\x00")
    scan = b"\x2b"
    eoi = b"\xff\xd9"
    return soi + dqt + sof0 + dht_dc + dht_ac + sos + scan + eoi


# ---------------------------------------------------------------------------
# Flate (FlateDecode -> zlib inflate + raw-sample unpack)
# ---------------------------------------------------------------------------

def _flate_raw_samples() -> bytes:
    """zlib stream of 64 raw 8-bit grayscale samples (an 8x8 DeviceGray image)."""
    raw = bytes((i * 4) & 0xff for i in range(64))
    return zlib.compress(raw, 6)


def _flate_short_samples() -> bytes:
    """zlib stream carrying FEWER samples than any plausible /Width*/Height.

    The unpack loop trusting the dictionary geometry over the actual inflated
    length is exactly the shape of an over-read.
    """
    return zlib.compress(b"\x00\x01\x02\x03", 6)


# ---------------------------------------------------------------------------
# Seed table
# ---------------------------------------------------------------------------

def _seeds() -> "dict[str, bytes]":
    j2k = _j2k_codestream()
    jp2 = _jp2_container(j2k)
    jpg = _jpeg_gray_8x8()

    seeds: dict[str, bytes] = {}

    # --- JPX arm -----------------------------------------------------------
    seeds["seed_jpx_j2k_codestream.bin"] = j2k
    seeds["seed_jpx_jp2_container.bin"] = jp2
    # Truncated mid-codestream: header accepted, payload gone.
    seeds["seed_jpx_truncated.bin"] = jp2[: len(jp2) // 2]
    # Header lies about geometry: ihdr/SIZ claim 65535x65535 for a payload
    # sized for 8x8 (integer-overflow / allocation-mismatch bait).
    seeds["seed_jpx_huge_dims.bin"] = _jp2_container(
        _j2k_codestream(width=65535, height=65535), width=65535, height=65535)

    # --- DCT arm -----------------------------------------------------------
    seeds["seed_dct_gray_8x8.bin"] = jpg
    # Truncated after SOS header: the scan data the decoder expects never
    # arrives.
    seeds["seed_dct_truncated.bin"] = jpg[:-3]
    # SOF0 rewritten to claim 0 height — a dimension a decoder must reject
    # before it multiplies it into a buffer size.
    zero_h = bytearray(jpg)
    sof_at = zero_h.find(b"\xff\xc0")
    assert sof_at >= 0, "SOF0 marker not found in generated JPEG"
    # +5 = marker(2) + length(2) + precision(1); then 2-byte height
    zero_h[sof_at + 5: sof_at + 7] = b"\x00\x00"
    seeds["seed_dct_zero_height.bin"] = bytes(zero_h)

    # --- Flate arm ---------------------------------------------------------
    seeds["seed_flate_gray_8x8.bin"] = _flate_raw_samples()
    seeds["seed_flate_short.bin"] = _flate_short_samples()
    # Valid zlib header, corrupted deflate body -> inflate error path.
    corrupt = bytearray(_flate_raw_samples())
    corrupt[len(corrupt) // 2] ^= 0xff
    seeds["seed_flate_corrupt.bin"] = bytes(corrupt)

    return seeds


def main() -> None:
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print(f"Output directory: {OUTPUT_DIR}\n")
    seeds = _seeds()
    for name in sorted(seeds):
        path = os.path.join(OUTPUT_DIR, name)
        with open(path, "wb") as fh:
            fh.write(seeds[name])
        print(f"  wrote {len(seeds[name]):>6} bytes  ->  {name}")
    print(f"\nDone. {len(seeds)} seed files written.")


if __name__ == "__main__":
    main()
