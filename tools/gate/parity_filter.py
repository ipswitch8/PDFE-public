#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""parity_filter.py -- emit the cross-platform CORE PARITY gtest_filter.

Register class C-10 (cross-platform parity). This is the SINGLE, shared
generator that every validation lane (windows-/linux-/macos-validation.yml)
invokes to obtain the gtest_filter for the core parity suite. Because all three
lanes run THIS script over the SAME source list (tools/gate/parity_suite.txt),
the selection is byte-identical on Windows, Linux, and macOS by construction --
there is no per-lane copy of the filter string to drift.

Reads tools/gate/parity_suite.txt (one gtest suite pattern per line; '#'
comments and blank lines ignored), normalizes each bare suite name to
'Name.*', and prints the ':'-joined gtest_filter to stdout as a single line
(no trailing newline unless --newline), UTF-8.

Usage:
  python parity_filter.py                 # print the ':'-joined filter
  python parity_filter.py --list          # print one normalized pattern per line
  python parity_filter.py --count-file X  # print number of patterns (sanity)
Exit 0 on success, 2 if the source list is missing/empty.
"""
import argparse
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LIST = os.path.join(HERE, "parity_suite.txt")


def load_patterns(path):
    patterns = []
    with io.open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            # Normalize a bare suite name to a suite-wildcard pattern. Leave a
            # pattern that already contains '.' or '*' untouched.
            if "." not in line and "*" not in line:
                line = line + ".*"
            patterns.append(line)
    return patterns


def _check(list_file, patterns):
    """Self-verify the emitted selection (GitHub #187).

    WHY THIS LIVES HERE RATHER THAN IN THE WORKFLOW. Every consumer of this
    generator -- windows-, linux- and macos-validation -- is
    workflow_dispatch-only, so a parity_suite.txt that had been truncated, or a
    join that silently collapsed entries, would not surface until somebody
    dispatched a lane by hand, and then only as "the parity run was
    suspiciously quick". The generator itself needs no build and no test
    results, so the CHECK can run on every push even though the CONSUMERS
    cannot.

    Written as a `--check` subcommand rather than a shell pipeline in the YAML
    for a concrete reason: tools/gate/run_fast_checks_locally.py mirrors CI by
    scanning fast-checks.yml for plain command lines, and its own test fails
    when a referenced gate script is not DISCOVERABLE that way. A verification
    buried in `$(...)` substitutions would have been enforced on push but
    invisible to the local mirror -- which is the same "looks covered, is not"
    shape this whole phase is about.
    """
    problems = []
    joined = ":".join(patterns)
    if not joined:
        problems.append("the joined filter is EMPTY; all three validation "
                        "lanes would select nothing and pass.")
    expected = 0
    with io.open(list_file, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if line and not line.startswith("#"):
                expected += 1
    if len(patterns) != expected:
        problems.append(
            "emitted %d pattern(s) for %d non-comment source line(s) -- the "
            "shared selection has drifted from %s."
            % (len(patterns), expected, list_file))
    # A ':'-joined list of N patterns has exactly N-1 separators. This catches a
    # join that collapsed entries, which counting patterns alone would not.
    seps = joined.count(":")
    if seps != len(patterns) - 1:
        problems.append(
            "the joined filter has %d separator(s) for %d pattern(s); the join "
            "dropped or merged entries." % (seps, len(patterns)))
    # Every pattern must actually be a gtest filter shape.
    for p in patterns:
        if "." not in p and "*" not in p:
            problems.append("pattern %r is neither Suite.Test nor a wildcard; "
                            "gtest would match nothing." % p)

    if problems:
        sys.stderr.write("parity_filter --check FAILED:\n")
        for p in problems:
            sys.stderr.write("  * %s\n" % p)
        return 1
    sys.stdout.write(
        "parity_filter --check OK: %d pattern(s), %d separator(s), all "
        "well-formed; the cross-platform parity selection is intact.\n"
        % (len(patterns), seps))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="Emit the C-10 core parity gtest_filter.")
    ap.add_argument("--list-file", default=DEFAULT_LIST,
                    help="path to parity_suite.txt (default: alongside this script)")
    ap.add_argument("--list", action="store_true",
                    help="print one normalized pattern per line instead of the joined filter")
    ap.add_argument("--check", action="store_true",
                    help="verify the emitted selection is non-empty, complete "
                         "and correctly joined; print nothing else")
    ap.add_argument("--newline", action="store_true",
                    help="terminate the joined filter with a newline")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.list_file):
        sys.stderr.write("parity_suite list not found: %s\n" % args.list_file)
        return 2
    patterns = load_patterns(args.list_file)
    if not patterns:
        sys.stderr.write("parity_suite list is empty: %s\n" % args.list_file)
        return 2

    if args.check:
        return _check(args.list_file, patterns)

    # Force UTF-8 stdout regardless of platform console codepage.
    out = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", newline="")
    if args.list:
        out.write("\n".join(patterns) + "\n")
    else:
        out.write(":".join(patterns) + ("\n" if args.newline else ""))
    out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
