#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Re-derive every figure asserted by the F20 disposition file FROM THE TREE.

Meta-audit follow-up phase-20 (gap ids PDF-34, PDF-35). Six phases of the
predecessor pipeline failed gates because a summary disagreed with its own
table, so nothing in `phase-20-dispositions.md` is asserted by hand: this
script reads the pipeline definition, the gap register, the capability
documents, `tests/CMakeLists.txt` and the test sources, and fails if the
document disagrees with any of them.

It is deliberately non-circular: every fact checked here is read out of
something other than the disposition file.

Usage:
    python tools/gate/verify_f20_dispositions.py [--json OUT.json]

Exit status 0 = every check passed.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

PIPELINE = os.path.join(
    REPO, ".claude", "pipelines", "meta-audit-f20-f25", "pipeline.json")
REGISTER = os.path.join(
    REPO, "docs", "audit", "meta-audit-2026-07", "gap-register.md")
DISPOSITIONS = os.path.join(
    REPO, "docs", "audit", "meta-audit-2026-07", "followup",
    "phase-20-dispositions.md")
PROFILE_DOC = os.path.join(REPO, "docs", "signature-profile-support.md")
TESTS_CMAKE = os.path.join(REPO, "tests", "CMakeLists.txt")
TEST_FILE = os.path.join(REPO, "tests", "test_pades_profiles.cpp")

# The profile-LEVEL vocabulary. Only docs/signature-profile-support.md may use
# it, because only that document says the levels are unsupported.
LEVEL_CLAIM = re.compile(r"PAdES[- ]B-")

failures: list[str] = []
facts: dict = {}


def read(path: str) -> str:
    with io.open(path, encoding="utf-8") as handle:
        return handle.read()


def check(condition: bool, message: str) -> bool:
    if not condition:
        failures.append(message)
    return bool(condition)


# ---------------------------------------------------------------------------
# 1. Scope — the ids this phase owns come from pipeline.json, not the doc.
# ---------------------------------------------------------------------------
pipeline = json.loads(read(PIPELINE))
phase1 = next(p for p in pipeline["phases"] if p["id"] == "phase-1")
assigned = sorted(phase1["gap_ids"])
facts["assigned_gap_ids"] = assigned
check(assigned == ["PDF-34", "PDF-35"],
      "pipeline.json phase-1 gap_ids changed: %r" % (assigned,))

register = read(REGISTER)
for gid in assigned:
    check(re.search(r"^\|\s*%s\s*\|" % re.escape(gid), register, re.M) is not None,
          "gap id %s has no row in gap-register.md" % gid)

# ---------------------------------------------------------------------------
# 2. The disposition table — parsed, counted, and compared to its own summary.
# ---------------------------------------------------------------------------
doc = read(DISPOSITIONS)

# The disposition table's first cell carries the id in bold followed by the
# register's own wording, so the id is extracted from the cell rather than
# assumed to BE the cell.
rows = {}
for line in doc.splitlines():
    stripped = line.strip()
    if not (stripped.startswith("|") and stripped.endswith("|")):
        continue
    cells = [c.strip() for c in stripped.strip("|").split("|")]
    if len(cells) < 4:
        continue
    m = re.match(r"^\*\*(PDF-\d+)\*\*", cells[0])
    if m:
        rows[m.group(1)] = cells
facts["disposition_rows"] = sorted(rows)
check(sorted(rows) == assigned,
      "disposition table ids %r != assigned %r" % (sorted(rows), assigned))
check(len(rows) == len(assigned),
      "disposition table has %d rows, expected %d" % (len(rows), len(assigned)))

# Each row must carry a disposition verb we recognise and a non-stub rationale.
DISPOSITION_WORDS = ("CLOSED", "UNSUPPORTED", "OPEN", "WONT-FIX")
for gid, cells in rows.items():
    joined = " | ".join(cells)
    check(any(word in joined for word in DISPOSITION_WORDS),
          "row %s names no disposition (%s)" % (gid, DISPOSITION_WORDS,))
    check(len(joined) > 120,
          "row %s rationale looks like a stub (%d chars)" % (gid, len(joined)))

# ---------------------------------------------------------------------------
# 3. Cited tests must exist, be registered, be enabled.
# ---------------------------------------------------------------------------
cmake = read(TESTS_CMAKE)
check("test_pades_profiles.cpp" in cmake,
      "tests/test_pades_profiles.cpp is not in the explicit list in "
      "tests/CMakeLists.txt — an unregistered test never runs")

