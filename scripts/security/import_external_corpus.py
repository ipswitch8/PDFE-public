#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Import a pinned subset of Mozilla pdf.js's test corpus (SEC-04 / GitHub #36).

WHY AN EXTERNAL CORPUS AT ALL. `tests/security/fuzz/seeds/` is grown entirely
from PDFs this project made. A fuzzer seeded only with its own author's idea of
a PDF explores the shapes that author already thought of; the point of importing
someone else's regression corpus is that it encodes bugs *we did not think of*.
pdf.js's `test/pdfs` is fifteen years of real-world reader crashes.

WHAT IS IMPORTED, AND WHY SO LITTLE. Eight files, ~3.7 KB total. Seeds should be
SMALL and structurally diverse: libFuzzer mutates them, so a 6 MB architectural
drawing is a worse seed than a 184-byte minimal file, and a large corpus mostly
costs campaign time re-deriving coverage it already had. These were selected as
the smallest files spanning distinct structural shapes -- a fuzzed input, a
filter, a form field with no appearance stream, a null inside an array, clipping
path operators, and three minimal documents.

LICENCE. pdf.js is Apache-2.0 and these files are genuinely tracked in its
repository (not `.link` stubs pointing at third-party downloads, which carry no
such guarantee). Attribution is recorded in `THIRD-PARTY-NOTICES.md` and pinned
by SHA-256 in `tools/gate/check_third_party_notices.py`, so the files cannot be
swapped without the guard noticing.

NOT THE MALICIOUS CORPUS. `tests/security/corpus/malicious_pdf/` is deliberately
gitignored -- those payloads phone home, trip contributor antivirus on clone, and
would attract GitHub's malware scanners. That policy is unchanged here; see
GitHub #37 for the separate discussion.

Usage:
    python scripts/security/import_external_corpus.py --verify   # default
    python scripts/security/import_external_corpus.py --fetch    # (re)download
"""
from __future__ import annotations

import argparse
import hashlib
import io
import os
import sys
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
# NOT ".../corpus/external/pdfjs". `.gitignore` carries a bare `external/`
# rule (for the ONNX SDK drop), which matches a directory of that name at ANY
# depth -- so files placed there would be silently ignored, and `git add` on an
# ignored file exits 0 while staging NOTHING. That is this repository's
# documented F36 failure mode. The directory is named `pdfjs` instead rather
# than relaxing the exclusion: relaxing an ignore rule is not the same act as
# judging content fit to commit, and doing so has already re-leaked data here
# once.
DEST = REPO_ROOT / "tests" / "security" / "corpus" / "pdfjs"

# Pinned to a specific pdf.js commit so "the file at that path" cannot change
# under us -- the same reasoning as the SHA-pinned GitHub Actions (#147).
UPSTREAM = "mozilla/pdf.js"
UPSTREAM_REF = "1c8020a7d4e43668ac287a3ecf9a8dbea17e4c56"   # tag v6.3.289
RAW = "https://raw.githubusercontent.com/%s/%s/test/pdfs/%s"

# name -> (sha256, why this shape is worth seeding)
FILES = {
    "bug1020226.pdf": (
        "", "minimal document -- the smallest well-formed shape"),
    "issue15590.pdf": (
        "", "minimal variant with a different trailer arrangement"),
    "extractPages_null_in_array.pdf": (
        "", "a null inside an array -- object-parser edge"),
    "clippath.pdf": (
        "", "clipping-path operators in the content stream"),
    "issue4461.pdf": (
        "", "page-tree shape that regressed a real reader"),
    "GHOSTSCRIPT-698804-1-fuzzed.pdf": (
        "", "already fuzzer-derived: a shape no hand-written seed produces"),
    "asciihexdecode.pdf": (
        "", "ASCIIHexDecode filter chain"),
    "checkbox_no_appearance.pdf": (
        "", "form field with no appearance stream -- synthesis path"),
}

# INSIDE the import directory, not beside it. The first version used
# DEST.parent, which is tests/security/corpus/ -- and that already holds the
# malicious-corpus manifest.json. Running --fetch OVERWROTE a tracked 619-line
# file with a 40-line one. Caught by `git status`, restored from HEAD.
MANIFEST = DEST / "manifest.json"


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def fetch(name: str) -> bytes:
    url = RAW % (UPSTREAM, UPSTREAM_REF, name)
    req = urllib.request.Request(url, headers={"User-Agent": "pdfe-corpus-import"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        # NAME the file. The first version let a bare HTTPError escape, which
        # did not say which of the eight was missing (two had been added to
        # upstream AFTER the pinned tag) and cost a round of guessing.
        raise SystemExit(
            "FAIL: %s -> HTTP %s at %s | The pin is a specific upstream "
            "commit, so a file added to upstream LATER will 404 here (two of "
            "these were). Either pick a file present at that commit, or move "
            "the pin deliberately." % (name, e.code, url))


def do_fetch() -> int:
    DEST.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, (_pin, why) in FILES.items():
        data = fetch(name)
        (DEST / name).write_bytes(data)
        digest = sha256_bytes(data)
        rows.append((name, digest, len(data), why))
        print("  fetched %-38s %7d bytes  %s" % (name, len(data), digest[:16]))
    _write_manifest(rows)
    print("\nimported %d file(s) into %s" % (len(rows), DEST))
    print("Now paste the digests into FILES and into "
          "tools/gate/check_third_party_notices.py.")
    return 0


def _write_manifest(rows) -> None:
    import json
    MANIFEST.write_text(json.dumps({
        "_what": ("Pinned subset of Mozilla pdf.js's test corpus, imported as "
                  "fuzz seeds (SEC-04 / GitHub #36)."),
        "_why_small": ("Seeds are mutated, so small and structurally diverse "
                       "beats large: a 184-byte minimal document is a better "
                       "seed than a 6 MB drawing."),
        "upstream": UPSTREAM,
        "upstream_ref": UPSTREAM_REF,
        "license": "Apache-2.0",
        "attribution": "THIRD-PARTY-NOTICES.md",
        "files": [
            {"name": n, "sha256": d, "size": s, "why": w} for n, d, s, w in rows
        ],
    }, indent=2) + "\n", encoding="utf-8")


def do_verify() -> int:
    import json
    if not MANIFEST.exists():
        print("FAIL: %s is missing; run --fetch first." % MANIFEST)
        return 1
    spec = json.loads(MANIFEST.read_text(encoding="utf-8"))
    problems = []
    for entry in spec["files"]:
        p = DEST / entry["name"]
        if not p.exists():
            problems.append("%s is missing" % entry["name"])
            continue
        actual = sha256_bytes(p.read_bytes())
        if actual != entry["sha256"]:
            problems.append("%s SHA256 mismatch: expected %s got %s"
                            % (entry["name"], entry["sha256"], actual))
    if problems:
        for p in problems:
            print("FAIL:", p)
        return 1
    print("OK: %d imported corpus file(s) match the manifest."
          % len(spec["files"]))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", action="store_true",
                    help="download from upstream and rewrite the manifest")
    args = ap.parse_args()
    return do_fetch() if args.fetch else do_verify()


if __name__ == "__main__":
    sys.exit(main())
