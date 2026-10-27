#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Machine-check the F21 (phase-2) dispositions against the tree.

Meta-audit 2026-07 follow-up, pipeline ``meta-audit-f20-f25``, phase-2.

This script never reads ``phase-21-dispositions.md`` for its FACTS — it derives
every figure from ``pipeline.json``, ``gap-register.md``,
``tests/CMakeLists.txt``, the test sources, the fixture corpus and its
manifest, and the committed mutation log. It reads the disposition file only to
compare what that file CLAIMS against what the tree SAYS, so it cannot agree
with a wrong summary.

The F20 gate's lesson is built in deliberately: a figure is checked at **every
place it is written**, not only in its primary home. A stale ``(6)`` in the RTM
survived F20's verifier because the verifier only looked at the disposition
file. Here, the test-count check enumerates every citation across ``docs/``
and fails if the citation has disappeared entirely, because a reworded row
would otherwise silently blind the check.

Usage::

    python tools/gate/verify_f21_dispositions.py [--json OUT]
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
DISPOSITIONS = FOLLOWUP / "phase-21-dispositions.md"
MUTATION_LOG = FOLLOWUP / "phase-21/mutation-log.json"
REGISTER = ROOT / "docs/audit/meta-audit-2026-07/gap-register.md"
CMAKE = ROOT / "tests/CMakeLists.txt"
SHADOW_TEST = ROOT / "tests/test_signature_shadow_attacks.cpp"
DOCMDP_TEST = ROOT / "tests/test_docmdp_enforcement.cpp"
GENERATOR = ROOT / "tests/fixtures/gen_sig_shadow_corpus.py"
MANIFEST = ROOT / "tests/fixtures/sig_shadow_corpus_manifest.json"
FIXTURES_README = ROOT / "tests/fixtures/README.md"
RTM = ROOT / "docs/audit/rtm-coverage.md"

EXPECTED_GAP_IDS = {"PDF-33", "PDF-36", "PDF-37"}

# Where FieldMDP may be named. Audit records under docs/audit/** describe the
# gap and its disposition and must keep saying so — rewriting an audit's own
# text destroys the evidence trail. Everywhere ELSE (the capability documents
# and the public README) naming FieldMDP would be a support claim made
# somewhere nobody is checking, which is the failure mode this programme
# exists to remove.
FIELDMDP_ALLOWED_PREFIX = "docs/audit/"