test_src = read(TEST_FILE)
declared = set(re.findall(r"TEST(?:_F)?\(\s*(\w+)\s*,\s*(\w+)\s*\)", test_src))
declared_names = {"%s.%s" % (suite, name) for suite, name in declared}
facts["tests_declared"] = sorted(declared_names)
facts["test_count"] = len(declared_names)

check(not any(name.startswith("DISABLED_") for _, name in declared),
      "a test in test_pades_profiles.cpp is DISABLED_-prefixed")
check(not re.search(r"\)\s*\{\s*GTEST_SKIP\(", test_src),
      "a test in test_pades_profiles.cpp opens with an unconditional "
      "GTEST_SKIP()")

# Every `tests/test_pades_profiles.cpp` — `Suite.Name` citation in the document
# must name a test that really exists.
cited = set(re.findall(r"`(Pades\w+\.\w+)`", doc))
facts["tests_cited"] = sorted(cited)
for name in sorted(cited):
    check(name in declared_names,
          "the disposition file cites test %s, which is not declared in "
          "tests/test_pades_profiles.cpp" % name)

# ---------------------------------------------------------------------------
# 4. The single-source-of-truth document must declare nothing supported.
# ---------------------------------------------------------------------------
profile = read(PROFILE_DOC)
block = re.search(r"BEGIN MACHINE-READABLE(.*?)END MACHINE-READABLE",
                  profile, re.S)
check(block is not None,
      "docs/signature-profile-support.md has no machine-readable block")
decls = {}
if block:
    for line in block.group(1).splitlines():
        m = re.match(r"^\s*([a-z0-9\-]+):\s*(\S+)\s*$", line)
        if m:
            decls[m.group(1)] = m.group(2)
facts["profile_declarations"] = decls
for key in ("pades-b-b", "pades-b-t", "pades-b-lt", "pades-b-lta"):
    check(decls.get(key) == "not-supported",
          "docs/signature-profile-support.md declares %s = %r; F20's "
          "disposition is that no baseline level is supported" %
          (key, decls.get(key)))
check(decls.get("document-timestamp") == "not-implemented",
      "docs/signature-profile-support.md declares document-timestamp = %r" %
      (decls.get("document-timestamp"),))
check(decls.get("emitted-subfilter") == "adbe.pkcs7.detached",
      "docs/signature-profile-support.md declares emitted-subfilter = %r" %
      (decls.get("emitted-subfilter"),))

capability_docs = re.findall(r"^\s*capability-doc:\s*(\S+)\s*$",
                             profile, re.M)
facts["capability_docs"] = capability_docs
check(len(capability_docs) >= 4,
      "only %d capability documents are listed" % len(capability_docs))

# ---------------------------------------------------------------------------
# 5. No capability document, and no src/ comment, claims a baseline level.
# ---------------------------------------------------------------------------
offenders = []
for rel in capability_docs:
    path = os.path.join(REPO, rel)
    if not os.path.exists(path):
        failures.append("capability document %s does not exist" % rel)
        continue
    for n, line in enumerate(read(path).splitlines(), 1):
        if LEVEL_CLAIM.search(line):
            offenders.append("%s:%d" % (rel, n))
    check("signature-profile-support.md" in read(path),
          "%s does not point at docs/signature-profile-support.md" % rel)
facts["capability_doc_level_claims"] = offenders
check(not offenders,
      "capability documents still name a baseline level: %r" % (offenders,))

src_hits = []
src_files = 0
for root, _dirs, names in os.walk(os.path.join(REPO, "src")):
    for name in names:
        if not name.endswith((".cpp", ".h", ".hpp", ".cc")):
            continue
        src_files += 1
        path = os.path.join(root, name)
        for n, line in enumerate(read(path).splitlines(), 1):
            if "PAdES" in line:
                src_hits.append("%s:%d" %
                                (os.path.relpath(path, REPO).replace("\\", "/"), n))
facts["src_files_scanned"] = src_files
facts["src_pades_mentions"] = src_hits
check(src_files > 100,
      "the src/ scan visited only %d files — a zero result would be vacuous"
      % src_files)
check(not src_hits, "src/ still names PAdES: %r" % (src_hits,))

