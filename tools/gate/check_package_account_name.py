#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""No artifact installed into the package carries the build account name.

GitHub #199 / #191. This is the control that keeps the fix fixed; everything
else in those issues is a one-off.

WHY THE EXISTING GUARDS CANNOT DO THIS
======================================
`UiSuitePathHygiene.NoTrackedFileCarriesTheBuildAccountName` and
`check_tracked_path_hygiene.py` both have the same population: files under git
control. Every binary that matters here is BUILD OUTPUT, so it was never in
their scope -- which is why 418 occurrences across 6 shipped binaries went
unnoticed while the string was being scrubbed out of the repository three times
over. GitHub #160 widened the tracked population to include untracked files via
`git ls-files --others --exclude-standard`, but `--exclude-standard` honours
`.gitignore` and `.gitignore:8` is `build/`, so that widening cannot see these
either. This needs a different PROPERTY, not a wider version of the old one.

THE POPULATION IS THE PACKAGE, NOT THE BUILD DIRECTORY
======================================================
Measured 2026-09-30: scanning `build/vs2022/bin/Release` finds 8 files carrying
the account name, but three of them -- `pdfe_tests.exe` (850 occurrences),
`gtest.dll`, `Qt6Test.dll` -- are test binaries the installer never ships. A
guard scoped to the build directory would fail permanently on
`pdfe_tests.exe`, 531 of whose hits are `__FILE__` paths from this project's own
test sources, which is correct and expected for a binary nobody distributes.
That is the shape that gets a guard disabled.

CMakeLists.txt confirms that split independently rather than on my reading of
it: the explicit `_required_dlls` list installs tesseract55, libcrypto-3-x64,
libssl-3-x64 and tiff, and the comment there records that the list is explicit
precisely so Qt6Test and friends are NOT bundled.

So the population is the EXTRACTED INSTALLER PAYLOAD.
`scripts/verify-installer-no-account-name.ps1` extracts a fresh copy and invokes
this scanner over it. It deliberately does NOT reuse the `%TEMP%/pdfe_depcheck`
tree that `verify-installer-deps.ps1` leaves behind: that tree is deleted and
recreated at the START of that script and never cleaned up at the end, so it is
reusable only while that stays true, and a guard whose population is another
script's incidental leftover state reports "clean" the moment the other script
starts tidying up after itself.

THE THREE TRAPS GitHub #199 NAMES, AND HOW EACH IS HANDLED
==========================================================
1. **`grep -iF` aborts in this environment** (exit 134), which inside an `if`
   reads as "no match" -- a silent false negative in a leak check. `grep -P` is
   also dead here. So all matching is done in Python.

2. **A file listed but not read must not read as clean.** A stale working
   directory once produced a non-empty file list, every open failing, and a
   conclusion of "none carry the account name". This compares `read` against
   `listed` and FAILS if they differ, rather than counting an unreadable file as
   a pass.

3. **The guard must not emit the account name itself**, or it becomes the next
   producer. Every sample is redacted to `<account>` before printing, and the
   failure message reports counts plus redacted samples.

UTF-16 AND FORWARD SLASHES ARE NOT OPTIONAL
===========================================
MSVC's `assert()` expands to `_wassert(L"expr", L"" __FILE__, line)`, so a
large share of hits are WIDE strings -- an ASCII-only pass under-reported one
binary by a third. CMake-injected `-D` definitions emit forward slashes. Both
encodings and both separator conventions are matched.

Usage:
    python tools/gate/check_package_account_name.py <payload-root> [...]
        [--account NAME] [--allowlist tools/gate/package_leak_allowlist.json]
        [--self-test]

Exit 0 = no unallowlisted occurrence, 1 = a leak, 2 = usage/inputs unusable.
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import json
import os
import sys

