#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #139 step 2 — a standing control tying a MuPDF bump to a fuzz recheck.

WHAT THIS GUARDS, AND WHY IT EXISTS

`tests/docs/known_issues.md` section `PDFE-SEC-MUPDF-RECURSION-DOS` records a
real fuzzer-found stack-overflow in MuPDF's page-tree walk, contained at the app
layer by the `pdfe_preflight` child process. Its stated resolution path has two
steps. Step 2 is:

    "on a MuPDF bump, re-run `fuzz_document_open` to confirm"

Nothing read that sentence. There was no checklist item, no workflow step and no
pin anywhere tying a version change to the recheck, so a bump could silently fix
the defect, silently fail to fix it, or silently make it worse, and nobody would
learn which. This guard is that missing control: it fails the build when the
MuPDF version this project resolves can have moved since the recorded recheck.

(Step 1 -- reporting the reproducer to Artifex/MuPDF upstream -- is a human
action outside any repository, and this guard deliberately does not pretend to
cover it. It is tracked in the issue, not here.)

THE TWO DOORS, which is the whole subtlety

`vcpkg.json` declares a FLOOR, not a version:

    { "name": "libmupdf", "version>=": "1.26.10#1" }

The version actually installed on this machine is **1.26.10#4** (measured from
`vcpkg_installed/<triplet>/share/libmupdf/vcpkg.spdx.json`). The floor and the
resolved version are different numbers, and they move independently:

  * DOOR 1 -- someone raises `version>=`. Visible in `vcpkg.json`.
  * DOOR 2 -- someone bumps `builtin-baseline`. `vcpkg.json`'s libmupdf stanza
    does not change by one character, yet the resolved version can move by
    several port revisions or a whole upstream release.

Door 2 is the realistic path: baseline bumps are routine dependency hygiene and
touch one SHA. A guard keyed only on the libmupdf stanza would be blind to it
and would have reported OK across exactly the change most likely to happen.

So this guard pins BOTH, and a change to either is a bump for its purposes. That
is intentionally conservative: a baseline bump that leaves libmupdf untouched
will redden this guard and cost a recheck that was not strictly required. That
is the correct direction to err -- the alternative is resolving the baseline to
a concrete version, which needs a configured vcpkg and a network, neither of
which a fast check has. The remedy message says so, so the cost is visible and
the record can be updated deliberately after confirming.

WHAT WOULD MAKE THIS GUARD VACUOUS, and why none of it passes

  * A missing record file -> FAIL, not pass.
  * A record present but missing a key, or holding an empty string -> FAIL.
  * A `vcpkg.json` restructured so no libmupdf stanza is found -> FAIL. An
    extractor that returns None on an unrecognised shape and then compares
    None == None is the single most common way a pin like this dies quietly.

Each of those is asserted as a RED direction in `--self-test`, alongside two
GREEN directions -- the live tree, and an unrelated dependency being added --
because a guard that only ever reddens is as useless as one that never does.

Usage:
    python tools/gate/check_mupdf_bump_recheck.py              # check the tree
    python tools/gate/check_mupdf_bump_recheck.py --self-test  # prove it works

