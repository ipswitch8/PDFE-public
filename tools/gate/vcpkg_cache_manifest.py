#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vcpkg_cache_manifest.py -- pin the CONTENTS of every vcpkg package (GitHub #177).

WHAT THIS IS, AND WHY IT IS DIFFERENT FROM check_vcpkg_binary_sources.py
=======================================================================

`check_vcpkg_binary_sources.py` asserts the binary cache's *configuration* --
which stores are in the chain. It deliberately says nothing about what is inside
them, so a poisoned entry in an approved store passes it. That was #177's
residual, and the validator gate correctly failed the phase for it.

THIS script closes that gap. It records a SHA-256 for every file vcpkg installs,
generated from a trusted from-source build, and verifies a later tree against it.
A poisoned cache entry changes a file's bytes and is therefore caught.

It is not circular. The manifest is COMMITTED and reviewed; an attacker who can
write the binary cache cannot also change the manifest without a reviewed commit.
That is precisely the property a hash-of-the-cache-against-itself would lack.

WHY THIS ONLY WORKS ON A DETERMINISTIC TRIPLET
==============================================

Pinning a checksum to an artifact whose bytes change on every build is useless --
it would red constantly for reasons unrelated to tampering, and a guard that reds
spuriously gets switched off. Ordinary MSVC output is NOT byte-stable: measured
on this project, two identical links of the same source differed in 19 bytes (a
per-link random CodeView GUID, plus the wall-clock COFF timestamp).

`tools/vcpkg-triplets/x64-windows-repro.cmake` removes both, and with it the
probe set reached 24/24 linked artifacts byte-identical across two from-source
builds. The manifest is only meaningful when generated and verified under that
triplet; --verify therefore refuses a tree built with a different one.

TWO FIELDS ARE NORMALISED, AND ONLY TWO
=======================================

`vcpkg.spdx.json` legitimately differs between two identical builds in exactly
two places, both of which SPDX *requires* to be unique per document:

    "created":           "2026-09-15T06:18:38Z"   (build timestamp)
    "documentNamespace": ".../sqlite3-...-d24fbc5f-7732-..." (fresh UUID)

Measured by diffing two builds: those are the ONLY differences. They are
normalised out rather than the file being skipped, so the rest of the SBOM -- the
package list, versions and licences -- is still pinned. Skipping the file
entirely would leave the SBOM unverified, which is the opposite of the point.

Exit codes: 0 = match (or manifest written), 1 = mismatch/missing/extra file,
2 = usage or environment error (including: nothing was scanned).
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import sys

TRIPLET = "x64-windows-repro"
SPDX_NAME = "vcpkg.spdx.json"

# Fields SPDX requires to be unique per document; see module docstring.
_SPDX_CREATED = re.compile(r'"created"\s*:\s*"[^"]*"')
_SPDX_NAMESPACE = re.compile(r'"documentNamespace"\s*:\s*"[^"]*"')

# Qt writes its own source tree into this file as a COMMENTED-OUT cache entry:
#
#   # set(QT_SOURCE_TREE "C:/.../vcpkg/buildtrees/qtbase/src/here-src-6-....clean" CACHE PATH
#
# Measured 2026-10-04: after the triplet's /d1trimfile change removed the build
# path from every compiled artifact (59 pinned DLLs went clean), this ONE line was
# the last thing keeping a pinned file's bytes dependent on where the tree was
# built -- and it is an inert comment, not code that runs.
#
# Normalised rather than unpinned, following the SPDX precedent above: skipping
# the file would leave 238 lines of Qt build configuration unverified to launder
# one comment. The pattern is deliberately anchored to this FILENAME so it cannot
# quietly neutralise paths anywhere else in the tree.
QT_BUILD_INTERNALS = "QtBuildInternalsExtra.cmake"
_BUILDTREES_PATH = re.compile(
    r'[A-Za-z]:[\\/][^"\r\n]*?vcpkg[\\/]buildtrees[\\/][^"\r\n]*')


