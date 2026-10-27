#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_release_asset_hygiene.py — release ASSETS are scanned before anyone
downloads them, and every release names its corresponding source.

WHY THIS EXISTS
---------------
Until p100, nothing scanned a shipped artifact. `scan_binary_path_hygiene.py`
reads only changed non-text files in the push diff, and `build/` is gitignored
— so the signed installer reached the release page without any scanner reading
a byte of it. Two recorded incidents are exactly the class this misses: a
linker-embedded `.pdb` path carrying the build account name (GitHub #256), and
gtest XML captured under `docs/`. p100 phase-1 scanned the archived signed
v0.3.0 installer by hand (83 files, 418 MB, clean); this guard makes that scan
a standing, per-release control.

It also carries the AGPL half of the release channel (GitHub #200 item 5): a
recipient of the binary alone must not be left to guess where the source is,
so the release body must name the corresponding source. Coupling the two in
one per-release guard means neither can be forgotten separately.

HOW IT SCANS
------------
For each asset: two decodings — latin-1 (a 1:1 byte map; ASCII strings in a PE,
the measured leak class) and UTF-16LE (Windows resources, NSIS strings) — plus
payload EXTRACTION for `.exe`/`.zip`/`.7z` via 7-Zip, because NSIS compresses
with LZMA and a needle inside the payload is invisible to a raw-byte scan. A
failed or empty extraction is a reported failure (`PAYLOAD-NOT-SCANNED`), never
a silent fallback to the raw scan — a raw-only scan of an installer is the
vacuous-pass shape.

Sensitivity is proved through BOTH decode paths before anything is scanned,
plus a specificity control; any control failure refuses to scan at all.

MODES
-----
  --release TAG   download every asset of that release (gh CLI auth; token
                  stays in gh's keyring / CI env, never on disk here), scan
                  them, and verify the release body names the corresponding
                  source (a `github.com/<repo>` URL plus the phrase
                  "corresponding source", case-insensitive).
  --paths P...    scan local artifacts BEFORE upload — the pre-release gate.
                  (No release body exists yet, so the AGPL check is skipped
                  and said so.)
  --self-test     pure decision logic, no network, no 7z.

Exit 0 = scanned and clean (and, for --release, source named); 1 = a hit or a
missing source statement; 2 = could not scan (fails closed).
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

for _n in ("stdout", "stderr"):
    _s = getattr(sys, _n, None)
    if _s is not None and hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

REPO_DEFAULT = "ipswitch8/PDFE"
_HERE = os.path.dirname(os.path.abspath(__file__))
SEVEN_Z_CANDIDATES = (
    r"C:\Program Files\7-Zip\7z.exe",
    r"C:\Program Files (x86)\7-Zip\7z.exe",
)
EXTRACT_EXT = (".exe", ".zip", ".7z", ".msi")


def _load_guard():
    spec = importlib.util.spec_from_file_location(
        "csr", os.path.join(_HERE, "check_scrub_regressions.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _gh():
    return (os.environ.get("PDFE_GH")
            or shutil.which("gh.exe") or shutil.which("gh"))


def _seven_z():
    for c in SEVEN_Z_CANDIDATES:
        if os.path.exists(c):
            return c
    return shutil.which("7z")


# ---------------------------------------------------------------------------
# Pure decision logic — unit-tested by test_check_release_asset_hygiene.py.
# ---------------------------------------------------------------------------

def scan_bytes(raw, where, needles, scan_text):
    """Scan one byte blob under both decodings. Returns [(where, label)]."""
    hits = []
    for enc in ("latin-1", "utf-16-le"):
        text = raw.decode(enc, "replace")
        for _ln, lab in scan_text(text, needles):
            hits.append(("%s [%s]" % (where, enc), lab))
    return hits


def download_complete(expected, got):
    """A partial download is a refusal, never a smaller clean scan.

    Extracted from main() because the claim-check gate found the refusal was
    inline glue no test reached: the one branch that decides whether a missing
    asset silently shrinks the scan population deserves a pin of its own.
    """
    return expected > 0 and got == expected


def workflow_shape_ok(text):
    """release-asset-hygiene.yml still fires on publish/edit and runs this
    guard against the release.

    This is the ratchet the claim-check gate demanded: deleting that workflow
    file -- or stripping its `release:` trigger -- used to fail NOTHING, which
    silently reproduces the exact v0.3.0 defect this guard exists to close
    (the signed installer reached its release page unscanned). The guard's own
    --self-test runs in fast-checks.yml on every push, so wiring the check
    here means the deletion now reddens the next push.
    """
    if not text:
        return False
    has_release_trigger = re.search(
        r"^\s*release:\s*$(?:\n\s+types:\s*\[[^\]]*published[^\]]*\])",
        text, re.M)
    runs_this_guard = "check_release_asset_hygiene.py" in text and \
        "--release" in text
    return bool(has_release_trigger and runs_this_guard)


def source_file_installed(cmake_text):
    """CMakeLists.txt still installs CORRESPONDING-SOURCE.txt.

    The release-body guard validates release TEXT; it cannot see the installed
    tree, so removing this install() rule would silently drop the in-tree half
    of the AGPL offer from every future installer. Also demanded by the
    claim-check gate.
    """
    return bool(re.search(
        r"install\s*\(\s*FILES\s+[^)]*CORRESPONDING-SOURCE\.txt[^)]*\)",
        cmake_text or ""))


def source_statement_ok(body, repo):
    """The release body must name the corresponding source: the phrase AND a
    URL to this repository. Either alone is not an offer a binary-only
    recipient can act on."""
    if not body:
        return False
    low = body.lower()
    if "corresponding source" not in low:
        return False
    return bool(re.search(r"github\.com/%s" % re.escape(repo.lower()), low))


def asset_verdict(assets_scanned, hits, extraction_failures):
    if assets_scanned <= 0:
        return "refuse", "0 assets scanned -- refusing to report clean"
    if extraction_failures:
        return "refuse", ("%d archive(s) could not be extracted -- their "
                          "payloads are UNSCANNED, which is not clean"
                          % len(extraction_failures))
    if hits:
        return "leak", "%d hit(s)" % len(hits)
    return "clean", "0 hits over %d asset(s)" % assets_scanned


# ---------------------------------------------------------------------------


def run_controls(m, needles):
    live = (os.environ.get("COMPUTERNAME") or "").strip()
    if len(live) < 4:
        print("FAIL: COMPUTERNAME absent/short -- sensitivity control cannot "
              "run; refusing to scan uncontrolled")
        return False
    probe = ("x %s y" % live)
    narrow = bool(m.scan_text(probe, needles))
    wide = bool(m.scan_text(probe.encode("utf-16-le").decode("utf-16-le",
                                                             "replace"),
                            needles))
    spec = not m.scan_text("zzsynth qqq", needles)
    print("controls: narrow %s, wide %s, specificity %s"
          % tuple("PASS" if x else "FAIL" for x in (narrow, wide, spec)))
    return narrow and wide and spec


def scan_file_tree(root, prefix, needles, m, hits):
    n = 0
    for dp, _dn, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            try:
                raw = io.open(p, "rb").read()
            except OSError:
                hits.append((prefix + "/" + f, "UNREADABLE"))
                continue
            n += 1
            hits.extend(scan_bytes(
                raw, prefix + "/" + os.path.relpath(p, root).replace("\\", "/"),
                needles, m.scan_text))
    return n


def scan_asset(path, label, needles, m, hits, extraction_failures):
    """Scan one asset; extract archives. Returns files-scanned count."""
    try:
        raw = io.open(path, "rb").read()
    except OSError as exc:
        extraction_failures.append((label, "unreadable: %s" % exc))
        return 0
    count = 1
    hits.extend(scan_bytes(raw, label + " (raw)", needles, m.scan_text))
    if os.path.splitext(path)[1].lower() in EXTRACT_EXT:
        sz = _seven_z()
        if not sz:
            extraction_failures.append((label, "7-Zip not available"))
            return count
        td = tempfile.mkdtemp(prefix="pdfe-relhyg-")
        try:
            r = subprocess.run([sz, "x", "-y", "-o" + td, path],
                               capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            extracted = sum(len(f) for _d, _dn, f in os.walk(td))
            if r.returncode in (0, 1) and extracted:
                print("   extracted %d payload file(s) from %s"
                      % (extracted, label))
                count += scan_file_tree(td, label + "/payload", needles, m,
                                        hits)
            else:
                extraction_failures.append(
                    (label, "7z exit %d, %d files extracted"
                     % (r.returncode, extracted)))
        finally:
            shutil.rmtree(td, ignore_errors=True)
    return count


def self_test():
    import hashlib
    syn_hash = hashlib.sha256(b"zzsynth host").hexdigest()
    needles = [{"label": "syn", "sha256": syn_hash,
                "n": len("zzsynth host")}]
    m = _load_guard()
    bad = 0

    def case(name, ok):
        nonlocal bad
        print("  [%s] %s" % ("ok " if ok else "FAIL", name))
        bad += 0 if ok else 1

    case("a needle in ASCII bytes is found",
         scan_bytes(b"see zzsynth-host here", "a", needles, m.scan_text)
         == [("a [latin-1]", "syn")])
    wide = "see zzsynth-host here".encode("utf-16-le")
    case("a needle in UTF-16LE bytes is found through the wide decode",
         ("a [utf-16-le]", "syn") in
         scan_bytes(wide, "a", needles, m.scan_text))
    case("clean bytes produce no hit",
         not scan_bytes(b"nothing of note \x00\x01", "b", needles,
                        m.scan_text))
    case("source statement: phrase + URL passes",
         source_statement_ok(
             "## Corresponding source\nhttps://github.com/o/r at this tag",
             "o/r"))
    case("source statement: URL alone FAILS",
         not source_statement_ok("see https://github.com/o/r", "o/r"))
    case("source statement: phrase alone FAILS",
         not source_statement_ok("the corresponding source is available",
                                 "o/r"))
    case("source statement: empty body FAILS",
         not source_statement_ok("", "o/r"))
    case("verdict refuses 0 assets", asset_verdict(0, [], [])[0] == "refuse")
    case("verdict refuses an unextracted archive",
         asset_verdict(3, [], [("x", "7z exit 2")])[0] == "refuse")
    case("verdict reports a leak",
         asset_verdict(3, [("x", "syn")], [])[0] == "leak")
    case("verdict clean only when scanned and hitless",
         asset_verdict(3, [], [])[0] == "clean")

    case("a partial download is refused", not download_complete(3, 2))
    case("a complete download passes", download_complete(3, 3))
    case("a zero-asset 'download' never passes", not download_complete(0, 0))

    # Wiring ratchets: these read the REAL sibling files, so a deletion of the
    # release workflow or of the CMake install() rule fails this self-test --
    # which fast-checks.yml runs on every push.
    root = os.path.dirname(_HERE.rstrip(os.sep))
    repo_root = os.path.dirname(root)
    wf = os.path.join(repo_root, ".github", "workflows",
                      "release-asset-hygiene.yml")
    try:
        wf_text = io.open(wf, encoding="utf-8").read()
    except OSError:
        wf_text = ""
    case("release-asset-hygiene.yml exists, fires on publish/edit, and runs "
         "this guard with --release", workflow_shape_ok(wf_text))
    case("a workflow without the release trigger is rejected",
         not workflow_shape_ok("on:\n  workflow_dispatch:\njobs: {}\n"))
    case("a workflow that no longer runs this guard is rejected",
         not workflow_shape_ok(
             "on:\n  release:\n    types: [published, edited]\njobs: {}\n"))
    try:
        cmake_text = io.open(os.path.join(repo_root, "CMakeLists.txt"),
                             encoding="utf-8").read()
    except OSError:
        cmake_text = ""
    case("CMakeLists.txt still installs CORRESPONDING-SOURCE.txt",
         source_file_installed(cmake_text))
    case("a CMakeLists without the install rule is rejected",
         not source_file_installed("install(FILES LICENSE DESTINATION .)"))

    print("self-test: %d failure(s)" % bad)
    return 0 if bad == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=REPO_DEFAULT)
    ap.add_argument("--release", metavar="TAG",
                    help="scan every asset of this release and require a "
                         "corresponding-source statement in its body")
    ap.add_argument("--paths", nargs="+", metavar="FILE",
                    help="scan local artifacts before upload (pre-release)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.release and not args.paths:
        print("nothing to do: pass --release TAG or --paths FILE...")
        return 2

    m = _load_guard()
    needles, err = m.load_all_needles()
    if err:
        print("FAIL: could not load the needle inventory: %s" % err)
        return 2
    print("needles: %d (full inventory)" % len(needles))
    if not run_controls(m, needles):
        return 2

    hits, extraction_failures, scanned = [], [], 0
    rc_agpl = 0

    if args.paths:
        for p in args.paths:
            scanned += scan_asset(p, os.path.basename(p), needles, m, hits,
                                  extraction_failures)
        print("NOTE: --paths mode scans pre-upload artifacts; the "
              "corresponding-source statement is a property of the release "
              "body and is NOT checked here")

    if args.release:
        gh = _gh()
        if not gh:
            print("FAIL: gh not found -- refusing to report clean")
            return 2
        r = subprocess.run(
            [gh, "api", "-X", "GET",
             "repos/%s/releases/tags/%s" % (args.repo, args.release)],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace")
        if r.returncode != 0:
            print("FAIL: could not read release %s" % args.release)
            return 2
        rel = json.loads(r.stdout)
        assets = rel.get("assets") or []
        print("release %s: %d asset(s)" % (args.release, len(assets)))
        if not assets:
            print("FAIL: a release with 0 assets has nothing to certify -- "
                  "refusing to report a clean scan of nothing")
            return 2

        if source_statement_ok(rel.get("body") or "", args.repo):
            print("corresponding-source statement: PRESENT")
        else:
            print("corresponding-source statement: MISSING -- the release "
                  "body must contain the phrase 'corresponding source' and a "
                  "github.com/%s URL (GitHub #200 item 5)" % args.repo)
            rc_agpl = 1

        td = tempfile.mkdtemp(prefix="pdfe-relassets-")
        try:
            dl = subprocess.run(
                [gh, "release", "download", args.release, "--repo", args.repo,
                 "--dir", td, "--pattern", "*"],
                capture_output=True, text=True, encoding="utf-8",
                errors="replace")
            got = os.listdir(td)
            if dl.returncode != 0 or not download_complete(len(assets),
                                                           len(got)):
                print("FAIL: downloaded %d of %d asset(s) -- a partial scan "
                      "is not a clean scan" % (len(got), len(assets)))
                return 2
            for f in sorted(got):
                scanned += scan_asset(os.path.join(td, f), f, needles, m,
                                      hits, extraction_failures)
        finally:
            shutil.rmtree(td, ignore_errors=True)

    print()
    print("files scanned (incl. payloads): %d" % scanned)
    v, why = asset_verdict(scanned, hits, extraction_failures)
    for label, detail in extraction_failures:
        print("   PAYLOAD-NOT-SCANNED: %s (%s)" % (label, detail))
    seen = set()
    for where, lab in hits:
        if (where, lab) in seen:
            continue
        seen.add((where, lab))
        print("   %s  needle=%s" % (where, lab))
    print("asset verdict: %s (%s)" % (v.upper(), why))
    rc_scan = {"clean": 0, "leak": 1, "refuse": 2}[v]
    return max(rc_scan, rc_agpl)


if __name__ == "__main__":
    raise SystemExit(main())