def read(p: pathlib.Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


class Checker:
    def __init__(self) -> None:
        self.checks: list[dict] = []
        self.facts: dict = {}

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.checks.append({"check": name, "ok": bool(ok), "detail": detail})
        return bool(ok)

    # -- derived facts ------------------------------------------------------

    def runtime_test_count(self, path: pathlib.Path, suite_prefix: str | None = None) -> int:
        """Number of gtest CASES a file registers.

        TEST/TEST_F contribute one each. TEST_P contributes one per value in
        the INSTANTIATE_TEST_SUITE_P ``Values(...)`` list for its fixture.
        """
        src = read(path)
        plain = re.findall(r"^TEST(?:_F)?\(\s*(\w+)\s*,\s*(\w+)\s*\)", src, re.M)
        param = re.findall(r"^TEST_P\(\s*(\w+)\s*,\s*(\w+)\s*\)", src, re.M)
        count = 0
        for suite, _name in plain:
            if suite_prefix is None or suite.startswith(suite_prefix):
                count += 1
        for fixture, _name in param:
            inst = re.search(
                r"INSTANTIATE_TEST_SUITE_P\(\s*\w+\s*,\s*" + re.escape(fixture)
                + r"\s*,\s*::testing::Values\((.*?)\)\s*,", src, re.S)
            if not inst:
                continue
            values = [v for v in inst.group(1).split(",") if v.strip()]
            count += len(values)
        return count

    # -- checks -------------------------------------------------------------

    def run(self) -> None:
        for p in (PIPELINE, DISPOSITIONS, REGISTER, CMAKE, SHADOW_TEST,
                  DOCMDP_TEST, GENERATOR, MANIFEST, FIXTURES_README, RTM,
                  MUTATION_LOG):
            self.check("file present: " + str(p.relative_to(ROOT)), p.exists())
        if not all(c["ok"] for c in self.checks):
            return

        disp = read(DISPOSITIONS)
        pipeline = json.loads(read(PIPELINE))
        phase = next(p for p in pipeline["phases"] if p["id"] == "phase-2")

        # 1. Gap ids: pipeline == expectation == disposition table.
        pipeline_ids = set(phase["gap_ids"])
        self.check("pipeline.json phase-2 gap_ids are the three F21 ids",
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

        # 2. Test registration and enabled-ness.
        cmake = read(CMAKE)
        self.check("test_signature_shadow_attacks.cpp registered verbatim in "
                   "tests/CMakeLists.txt",
                   "    test_signature_shadow_attacks.cpp\n" in cmake)
        self.check("test_docmdp_enforcement.cpp registered verbatim",
                   "    test_docmdp_enforcement.cpp\n" in cmake)
        for path in (SHADOW_TEST, DOCMDP_TEST):
            src = read(path)
            rel = str(path.relative_to(ROOT))
            self.check("no DISABLED_ test in " + rel,
                       not re.search(r"^TEST(?:_[PF])?\(\s*\w+\s*,\s*DISABLED_",
                                     src, re.M))
            bad = re.findall(r"\{\s*\n\s*GTEST_SKIP\(\)", src)
            self.check("no unconditionally skipped test in " + rel, not bad,
                       "%d site(s)" % len(bad))

        # 3. Test counts — derived, then compared to EVERY place they are written.
        shadow_n = self.runtime_test_count(SHADOW_TEST)
        fieldmdp_n = self.runtime_test_count(DOCMDP_TEST, "FieldMdpEnforcement")
        self.facts["shadow_test_cases"] = shadow_n
        self.facts["fieldmdp_test_cases"] = fieldmdp_n

        m = re.search(r"new registered, enabled tests in "
                      r"`tests/test_signature_shadow_attacks\.cpp`\s*\|\s*"
                      r"\*\*(\d+)\*\*", disp)
        self.check("disposition states the shadow-test count", bool(m))
        if m:
            self.check("disposition shadow-test count matches the source",
                       int(m.group(1)) == shadow_n,
                       "doc says %s, source registers %d" % (m.group(1), shadow_n))
        m2 = re.search(r"new `FieldMdpEnforcement` tests\s*\|\s*\*\*(\d+)\*\*",
                       disp)
        self.check("disposition states the FieldMDP test count", bool(m2))
        if m2:
            self.check("disposition FieldMDP test count matches the source",
                       int(m2.group(1)) == fieldmdp_n,
                       "doc says %s, source registers %d"
                       % (m2.group(1), fieldmdp_n))

        # EVERY citation, anywhere under docs/ — the F20 blind-spot fix.
        cites = []
        for md in sorted((ROOT / "docs").rglob("*.md")):
            for hit in re.finditer(r"test_signature_shadow_attacks \((\d+)\)",
                                   read(md)):
                cites.append((str(md.relative_to(ROOT)), int(hit.group(1))))
        self.check("at least one docs/ citation of the shadow-test count "
                   "exists (a reworded row would blind this check)",
                   len(cites) > 0)
        bad_cites = [c for c in cites if c[1] != shadow_n]
        self.check("every docs/ citation of the shadow-test count equals %d"
                   % shadow_n, not bad_cites, repr(bad_cites))
        self.facts["shadow_test_citations"] = cites

        fm_cites = []
        for md in sorted((ROOT / "docs").rglob("*.md")):
            for hit in re.finditer(r"\((\d+) FieldMdpEnforcement tests?",
                                   read(md)):
                fm_cites.append((str(md.relative_to(ROOT)), int(hit.group(1))))
        self.check("at least one docs/ citation of the FieldMDP test count",
                   len(fm_cites) > 0)
        self.check("every docs/ citation of the FieldMDP test count equals %d"
                   % fieldmdp_n,
                   all(c[1] == fieldmdp_n for c in fm_cites), repr(fm_cites))

        # 4. Fixture corpus: manifest == disk == README == disposition.
        manifest = json.loads(read(MANIFEST))
        fixtures = manifest["fixtures"]
        self.check("manifest declares the corpus synthetic",
                   manifest.get("synthetic") is True)
        self.check("manifest names the committed generator",
                   manifest.get("generator")
                   == "tests/fixtures/gen_sig_shadow_corpus.py")
        self.check("manifest gap ids match the phase",
                   set(manifest.get("gap_ids", [])) == EXPECTED_GAP_IDS)

        on_disk = sorted(p.name for p in (ROOT / "tests/fixtures").glob("sig_*.pdf"))
        self.check("every manifest fixture exists on disk and vice versa",
                   sorted(fixtures) == on_disk,
                   "manifest %s vs disk %s" % (sorted(fixtures), on_disk))
        size_drift = [n for n in fixtures
                      if (ROOT / "tests/fixtures" / n).exists()
                      and (ROOT / "tests/fixtures" / n).stat().st_size
                      != fixtures[n]["bytes"]]
        self.check("no fixture has drifted from its manifest byte count",
                   not size_drift, repr(size_drift))

        readme = read(FIXTURES_README)
        missing_doc = [n for n in fixtures if n not in readme]
        self.check("every fixture is documented in tests/fixtures/README.md",
                   not missing_doc, repr(missing_doc))
        self.check("the generator is listed in the README regeneration block",
                   "python gen_sig_shadow_corpus.py" in readme)

        md = re.search(r"fixtures in the corpus\s*\|\s*\*\*(\d+)\*\*", disp)
        self.check("disposition states the fixture count", bool(md))
        if md:
            self.check("disposition fixture count matches the corpus",
                       int(md.group(1)) == len(fixtures),
                       "doc says %s, corpus has %d" % (md.group(1), len(fixtures)))
        self.facts["fixture_count"] = len(fixtures)

        # 5. Mutation arithmetic: table rows - invalid == stated == killed.
        log = json.loads(read(MUTATION_LOG))
        rows = log["mutants"]
        killed = [r for r in rows if r["verdict"] == "KILLED"]
        invalid = [r for r in rows if r.get("invalid")]
        stated = log["total"]
        self.check("mutation log arithmetic: rows - invalid == total",
                   len(rows) - len(invalid) == stated,
                   "%d rows, %d invalid, total %d"
                   % (len(rows), len(invalid), stated))
        self.check("mutation log arithmetic: killed == total",
                   len(killed) == stated,
                   "%d killed, total %d" % (len(killed), stated))
        mm = re.search(r"\*\*(\d+) mutants, (\d+) killed\.\*\*", disp)
        self.check("disposition states the mutant totals", bool(mm))
        if mm:
            self.check("disposition mutant totals match the log",
                       int(mm.group(1)) == stated and int(mm.group(2)) == len(killed),
                       "doc says %s/%s, log says %d/%d"
                       % (mm.group(1), mm.group(2), stated, len(killed)))
        doc_rows = re.findall(r"^\| (M\d+\w*) \|", disp, re.M)
        self.check("disposition mutation table has one row per logged mutant",
                   len(doc_rows) == len(rows),
                   "doc %d rows, log %d rows" % (len(doc_rows), len(rows)))
        self.check("disposition mutation table ids match the log",
                   set(doc_rows) == {r["id"] for r in rows},
                   "doc %s vs log %s" % (sorted(doc_rows),
                                         sorted(r["id"] for r in rows)))
        self.facts["mutants"] = {"rows": len(rows), "killed": len(killed),
                                 "invalid": len(invalid), "total": stated}

        # 6. FieldMDP claim policy.
        offenders = []
        for md_path in sorted((ROOT / "docs").rglob("*.md")):
            rel = str(md_path.relative_to(ROOT)).replace("\\", "/")
            if rel.startswith(FIELDMDP_ALLOWED_PREFIX):
                continue
            if "FieldMDP" in read(md_path):
                offenders.append(rel)
        if "FieldMDP" in read(ROOT / "README.md"):
            offenders.append("README.md")
        self.check("no capability document claims FieldMDP support",
                   not offenders, repr(offenders))
        self.check("disposition records the FieldMDP write-side residual",
                   "/V /Reference" in disp and "not written" in disp)

        # 7. Product code really exists (a disposition claiming enforcement
        #    while nothing implements it is the failure mode being guarded).
        enforcer = read(ROOT / "src/signatures/DocMdpEnforcer.cpp")
        self.check("DocMdpEnforcer reads /Lock (FieldMDP)",
                   "PDF_NAME(Lock)" in enforcer and "lockedObjects" in enforcer)
        self.check("DocMdpEnforcer reads the ISO /Reference /SigRef layout",
                   "PDF_NAME(Reference)" in enforcer
                   and "IsoSigRefReference" in enforcer)
        self.check("SignatureCoverageAnalyzer exists and is compiled",
                   (ROOT / "src/signatures/SignatureCoverageAnalyzer.cpp").exists()
                   and "SignatureCoverageAnalyzer.cpp"
                   in read(ROOT / "src/signatures/CMakeLists.txt"))

        # 8. RTM liveness marker.
        self.check("RTM marker advanced to phase 21",
                   "**Reflects follow-up phase: 21.**" in read(RTM))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    c = Checker()
    c.run()
    ok = all(x["ok"] for x in c.checks)
    out = {"ok": ok, "phase": "phase-2 (F21)",
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