def normalise(rel_path: str, data: bytes) -> bytes:
    """Remove per-document identifiers and build paths that are not build output.

    Keyed on basename, and narrowly: each rule names the one file it applies to,
    so a future addition cannot widen into "strip paths everywhere", which would
    hide exactly the tampering this manifest exists to catch.
    """
    base = os.path.basename(rel_path)
    if base == SPDX_NAME:
        text = data.decode("utf-8", "replace")
        text = _SPDX_CREATED.sub('"created":"<normalised>"', text)
        text = _SPDX_NAMESPACE.sub('"documentNamespace":"<normalised>"', text)
        return text.encode("utf-8")
    if base == QT_BUILD_INTERNALS:
        text = data.decode("utf-8", "replace")
        return _BUILDTREES_PATH.sub("<normalised-buildtrees-path>", text).encode("utf-8")
    return data


UNPINNED_PATH = "docs/security/vcpkg-cache-unpinned.json"

# Lines of share/<port>/vcpkg_abi_info.txt that identify the TOOLCHAIN rather
# than the port. `triplet_abi` is a hash over the triplet files plus the
# compiler; measured 2026-10-03, it is byte-identical across all 47 ports in the
# tree, which is what makes it usable as a single fingerprint.
_TOOLCHAIN_KEYS = ("triplet_abi", "cmake", "powershell")


