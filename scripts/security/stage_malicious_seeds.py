#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stage the structurally-valid malicious payloads as fuzz seeds (SEC-05 / #37).

WHAT #37 ASKS FOR, AND THE ONE THING IT CANNOT HAVE
---------------------------------------------------
SEC-05 asks that the structurally-valid subset of
`tests/security/corpus/malicious_pdf/` be copied into
`seeds/fuzz_document_open/`. The seeds directory is TRACKED. That corpus is
deliberately NOT, and the reasons are recorded in its own README:

  1. the payloads phone home (loopback callback, but still),
  2. seventy of them in git history trips every contributor's antivirus on
     clone and attracts GitHub's malware scanners,
  3. having them in the working tree by default invites someone to open one
     outside the sandboxed runner.

Committing them into `seeds/` would defeat all three. So the *intent* of SEC-05
-- give the document-open fuzzer real hostile structures to mutate -- is
implemented here WITHOUT changing that policy: the subset is staged into a
GITIGNORED overlay at campaign time, from a corpus the operator generated
locally. Nothing new is persisted to history.

WHAT "STRUCTURALLY VALID" MEANS HERE, and why it is not a guess
--------------------------------------------------------------
`manifest.json` is committed and records, per payload, the behaviour PDFE is
expected to exhibit. A payload marked `open_clean` is one PDFE opens
successfully -- i.e. it parses, which is exactly the property a seed needs. A
seed the parser rejects at byte 9 teaches the mutator nothing.

Two categories are excluded on top of that, for reasons unrelated to structure:

  * `eicar`  -- the EICAR antivirus test string. Staging it into a build tree
                is a reliable way to have a scanner quarantine the workspace
                mid-campaign.
  * `svg_polyglot` -- not a plain PDF; it belongs to a different harness.

Usage:
    python scripts/security/stage_malicious_seeds.py --dry-run   # no corpus needed
    python scripts/security/stage_malicious_seeds.py --stage
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "tests" / "security" / "corpus" / "manifest.json"
CORPUS = REPO_ROOT / "tests" / "security" / "corpus" / "malicious_pdf"
OVERLAY = REPO_ROOT / "tests" / "security" / "fuzz" / "seeds" / "_malicious_overlay"

EXCLUDED_CATEGORIES = {
    "eicar": "EICAR antivirus test string -- staging it invites a scanner to "
             "quarantine the build tree mid-campaign",
    "svg_polyglot": "not a plain PDF; belongs to a different harness",
}
REQUIRED_BEHAVIOUR = "open_clean"


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with io.open(p, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def select(manifest: dict):
    """(selected, skipped) -- driven by the manifest alone, so this is
    verifiable without the corpus present."""
    selected, skipped = [], []
    for p in manifest["payloads"]:
        cat = p.get("category", "")
        if cat in EXCLUDED_CATEGORIES:
            skipped.append((p, EXCLUDED_CATEGORIES[cat]))
        elif p.get("expected_pdfe_behavior") != REQUIRED_BEHAVIOUR:
            skipped.append((p, "expected behaviour is %r, not %r -- PDFE does "
                               "not open it cleanly, so it is not a "
                               "structurally-valid seed"
                            % (p.get("expected_pdfe_behavior"), REQUIRED_BEHAVIOUR)))
        else:
            selected.append(p)
    return selected, skipped


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", action="store_true",
                    help="copy the subset into the gitignored overlay")
    ap.add_argument("--dry-run", action="store_true",
                    help="report the selection only; works without the corpus")
    args = ap.parse_args()

    if not MANIFEST.exists():
        print("FAIL: %s is missing." % MANIFEST)
        return 2
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    selected, skipped = select(manifest)

    print("[malicious-seeds] manifest records %d payload(s)"
          % len(manifest["payloads"]))
    print("[malicious-seeds] selected %d, skipped %d"
          % (len(selected), len(skipped)))
    reasons = {}
    for _p, why in skipped:
        reasons[why] = reasons.get(why, 0) + 1
    for why, n in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print("    %3d  %s" % (n, why))

    if not selected:
        print("[malicious-seeds] FAIL: nothing selected. Either the manifest "
              "schema changed or the filter is wrong; refusing to report a "
              "successful stage that would stage nothing.")
        return 1

    if args.dry_run or not args.stage:
        print("[malicious-seeds] dry run -- nothing copied.")
        return 0

    if not CORPUS.is_dir():
        print("[malicious-seeds] FAIL: %s does not exist. The corpus is "
              "gitignored by design; generate it first with "
              "scripts/security/gen_malicious_corpus.ps1." % CORPUS)
        return 1

    OVERLAY.mkdir(parents=True, exist_ok=True)
    staged = 0
    mismatched = []
    missing = []
    for p in selected:
        src = CORPUS / p["relative_path"]
        if not src.exists():
            missing.append(p["relative_path"])
            continue
        # Verify BEFORE copying: a payload whose bytes do not match the manifest
        # is not the reviewed artifact, and staging it would put unreviewed
        # bytes into a campaign.
        if sha256_file(src) != p["sha256"]:
            mismatched.append(p["relative_path"])
            continue
        flat = p["relative_path"].replace("/", "_").replace("\\", "_")
        shutil.copyfile(src, OVERLAY / flat)
        staged += 1

    print("[malicious-seeds] staged %d file(s) into %s" % (staged, OVERLAY))
    if missing:
        print("[malicious-seeds] %d selected payload(s) ABSENT from the corpus "
              "(regenerate it): %s" % (len(missing), ", ".join(missing[:5])))
    if mismatched:
        print("[malicious-seeds] %d payload(s) FAILED their manifest SHA-256 "
              "and were NOT staged: %s"
              % (len(mismatched), ", ".join(mismatched[:5])))
        return 1
    return 0 if staged else 1


if __name__ == "__main__":
    sys.exit(main())