Exit 0 = the recorded recheck still describes the MuPDF this tree resolves.
Exit 1 = a bump happened, or the record cannot be trusted. Re-run the harness.
"""

from __future__ import annotations

import argparse
import io
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

MANIFEST_REL = "vcpkg.json"
RECORD_REL = "tools/gate/mupdf_recheck_baseline.json"

# The entry whose resolution path this guard enforces, named so a reader of a
# failure message can find the evidence rather than guess at it.
KNOWN_ISSUE = "PDFE-SEC-MUPDF-RECURSION-DOS"
HARNESS = "fuzz_document_open"


class Vacuous(Exception):
    """The guard cannot determine what it is supposed to compare.

    Raised rather than returning a sentinel, so there is no code path on which
    an unreadable input is mistaken for a match.
    """


def extract_pins(manifest_text: str) -> dict:
    """Pull the two things a MuPDF bump can move out of the manifest.

    Raises Vacuous if either is absent or not a non-empty string -- including
    the case where `vcpkg.json` has been restructured and the libmupdf stanza
    is no longer where this looks. "I could not find it" must never read as
    "it did not change".
    """
    try:
        manifest = json.loads(manifest_text)
    except ValueError as exc:
        raise Vacuous("%s is not parseable JSON: %s" % (MANIFEST_REL, exc))

    baseline = manifest.get("builtin-baseline")
    if not isinstance(baseline, str) or not baseline.strip():
        raise Vacuous(
            "%s has no usable 'builtin-baseline'. That SHA is what actually "
            "moves the resolved libmupdf version, so without it this guard "
            "cannot see the most likely bump path." % MANIFEST_REL)

    floor = None
    for dep in manifest.get("dependencies") or []:
        # Dependencies are either a bare string or an object. Only the object
        # form can carry a version constraint, and a bare "libmupdf" means the
        # floor has been DROPPED -- which is itself a change worth failing on.
        if isinstance(dep, dict) and dep.get("name") == "libmupdf":
            floor = dep.get("version>=")
            break
        if isinstance(dep, str) and dep == "libmupdf":
            raise Vacuous(
                "%s declares libmupdf with no 'version>=' constraint. The "
                "floor recorded in %s can no longer be compared against "
                "anything." % (MANIFEST_REL, RECORD_REL))

    if not isinstance(floor, str) or not floor.strip():
        raise Vacuous(
            "no libmupdf dependency with a 'version>=' found in %s. If the "
            "manifest's shape changed, this guard must be taught the new shape "
            "-- it will not silently pass." % MANIFEST_REL)

    return {"declared_floor": floor.strip(), "builtin_baseline": baseline.strip()}


def load_record(record_text: str) -> dict:
    """The committed statement of which MuPDF was last fuzz-rechecked."""
    try:
        record = json.loads(record_text)
    except ValueError as exc:
        raise Vacuous("%s is not parseable JSON: %s" % (RECORD_REL, exc))

    out = {}
    for key in ("declared_floor", "builtin_baseline"):
        val = record.get(key)
        if not isinstance(val, str) or not val.strip():
            raise Vacuous(
                "%s is missing a usable '%s'. A record that does not state "
                "what was rechecked cannot clear a bump." % (RECORD_REL, key))
        out[key] = val.strip()
    return out


def _remedy(field: str, recorded: str, actual: str) -> str:
    return "\n".join([
        "MuPDF may have moved since the recorded %s recheck." % HARNESS,
        "",
        "  %-17s recorded: %s" % (field, recorded),
        "  %-17s actual:   %s" % ("", actual),
        "",
        "%s (tests/docs/known_issues.md) states its resolution path as:" % KNOWN_ISSUE,
        "  \"on a MuPDF bump, re-run `%s` to confirm\"" % HARNESS,
        "",
        "This guard is that step. To clear it:",
        "  1. Re-run the %s harness against the new MuPDF." % HARNESS,
        "  2. Record the outcome in the %s entry -- whether the" % KNOWN_ISSUE,
        "     recursion DoS still reproduces, is fixed, or has changed shape.",
        "  3. Update %s to the values above." % RECORD_REL,
        "",
        "Do NOT update the record without step 1. The record's only value is",
        "that a human ran the harness; editing it to match is how this control",
        "becomes decoration.",
        "",
        "NOTE on builtin_baseline: a baseline bump reddens this guard even when",
        "libmupdf itself did not move, because resolving the baseline to a",
        "concrete version needs a configured vcpkg and a network that a fast",
        "check does not have. If you confirm libmupdf is unchanged across the",
        "bump, say so in the entry and update the record -- that is a real",
        "verification, not a bypass.",
    ])


def check(problems: list, *, manifest_text=None, record_text=None) -> None:
    """Append a problem string for each way the recheck record is stale."""
    if manifest_text is None:
        manifest_text = (REPO_ROOT / MANIFEST_REL).read_text(
            encoding="utf-8", errors="replace")

    if record_text is None:
        path = REPO_ROOT / RECORD_REL
        if not path.is_file():
            problems.append(
                "MISSING RECORD: %s does not exist, so nothing states which "
                "MuPDF was last fuzz-rechecked. %s's resolution path step 2 "
                "is unenforceable without it." % (RECORD_REL, KNOWN_ISSUE))
            return
        record_text = path.read_text(encoding="utf-8", errors="replace")

    try:
        actual = extract_pins(manifest_text)
        recorded = load_record(record_text)
    except Vacuous as exc:
        problems.append("CANNOT COMPARE: %s" % exc)
        return

    for field in ("declared_floor", "builtin_baseline"):
        if actual[field] != recorded[field]:
            problems.append(_remedy(field, recorded[field], actual[field]))


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

def _live_texts():
    return (
        (REPO_ROOT / MANIFEST_REL).read_text(encoding="utf-8", errors="replace"),
        (REPO_ROOT / RECORD_REL).read_text(encoding="utf-8", errors="replace"),
    )


def _self_test() -> int:
    manifest, record = _live_texts()
    rec = json.loads(record)
    failures = []

    def direction(label, expect_problems, manifest_text, record_text):
        problems = []
        check(problems, manifest_text=manifest_text, record_text=record_text)
        got = bool(problems)
        ok = got == expect_problems
        print("  [%s] %-58s expected=%s got=%s"
              % ("ok" if ok else "FAIL", label,
                 "RED" if expect_problems else "GREEN",
                 "RED" if got else "GREEN"))
        if not ok:
            failures.append(label)
            for p in problems:
                print("        | " + p.splitlines()[0])

    print("GREEN directions -- the guard must be SPECIFIC, not merely sensitive")

    direction("the live tree as committed", False, manifest, record)

    # Specificity: an unrelated manifest edit must not redden. Without this,
    # any guard that simply hashed vcpkg.json would pass the red directions
    # below while making every routine dependency change a false positive.
    m = json.loads(manifest)
    m["dependencies"].append({"name": "zlib", "default-features": False})
    direction("an UNRELATED dependency is added", False,
              json.dumps(m, indent=2), record)

    m2 = json.loads(manifest)
    m2["description"] = "a reworded description"
    direction("an unrelated manifest field is reworded", False,
              json.dumps(m2, indent=2), record)

    print()
    print("RED directions -- the two bump doors")

    m3 = json.loads(manifest)
    for dep in m3["dependencies"]:
        if isinstance(dep, dict) and dep.get("name") == "libmupdf":
            dep["version>="] = "1.27.0"
    direction("DOOR 1: the declared floor is raised", True,
              json.dumps(m3, indent=2), record)

    m4 = json.loads(manifest)
    m4["builtin-baseline"] = "0" * 40
    direction("DOOR 2: builtin-baseline is bumped, stanza untouched", True,
              json.dumps(m4, indent=2), record)

    m5 = json.loads(manifest)
    m5["builtin-baseline"] = "0" * 40
    for dep in m5["dependencies"]:
        if isinstance(dep, dict) and dep.get("name") == "libmupdf":
            dep["version>="] = "1.27.0"
    direction("both doors at once", True, json.dumps(m5, indent=2), record)

    print()
    print("RED directions -- the record itself cannot be trusted")

    for key in ("declared_floor", "builtin_baseline"):
        r = dict(rec)
        del r[key]
        direction("the record is missing '%s'" % key, True,
                  manifest, json.dumps(r))
        r2 = dict(rec)
        r2[key] = "   "
        direction("the record's '%s' is blank" % key, True,
                  manifest, json.dumps(r2))

    direction("the record is not valid JSON", True, manifest, "{ not json")
    direction("the record is an empty object", True, manifest, "{}")

    print()
    print("RED directions -- the manifest's shape changed under the guard")
    print("  (an extractor returning None here would compare None==None and PASS)")

    m6 = json.loads(manifest)
    m6["dependencies"] = [d for d in m6["dependencies"]
                          if not (isinstance(d, dict) and d.get("name") == "libmupdf")]
    direction("the libmupdf stanza is REMOVED entirely", True,
              json.dumps(m6, indent=2), record)

    m7 = json.loads(manifest)
    m7["dependencies"] = ["libmupdf" if (isinstance(d, dict) and d.get("name") == "libmupdf")
                          else d for d in m7["dependencies"]]
    direction("libmupdf becomes a bare string (floor DROPPED)", True,
              json.dumps(m7, indent=2), record)

    m8 = json.loads(manifest)
    for dep in m8["dependencies"]:
        if isinstance(dep, dict) and dep.get("name") == "libmupdf":
            del dep["version>="]
    direction("the stanza loses its 'version>=' key", True,
              json.dumps(m8, indent=2), record)

    m9 = json.loads(manifest)
    del m9["builtin-baseline"]
    direction("builtin-baseline is removed", True,
              json.dumps(m9, indent=2), record)

    direction("the manifest is not valid JSON", True, "{ not json", record)

    print()
    if failures:
        print("SELF-TEST FAILED: %d direction(s): %s"
              % (len(failures), ", ".join(failures)))
        return 1
    print("SELF-TEST OK -- all directions behaved as asserted")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true",
                    help="prove the guard reddens and stays green as asserted")
    ap.add_argument("--repo-root", default=None,
                    help="override the repo root (testing)")
    args = ap.parse_args(argv)

    global REPO_ROOT
    if args.repo_root:
        REPO_ROOT = Path(args.repo_root).resolve()

    if args.self_test:
        return _self_test()

    problems = []
    check(problems)
    if problems:
        print("FAIL -- MuPDF bump recheck control (GitHub #139)")
        for p in problems:
            print()
            print(p)
        return 1

    print("OK -- the recorded %s recheck still describes the MuPDF this tree "
          "resolves" % HARNESS)
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
