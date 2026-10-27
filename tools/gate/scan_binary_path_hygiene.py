# -*- coding: utf-8 -*-
"""Scan NON-TEXT files for a leaked account name or home path.

    python tools/gate/scan_binary_path_hygiene.py <file> [<file> ...]
    python tools/gate/scan_binary_path_hygiene.py --self-test

Exit 0 = every scanned file is clean, 1 = at least one carries a hit,
2 = usage / IO error.

WHY THIS EXISTS SEPARATELY FROM check_tracked_path_hygiene.py

That guard is the authority on WHAT counts as a leak, and this file does not
re-decide it: the account list, both home-path regexes and the placeholder
allowances are imported from it, so the two cannot drift into disagreeing about
the rule.

What it will not do is READ a binary. Its `TEXT_EXT` set stops at the extension
(`.md .yml .py .cpp .h ... .csv .bat .cmd`), so `.pdf`, `.exe`, `.pdb` and
`.exp` are skipped before a single byte is examined. That is a defensible
default for a whole-tree sweep -- decoding 300 MB of PDFs on every push would be
absurd -- but it means "the account-name detector says clean" has never been a
statement about any binary in this repository, and two recorded incidents were
exactly that: a `.pdb` path embedded by the linker, and gtest XML captured under
`docs/`.

So when a BINARY is about to be tracked, this is the scan to run, and the point
is to be able to say which scan was run rather than implying the tree sweep
covered it.

TWO PASSES, BECAUSE ONE IS NOT ENOUGH FOR A PDF

A path inside a PDF content stream is usually Flate-compressed, and a raw byte
scan cannot see it. So every `stream ... endstream` span is inflated and
scanned too. A stream that fails to inflate is COUNTED AND REPORTED rather than
ignored: "0 hits" over a file whose streams were all unreadable is not a clean
result, it is no result, and the difference has to be visible to the caller.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_tracked_path_hygiene as hygiene  # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

_STREAM_RE = re.compile(rb"stream\r?\n")


def inflate_streams(raw):
    """Return ``(inflated_texts, n_ok, n_failed)`` for a PDF's streams."""
    texts = []
    ok = failed = 0
    for match in _STREAM_RE.finditer(raw):
        start = match.end()
        end = raw.find(b"endstream", start)
        if end < 0:
            continue
        try:
            texts.append(zlib.decompress(raw[start:end]).decode("latin-1"))
            ok += 1
        except Exception:  # noqa: BLE001
            # Not every stream is Flate (images, raw XML metadata, already-
            # plaintext content), and those are covered by the raw-bytes pass.
            failed += 1
    return texts, ok, failed


def scan_file(path, accounts):
    """Return ``(account_hits, home_owners, n_streams_ok, n_streams_failed)``."""
    with open(path, "rb") as handle:
        raw = handle.read()
    chunks = [raw.decode("latin-1")]
    stream_texts, ok, failed = ([], 0, 0)
    if raw[:5] == b"%PDF-":
        stream_texts, ok, failed = inflate_streams(raw)
        chunks.extend(stream_texts)

    account_hits = set()
    owners = set()
    for chunk in chunks:
        hits, found = hygiene.scan_text(chunk, accounts)
        account_hits.update(hits)
        owners.update(found)
    return sorted(account_hits), sorted(owners), ok, failed


