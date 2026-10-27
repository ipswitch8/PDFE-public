#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Machine-check the F22 (phase-3) dispositions against the tree.

Meta-audit 2026-07 follow-up, pipeline ``meta-audit-f20-f25``, phase-3,
gap id ``PDF-38`` — "can a save-path garbage-collection pass drop /DSS?".

This script never reads ``phase-22-dispositions.md`` for its FACTS. Every figure
is derived from ``pipeline.json``, ``gap-register.md``, ``tests/CMakeLists.txt``,
the test source, the product source and the committed mutation log. The
disposition file is read only to compare what it CLAIMS against what the tree
SAYS, so it cannot agree with a wrong summary.

Two lessons from earlier phases are built in deliberately:

* **F20's blind spot** — a figure is checked at *every* place it is written, and
  the check fails if the citation has DISAPPEARED, because a reworded row would
  otherwise silently blind it.
* **Hazard H3** — ``core.autocrlf`` is true in this repo, so every file is read
  with CRLF normalised. A content comparison that skipped this would pass in the
  session that wrote the file and fail after any ``git checkout``.

The phase's headline claim is a *measured negative* ("no GC level drops /DSS"),
which is the easiest kind of claim to fake: a test that asserts survival while
GC is off passes trivially. So the checks below concentrate on the CONTROL —
that both halves of the conjunction are actually asserted, at every level, and
that the mutation log records real kills against the product source.

Usage::

    python tools/gate/verify_f22_dispositions.py [--json OUT]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PIPELINE = ROOT / ".claude/pipelines/meta-audit-f20-f25/pipeline.json"
FOLLOWUP = ROOT / "docs/audit/meta-audit-2026-07/followup"
DISPOSITIONS = FOLLOWUP / "phase-22-dispositions.md"
MUTATION_LOG = FOLLOWUP / "phase-22/mutation-log.json"
REGISTER = ROOT / "docs/audit/meta-audit-2026-07/gap-register.md"
CMAKE = ROOT / "tests/CMakeLists.txt"
GC_TEST = ROOT / "tests/test_ltv_dss_gc_survival.cpp"
DOCUMENT = ROOT / "src/core/Document.cpp"
RTM = ROOT / "docs/audit/rtm-coverage.md"

EXPECTED_GAP_IDS = {"PDF-38"}

# The name the gap register itself proposes for this gap's test. If the register
# cites a name, a test must exist under it — that is the whole point of the
# evidence column.
REGISTER_TEST_NAME = "DssSurvivesFullRewriteSaveWithGc"

# Every garbage level the phase claims to have exercised, and the test that
# carries the claim. Derived here rather than parsed from the doc so the doc
# cannot widen the claim by editing prose.
LEVEL_TESTS = {
    0: ["ProductSaveIncremental_DssAndOrphanBothSurvive",
        "RawGcLevel0_DssAndOrphanBothSurvive"],
    1: ["ProductSaveAsDefaultGc_DssSurvives_OrphanCollected",
        "ProductSaveInPlaceFullRewrite_DssSurvives_OrphanCollected",
        "RawGcLevel1_DssSurvives_OrphanCollected"],
    2: ["RawGcLevel2_DssSurvives_OrphanCollected"],
    3: ["ProductSaveAsFullGc_DssSurvives_OrphanCollected",
        "RawGcLevel3_DssSurvives_OrphanCollected",
        "RepeatedFullGcRewritesDoNotErodeDss",
        "SameSessionDssWriteThenFullGcRewrite_DssSurvives"],
}


def read(p: pathlib.Path) -> str:
    """Read UTF-8 with CRLF normalised (hazard H3)."""
    return p.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


