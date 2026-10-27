# -*- coding: utf-8 -*-
"""Classify evidence files under docs/ as CITED or UNCITED, and report both.

WHY THIS EXISTS. `.gitignore` carried a blanket `*.xml` for a long time, so
evidence captures written under `docs/` were silently excluded: `git add` on an
ignored file SUCCEEDS, stages nothing, and reports no error, so a commit looks
complete while the citation pointing at the file resolves to nothing months
later. F36 lost eight captures exactly that way. When the exclusion was finally
replaced with `!docs/**/*.xml`, 93 previously-invisible files appeared at once
and someone had to decide, file by file, which were real evidence.

This is that decision, written down and re-runnable, rather than a judgement
call made once in a terminal and forgotten.

HOW A CITATION IS RECOGNISED. Two shapes count, and only using the first one is
a mistake this script exists to prevent:

  path      a tracked file names at least the last TWO path components
            ("phase-23/gtest_full.xml"). Bare basename is deliberately NOT
            enough: `gtest-results.xml` appears in 53 tracked files, nearly all
            of them CI workflows naming their own run output, which says nothing
            about a committed capture under docs/.

  relative  a manifest names a SIBLING by basename -- `phase-34/runs.json` lists
            `run-01.xml`, `harness-audit/phase-3/mutation-results.json` lists
            `runs/res_*.xml`. Genuine references, written relative to the
            directory they live in. The discriminator is the CITER'S LOCATION:
            a basename match from inside the same subtree is a citation; the
            identical match from `.github/workflows/` is not.

Anything matching neither is reported UNCITED. It is NOT deleted and NOT
auto-staged -- "nothing currently points at this" is a prompt for a human, not a
verdict. Encoding an unknown as one of the two answers is the defect DCA 0044
exists for.

Usage:
    python tools/gate/check_cited_evidence.py [--ext .xml] [--root docs]
    python tools/gate/check_cited_evidence.py --untracked-only
    python tools/gate/check_cited_evidence.py --fail-on-untracked-cited

`--fail-on-untracked-cited` is the CI-useful mode: it exits 1 when a file that
something CITES is not tracked, which is precisely the F36 failure. It stays
silent about uncited leftovers, which are untidy but harmless.
"""
from __future__ import annotations

import argparse
import io
import os
import subprocess
import sys

TEXT_EXT = {".md", ".yml", ".yaml", ".py", ".cpp", ".h", ".hpp", ".txt",
            ".json", ".ps1", ".sh", ".cmake", ".in", ".cfg", ".ini", ".toml"}

#: How far up from the file a citing manifest may live and still count as a
#: relative reference. 2 covers `runs/res_X.xml` named from the parent
#: directory's manifest; beyond that, "same subtree" stops meaning anything.
RELATIVE_ANCESTOR_LEVELS = 2

#: Files above this size are not read into the citation corpus. Reported rather
#: than skipped silently -- a cap nobody is told about reads as coverage that
#: was never there.
CORPUS_MAX_BYTES = 8 * 1024 * 1024


def _git(args):
    return subprocess.run(["git"] + args, capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout


def _norm(s):
    return str(s).replace("\\", "/")


def build_corpus():
    """(path, text) for every tracked text file. Returns (corpus, skipped)."""
    corpus, skipped = [], []
    for tracked in _git(["ls-files"]).splitlines():
        if os.path.splitext(tracked)[1].lower() not in TEXT_EXT:
            continue
        try:
            if os.path.getsize(tracked) > CORPUS_MAX_BYTES:
                skipped.append(tracked)
                continue
            with io.open(tracked, encoding="utf-8", errors="replace") as fh:
                corpus.append((_norm(tracked), _norm(fh.read())))
        except OSError:
            skipped.append(tracked)
    return corpus, skipped


def citation_for(path, corpus):
    """('path'|'relative', citing_file) or None."""
    np = _norm(path)
    parts = np.split("/")
    base = parts[-1]
    two = "/".join(parts[-2:])
    three = "/".join(parts[-3:]) if len(parts) >= 3 else two

    for tracked, body in corpus:
        if tracked == np:
            continue          # a file citing itself proves nothing
        if np in body or three in body or two in body:
            return ("path", tracked)

    xml_dir = "/".join(parts[:-1])
    allowed = {xml_dir}
    d = xml_dir
    for _ in range(RELATIVE_ANCESTOR_LEVELS):
        d = d.rsplit("/", 1)[0] if "/" in d else d
        allowed.add(d)
    for tracked, body in corpus:
        if tracked == np:
            continue
        if base in body and tracked.rsplit("/", 1)[0] in allowed:
            return ("relative", tracked)
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default="docs")
    ap.add_argument("--ext", default=".xml")
    ap.add_argument("--untracked-only", action="store_true",
                    help="consider only files git does not track")
    ap.add_argument("--fail-on-untracked-cited", action="store_true",
                    help="exit 1 if a CITED file is untracked (the F36 failure)")
    args = ap.parse_args(argv)

    root, ext = _norm(args.root).rstrip("/"), args.ext.lower()

    tracked_set = {_norm(p) for p in _git(["ls-files"]).splitlines()}
    untracked = {_norm(p) for p in
                 _git(["ls-files", "--others", "--exclude-standard"]).splitlines()}

    candidates = sorted(
        p for p in (untracked if args.untracked_only else tracked_set | untracked)
        if p.startswith(root + "/") and p.lower().endswith(ext))

    if not candidates:
        print("[cited-evidence] no %s files under %s/" % (ext, root))
        return 0

    corpus, skipped = build_corpus()
    if skipped:
        print("[cited-evidence] NOT scanned for citations (too large/unreadable): %d file(s)"
              % len(skipped))
        for s in skipped[:5]:
            print("    %s" % s)

    cited, uncited, untracked_cited = [], [], []
    for path in candidates:
        why = citation_for(path, corpus)
        if why:
            cited.append((path, why))
            if path in untracked:
                untracked_cited.append((path, why))
        else:
            uncited.append(path)

    # THREE buckets, not two. Reporting only CITED/UNCITED reads as a clean
    # split and is not one: most UNCITED files here are TRACKED (committed
    # evidence that no document names by exact filename), and only some are
    # untracked. Summarising those together produced a wrong count in the very
    # commit message that introduced this script, so the report now separates
    # them and the ambiguity cannot recur.
    uncited_tracked = [p for p in uncited if p in tracked_set]
    uncited_untracked = [p for p in uncited if p not in tracked_set]

    print("[cited-evidence] %d candidate(s) under %s/ (%s)" % (len(candidates), root, ext))
    print("[cited-evidence]   CITED            : %d" % len(cited))
    print("[cited-evidence]   UNCITED, tracked : %d  (committed, nothing names them)"
          % len(uncited_tracked))
    print("[cited-evidence]   UNCITED, untracked: %d  (not committed)"
          % len(uncited_untracked))
    for path in uncited_untracked:
        print("      uncited+untracked: %s" % path)

    if untracked_cited:
        print("[cited-evidence] FAIL: %d CITED file(s) are NOT TRACKED. Something "
              "in the tree points at them and they are not in it -- this is the "
              "F36 failure, and `git add` on an ignored file exits 0 while "
              "staging nothing, so it will not announce itself."
              % len(untracked_cited))
        for path, (kind, citer) in untracked_cited:
            print("      %s   <- [%s] %s" % (path, kind, citer))
        if args.fail_on_untracked_cited:
            return 1
    else:
        print("[cited-evidence] OK: every cited file is tracked.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