# THE MATCHER IS IMPORTED, NOT REIMPLEMENTED.
#
# tools/audit/scan_build_output_account_name.py already extracts account-name
# strings from a binary, decoding UTF-16LE as well as ASCII and redacting on
# output. GitHub #199 names it explicitly and says it sits in tools/audit/,
# wired to nothing, because "promoting it means deciding an allowlist".
#
# This file is that promotion, and it deliberately does NOT absorb or copy the
# extraction. The two scripts do different jobs -- that one DIAGNOSES (per
# mechanism counts, plus the PE debug directory read structurally), this one
# GATES (pass/fail, allowlist, population checks) -- but they must never
# disagree about what counts as an occurrence. Two matchers drift, and the
# failure mode of that drift is the worst available: the gate passes while the
# audit tool reports hits.
#
# Inherited from there, deliberately: the printable-run minimum is 16
# characters, which is what #199's 418-occurrence figure was measured at.
# Lowering it here alone would make this gate's numbers incomparable with the
# issue's. If it should be lowered, lower it in the shared module.
def _load_audit_module():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, os.pardir, "audit",
                        "scan_build_output_account_name.py")
    path = os.path.normpath(path)
    if not os.path.exists(path):
        # FAIL LOUDLY. A fallback to a local copy of the matcher is exactly the
        # drift this import exists to prevent, and it would be invisible.
        raise SystemExit(
            "error: the shared matcher is missing: %s\n"
            "This gate imports its extraction from the audit scanner on "
            "purpose (GitHub #199). It will not fall back to a private copy, "
            "because a second matcher can disagree with the first and the gate "
            "would be the one that passes." % path)
    spec = importlib.util.spec_from_file_location("pdfe_acct_scan", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_audit = _load_audit_module()

redact = _audit.redact
occurrences = _audit.extract

BINARY_EXTS = tuple(sorted(set(_audit.BINARY_EXTS) | {".so"}))

DEFAULT_ALLOWLIST = os.path.join("tools", "gate", "package_leak_allowlist.json")


def scan(roots, account):
    """(per_file_counts, samples, listed, read, unreadable)."""
    per_file, samples = {}, {}
    listed = read = 0
    unreadable = []
    for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                if not fn.lower().endswith(BINARY_EXTS):
                    continue
                p = os.path.join(dirpath, fn)
                listed += 1
                try:
                    with open(p, "rb") as fh:
                        data = fh.read()
                except OSError as exc:
                    unreadable.append((p, str(exc)))
                    continue
                read += 1
                rel = os.path.relpath(p, root).replace("\\", "/")
                n = 0
                for s, enc in occurrences(data, account):
                    n += 1
                    if rel not in samples:
                        tag = "" if enc == "ascii" else "  [%s]" % enc
                        samples[rel] = redact(s.strip(), account)[:160] + tag
                if n:
                    per_file[rel] = per_file.get(rel, 0) + n
    return per_file, samples, listed, read, unreadable


def load_allowlist(path):
    if not path or not os.path.exists(path):
        return {}, {}
    data = json.loads(io.open(path, encoding="utf-8").read())
    entries = {k: v for k, v in data.items() if not k.startswith("_")}
    caps = {k: (v.get("max") if isinstance(v, dict) else None)
            for k, v in entries.items()}
    return entries, caps


def self_test() -> int:
    ok = True
    acct = "someaccount"

    # The fixture prefixes are ASSEMBLED rather than written as literals, and
    # that is not stylistic. check_tracked_path_hygiene.py scans every
    # committable text file for a home-path-shaped string and reports its
    # apparent owner -- and it cannot tell a synthetic account name from a real
    # one, which is exactly the property that makes it useful. A literal
    # drive-rooted user-profile path in this file therefore fails that guard.
    # Measured twice: once for the fixtures below, and then again for the
    # comment that explained the first failure, which had quoted such a path to
    # illustrate it. Weakening the other guard to recognise "obviously
    # fake" names would be the wrong trade: this file is the one place in the
    # tree that must contain no home path at all, because it is the leak
    # scanner.
    users = "C:\\" + "Users" + "\\"
    base = users + acct

    ascii_blob = ("...." + base + "\\src\\thing.cpp....").encode("ascii")
    if not list(occurrences(ascii_blob, acct)):
        print("self-test FAIL: an ASCII occurrence was not found")
        ok = False

    wide = (base + "\\src\\wide.cpp").encode("utf-16-le")
    if not list(occurrences(wide, acct)):
        print("self-test FAIL: a UTF-16LE occurrence was not found -- MSVC's "
              "assert() expands to _wassert with a WIDE __FILE__, so an "
              "ASCII-only pass under-reports")
        ok = False

    fwd = ("-DFIXTURES=" + base.replace("\\", "/") + "/src/fixtures").encode("ascii")
    if not list(occurrences(fwd, acct)):
        print("self-test FAIL: a forward-slash path was not found -- "
              "CMake-injected -D definitions emit '/'")
        ok = False

    clean = b"nothing to see here, just ordinary strings and paths"
    if list(occurrences(clean, acct)):
        print("self-test FAIL: a clean blob reported an occurrence")
        ok = False

    if acct in redact(base + "\\x", acct):
        print("self-test FAIL: redact() left the account name in its output -- "
              "the guard would become the next producer")
        ok = False

    print("self-test %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", nargs="*")
    ap.add_argument("--account", default=os.environ.get("USERNAME") or "")
    ap.add_argument("--allowlist", default=DEFAULT_ALLOWLIST)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if not args.account:
        print("error: no account name; pass --account", file=sys.stderr)
        return 2
    if not args.roots:
        print("error: no payload root given", file=sys.stderr)
        return 2
    missing = [r for r in args.roots if not os.path.isdir(r)]
    if missing:
        print("error: payload root(s) do not exist: %s" % ", ".join(missing),
              file=sys.stderr)
        return 2

    per_file, samples, listed, read, unreadable = scan(args.roots, args.account)
    entries, caps = load_allowlist(args.allowlist)

    print("=== package account-name scan (GitHub #199 / #191) ===")
    print("  roots            : %s" % ", ".join(args.roots))
    print("  binaries listed  : %d" % listed)
    print("  binaries read    : %d" % read)

    # Trap 2: a file listed but not read must not read as clean.
    if read != listed:
        print("\n  FAIL: %d binar(ies) were listed but could not be READ. A "
              "guard that counts an unreadable file as clean reports 'no leak' "
              "about bytes it never looked at -- GitHub #199 names this trap "
              "explicitly." % (listed - read))
        for p, exc in unreadable[:10]:
            print("    unreadable: %s (%s)" % (p, exc))
        return 1

    if listed == 0:
        print("\n  FAIL: no binaries found under the given root(s). An empty "
              "population satisfies any 'no occurrences' assertion forever, "
              "which is the vacuous shape this project records as "
              "'empty collection'. Point this at the EXTRACTED installer "
              "payload -- scripts/verify-installer-no-account-name.ps1 "
              "extracts one and invokes this scanner over it.")
        return 1

    violations, allowed = {}, {}
    for rel, n in sorted(per_file.items()):
        cap = caps.get(rel)
        if rel in entries and (cap is None or n <= cap):
            allowed[rel] = n
        else:
            violations[rel] = n

    print("  carrying the name: %d file(s), %d occurrence(s)"
          % (len(per_file), sum(per_file.values())))
    if allowed:
        print("\n  ALLOWLISTED (each with a stated reason in %s):"
              % args.allowlist)
        for rel, n in sorted(allowed.items()):
            cap = caps.get(rel)
            print("    %-34s %5d%s" % (rel, n,
                                       "" if cap is None else " (cap %d)" % cap))

    if not violations:
        print("\ncheck_package_account_name: PASS")
        return 0

    print("\n  NOT ALLOWLISTED -- the shipped package carries the build "
          "account name:")
    for rel, n in sorted(violations.items(), key=lambda kv: -kv[1]):
        cap = caps.get(rel)
        over = "" if cap is None else " (allowlist cap %d, found %d)" % (cap, n)
        print("    %-34s %5d%s" % (rel, n, over))
        if rel in samples:
            print("        %s" % samples[rel])
    print("\n  Samples are redacted; this guard never prints the account name.")
    print("\ncheck_package_account_name: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
