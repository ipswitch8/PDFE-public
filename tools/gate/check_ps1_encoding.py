#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_ps1_encoding.py -- fail a .ps1 that Windows PowerShell 5.1 cannot parse.

GitHub #130. `Install-PdfeRunner.ps1` -- step 1 of the deployment, the script
the whole package hangs on -- did not parse under Windows PowerShell 5.1. It
was UTF-8 with no BOM and contained 9 non-ASCII bytes (em dashes inside two
string literals):

    ParseFile (host picks encoding)      : 7 error(s)
    ParseInput (read explicitly as UTF-8): 0 error(s)
    non-ASCII bytes: 9   has UTF-8 BOM: False

The syntax was never wrong. The ENCODING was.

WHY THIS CANNOT BE CAUGHT BY READING THE SCRIPT

Windows PowerShell 5.1 assumes the machine's ANSI codepage for a BOM-less
file. PowerShell 7 defaults to UTF-8. So a file with typographic punctuation
and no BOM parses perfectly in PS7 -- which is where these scripts get
exercised -- and fails on Server 2019, where `powershell.exe` IS 5.1 and is
what an operator actually types. The script was fine everywhere it had been
tested and unrunnable where it was meant to run.

This repo's scripts are written by tools and people that emit em dashes, curly
quotes and arrows without thinking about it. Every one is a latent parse
failure on the deployment's target OS, invisible from a PS7 session, and
nothing in tools/gate/ or tests/ parses these scripts at all.

THE RULE

A tracked .ps1 FAILS when it contains a non-ASCII byte AND has no UTF-8 BOM.

Either half alone is fine:
  * pure ASCII, no BOM      -> every codepage agrees on every byte
  * non-ASCII WITH a BOM    -> 5.1 reads the BOM and decodes as UTF-8

The fix for a failing file is to add the BOM, not to strip the punctuation --
though stripping is also valid and is what a caller may prefer for a file that
has no reason to carry non-ASCII at all.

Exit 0 = pass, 1 = violations found, 2 = usage error.
Run --self-test to check the decision function itself.
"""
import argparse
import io
import os
import subprocess
import sys

UTF8_BOM = b"\xef\xbb\xbf"


def classify(data):
    """Pure decision function over a .ps1 file's RAW BYTES.

    Returns (ok, reason, n_non_ascii, has_bom). Kept free of I/O so the
    self-tests exercise the actual rule rather than a paraphrase of it.
    """
    has_bom = data.startswith(UTF8_BOM)
    body = data[len(UTF8_BOM):] if has_bom else data
    n_non_ascii = sum(1 for b in bytearray(body) if b > 0x7F)

    if n_non_ascii == 0:
        return True, "pure ASCII", 0, has_bom
    if has_bom:
        return True, "non-ASCII with UTF-8 BOM", n_non_ascii, True
    return (False,
            "%d non-ASCII byte(s) and NO UTF-8 BOM -- Windows PowerShell 5.1 "
            "will decode these with the ANSI codepage and the parse will fail"
            % n_non_ascii,
            n_non_ascii, False)


def tracked_ps1_files(root="."):
    """Every tracked .ps1, via git so untracked scratch files are ignored."""
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z", "*.ps1"],
            cwd=root, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            check=True)
    except (OSError, subprocess.CalledProcessError):
        return []
    names = [n for n in out.stdout.decode("utf-8", "replace").split("\0") if n]
    return sorted(names)


def check_paths(paths, root="."):
    violations = []
    checked = 0
    for rel in paths:
        full = os.path.join(root, rel)
        try:
            with io.open(full, "rb") as fh:
                data = fh.read()
        except OSError as exc:
            violations.append((rel, "unreadable: %s" % exc))
            continue
        checked += 1
        ok, reason, _n, _bom = classify(data)
        if not ok:
            violations.append((rel, reason))
    return checked, violations


# --------------------------------------------------------------------------
# Self-tests. The guard must be able to go RED, and must not redden correct
# files -- a guard that reddens on correct code is worse than none, and one
# that can only ever be green is decoration.
# --------------------------------------------------------------------------
SELF_TESTS = [
    # (label, bytes, expected_ok)
    ("plain ASCII, no BOM",
     b"Write-Host 'hello'\r\n", True),
    ("plain ASCII with BOM",
     UTF8_BOM + b"Write-Host 'hello'\r\n", True),
    # The exact shape of the #130 failure: an em dash (U+2014) in a literal.
    ("em dash, no BOM -- THE BUG",
     u"Write-Host 'step 1 — install'\r\n".encode("utf-8"), False),
    ("em dash WITH BOM",
     UTF8_BOM + u"Write-Host 'step 1 — install'\r\n".encode("utf-8"), True),
    ("curly quote, no BOM",
     u"Write-Host “hello”\r\n".encode("utf-8"), False),
    ("arrow, no BOM",
     u"# a → b\r\n".encode("utf-8"), False),
    ("empty file",
     b"", True),
    ("BOM only",
     UTF8_BOM, True),
    # A lone high byte in a latin-1 file: still non-ASCII, still no BOM.
    ("latin-1 high byte, no BOM",
     b"Write-Host 'caf\xe9'\r\n", False),
]


def self_test():
    failures = 0
    for label, data, expected_ok in SELF_TESTS:
        ok, reason, n, bom = classify(data)
        status = "ok  " if ok == expected_ok else "FAIL"
        if ok != expected_ok:
            failures += 1
        print("  [%s] %-32s ok=%-5s non_ascii=%-3d bom=%-5s  %s"
              % (status, label, ok, n, bom, reason))
    print("self-test: %d/%d passed" % (len(SELF_TESTS) - failures, len(SELF_TESTS)))
    return 1 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(
        description="Fail a tracked .ps1 that Windows PowerShell 5.1 cannot parse "
                    "(non-ASCII bytes with no UTF-8 BOM).")
    ap.add_argument("paths", nargs="*",
                    help="specific files to check; default is every tracked .ps1")
    ap.add_argument("--root", default=".", help="repository root")
    ap.add_argument("--self-test", action="store_true",
                    help="run the decision function's own tests and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    paths = args.paths or tracked_ps1_files(args.root)
    if not paths:
        print("check_ps1_encoding: no tracked .ps1 files found -- nothing to check")
        return 0

    checked, violations = check_paths(paths, args.root)

    if violations:
        print("check_ps1_encoding: FAIL -- %d of %d file(s) will not parse under "
              "Windows PowerShell 5.1:" % (len(violations), checked))
        for rel, reason in violations:
            print("  %s\n      %s" % (rel, reason))
        print("\nFix: save the file as UTF-8 WITH a BOM, or remove the non-ASCII "
              "characters. Both are valid; the BOM is the safer default because "
              "it keeps working when someone re-adds punctuation later.")
        return 1

    print("check_ps1_encoding: PASS -- %d tracked .ps1 file(s), none rely on the "
          "host's codepage guess" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