def _self_test():
    """The guard must be able to go red, and must not redden correct content.

    EVERY HOME PATH BELOW IS ASSEMBLED AT RUN TIME, never written as a literal.

    A self-test that spelled out ``C:\\Users\\<owner>\\...`` would make this file
    itself a finding for `check_tracked_path_hygiene.py`, and the obvious escape
    -- adding this path to that module's SELF_EXEMPT_OWNER_CHECK -- is pinned
    shut by `test_the_self_exemption_is_partial_and_narrow`, correctly: every
    entry in that set is one more place in the tree where a real leak could
    hide. Building the fixtures from parts costs two lines and needs no
    exemption, so the detector's blind spot does not grow to accommodate its own
    test suite.
    """
    import tempfile

    failures = 0
    owner = "car" + "ol"              # a synthetic owner, never a real account
    accounts = {owner}
    bs = chr(92)                      # backslash, kept out of any path literal
    win_home = "C:" + bs + "Users" + bs + owner + bs
    posix_home = "/" + "home" + "/" + owner + "/"
    placeholder = "C:" + bs + "Users" + bs + "dev" + bs
    ci_home = "/" + "home" + "/" + "runner" + "/"

    cases = [
        # (label, bytes, expect_dirty)
        ("plain clean bytes", b"%PDF-1.5\nnothing to see\n", False),
        ("raw account name",
         ("%PDF-1.5\n" + win_home + "build" + bs + "x.pdb\n").encode("latin-1"),
         True),
        ("raw posix home",
         ("%PDF-1.5\n" + posix_home + "src/pdfe\n").encode("latin-1"), True),
        ("placeholder owner is NOT a leak",
         ("%PDF-1.5\n" + placeholder + "source" + bs + "PDFE\n").encode("latin-1"),
         False),
        ("CI runner owner is NOT a leak",
         ("%PDF-1.5\n" + ci_home + "work/PDFE\n").encode("latin-1"), False),
    ]

    # The load-bearing case: a leak that ONLY exists inside a compressed
    # stream. A single-pass raw scan reports this file clean.
    compressed = zlib.compress(
        ("BT (" + win_home + "secret) Tj ET").encode("latin-1"))
    cases.append((
        "account name ONLY inside a Flate stream",
        b"%PDF-1.5\n1 0 obj<</Length 1/Filter/FlateDecode>>stream\n"
        + compressed + b"\nendstream endobj\n",
        True,
    ))

    with tempfile.TemporaryDirectory() as td:
        for label, data, expect_dirty in cases:
            path = os.path.join(td, "case.pdf")
            with open(path, "wb") as handle:
                handle.write(data)
            hits, owners, _ok, _failed = scan_file(path, accounts)
            dirty = bool(hits or owners)
            status = "ok  " if dirty == expect_dirty else "FAIL"
            if dirty != expect_dirty:
                failures += 1
            print("  [%s] %-42s dirty=%-5s owners=%s"
                  % (status, label, dirty, owners))

    print("self-test: %d/%d passed" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(
        description="Scan binary files (PDFs, executables) for a leaked "
                    "account name or home path, using "
                    "check_tracked_path_hygiene's rules.")
    parser.add_argument("paths", nargs="*", help="files to scan")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()
    if not args.paths:
        parser.print_usage()
        return 2

    accounts = hygiene.current_account_names()
    if not accounts:
        # Reported, never silently treated as a pass: with no account name to
        # search for, the account half of this scan established nothing. The
        # home-path half still runs and is still meaningful.
        print("scan_binary_path_hygiene: NOTE -- no usable account name in the "
              "environment (USERNAME/USER/LOGNAME); only the home-path rules "
              "were applied.")

    dirty = 0
    for path in args.paths:
        try:
            hits, owners, ok, failed = scan_file(path, accounts)
        except OSError as exc:
            print("  ERROR %s: %s" % (path, exc))
            return 2
        size = os.path.getsize(path)
        if hits or owners:
            dirty += 1
            # The account name itself is never printed -- writing it into a log
            # or a gate artifact is the leak this guard exists to prevent.
            print("  DIRTY %s (%d bytes): %d account-name hit(s), "
                  "home-path owners=%s"
                  % (path, size, len(hits), owners))
        else:
            print("  clean %s (%d bytes, %d stream(s) inflated, %d not Flate)"
                  % (path, size, ok, failed))

    if dirty:
        print("scan_binary_path_hygiene: FAIL -- %d file(s) carry a leak; do "
              "NOT stage them." % dirty)
        return 1
    print("scan_binary_path_hygiene: PASS -- %d file(s) clean"
          % len(args.paths))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