# ---------------------------------------------------------------------------
# 6. The mutation table — every listed mutant must be recorded as killed, and
#    the count in the prose must equal the number of rows.
# ---------------------------------------------------------------------------
mut_rows = re.findall(r"^\|\s*(M\d+[a-z]?)\s*\|([^\n]*)\|\s*$", doc, re.M)
facts["mutants_listed"] = [m[0] for m in mut_rows]
facts["mutant_rows_total"] = len(mut_rows)

# A mutant may be recorded as INVALID — a defect in the mutation itself rather
# than in the oracle. Those are listed rather than deleted (a mutation log that
# only records successes is not evidence), but they do not count toward the
# valid total and must say so in their own row.
invalid = [m[0] for m in mut_rows if "INVALID" in m[1].upper()]
valid = [m for m in mut_rows if m[0] not in invalid]
survivors = [m[0] for m in valid if "KILLED" not in m[1].upper()]
facts["mutants_invalid"] = invalid
facts["mutants_valid"] = [m[0] for m in valid]
facts["mutants_not_killed"] = survivors
check(not survivors,
      "mutation table lists valid mutants that were not killed: %r"
      % (survivors,))
check(len(valid) >= 10,
      "only %d valid mutants recorded; every oracle must be mutation-tested"
      % len(valid))

stated = re.search(r"\*\*(\d+) mutants?, (\d+) killed\*\*", doc)
check(stated is not None,
      "the disposition file does not state a '**N mutants, N killed**' total")
if stated:
    facts["mutants_stated"] = [int(stated.group(1)), int(stated.group(2))]
    check(int(stated.group(1)) == len(valid),
          "prose says %s mutants; the table has %d valid rows (%d total, "
          "%d recorded invalid)"
          % (stated.group(1), len(valid), len(mut_rows), len(invalid)))
    check(int(stated.group(2)) == len(valid) - len(survivors),
          "prose says %s killed; the table records %d"
          % (stated.group(2), len(valid) - len(survivors)))

stated_tests = re.search(r"\*\*(\d+) registered, enabled tests\*\*", doc)
check(stated_tests is not None,
      "the disposition file does not state a '**N registered, enabled tests**' "
      "total")
if stated_tests:
    check(int(stated_tests.group(1)) == len(declared_names),
          "prose says %s tests; test_pades_profiles.cpp declares %d"
          % (stated_tests.group(1), len(declared_names)))

# ---------------------------------------------------------------------------
# 7. The RTM must have been re-read, not just marker-bumped.
# ---------------------------------------------------------------------------
rtm = read(os.path.join(REPO, "docs", "audit", "rtm-coverage.md"))
check("**Reflects follow-up phase: 20.**" in rtm,
      "docs/audit/rtm-coverage.md does not carry the phase-20 marker")
check("F20" in rtm, "docs/audit/rtm-coverage.md has no F20 section")

# Every `test_pades_profiles (N)` citation in the RTM must equal the number of
# tests the file actually declares.
#
# This check exists because it was MISSED. The phase-1 karen gate FAILED F20 on
# exactly this: the RTM cited (6) in two places while the file declares 7. The
# disposition file's own total was checked above and was correct — only the RTM
# citation drifted, and nothing looked at it. A verifier that checks one place a
# number is written and not the others just moves the blind spot.
rtm_citations = re.findall(r"test_pades_profiles \((\d+)\)", rtm)
check(len(rtm_citations) > 0,
      "docs/audit/rtm-coverage.md carries no `test_pades_profiles (N)` "
      "citation; the RTM row for this phase is missing or was reworded, so "
      "this count check has gone blind")
for cited in rtm_citations:
    check(int(cited) == len(declared_names),
          "docs/audit/rtm-coverage.md cites test_pades_profiles (%s) but the "
          "file declares %d tests" % (cited, len(declared_names)))
facts["rtm_test_citations"] = rtm_citations


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", dest="json_out", default=None)
    args = parser.parse_args()

    result = {
        "phase": "phase-20",
        "pipeline": "meta-audit-f20-f25",
        "ok": not failures,
        "failures": failures,
        "facts": facts,
    }
    if args.json_out:
        os.makedirs(os.path.dirname(os.path.abspath(args.json_out)),
                    exist_ok=True)
        with io.open(args.json_out, "w", encoding="utf-8") as handle:
            json.dump(result, handle, indent=2, sort_keys=True)
            handle.write("\n")

    print(json.dumps(result, indent=2, sort_keys=True))
    if failures:
        print("\nFAILED: %d check(s)" % len(failures), file=sys.stderr)
        return 1
    print("\nOK: all checks passed", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