class Checker:
    def __init__(self) -> None:
        self.checks: list[dict] = []
        self.facts: dict = {}

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.checks.append({"check": name, "ok": bool(ok), "detail": detail})
        return bool(ok)

    # -- derived facts ------------------------------------------------------

    @staticmethod
    def test_names(src: str) -> list[str]:
        return [m[1] for m in
                re.findall(r"^TEST(?:_F)?\(\s*(\w+)\s*,\s*(\w+)\s*\)", src, re.M)]

    # -- checks -------------------------------------------------------------

    def run(self) -> None:
        for p in (PIPELINE, DISPOSITIONS, REGISTER, CMAKE, GC_TEST, DOCUMENT,
                  RTM, MUTATION_LOG):
            self.check("file present: " + str(p.relative_to(ROOT)), p.exists())
        if not all(c["ok"] for c in self.checks):
            return

        disp = read(DISPOSITIONS)
        src = read(GC_TEST)
        pipeline = json.loads(read(PIPELINE))
        phase = next(p for p in pipeline["phases"] if p["id"] == "phase-3")

        # 1. Gap ids: pipeline == expectation == disposition table == register.
        pipeline_ids = set(phase["gap_ids"])
        self.check("pipeline.json phase-3 gap_ids are the F22 id",
                   pipeline_ids == EXPECTED_GAP_IDS,
                   "pipeline says %s" % sorted(pipeline_ids))
        table_ids = set(re.findall(r"\*\*(PDF-\d+)\*\*", disp))
        self.check("disposition table covers exactly the pipeline's gap ids",
                   table_ids == pipeline_ids,
                   "table %s vs pipeline %s" % (sorted(table_ids),
                                                sorted(pipeline_ids)))
        register = read(REGISTER)
        for gid in sorted(EXPECTED_GAP_IDS):
            self.check("gap id %s exists in gap-register.md" % gid,
                       ("| %s |" % gid) in register)
        self.facts["gap_ids"] = sorted(pipeline_ids)

        # 2. Registration and enabled-ness.
        cmake = read(CMAKE)
        self.check("test_ltv_dss_gc_survival.cpp registered verbatim in "
                   "tests/CMakeLists.txt",
                   "    test_ltv_dss_gc_survival.cpp\n" in cmake)
        self.check("no DISABLED_ test in the F22 test file",
                   not re.search(r"^TEST(?:_[PF])?\(\s*\w+\s*,\s*DISABLED_",
                                 src, re.M))
        # "Unconditional" means the skip is not guarded by an `if`. A guarded
        # skip that only fires when the PKCS#7 bridge is unavailable is not a
        # scope reduction; an unguarded one is. Count both and require the
        # unguarded count to be zero rather than banning the token, which would
        # only push the same behaviour somewhere less visible.
        all_skips = re.findall(r"GTEST_SKIP\(\)", src)
        guarded = re.findall(r"if\s*\([^{;]*\)\s*\{?\s*\n?\s*GTEST_SKIP\(\)",
                             src, re.S)
        self.check("every GTEST_SKIP in the F22 test file is conditional",
                   len(all_skips) == len(guarded),
                   "%d skip(s), %d guarded" % (len(all_skips), len(guarded)))
        # …and it must live in the fixture's SetUp, not inside a test body,
        # where it could silently hollow out one specific assertion.
        bodies = re.split(r"^TEST(?:_F)?\(", src, flags=re.M)[1:]
        in_body = sum(b.count("GTEST_SKIP()") for b in bodies)
        self.check("no GTEST_SKIP inside any F22 TEST body", in_body == 0,
                   "%d site(s)" % in_body)
        self.facts["gtest_skip_sites"] = len(all_skips)

        names = self.test_names(src)
        self.facts["test_cases"] = len(names)
        self.facts["test_names"] = sorted(names)

        # 3. The register's own proposed test name must exist.
        self.check("the gap register's proposed test name %r exists as a test"
                   % REGISTER_TEST_NAME, REGISTER_TEST_NAME in names)

        # 4. Every claimed level has its tests, and the claim set is complete.
        missing = {}
        for level, tests in LEVEL_TESTS.items():
            absent = [t for t in tests if t not in names]
            if absent:
                missing[level] = absent
        self.check("every garbage level 0..3 has its claimed tests present",
                   not missing, repr(missing))
        for level in (0, 1, 2, 3):
            self.check("disposition table has a row for garbage level %d" % level,
                       re.search(r"^\|\s*\*\*%d\*\*\s*\|" % level, disp, re.M)
                       is not None)

        # 5. THE CONTROL. Each sweeping level must assert the orphan is GONE and
        #    each non-sweeping level that it REMAINS. Without both polarities the
        #    control is decoration.
        collected = re.findall(r"EXPECT_FALSE\(controlOrphanPresent", src)
        retained = re.findall(r"EXPECT_TRUE\(controlOrphanPresent", src)
        self.check("the control asserts collection at sweeping levels",
                   len(collected) >= 4, "%d site(s)" % len(collected))
        self.check("the control asserts retention at non-sweeping levels "
                   "(a one-polarity control cannot discriminate)",
                   len(retained) >= 2, "%d site(s)" % len(retained))
        self.facts["control_assertions"] = {"collected": len(collected),
                                            "retained": len(retained)}
        self.check("the control orphan is located via the xref, not a byte grep",
                   "pdf_xref_len" in src and "pdf_load_object" in src)
        self.check("the control orphan is genuinely unreferenced "
                   "(added by pdf_add_object and linked nowhere)",
                   "pdf_add_object" in src
                   and not re.search(r"pdf_dict_puts?\w*\([^)]*kOrphanKey[^)]*\)\s*;\s*\n\s*pdf_array_push",
                                     src))

        # 6. The oracle-proof test: the survival oracle must be shown to FAIL on
        #    a synthesised DSS loss, else every "survived" assertion is vacuous.
        self.check("an in-suite mutant unlinks /DSS and re-runs the sweep",
                   "rawFullRewriteWithDssUnlinked" in src
                   and "OracleDetectsDssLossWhenTheSweepReallyDropsIt" in names)
        self.check("the oracle-proof asserts the LTV bytes are gone from the "
                   "whole file, not merely absent from /DSS",
                   "distinctTracersInFile" in src)

        # 7. Content-not-object-number assertions (gc=3 renumbers).
        self.check("survival is asserted on stream CONTENT (byte sets), not "
                   "object numbers or array order", "sameBlobSet" in src)

        # 8. The product's selectable levels are pinned to the source constants.
        doc_src = read(DOCUMENT)
        m = re.search(r"static constexpr int kDefaultFullRewriteGc = (\d+);",
                      doc_src)
        self.check("kDefaultFullRewriteGc found in src/core/Document.cpp",
                   bool(m))
        default_gc = int(m.group(1)) if m else None
        m2 = re.search(r"return pImpl->needsFullGc_ \? (\d+) : kDefaultFullRewriteGc;",
                       doc_src)
        self.check("effectiveGarbageLevel() found in src/core/Document.cpp",
                   bool(m2))
        dedup_gc = int(m2.group(1)) if m2 else None
        self.facts["product_gc_levels"] = {"default": default_gc,
                                           "dedup": dedup_gc}
        self.check("the product's selectable full-rewrite levels are still "
                   "1 and 3 (the matrix this phase measured)",
                   default_gc == 1 and dedup_gc == 3,
                   "default=%s dedup=%s" % (default_gc, dedup_gc))
        self.check("a registered test pins that enumeration, so a change to it "
                   "cannot land unmeasured",
                   "ProductSelectableGarbageLevelsAreOneAndThree" in names)

        # 9. Mutation log: real kills, arithmetic consistent, doc agrees.
        log = json.loads(read(MUTATION_LOG))
        rows = log["mutants"]
        killed = [r for r in rows if r["verdict"] == "KILLED"]
        self.check("mutation log arithmetic: rows == total",
                   len(rows) == log["total"],
                   "%d rows, total %d" % (len(rows), log["total"]))
        self.check("mutation log arithmetic: every mutant was KILLED",
                   len(killed) == len(rows),
                   "%d killed of %d" % (len(killed), len(rows)))
        self.check("every mutant targets the PRODUCT source, not the test",
                   all(r["file"].startswith("src/") for r in rows),
                   repr([r["file"] for r in rows]))
        unknown = sorted({t for r in rows for t in r["killed_by"]
                          if t.split(".", 1)[-1] not in names})
        self.check("every killing test named in the log exists in the source",
                   not unknown, repr(unknown))
        # The two product-selectable levels must be covered INDEPENDENTLY: if
        # both mutants were killed by exactly the same set, one blanket
        # assertion is doing all the work.
        kill_sets = [set(r["killed_by"]) for r in rows]
        self.check("the two GC-disabling mutants are killed by DIFFERENT test "
                   "sets (levels covered independently)",
                   len(kill_sets) < 2 or kill_sets[0] != kill_sets[1],
                   repr([sorted(s) for s in kill_sets]))
        self.facts["mutants"] = {"rows": len(rows), "killed": len(killed)}

        doc_rows = re.findall(r"^\| \*\*(M\d+)\*\* \|", disp, re.M)
        self.check("disposition mutation table has one row per logged mutant",
                   len(doc_rows) == len(rows),
                   "doc %d rows, log %d rows" % (len(doc_rows), len(rows)))
        self.check("disposition mutation table ids match the log",
                   set(doc_rows) == {r["id"] for r in rows},
                   "doc %s vs log %s" % (sorted(doc_rows),
                                         sorted(r["id"] for r in rows)))

        # 10. The test-count citation, checked at EVERY place it is written —
        #     and failing if it has vanished entirely (F20's blind spot).
        cites = []
        for md in sorted((ROOT / "docs").rglob("*.md")):
            for hit in re.finditer(r"test_ltv_dss_gc_survival \((\d+)\)",
                                   read(md)):
                cites.append((str(md.relative_to(ROOT)).replace("\\", "/"),
                              int(hit.group(1))))
        self.check("at least one docs/ citation of the F22 test count exists "
                   "(a reworded row would otherwise blind this check)",
                   len(cites) > 0)
        bad_cites = [c for c in cites if c[1] != len(names)]
        self.check("every docs/ citation of the F22 test count equals %d"
                   % len(names), not bad_cites, repr(bad_cites))
        self.facts["test_count_citations"] = cites

        # 11. The disposition must not overstate: a measured NEGATIVE is the
        #     claim, so it must say so and must not claim a product fix.
        self.check("disposition states the result as a measured negative",
                   "measured negative" in disp.lower())
        self.check("disposition records that no product fix was required",
                   "No product fix was required" in disp)

        # 12. RTM liveness marker.
        self.check("RTM marker advanced to phase 22",
                   "**Reflects follow-up phase: 22.**" in read(RTM))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    c = Checker()
    c.run()
    ok = all(x["ok"] for x in c.checks)
    out = {"ok": ok, "phase": "phase-3 (F22)",
           "pipeline": "meta-audit-f20-f25",
           "facts": c.facts, "checks": c.checks,
           "failed": [x for x in c.checks if not x["ok"]]}
    text = json.dumps(out, indent=2, sort_keys=False) + "\n"
    if args.json:
        pathlib.Path(args.json).write_text(text, encoding="utf-8")
    print(text)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
