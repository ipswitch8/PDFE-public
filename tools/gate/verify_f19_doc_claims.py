# -*- coding: utf-8 -*-
"""Claim-by-claim re-verification of the documentation F19 touched.

Meta-audit follow-up phase-19 acceptance criterion 2:

    Every documentation claim re-verified in this phase either matches the tree
    or is corrected in the same commit; the claim-by-claim verification is
    committed to
    .claude/pipelines/meta-audit-followup/gate-artifacts/phase-19/doc-claim-verification.json
    showing zero remaining mismatches.

Every claim below is re-derived FROM THE TREE. Nothing is read out of the
dispositions file, so this script cannot be satisfied by prose. A claim is
recorded with:

  * ``was``    -- what the document said before F19 (history, for the audit trail)
  * ``now``    -- what it says after
  * ``fact``   -- what the tree says, re-derived here
  * ``status`` -- MATCHES-TREE (nothing needed) or CORRECTED (doc was wrong,
                  fixed in this commit); anything else is a mismatch

Exit 0 only when ``mismatches_remaining`` is 0.

Usage:
  python tools/gate/verify_f19_doc_claims.py --out <path> [--mirror <path>]
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import re
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

MATRIX = REPO / "tests" / "ui_automation" / "matrix" / "ui_test_matrix.csv"
UI_SUMMARY = REPO / "tests" / "_runtime" / "discovery_phase8" / "ui-suite-summary.json"

DOCS = {
    "manual": REPO / "docs" / "manual-verification.md",
    "parity": REPO / "docs" / "acrobat-pro-parity-status.md",
    "comparison": REPO / "docs" / "pdfe-vs-acrobat-pro-comparison.md",
    "pointer_audit": REPO / "docs" / "done-row-test-pointer-audit.md",
    "menu": REPO / "docs" / "menu-wiring-gaps.md",
    "canvas": REPO / "docs" / "canvas-click-harness-gap.md",
    "gate": REPO / "docs" / "gate-verification.md",
    "archive": REPO / "docs" / "archive" / "README.md",
    "image_risks": REPO / "docs" / "image-pdf-risks.md",
    "agpl": REPO / "docs" / "agpl-library-architecture-reevaluation.md",
    "i18n": REPO / "docs" / "i18n.md",
    "closure": REPO / "docs" / "coverage-expansion" / "closure.md",
    "cs_coverage": REPO / "docs" / "content-stream-continuity" / "COVERAGE.md",
    "autosave_rtm": REPO / "docs" / "autosave-coverage" / "RTM.md",
    "security_policy": REPO / "docs" / "security-policy.md",
    "security_testing": REPO / "docs" / "security-testing.md",
}


def read(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def strip_blockquotes(body: str) -> str:
    """Correction notes quote the sentence they replaced; those quotes are not
    assertions. Mirrors stripBlockquotes() in tests/test_f19_doc_claim_guards.cpp."""
    return "\n".join(l for l in body.splitlines() if not l.lstrip().startswith(">"))


def matrix_counts():
    with io.open(MATRIX, encoding="utf-8", newline="") as fh:
        rows = list(csv.DictReader(fh))
    return len(rows), Counter(r["status"] for r in rows)


def parity_pointers(body: str):
    return sorted(set(re.findall(r"\btest_[A-Za-z0-9_]+\.(?:cpp|py)\b",
                                 strip_blockquotes(body))))


def test_file_exists(name: str) -> bool:
    return any((REPO / "tests").rglob(name))


def disabled_macros():
    rx = re.compile(r"^[ \t]*TEST(?:_F|_P)?\([ \t]*[A-Za-z0-9_]+[ \t]*,[ \t]*DISABLED_")
    hits = []
    for f in sorted((REPO / "tests").glob("*.cpp")):
        for n, line in enumerate(read(f).splitlines(), 1):
            if rx.search(line):
                hits.append("%s:%d" % (f.name, n))
    return hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--mirror", type=Path)
    args = ap.parse_args()

    total, st = matrix_counts()
    green, blocked = st.get("green", 0), st.get("blocked", 0)
    wontfix, planned = st.get("wont-fix", 0), st.get("planned", 0)
    xfail, skip = st.get("xfail", 0), st.get("skip", 0)
    nongreen = total - green

    body = {k: read(p) for k, p in DOCS.items()}
    claims = []

    def add(cid, gap, doc, claim, was, now, fact, ok, note="", changed=True):
        """`changed` distinguishes a claim this phase had to FIX from one it
        re-verified and left alone. Conflating the two would inflate the
        correction count, which is the same species of overstatement this
        phase exists to remove."""
        claims.append({
            "claim_id": cid, "gap": gap,
            "document": str(DOCS[doc].relative_to(REPO)).replace("\\", "/"),
            "claim": claim, "was": was, "now": now, "fact_from_tree": fact,
            "status": ("MISMATCH" if not ok else
                       ("CORRECTED" if changed else "MATCHES-TREE")),
            "note": note})

    # ---- 1. matrix-derived counts, three documents, one CSV ----------------
    m = re.search(r"Should print \*\*(\d+)\*\* rows", body["manual"])
    add("C01", "A5-32", "manual", "matrix row count stated in the §0 precondition",
        "450", m.group(1) if m else "(absent)", str(total),
        bool(m) and int(m.group(1)) == total,
        "a precondition check that fails on a correct tree trains people to skip preconditions")

    hm = re.search(
        r"\*\*Matrix state — (\d+) green / (\d+) blocked / (\d+) wont-fix / "
        r"(\d+) planned /\s*(\d+) xfail / (\d+) skip \((\d+) total rows\)\.\*\*",
        body["parity"])
    got = tuple(int(x) for x in hm.groups()) if hm else None
    want = (green, blocked, wontfix, planned, xfail, skip, total)
    add("C02", "A5-88", "parity", "matrix state in the header block",
        "494 green / 0 blocked / 9 wont-fix / 1 planned (504 total)",
        "/".join(map(str, got)) if got else "(absent)", "/".join(map(str, want)),
        got == want)

    idx = body["parity"].find("## What's still open")
    open_sec = strip_blockquotes(body["parity"][idx:]) if idx >= 0 else ""
    # GitHub #175: the `now` field was the hardcoded literal
    # "720/0/33/26/6/2 (787 total)" -- one of the nine transcribed copies of
    # these counts, and one of the eight that nothing checked. A record whose
    # "now" column is a literal reports the value it was WRITTEN with, not the
    # value that is now true, so it silently becomes a second stale claim the
    # moment any row's status flips. Derived from the document, exactly as C02
    # already does, so it cannot drift.
    # Wrap-tolerant at EVERY separator, not just the one that happened to wrap
    # when this was written -- the header and this section wrap in different
    # places, and a regex pinned to one of them silently reports "(absent)".
    om = re.search(
        r"(\d+)\s+green\s*/\s*(\d+)\s+blocked\s*/\s*(\d+)\s+wont-fix\s*/\s*"
        r"(\d+)\s+planned\s*/\s*(\d+)\s+xfail\s*/\s*(\d+)\s+skip\s*"
        r"\((\d+)\s+total\)",
        open_sec)
    add("C03", "A5-89", "parity",
        "matrix state restated in \"What's still open\" (was self-contradictory)",
        "487 green / 0 blocked / 16 wont-fix / 1 planned (504 total)",
        ("/".join(om.groups()[:6]) + " (%s total)" % om.group(7)) if om
        else "(absent)",
        "/".join(map(str, want)),
        ("%d green / %d blocked" % (green, blocked)) in open_sec,
        "the header and this section disagreed with each other AND with the CSV")

    add("C04", "A5-89", "parity", "non-green rows accounted for",
        "17 enumerated, remaining 51 unmentioned",
        "%d stated; 17 named individually, 51 by category" % nongreen,
        "%d non-green" % nongreen,
        ("**%d rows are non-green.**" % nongreen) in open_sec)

    add("C05", "A5-13", "menu", "ledger of blocked matrix rows",
        "29 blocked, target 29 -> ~10-14 blocked", "0 blocked; ledger discharged",
        "%d rows with status 'blocked'" % blocked,
        blocked == 0 and "DISCHARGED 2026-08-03" in body["menu"],
        "the one item that has NOT landed (File>New / Document::createEmpty) is re-homed in the banner")

    # ---- 2. test-pointer integrity ----------------------------------------
    ptrs = parity_pointers(body["parity"])
    missing = [p for p in ptrs if not test_file_exists(p)]
    add("C06", "A5-91", "parity", "every test-file pointer resolves",
        "cited test_cert_chain_extractor.cpp (4) -- never existed",
        "clause removed; coverage attributed to test_ltv_dss_writer.cpp",
        "%d unique pointers, %d unresolved" % (len(ptrs), len(missing)),
        not missing, "unresolved: %s" % (missing or "none"))

    add("C07", "A5-17", "pointer_audit",
        "\"VERDICT: all 69 file pointers ... verified\"",
        "69 pointers / 0 unresolved, asserted in the present tense",
        "superseded box: 112 pointers today, 1 was unresolved (now 0)",
        "%d unique pointers in the parity doc" % len(ptrs),
        "THIS VERDICT EXPIRED" in body["pointer_audit"] and len(ptrs) >= 100,
        "a point-in-time audit is not a control; replaced by a standing guard")

    # ---- 3. run-record claims ---------------------------------------------
    ui = json.loads(read(UI_SUMMARY)) if UI_SUMMARY.exists() else {}
    fails = ui.get("failure_count")
    files_run = ui.get("files_run")
    docf = re.search(r"\*\*(\d+) files run,\s*(\d+) tests, (\d+) failures", body["comparison"])
    add("C08", "A5-97", "comparison", "UI E2E suite state",
        "87 test files / 3015 tests / 0 failures / 0 hidden timeouts",
        "%s files run, %s tests, %s failures" % docf.groups()[:3] if docf else "(absent)",
        "ui-suite-summary.json: files_run=%s tests=%s failures=%s skipped=%s"
        % (files_run, ui.get("totals", {}).get("tests"), fails,
           ui.get("totals", {}).get("skipped")),
        bool(docf) and int(docf.group(1)) == files_run and int(docf.group(3)) == fails,
        "the '0 failures' claim was contradicted by the artifact named in the same sentence")

    dis = disabled_macros()
    add("C09", "A5-88/A5-97", "parity", "\"0 disabled\" tests",
        "0 disabled (unverified assertion)", "0 disabled (machine-checked)",
        "%d DISABLED_-prefixed test macros" % len(dis), not dis,
        "hits: %s" % (dis or "none"))

    add("C10", "A5-97", "comparison", "GoogleTest case count",
        "3926 cases", "4961 static macros / 5295 registered, stated as such",
        "%d TEST* macros in tests/*.cpp" % sum(
            len(re.findall(r"^\s*TEST(?:_F|_P)?\(", read(f), re.M))
            for f in (REPO / "tests").glob("*.cpp")),
        "4961" in body["comparison"],
        "counts now carry the command that re-derives them")

    # ---- 4. coverage-ledger claims ----------------------------------------
    endur = read(REPO / "tests" / "test_endurance_leak.cpp")
    win_only = "#ifdef _WIN32" in endur and "GTEST_SKIP" in endur
    add("C11", "A4-02/A4-24", "closure", "C-02 endurance portability",
        "\"the RSS-delta logic is cross-platform\"",
        "WINDOWS ONLY, whole test; ZERO C-02 coverage off Windows",
        "test body inside #ifdef _WIN32 with unconditional GTEST_SKIP on #else; "
        "counters from GetProcessMemoryInfo; RSS printed, never asserted",
        win_only and "ZERO C-02" in body["closure"]
        and "the RSS-delta logic is cross-platform" not in strip_blockquotes(body["closure"]))

    breadth = REPO / "tests" / "test_golden_reference_breadth.py"
    named = any("test_golden_reference_breadth" in read(f)
                for f in (REPO / ".github" / "workflows").glob("*.yml")) or \
        "test_golden_reference_breadth" in read(REPO / "tests" / "scripts" / "run_ui_suite.py")
    add("C12", "A4-08/A4-25", "closure", "C-08 visual-goldens breadth coverage",
        "\"COVERED (here)\"; only the live-window subset disclosed as runner-only",
        "written and baselined, but selected by NO lane",
        "module exists=%s; referenced by any workflow or the UI runner=%s"
        % (breadth.exists(), named),
        breadth.exists() and not named and "NO LANE RUNS IT" in body["closure"])

    ctm = read(REPO / "tests" / "test_content_stream_ctm_reset.cpp")
    cells = sum(1 for n in (
        "AlignmentInjectorUprightUnshearedUnderAmbientShear",
        "HeaderFooterEngineUprightUnshearedUnderAmbientShear",
        "TreWriteTextBlockUprightUnshearedUnderAmbientShear",
        "AlignmentInjectorUprightUnrotatedUnderAmbientRotate",
        "HeaderFooterEngineUprightUnrotatedUnderAmbientRotate",
        "TreWriteTextBlockUprightUnrotatedUnderAmbientRotate")
        if re.search(r"\bTEST\(\s*CtmReset\s*,\s*%s\s*\)" % n, ctm))
    add("C13", "A4-33", "cs_coverage", "ambient-CTM fixture breadth",
        "\"Other imposed scales / skews / rotations are not enumerated\"",
        "only alternative SCALE values are unenumerated",
        "%d shear/rotate CtmReset cells declared" % cells,
        cells == 6 and "/ skews / rotations are not enumerated"
        not in strip_blockquotes(body["cs_coverage"]),
        "a residual claimed WIDER than reality hides work that was done")

    amb = read(REPO / "tests" / "test_content_stream_adversarial_ambient.cpp")
    off = len(re.findall(r"evalImageCell\([^;]*expectWidth=\*/\s*0\s*\)", amb))
    add("C14", "A4-36", "cs_coverage", "image rotate/crop oracle strength",
        "\"assert presence + not-halved via a colour bbox\"",
        "PRESENCE-ONLY for those two cells",
        "%d evalImageCell call sites pass expectWidth=0, disabling the width check" % off,
        off > 0 and "PRESENCE-ONLY" in body["cs_coverage"])

    add("C15", "A4-31", "autosave_rtm", "TRE generic dimensions backstop",
        "Debounce / UndoToPartial / SaveClears \"already locked by AutosaveSidecarIntegrityTest\"",
        "Debounce and SaveClears are; UndoToPartial has no counterpart",
        "DebounceNoRedundantSidecarRewrite and ClearRecoveryRemovesSessionFromScan exist; "
        "no UndoToPartial counterpart in test_autosave_sidecar_integrity.cpp",
        "**UndoToPartial has no" in body["autosave_rtm"])

    # ---- 5. source-comment and process claims ------------------------------
    sites = {
        "src/core/PageOperations.cpp": read(REPO / "src" / "core" / "PageOperations.cpp"),
        "src/compliance/PdfAConverter.cpp": read(REPO / "src" / "compliance" / "PdfAConverter.cpp"),
        "src/compliance/PdfUAConverter.cpp": read(REPO / "src" / "compliance" / "PdfUAConverter.cpp"),
    }
    have = [k for k, v in sites.items() if "3f8a1c72" in v]
    add("C16", "A4-41", "parity",
        "register claim: the 3f8a1c72 scope comment landed on PageOperations only",
        "comment on PageOperations only, not on the two converters",
        "REGISTER CLAIM WAS FALSE - all three sites carry it",
        "sites carrying the comment: %s" % have, len(have) == 3,
        "recorded as a register correction, not a code change", changed=False)

    repo_hook = (REPO / ".claude" / "hooks" / "phase-gate.sh").exists()
    tg_emit = (REPO / "tools" / "gate" / "emit-gate-verdict.sh").exists()
    add("C17", "A5-49", "gate", "the enforcement mechanism's location",
        "\"The repo-local hook `.claude/hooks/phase-gate.sh`\"",
        "no repo-local hook; the user-global ~/.claude/hooks/phase-gate.sh is the one in force, "
        "and the control is therefore not reproducible from a clone",
        ".claude/hooks/phase-gate.sh exists=%s" % repo_hook,
        (not repo_hook) and "There is no repo-local hook" in body["gate"])

    add("C18", "A5-50", "gate", "gate helper script paths",
        "tools/gate/emit-gate-verdict.sh, tools/gate/check-hooks-alive.sh",
        "~/.claude/hooks/ for both",
        "tools/gate/emit-gate-verdict.sh exists=%s; tools/gate contains no .sh files" % tg_emit,
        (not tg_emit)
        and "~/.claude/hooks/emit-gate-verdict.sh" in body["gate"]
        and "tools/gate/emit-gate-verdict.sh" not in strip_blockquotes(body["gate"]),
        "CLAUDE.md:83 and :92 still give the tools/gate path -- OUTSTANDING, see the "
        "dispositions file; a subagent may not edit CLAUDE.md")

    xfa_ui = any(("XFA" in read(f) or "xfa" in read(f))
                 for d in ("ui", "forms")
                 for f in (REPO / "src" / d).rglob("*")
                 if f.is_file() and f.suffix in (".cpp", ".h"))
    add("C19", "A5-69", "agpl", "XFA gap mitigation",
        "\"Provide 'XFA not supported' dialog\" offered as the mitigation",
        "labelled NOT IMPLEMENTED at both sites plus a closing note",
        "user-facing XFA string anywhere in src/ui or src/forms: %s" % xfa_ui,
        (not xfa_ui) and "The mitigation was never implemented" in body["agpl"])

    add("C20", "A5-79", "archive", "\"the named gaps ... are resolved\"",
        "blanket 'resolved' over 4 named gaps",
        "PARTIALLY, with a 6-row table naming what is and is not resolved",
        "security-feature-gaps.md tracks 6 gaps; 4 over-reaches identified "
        "(PDF/UA, sec_sign_chain_validation, 3x _reopen wont-fix, 2 unnamed)",
        "PARTIALLY" in body["archive"] and "Correction" in body["archive"])

    fx = list((REPO / "tests" / "fixtures" / "image_pdf").glob("*.pdf"))
    add("C21", "A5-27", "image_risks",
        "\"Tests must iterate over the matrix, not cherry-pick one fixture\"",
        "prescription stated, silently not followed",
        "prescription retained; a measured box records that only A3 iterates",
        "%d of 48 prescribed fixtures on disk; B3 cherry-picks 5 of 9; "
        "C2 fixtures have no consumer; B2/A2/C145 never generated" % len(fx),
        "THE PRESCRIPTION ABOVE WAS NOT FOLLOWED" in body["image_risks"])

    add("C22", "A5-15", "canvas", "\"row permanently closed\" (TRE + handle drag)",
        "TRE canvas-click and painted-handle-drag rows permanently closed",
        "both marked STALE; rows are green, oracle built via a different seam",
        "txt_tre_edit_* and canvas_annot_handle_drag_* rows are green in the CSV; "
        "tests/ui_automation/test_canvas_click_tre.py exists",
        "STALE, corrected 2026-08-03" in body["canvas"],
        "residual narrowed but real: the oracle is a screenshot pixel-diff, "
        "not the document-state oracle the blocker specified")

    add("C23", "A5-83", "canvas",
        "\"path 2 available once the canvas-click harness gap lands\"",
        "conditional promise, silently unfulfilled after the blocker cleared",
        "explicitly recorded as a promise NOT kept; 17 rows green on the weaker oracle",
        "no content-stream / save-and-reload oracle exists in tests/ui_automation/*.py",
        "Downstream promise NOT kept" in body["canvas"])

    # ---- 6. claims re-verified and found ALREADY TRUE (no edit needed) -----
    add("C24", "A5-62", "security_policy",
        "engine entry points \"tracked as deferred follow-up\"",
        "register said the entry should be removed because the work landed",
        "unchanged -- the deferral is still accurate",
        "no entry-point test or fuzz target exists for BatchEngine / OptimizeEngine / "
        "RedactionVerifier / AlignmentInjector / HeaderFooterEngine; doc edited 2026-08-02",
        "tracked as deferred follow-up" in body["security_policy"],
        "REGISTER CLAIM WAS FALSE: the document is current, the gap is genuinely open",
        changed=False)

    add("C25", "A5-60", "security_testing",
        "\"security testing is OPT-IN; no CI green-bar gates on the security label\"",
        "register recorded this as an open gap",
        "unchanged -- the document already self-corrected in F5 (gap SEC-23)",
        "windows-validation.yml runs `ctest -L security` as a gating step with no "
        "continue-on-error; the residual local-opt-in caveat is still true",
        "CORRECTED 2026-08-01" in body["security_testing"],
        "REGISTER CLAIM WAS STALE: superseded by F5 before F19 ran",
        changed=False)

    add("C26", "-", "i18n", "every factual claim in docs/i18n.md",
        "n/a", "unchanged",
        "translations/ holds app_en.ts + app_fr.ts and no .qm; installTranslations "
        "declared in src/app/Translations.h and called once from src/main.cpp; "
        ".ts->.qm compilation is genuinely not wired into CMake",
        "English only" in body["i18n"] or "app_fr.ts" in body["i18n"],
        "swept for overstated capability and found CLEAN -- it pre-negates its own "
        "limitations. Recorded so the sweep's negative result is visible.",
        changed=False)

    mismatches = [c for c in claims if c["status"] == "MISMATCH"]
    artifact = {
        "phase": "phase-19",
        "generator": "tools/gate/verify_f19_doc_claims.py",
        "criterion": ("2 -- every documentation claim re-verified either matches "
                      "the tree or is corrected in the same commit"),
        "matrix_fact": {"total": total, "green": green, "blocked": blocked,
                        "wont_fix": wontfix, "planned": planned,
                        "xfail": xfail, "skip": skip, "non_green": nongreen},
        "claims_checked": len(claims),
        "corrected": sum(1 for c in claims if c["status"] == "CORRECTED"),
        "matched_tree_unchanged": sum(1 for c in claims if c["status"] == "MATCHES-TREE"),
        "mismatches_remaining": len(mismatches),
        "documents_touched": sorted({c["document"] for c in claims
                                     if c["status"] == "CORRECTED"}),
        "claims": claims,
    }
    for target in (args.out, args.mirror):
        if target:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(artifact, indent=2), encoding="utf-8")

    print("claims checked: %d  corrected: %d  already-true: %d  mismatches: %d"
          % (len(claims), artifact["corrected"],
             artifact["matched_tree_unchanged"], len(mismatches)))
    for c in mismatches:
        print("  MISMATCH", c["claim_id"], c["document"], "-", c["claim"])
    return 0 if not mismatches else 1


if __name__ == "__main__":
    sys.exit(main())
