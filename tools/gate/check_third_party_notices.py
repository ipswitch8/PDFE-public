#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Standing control: committed third-party material stays attributed (F46).

WHAT THIS CLOSES
================
`test_PDFs/crazyones-pdfa.pdf` was committed in phase-46 with **no repo record
of where it came from** — no fetch script, no manifest, no README. It was later
identified as `021-pdfa/crazyones-pdfa.pdf` from `py-pdf/sample-files`, which is
**CC-BY-SA-4.0**, a licence that REQUIRES attribution. At that point the
repository had no attribution mechanism at all: the only licence file was
`LICENSE` (AGPL-3.0), and `git ls-files` matching
`^(NOTICE|THIRD|CREDITS|COPYRIGHT|AUTHORS)` returned nothing.

`THIRD-PARTY-NOTICES.md` now carries the entry. This checker is the ratchet that
stops it rotting away from the artifact it describes.

THE FAILURE MODE IT PREVENTS
============================
An attribution that names a file is only worth anything while it still describes
THAT file. Three ways it silently stops being true, all gated here:

  * the fixture is replaced or regenerated and the notice keeps the old hash;
  * the notice entry is edited away while the fixture stays committed;
  * the fixture is removed while the notice keeps claiming it is here.

Pinning the SHA-256 makes all three mechanical. This is the same shape phase-45
used for risk acceptances, and the reason is the same: this programme's
recurring defect is a claim whose evidence cannot establish it, and an
attribution nobody re-checks becomes exactly that.

NOT GATED, DELIBERATELY
=======================
This checker does NOT attempt to verify the upstream licence over the network,
and does not opine on whether any use is permitted. It checks that the recorded
FACTS still match the committed bytes. Licence assessment is a human judgement
and is recorded as such in `THIRD-PARTY-NOTICES.md`.

CRLF NOTE: `core.autocrlf=true`, so the notice is read as text with universal
newlines and only the BINARY fixture is hashed byte-wise. Hashing a tracked text
file here would pass in the session that wrote it and fail after any checkout.

