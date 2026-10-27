# -*- coding: utf-8 -*-
"""
Generate seed corpus files for the PDF encryption dictionary fuzzer.

The fuzzer wraps seed bytes into:
    2 0 obj << {SEED_BYTES} >> endobj
with a trailer that references /Encrypt 2 0 R.

Seeds therefore contain only the dictionary BODY (keys + values),
with no surrounding << >> delimiters.
"""

import os

OUTPUT_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "fuzz_encryption_dict",
)

# ---------------------------------------------------------------------------
# Seed definitions
# ---------------------------------------------------------------------------

# 1. Valid AES-256 (V=5, R=6, AESV3)
SEED_AES256 = (
    b"/Filter /Standard "
    b"/V 5 /R 6 /Length 256 "
    b"/CF << /StdCF << /AuthEvent /DocOpen /CFM /AESV3 /Length 32 >> >> "
    b"/StmF /StdCF /StrF /StdCF "
    b"/O (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/U (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/OE (xxxxxxxxxxxxxxxx) "
    b"/UE (xxxxxxxxxxxxxxxx) "
    b"/P -3904 "
    b"/Perms (xxxxxxxxxxxxxxxx)"
)

# 2. Valid RC4-40 (V=1, R=2)
SEED_RC4_40 = (
    b"/Filter /Standard "
    b"/V 1 /R 2 /Length 40 "
    b"/O (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/U (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/P -3904"
)

# 3. Malformed: oversize key length — should trigger bounds check
SEED_OVERSIZE_KEY = (
    b"/Filter /Standard "
    b"/V 5 /R 6 /Length 99999 "
    b"/CF << /StdCF << /CFM /AESV3 /Length 99999 >> >> "
    b"/StmF /StdCF /StrF /StdCF "
    b"/O (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/U (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/P -1"
)

# 4. Malformed: circular CF reference (/StmF -> /AltCF, /StrF -> /StdCF,
#    /AltCF value points back to /StdCF name)
SEED_CIRCULAR_CF = (
    b"/Filter /Standard "
    b"/V 4 /R 4 /Length 128 "
    b"/CF << /StdCF << /CFM /AESV2 /Length 16 >> /AltCF /StdCF >> "
    b"/StmF /AltCF /StrF /StdCF "
    b"/O (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/U (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/P -1"
)

# 5. Malformed: negative /V and /R values, zero length
SEED_NEGATIVE_V = (
    b"/Filter /Standard "
    b"/V -1 /R -1 /Length 0 "
    b"/O (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/U (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/P 0"
)

# 6. Malformed: /Filter key absent entirely
SEED_MISSING_FILTER = (
    b"/V 5 /R 6 /Length 256 "
    b"/O (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/U (xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx) "
    b"/P -3904"
)

# 7. Empty dict body — whitespace only
SEED_EMPTY = b"   "

# ---------------------------------------------------------------------------
# Write
# ---------------------------------------------------------------------------

SEEDS = [
    ("seed_aes256.bin",       SEED_AES256),
    ("seed_rc4_40.bin",       SEED_RC4_40),
    ("seed_oversize_key.bin", SEED_OVERSIZE_KEY),
    ("seed_circular_cf.bin",  SEED_CIRCULAR_CF),
    ("seed_negative_v.bin",   SEED_NEGATIVE_V),
    ("seed_missing_filter.bin", SEED_MISSING_FILTER),
    ("seed_empty.bin",        SEED_EMPTY),
]


def main() -> None:
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print(f"Output directory: {OUTPUT_DIR}\n")

    for filename, data in SEEDS:
        path = os.path.join(OUTPUT_DIR, filename)
        with open(path, "wb") as fh:
            fh.write(data)
        print(f"  wrote {len(data):>6} bytes  ->  {path}")

    print(f"\nDone. {len(SEEDS)} seed files written.")


if __name__ == "__main__":
    main()
