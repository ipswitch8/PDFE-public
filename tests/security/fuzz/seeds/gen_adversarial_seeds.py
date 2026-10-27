#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_adversarial_seeds.py -- deterministic adversarial fuzz seeds (C-09).

Coverage-Expansion phase-4: extend the committed seed corpora for the
write/mutation-surface harnesses with adversarial inputs the shallow
happy-path seeds never represented -- malformed headers, truncation, oversized
numeric values, and deep nesting. Every seed is DETERMINISTIC (no RNG, no
timestamp), so the bytes are byte-stable across machines and the smoke test
(tests/test_fuzz_harness_smoke.cpp) drives each one through the real harness
core with the strengthened no-crash / bounded-time / triage oracle.

These are ADVERSARIAL-BUT-BOUNDED structural PDFs: MuPDF should refuse or
repair them (a benign MuPdfException the harness swallows), never crash. A seed
that trips a REAL fault is a genuine finding -- exactly what the oracle surfaces.

Run:
    python tests/security/fuzz/seeds/gen_adversarial_seeds.py
"""
from __future__ import annotations

import os

HERE = os.path.dirname(os.path.abspath(__file__))


def _minimal_page_pdf(extra_obj4: bytes = b"") -> bytes:
    """A structurally-parseable 1-page PDF; extra_obj4 is spliced into a bogus
    resource object so a harness that walks resources reaches the payload."""
    body = (
        b"%PDF-1.7\n"
        b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        b"3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Contents 4 0 R >>\nendobj\n"
        b"4 0 obj\n<< /Length 44 " + extra_obj4 + b">>\nstream\n"
        b"BT /F1 12 Tf 100 700 Td (Hello) Tj ET\n"
        b"endstream\nendobj\n"
        b"trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n"
    )
    return body


# name -> bytes.  Each is a distinct adversarial CLASS.
def _seeds() -> dict[str, bytes]:
    seeds: dict[str, bytes] = {}

    # 1. Malformed header: valid objects but a corrupt %PDF signature so the
    #    normal parse fails and the repair path must kick in.
    seeds["adv_malformed_header.pdf"] = (
        b"%!GARBAGE-NOT-A-PDF\n" + _minimal_page_pdf()[8:]
    )

    # 2. Truncation: a well-formed prefix cut mid-object (no trailer/xref).
    full = _minimal_page_pdf()
    seeds["adv_truncated.pdf"] = full[: len(full) // 2]

    # 3. Oversized values: absurd /Length and MediaBox integers (overflow bait).
    seeds["adv_oversized_values.pdf"] = (
        b"%PDF-1.7\n"
        b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        b"3 0 obj\n<< /Type /Page /Parent 2 0 R "
        b"/MediaBox [0 0 999999999999 999999999999] /Contents 4 0 R >>\nendobj\n"
        b"4 0 obj\n<< /Length 2147483647 >>\nstream\n"
        b"BT /F1 12 Tf 0 0 Td (x) Tj ET\n"
        b"endstream\nendobj\n"
        b"trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n"
    )

    # 4. Deep nesting: a deeply-nested array in a resource dict (recursion bait).
    nest = b"[" * 400 + b"0" + b"]" * 400
    seeds["adv_deep_nesting.pdf"] = _minimal_page_pdf(b"/Deep " + nest + b" ")

    # 5. Circular reference: object 3 points to itself as Parent.
    seeds["adv_circular_ref.pdf"] = (
        b"%PDF-1.7\n"
        b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        b"3 0 obj\n<< /Type /Page /Parent 3 0 R /MediaBox [0 0 612 792] "
        b">>\nendobj\n"
        b"trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n"
    )

    return seeds


# Harnesses (seed subdirs) that consume real PDF bytes -> get every adversarial
# seed. These match tests/test_fuzz_harness_smoke.cpp driveCore(...) harness ids.
TARGET_HARNESSES = [
    "fuzz_content_stream_edit",
    "fuzz_save_roundtrip",
    "fuzz_redaction_apply",
    "fuzz_document_open",
]


def main() -> int:
    seeds = _seeds()
    written = 0
    for harness in TARGET_HARNESSES:
        d = os.path.join(HERE, harness)
        os.makedirs(d, exist_ok=True)
        for name, data in seeds.items():
            path = os.path.join(d, name)
            with open(path, "wb") as fh:
                fh.write(data)
            written += 1
    print("wrote %d adversarial seed file(s) across %d harness dir(s)"
          % (written, len(TARGET_HARNESSES)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