def toolchain_fingerprint(tree: str) -> dict:
    """Identify the toolchain that produced `tree`, from vcpkg's own ABI records.

    WHY THIS EXISTS (2026-10-03)
    ============================
    A byte-pinning manifest CANNOT be made immune to a toolchain change: a new
    compiler emits different code, so the bytes differ and SHOULD be re-reviewed.
    That is correct behaviour, not a defect.

    What was a defect is how it PRESENTED. On 2026-10-03 the manifest failed with
    184 opaque `CONTENTS CHANGED` lines, of which 47 were `vcpkg_abi_info.txt`
    itself, and nothing said "your compiler moved". Two mechanisms -- a relocated
    build path and a toolchain update -- produced one undifferentiated wall of
    hashes, and working out which was which took a byte-level investigation.

    So the fingerprint is recorded at --write and compared FIRST at --verify. It
    is an EXPLANATION mechanism, not an integrity control: a tamperer who edited
    these text files to forge a matching fingerprint would still be caught by the
    binaries, which stay pinned. That is also why unpinning the abi_info files
    (see the unpinned list) costs nothing -- their content is precisely the
    toolchain identity now captured here.

    Returns {} when no abi_info is found, so an old manifest without the field,
    or a tree that does not carry them, degrades to the previous behaviour rather
    than erroring.
    """
    values = {}
    conflicts = set()
    share = os.path.join(tree, "share")
    if not os.path.isdir(share):
        return {}
    for port in sorted(os.listdir(share)):
        abi = os.path.join(share, port, "vcpkg_abi_info.txt")
        if not os.path.isfile(abi):
            continue
        try:
            with io.open(abi, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    parts = line.strip().split(None, 1)
                    if len(parts) != 2 or parts[0] not in _TOOLCHAIN_KEYS:
                        continue
                    key, val = parts
                    if key in values and values[key] != val:
                        # Recorded rather than silently last-wins: ports
                        # disagreeing here would mean the premise above (one
                        # toolchain per tree) no longer holds.
                        conflicts.add(key)
                    values[key] = val
        except OSError:
            continue
    if conflicts:
        values["_inconsistent"] = ",".join(sorted(conflicts))
    return values


def load_unpinned(path: str = UNPINNED_PATH) -> dict:
    """Literal path -> reason-group. NEVER globs; see the file's own comment.

    Determinism is not total. Measured across two full from-source builds under
    the deterministic triplet: 7,411 of 7,638 files were byte-identical and 227
    were not, for four measured reasons (MSVC PDB format, ports that embed their
    OWN build timestamp such as OpenSSL's "built on: ...", static .lib archive
    member timestamps, and vcpkg's SBOM which records checksums of those files).

    Those 227 are listed LITERALLY rather than by pattern, so that:
      * a rule cannot silently widen and quietly stop pinning real binaries -- a
        glob like "*.lib" would have unpinned every import library too, and they
        are deterministic;
      * adding an entry requires a reviewed commit that says which file and why.

    This is a stated boundary, not a hidden one. Anything unpinned is NOT
    integrity-checked, and that includes libcrypto -- called out in the file and
    in docs/security/supply-chain-scope-decision.md rather than buried.
    """
    if not os.path.exists(path):
        return {}
    with io.open(path, encoding="utf-8") as fh:
        return json.load(fh).get("paths", {})


def scan(root: str) -> dict:
    """Map repo-relative path -> sha256 for every file under root."""
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace("\\", "/")
            data = io.open(full, "rb").read()
            out[rel] = hashlib.sha256(normalise(rel, data)).hexdigest()
    return out


def check_manifest_obj(d) -> tuple:
    """Sanity-check a parsed manifest. PURE, so the self-test can exercise it.

    This exists because an emptied or truncated manifest does not make the build
    lane FAIL -- it makes it verify nothing. That is this project's recurring
    defect shape (a guard that exits 0 having checked nothing), and here it would
    be silent, because the lane that would notice is dispatch-only.
    """
    if not isinstance(d, dict):
        return False, "manifest is not a JSON object"
    files = d.get("files")
    if not isinstance(files, dict):
        return False, "manifest has no 'files' object"
    if d.get("triplet") != TRIPLET:
        return (False,
                "manifest triplet is %r, expected %r -- these hashes are only "
                "meaningful under the deterministic triplet"
                % (d.get("triplet"), TRIPLET))
    if not files:
        return (False,
                "manifest records ZERO files: it cannot disagree with anything, "
                "so the build-lane check would pass vacuously")
    if d.get("file_count") != len(files):
        return (False,
                "manifest file_count=%r but it lists %d files -- it was "
                "hand-edited rather than regenerated"
                % (d.get("file_count"), len(files)))
    bad = [p for p, h in files.items()
           if not (isinstance(h, str) and len(h) == 64)]
    if bad:
        return False, "manifest has %d malformed digest(s), e.g. %s" % (
            len(bad), bad[0])
    return True, ("vcpkg cache manifest OK: %d files, triplet %s"
                  % (len(files), d["triplet"]))


def check_manifest(path: str) -> tuple:
    if not os.path.exists(path):
        return False, "manifest %s is missing" % path
    try:
        d = load(path)
    except Exception as exc:
        return False, "manifest %s is unparseable: %s" % (path, exc)
    return check_manifest_obj(d)


def load(path: str) -> dict:
    with io.open(path, encoding="utf-8") as fh:
        return json.load(fh)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    # NOT required=True: --self-test must run with no repo state at all, so that
    # "can this checker still say NO?" is answerable offline and in CI before the
    # real check runs.
    ap.add_argument("--install-root",
                    help="vcpkg install root, e.g. build/repro-full/installed")
    ap.add_argument("--manifest", default="docs/security/vcpkg-cache-manifest.json")
    ap.add_argument("--write", action="store_true",
                    help="generate the manifest from a TRUSTED from-source build")
    ap.add_argument("--verify", action="store_true",
                    help="check an install root against the committed manifest")
    ap.add_argument("--check-manifest", action="store_true",
                    help="offline sanity of the manifest itself; needs no build")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if args.check_manifest:
        ok, msg = check_manifest(args.manifest)
        print(msg, file=sys.stdout if ok else sys.stderr)
        return 0 if ok else 1

    if args.write == args.verify:
        print("vcpkg_cache_manifest: choose exactly one of --write / --verify",
              file=sys.stderr)
        return 2

    if not args.install_root:
        print("vcpkg_cache_manifest: --install-root is required for "
              "--write/--verify", file=sys.stderr)
        return 2

    tree = os.path.join(args.install_root, TRIPLET)
    if not os.path.isdir(tree):
        print("vcpkg_cache_manifest: no %s tree under %s -- refusing to report a "
              "verdict on a tree that is not there, or that was built with a "
              "different triplet (this manifest is only meaningful under the "
              "deterministic triplet). Exit 2." % (TRIPLET, args.install_root),
              file=sys.stderr)
        return 2

    unpinned = load_unpinned()
    found = scan(tree)
    # Unpinned files are still required to EXIST -- only their contents go
    # unverified. Dropping them from the population entirely would let a
    # deletion pass unnoticed.
    present_unpinned = sorted(p for p in found if p in unpinned)
    found = {p: h for p, h in found.items() if p not in unpinned}
    if not found:
        print("vcpkg_cache_manifest: SCANNED ZERO FILES under %s. That means the "
              "locator broke, not that everything matched. Exit 2." % tree,
              file=sys.stderr)
        return 2

    if args.write:
        payload = {
            "_comment": ("SHA-256 of every file vcpkg installs, from a trusted "
                         "from-source build under the deterministic triplet "
                         "%s. Regenerate ONLY from a build with the binary cache "
                         "disabled. See tools/gate/vcpkg_cache_manifest.py."
                         % TRIPLET),
            "triplet": TRIPLET,
            "toolchain": toolchain_fingerprint(tree),
            "toolchain_note": (
                "Identifies the toolchain that produced these bytes. A "
                "byte-pinning manifest cannot be immune to a compiler change -- "
                "new codegen means new bytes, which SHOULD be re-reviewed -- so "
                "this field exists to make that the FIRST thing --verify says, "
                "instead of an undifferentiated wall of CONTENTS CHANGED."),
            "file_count": len(found),
            "files": dict(sorted(found.items())),
            "unpinned_present": present_unpinned,
            "unpinned_note": (
                "%d file(s) are deliberately NOT content-verified; see %s for "
                "the per-file reason. They are still required to be PRESENT."
                % (len(present_unpinned), UNPINNED_PATH)),
        }
        with io.open(args.manifest, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(payload, fh, indent=1, sort_keys=True)
            fh.write("\n")
        print("vcpkg_cache_manifest: wrote %s (%d files)"
              % (args.manifest, len(found)))
        return 0

    if not os.path.exists(args.manifest):
        print("vcpkg_cache_manifest: manifest %s is missing. Exit 2."
              % args.manifest, file=sys.stderr)
        return 2
    expected = load(args.manifest).get("files", {})
    if not expected:
        print("vcpkg_cache_manifest: manifest records ZERO files -- it cannot "
              "disagree with anything. Exit 2.", file=sys.stderr)
        return 2

    # An unpinned file must still be THERE. Its contents are unverified; its
    # absence is not.
    expected_unpinned = load(args.manifest).get("unpinned_present", [])
    vanished = sorted(set(expected_unpinned) - set(present_unpinned))

    missing = sorted(set(expected) - set(found)) + vanished
    extra = sorted(set(found) - set(expected))
    changed = sorted(p for p in (set(expected) & set(found))
                     if expected[p] != found[p])

    # Compared BEFORE the verdict is printed, so a toolchain change is the first
    # thing an operator reads rather than something to infer from 184 hashes.
    # An absent field (an older manifest) yields {} and is simply not reported.
    want_tc = load(args.manifest).get("toolchain", {}) or {}
    got_tc = toolchain_fingerprint(tree)
    # ABSENT IS NOT CHANGED. A manifest written before this field existed has no
    # fingerprint, and diffing against {} made every key "differ", so --verify
    # announced "THE TOOLCHAIN CHANGED ... manifest=None" on a manifest that
    # simply predates the feature -- a confidently wrong diagnosis, which is the
    # exact failure this field was added to remove. Observed against the
    # 2026-10-03 manifest. With no recorded fingerprint there is nothing to
    # compare, so say that instead of inventing a change.
    tc_known = bool(want_tc)
    tc_diff = sorted(k for k in set(want_tc) | set(got_tc)
                     if want_tc.get(k) != got_tc.get(k)) if tc_known else []

    if not (missing or extra or changed):
        print("vcpkg_cache_manifest: PASS -- %d file(s) match the committed "
              "manifest." % len(found))
        if tc_diff:
            # Bytes match while the fingerprint moved: worth saying, but it is
            # not a failure -- the pinned contents are what the control asserts.
            print("vcpkg_cache_manifest: NOTE -- toolchain fingerprint differs "
                  "(%s) yet every pinned file matches."
                  % ", ".join("%s: %r -> %r" % (k, want_tc.get(k), got_tc.get(k))
                              for k in tc_diff))
        return 0

    print("vcpkg_cache_manifest: FAIL", file=sys.stderr)
    if tc_diff:
        print("\n  THE TOOLCHAIN CHANGED -- this very likely explains the "
              "differences below:", file=sys.stderr)
        for k in tc_diff:
            print("    %-12s manifest=%r  tree=%r"
                  % (k, want_tc.get(k), got_tc.get(k)), file=sys.stderr)
        print("  A different compiler emits different code, so these bytes are "
              "EXPECTED to differ.\n  That is not tampering, and it is not "
              "fixable by a flag: regenerate the manifest\n  (see the runbook in "
              "docs/security/supply-chain-scope-decision.md) and review the "
              "diff.\n", file=sys.stderr)
    elif changed and not tc_known:
        print("\n  This manifest records NO toolchain fingerprint (it predates the "
              "field), so whether\n  a compiler update explains the differences "
              "below cannot be determined from it.\n  Regenerating will record "
              "one.\n", file=sys.stderr)
    elif changed:
        print("\n  The toolchain fingerprint is UNCHANGED, so a compiler update "
              "does NOT explain\n  the differences below. Treat them as "
              "unexplained until they are explained.\n", file=sys.stderr)
    for p in changed:
        print("  CONTENTS CHANGED : %s" % p, file=sys.stderr)
        print("      expected %s" % expected[p], file=sys.stderr)
        print("      found    %s" % found[p], file=sys.stderr)
    for p in missing:
        print("  MISSING          : %s" % p, file=sys.stderr)
    for p in extra:
        print("  UNEXPECTED       : %s" % p, file=sys.stderr)
    print("\nIf this followed a deliberate dependency change, regenerate with "
          "--write from a from-source build (binary cache disabled) and commit "
          "the result. Do NOT regenerate from a cache-restored tree: that would "
          "pin whatever the cache contained, which is the thing being checked.",
          file=sys.stderr)
    return 1


def self_test() -> int:
    checks = []

    def check(name, got, want=True):
        ok = (got == want)
        checks.append(ok)
        print("  [%s] %-58s got=%r" % ("ok  " if ok else "FAIL", name, got))

    base = b'{"created": "2026-01-01T00:00:00Z", "documentNamespace": "x/a-1", "k": 1}'
    other = b'{"created": "2027-02-02T11:11:11Z", "documentNamespace": "x/b-2", "k": 1}'
    check("spdx per-document fields normalise to equal",
          normalise("share/p/" + SPDX_NAME, base) == normalise("share/p/" + SPDX_NAME, other))

    # ...but a REAL content change must still be caught.
    changed = b'{"created": "2026-01-01T00:00:00Z", "documentNamespace": "x/a-1", "k": 2}'
    check("a real spdx content change is NOT normalised away",
          normalise("share/p/" + SPDX_NAME, base) != normalise("share/p/" + SPDX_NAME, changed))

    # normalisation must apply ONLY to the spdx file
    check("a non-spdx file is left byte-identical",
          normalise("bin/x.dll", base) == base)
    check("a dll containing the word created is untouched",
          normalise("bin/x.dll", base) != normalise("share/p/" + SPDX_NAME, base))

    # ---- manifest sanity: one NEGATIVE case per rule -------------------
    good = {"triplet": TRIPLET, "file_count": 1, "files": {"bin/a.dll": "a" * 64}}
    check("a well-formed manifest passes", check_manifest_obj(good)[0])
    check("ZERO files is rejected (the vacuous-pass case)",
          check_manifest_obj({"triplet": TRIPLET, "file_count": 0,
                              "files": {}})[0], False)
    check("wrong triplet is rejected",
          check_manifest_obj(dict(good, triplet="x64-windows"))[0], False)
    check("file_count disagreeing with files is rejected",
          check_manifest_obj(dict(good, file_count=99))[0], False)
    check("a malformed digest is rejected",
          check_manifest_obj({"triplet": TRIPLET, "file_count": 1,
                              "files": {"bin/a.dll": "nope"}})[0], False)
    check("a missing 'files' key is rejected",
          check_manifest_obj({"triplet": TRIPLET, "file_count": 1})[0], False)
    check("a non-object manifest is rejected",
          check_manifest_obj([])[0], False)
    check("a missing manifest FILE is rejected",
          check_manifest("does/not/exist.json")[0], False)

    # ---- the unpinned list must be LITERAL and NARROW ------------------
    # The failure mode this guards is a glob quietly unpinning real binaries.
    unp = load_unpinned()
    if unp:
        globby = [p for p in unp if "*" in p or "?" in p or p.endswith("/")]
        check("unpinned list contains NO glob/prefix entries", globby, [])
        check("unpinned list is a small minority of the tree",
              len(unp) < 1000)
        # the import libraries of deterministic DLLs must NOT be unpinned
        check("no Qt6Core/Qt6Gui import lib was unpinned",
              any(p.endswith(("lib/Qt6Core.lib", "lib/Qt6Gui.lib")) for p in unp),
              False)
        check("every unpinned entry names a known reason group",
              sorted({g for g in unp.values()}) ==
              sorted(set(json.load(io.open(UNPINNED_PATH, encoding="utf-8"))
                         ["groups"])), True)

    # ---- normalisation must be NARROW (2026-10-04) ---------------------
    # Each rule names one filename. The risk of a path-stripping rule is that it
    # widens and starts hiding the differences the manifest exists to report, so
    # the negative case is pinned here: the same bytes under ANY other name must
    # come through untouched.
    _dirty = (b'# set(QT_SOURCE_TREE "C:/actions-runner/_work/PDFE/PDFE/vcpkg/'
              b'buildtrees/qtbase/src/here-src-6-6c7e2cdd3d.clean" CACHE PATH\n')
    # Asserts on the ABSOLUTE PATH, not on the word "buildtrees": the
    # placeholder is "<normalised-buildtrees-path>", which contains that word, so
    # the first version of this check failed against correct output.
    check("the Qt build-internals path IS normalised",
          b"actions-runner" in normalise("share/Qt6BuildInternals/%s"
                                         % QT_BUILD_INTERNALS, _dirty), False)
    check("the SAME bytes under another name are NOT touched",
          normalise("share/qtbase/some-other-file.cmake", _dirty) == _dirty, True)
    check("normalisation keeps the rest of the line",
          b"QT_SOURCE_TREE" in normalise("share/Qt6BuildInternals/%s"
                                         % QT_BUILD_INTERNALS, _dirty), True)
    check("a file with no path is returned byte-identical",
          normalise("share/Qt6BuildInternals/%s" % QT_BUILD_INTERNALS,
                    b"set(FOO bar)\n") == b"set(FOO bar)\n", True)

    # ---- the toolchain fingerprint (2026-10-03) ------------------------
    # Built on a synthetic tree rather than the real one, so these checks hold
    # on a machine with no build at all -- and, more importantly, so the
    # NEGATIVE cases can be constructed. A fingerprint that cannot come out
    # different would make the "THE TOOLCHAIN CHANGED" branch unreachable, which
    # is this project's documented decoration failure mode.
    import shutil
    import tempfile
    tmp = tempfile.mkdtemp(prefix="pdfe-vcpkg-tc-")
    try:
        def _abi(port, triplet_abi="abi-111", cmake="3.31.10", ps="7.5.4"):
            d = os.path.join(tmp, "share", port)
            os.makedirs(d, exist_ok=True)
            with io.open(os.path.join(d, "vcpkg_abi_info.txt"), "w",
                         encoding="utf-8", newline="\n") as fh:
                fh.write("cmake %s\nfeatures core\npowershell %s\n"
                         "triplet %s\ntriplet_abi %s\n"
                         % (cmake, ps, TRIPLET, triplet_abi))

        check("fingerprint of a tree with no share/ is empty",
              toolchain_fingerprint(os.path.join(tmp, "nope")), {})

        _abi("alpha")
        _abi("beta")
        fp = toolchain_fingerprint(tmp)
        check("fingerprint reads triplet_abi", fp.get("triplet_abi"), "abi-111")
        check("fingerprint reads cmake", fp.get("cmake"), "3.31.10")
        check("fingerprint reads powershell", fp.get("powershell"), "7.5.4")
        check("consistent ports report no inconsistency",
              "_inconsistent" in fp, False)

        # NEGATIVE 1: a compiler change must move the fingerprint, or the
        # explanation branch can never fire.
        _abi("beta", triplet_abi="abi-222")
        moved = toolchain_fingerprint(tmp)
        check("a different triplet_abi CHANGES the fingerprint",
              moved.get("triplet_abi") != "abi-111" or
              "_inconsistent" in moved, True)
        # NEGATIVE 2: ports disagreeing is recorded, not silently last-wins.
        check("disagreeing ports are flagged inconsistent",
              "_inconsistent" in moved, True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("PASS" if all(checks) else "FAILURE(S)"))
    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