Exit: 0 all checks pass; 1 a check failed; 2 usage/IO error.
"""

from __future__ import annotations

import hashlib
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

NOTICES = "THIRD-PARTY-NOTICES.md"

# Each entry: the committed artifact, and the facts the notice must still state
# about it. Adding a committed third-party artifact means adding a row here.
ENTRIES = {
    "test_PDFs/crazyones-pdfa.pdf": {
        "sha256": "f05f2738a1fa8c1d2e1147881fe1a62516a7f8caaf784067790731f56df626c4",
        "size": 16368,
        # Substrings the entry must contain. Attribution is the POINT of the
        # notice, so the upstream project, its URL and the licence id are
        # required, not optional prose.
        "must_mention": [
            "py-pdf/sample-files",
            "https://github.com/py-pdf/sample-files",
            "CC-BY-SA-4.0",
        ],
    },

    # SEC-04 / GitHub #36 -- pinned subset of Mozilla pdf.js's test corpus,
    # imported as fuzz seeds. Apache-2.0, and genuinely tracked upstream
    # (not `.link` stubs). Digests generated from the import manifest.
    # SEC-90 / GitHub #69 -- the three Tesseract language models the OCR suites
    # load. Apache-2.0, from tesseract-ocr/tessdata_fast @ 4.1.0. These were
    # committed with no README and no attribution at all: this checker reported
    # "9 committed third-party artifact(s) attributed ... OK" while three
    # redistributed 16 MB binaries sat outside its hand-maintained ENTRIES,
    # which is the failure mode a green count is worst at showing.
    "tests/fixtures/tessdata/eng.traineddata": {
        "sha256": "7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2",
        "size": 4113088,
        "must_mention": [
            "tesseract-ocr/tessdata_fast",
            "https://github.com/tesseract-ocr/tessdata_fast",
            "Apache-2.0",
        ],
    },
    "tests/fixtures/tessdata/ara.traineddata": {
        "sha256": "e3206d3dc87fd50c24a0fb9f01838615911d25168f4e64415244b67d2bb3e729",
        "size": 1432056,
        "must_mention": [
            "tesseract-ocr/tessdata_fast",
            "https://github.com/tesseract-ocr/tessdata_fast",
            "Apache-2.0",
        ],
    },
    "tests/fixtures/tessdata/osd.traineddata": {
        "sha256": "9cf5d576fcc47564f11265841e5ca839001e7e6f38ff7f7aacf46d15a96b00ff",
        "size": 10562727,
        "must_mention": [
            "tesseract-ocr/tessdata_fast",
            "https://github.com/tesseract-ocr/tessdata_fast",
            "Apache-2.0",
        ],
    },

    "tests/security/corpus/pdfjs/bug1020226.pdf": {
        "sha256": "50e91d373aa313537972519c65b050f8d4d5bd996859fa20756432c2029c3ff6",
        "size": 184,
        "must_mention": [
            "mozilla/pdf.js",
            "https://github.com/mozilla/pdf.js",
            "Apache-2.0",
        ],
    },
    "tests/security/corpus/pdfjs/issue15590.pdf": {
        "sha256": "c711bb3357eccec499968ac7b4e286ebd3f49910b458b0596d81731c890eedb6",
        "size": 191,
        "must_mention": [
            "mozilla/pdf.js",
            "https://github.com/mozilla/pdf.js",
            "Apache-2.0",
        ],
    },
    "tests/security/corpus/pdfjs/extractPages_null_in_array.pdf": {
        "sha256": "37c087807b2a6a0a4545a077ee45ebb23c3d297e0307feebd1897391d23fa843",
        "size": 360,
        "must_mention": [
            "mozilla/pdf.js",
            "https://github.com/mozilla/pdf.js",
            "Apache-2.0",
        ],
    },
    "tests/security/corpus/pdfjs/clippath.pdf": {
        "sha256": "cbcbf53e8109ed56f9a3fbc696081455c8a5d32cc5cff4e741e85250788dcbff",
        "size": 459,
        "must_mention": [
            "mozilla/pdf.js",
            "https://github.com/mozilla/pdf.js",
            "Apache-2.0",
        ],
    },
    "tests/security/corpus/pdfjs/issue4461.pdf": {
        "sha256": "86f561b0b17a9487326fba41f717b7e085ff838bc101c07ebad929c29466f52e",
        "size": 476,
        "must_mention": [
            "mozilla/pdf.js",
            "https://github.com/mozilla/pdf.js",
            "Apache-2.0",
        ],
    },
    "tests/security/corpus/pdfjs/GHOSTSCRIPT-698804-1-fuzzed.pdf": {
        "sha256": "62e518ca7894e8b8b8ed9927da3007cdc5e9ccc5b111bbb22d6101d4a8d7490e",
        "size": 627,
        "must_mention": [
            "mozilla/pdf.js",
            "https://github.com/mozilla/pdf.js",
            "Apache-2.0",
        ],
    },
    "tests/security/corpus/pdfjs/asciihexdecode.pdf": {
        "sha256": "17cc92aa58ec48f75b01c6b6e7f8c8bade0a36f7295d0f833a16bafae854dc79",
        "size": 743,
        "must_mention": [
            "mozilla/pdf.js",
            "https://github.com/mozilla/pdf.js",
            "Apache-2.0",
        ],
    },
    "tests/security/corpus/pdfjs/checkbox_no_appearance.pdf": {
        "sha256": "31e82e933f4d4664c6ae1c4418cc44efce0c614c10c85c34b515b38e04e08bcd",
        "size": 694,
        "must_mention": [
            "mozilla/pdf.js",
            "https://github.com/mozilla/pdf.js",
            "Apache-2.0",
        ],
    },
}


class Failure(Exception):
    """Usage / IO error -- exit 2, distinct from a gate failure."""


def _read_text(rel: str) -> str:
    path = REPO_ROOT / rel
    if not path.is_file():
        raise Failure(f"{rel} does not exist")
    return path.read_text(encoding="utf-8")


def _tracked(rel: str) -> bool:
    out = subprocess.run(["git", "ls-files", "--error-unmatch", rel],
                         cwd=str(REPO_ROOT), capture_output=True, text=True)
    return out.returncode == 0


def check(problems, notices_text: str | None = None, entries=None):
    """Append a message per problem. Pure enough to self-test with a fake notice."""
    if notices_text is None:
        notices_text = _read_text(NOTICES)
    if entries is None:
        entries = ENTRIES

    for rel, spec in sorted(entries.items()):
        path = REPO_ROOT / rel

        if rel not in notices_text:
            problems.append(
                f"{NOTICES} does not mention {rel!r}. It is committed third-party "
                "material; its attribution entry must not be removed while the file "
                "is still in the tree."
            )
            continue

        if not path.is_file():
            problems.append(
                f"{NOTICES} carries an attribution entry for {rel!r}, but that file "
                "is not in the tree. Remove the entry or restore the file -- a notice "
                "describing an artifact that is gone is worse than no notice."
            )
            continue

        if not _tracked(rel):
            problems.append(
                f"{rel} is present but NOT TRACKED by git, so a clean checkout will "
                "not have it and the attribution describes nothing. (`git "
                "check-ignore` is the wrong probe for a `!` re-include -- it exits 0 "
                "on the re-include match; this uses `git ls-files`.)"
            )
            continue

        raw = path.read_bytes()
        actual = hashlib.sha256(raw).hexdigest()
        expected = spec["sha256"]

        if actual != expected:
            problems.append(
                f"{rel} SHA-256 is {actual}, but this checker and {NOTICES} record "
                f"{expected}. The committed artifact changed and its attribution did "
                "not. Re-derive the provenance for the NEW bytes -- do not just paste "
                "the new hash in, because the upstream identity is what the licence "
                "claim rests on."
            )
        elif expected not in notices_text:
            problems.append(
                f"{NOTICES} does not record the SHA-256 {expected} for {rel}. The "
                "hash is what binds the attribution to the artifact; without it the "
                "entry can drift silently."
            )

        if len(raw) != spec["size"]:
            problems.append(
                f"{rel} is {len(raw)} bytes; the recorded size is {spec['size']}."
            )
        elif str(spec["size"]) not in notices_text.replace(",", ""):
            problems.append(
                f"{NOTICES} does not record the size {spec['size']} for {rel}."
            )

        for needle in spec["must_mention"]:
            if needle not in notices_text:
                problems.append(
                    f"{NOTICES} must name {needle!r} for {rel} -- attribution "
                    "(upstream project, URL, licence) is the obligation the entry "
                    "exists to satisfy."
                )

    # A notice that is not a clearance must not read like one. These phrasings
    # are the ones this programme keeps producing under time pressure.
    for banned in ("licence is cleared", "license is cleared",
                   "cleared for use", "we are permitted to",
                   "no licensing concerns", "legally safe"):
        if re.search(re.escape(banned), notices_text, re.IGNORECASE):
            problems.append(
                f"{NOTICES} contains {banned!r}. This file records FACTS; it is "
                "explicitly not a licence clearance, and must not be edited into one."
            )

    return problems


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):  # pragma: no cover
            pass

    try:
        problems = check([])
    except Failure as exc:
        print(f"[third-party-notices] ERROR: {exc}", file=sys.stderr)
        return 2

    if problems:
        print(f"[third-party-notices] FAIL — {len(problems)} problem(s):")
        for p in problems:
            print(f"  * {p}")
        return 1

    print(f"[third-party-notices] OK — {len(ENTRIES)} committed third-party "
          f"artifact(s) attributed in {NOTICES}, each pinned to its SHA-256.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
