# -*- coding: utf-8 -*-
"""Reduce a merged gtest JUnit XML to a small, committable evidence digest.

WHY. A full-suite `gtest_full.xml` is ~1.6 MB and lives under `build/`, which is
gitignored. So a figure like "5903 tests, 0 failures, 16 skipped" ends up
existing only as PROSE in a summary document: honest, but not independently
checkable from the tree without a ~35-minute rebuild. An ad-hoc `karen` review
named that gap, and F36 is this project's standing record of what happens when
cited evidence is not in the tree.

Committing 1.6 MB per run is the wrong trade. This writes the part that carries
the claim -- totals, per-suite counts, and the full skip list with reasons --
which is a few tens of KB and is enough to check the summary against, to diff two
runs, and to notice a test that became newly skipped rather than newly passing.

What it deliberately does NOT preserve: per-test timings and stdout. Those are
the bulk and they carry host paths, which is how the account name got into the
tree in the first place (see history-scrub-2026-08-27.md). The digest is
scrubbed of absolute paths as a second line of defence regardless.

Usage:
    python tests/scripts/gtest_run_digest.py <gtest_full.xml> <out.json> [--label TEXT]
"""
from __future__ import annotations

import argparse
import io
import json
import re
import sys
import xml.etree.ElementTree as ET

_ABS_PATH = re.compile(r"(?:[A-Za-z]:[\\/]|\\\\)[^\s\"'<>|*?\r\n]*")


def _scrub(text):
    return _ABS_PATH.sub("<path>", text or "")


def digest(xml_path, label=""):
    root = ET.parse(xml_path).getroot()

    totals = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0}
    suites = {}
    skipped = []
    failed = []

    for case in root.iter("testcase"):
        name = "%s.%s" % (case.get("classname", ""), case.get("name", ""))
        suite = case.get("classname", "")
        entry = suites.setdefault(suite, {"tests": 0, "failures": 0,
                                          "errors": 0, "skipped": 0})
        totals["tests"] += 1
        entry["tests"] += 1

        skip = case.find("skipped")
        fail = case.find("failure")
        err = case.find("error")
        if fail is not None:
            totals["failures"] += 1
            entry["failures"] += 1
            failed.append({"test": name, "kind": "failure",
                           "message": _scrub(fail.get("message"))[:400]})
        if err is not None:
            totals["errors"] += 1
            entry["errors"] += 1
            failed.append({"test": name, "kind": "error",
                           "message": _scrub(err.get("message"))[:400]})
        if skip is not None:
            totals["skipped"] += 1
            entry["skipped"] += 1
            skipped.append({"test": name,
                            "message": _scrub(skip.get("message"))[:300]})

    totals["passed"] = (totals["tests"] - totals["failures"]
                        - totals["errors"] - totals["skipped"])
    return {
        "schema": "pdfe.gtest-run-digest/1",
        "label": label,
        "totals": totals,
        # Sorted so two digests diff cleanly; a run is compared against a run,
        # not merely read.
        "skipped": sorted(skipped, key=lambda s: s["test"]),
        "failed": sorted(failed, key=lambda f: f["test"]),
        "suites": {k: suites[k] for k in sorted(suites)},
        "suite_count": len(suites),
    }


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("xml")
    ap.add_argument("out")
    ap.add_argument("--label", default="")
    args = ap.parse_args(argv)

    data = digest(args.xml, args.label)
    with io.open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
        fh.write("\n")

    t = data["totals"]
    print("[digest] %s -> %s" % (args.xml, args.out))
    print("[digest] tests=%d passed=%d failures=%d errors=%d skipped=%d "
          "across %d suite(s)"
          % (t["tests"], t["passed"], t["failures"], t["errors"], t["skipped"],
             data["suite_count"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
