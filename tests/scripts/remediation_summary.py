# -*- coding: utf-8 -*-
"""Phase-6 / Phase-18 closeout: produce a machine-readable summary of
all testing-methodology remediation work landed by pipelines c4e71a39
and a7c2e541.

Pipeline a7c2e541 phase-18 closeout extends the original phase-6
summary with six additional sections covering the E2E expansion work:
``ui_catalog_completeness``, ``fixture_diversity_count``,
``visual_baseline_count``, ``e2e_test_count_per_subsystem``,
``methodology_lint_results``, and ``stress_perf_baseline``.  See the
section-specific collectors below for the JSON schema of each.

What it counts
--------------
1. **Phase-1 hardening headers** (`PHASE1_HARDENING: skip=N expect_ge=M
   no_throw=K`): scans every test_*.cpp under tests/, sums skip /
   expect_ge / no_throw across files. These were added by phase-1's
   mechanical sweep; phase-6 verifies the totals.
2. **Round-trip test files**: tests/test_*_roundtrip.cpp +
   tests/test_xfdf_encode_decode.cpp + tests/test_pkcs7_roundtrip.cpp
   added by phases 3-5. Counts TEST_F bodies per file.
3. **Real-world fixture corpus**: PDFs + sidecars under
   tests/fixtures/realworld/.
4. **Known-weak registry**: parses tests/KNOWN_WEAK_TESTS.txt and
   reports the entry count.
5. **Lint script gate**: invokes
   tests/scripts/lint_save_without_verify.py and reports the finding
   count + exit code.

Output
------
JSON document on stdout suitable for piping into a CI artifact or a
manual diff with prior runs:

    {
      "pipeline_id": "c4e71a39-09f5-4b82-a3d7-8f1e6d2c0b45",
      "phase1_hardening": {
        "files": [{"path": "...", "skip": 14, "expect_ge": 3, "no_throw": 0}, ...],
        "totals": {"skip": 68, "expect_ge": 24, "no_throw": 15}
      },
      "round_trip_tests": {
        "files": [{"path": "...", "test_count": 12}, ...],
        "total_tests": 75
      },
      "realworld_fixtures": {"pdfs": 12, "sidecars": 12},
      "known_weak_tests": {"count": N, "entries": [...]},
      "lint": {"exit_code": 0, "findings": 35}
    }

Exit codes
----------
0 — summary produced. The summary itself doesn't fail the run.
2 — IO error (e.g., tests/ missing).

Use `--strict` to fail (exit 1) when any of these fail:
- phase1 totals don't match the pipeline AC minimums (skip≥68, ge≥20, throw≥15)
- round-trip test count is below the AC sum (≥75 — pipeline.json phase-6 AC)
- realworld fixtures below the AC floor (≥12 PDFs)
- known-weak entries exceed 20.  THE CAP IS 20 and this docstring used to
  say 23, contradicting the assert below it (``if kw > 20``) and
  tests/docs/test_strategy.md ("Hard cap: 20 entries").  Corrected
  2026-09-16, pipeline p86-followups phase-6.  Cap history: c4e71a39
  phase-6 set 20 → a7c2e541 phase-12 raised to 25 → phase-15 raised to
  30 → phase-16 relaxed to 23/24 with a phase-17 closure obligation →
  phase-17 closed 5 entries → **a7c2e541 phase-18 hard-asserted the
  original phase-6 cap of 20**, and c91e7f3a phase-7 held it there.  The
  relaxation was transient and was wound back; only the prose copies
  rotted.  20 governs because it is what the machine checks.

  NOTE for whoever next reads this: until p86-followups phase-6 NOTHING
  INVOKED THIS SCRIPT.  ``--strict`` appeared only in documentation and in
  this file — no workflow and no CMake ``add_test`` ran it — so the cap sat
  breached at 52 entries undetected.  It is now invoked by governance.yml
  (dispatch-only, full summary) and, for the cap specifically, by
  tools/gate/check_known_weak_cap.py in fast-checks.yml on every push.
  If you make this script slower, keep the fast guard fast.
- lint exit code is non-zero
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

REPO = Path(__file__).resolve().parent.parent.parent

TESTS = REPO / "tests"
SCRIPTS = TESTS / "scripts"
FIXTURES = TESTS / "fixtures"
REALWORLD = FIXTURES / "realworld"
KNOWN_WEAK = TESTS / "KNOWN_WEAK_TESTS.txt"
DOCS = TESTS / "docs"
VISUAL = TESTS / "visual"
SCREENSHOTS = FIXTURES / "screenshots"
UI_SCREENSHOTS = FIXTURES / "ui_screenshots"
UI_CATALOG = DOCS / "ui_element_catalog.json"
STRESS_BASELINE = DOCS / "stress_perf_baseline.json"
QT_GAP_AUDIT = DOCS / "qt_test_gap_audit.md"

PIPELINE_ID = "b3d7f942-1c8e-4a06-9e51-d2f60c87a314"
CURRENT_PIPELINE_ID = "b3d7f942"

# Phase-18 closeout: per-subsystem E2E test file mapping. Each entry
# maps a logical subsystem to one or more pytest test files under
# tests/.  The closeout collector iterates this map, runs
# ``pytest --collect-only -q`` on each file, and records the parsed
# count.  The 11 subsystems listed below MUST all be populated for
# remediation_summary.py --strict to pass (phase-18 AC2).
E2E_SUBSYSTEM_FILES = {
    "file_operations":          ["test_ui_file_operations.py"],
    "view_navigation":          ["test_ui_view_navigation.py"],
    "text_editing":             ["test_ui_text_editing.py"],
    "annotations":              ["test_ui_annotations.py"],
    "forms":                    ["test_ui_forms.py"],
    "page_operations":          ["test_ui_page_operations.py"],
    "redaction":                ["test_ui_redaction.py"],
    "encryption_signatures":    ["test_ui_encryption_signatures.py"],
    "compliance_accessibility": ["test_ui_compliance_accessibility.py"],
    "preferences_chrome":       ["test_ui_preferences_chrome.py"],
    "stress":                   ["test_ui_stress.py"],
}

# Phase-1 header pattern. The headers were added by the phase-1
# mechanical sweep at the top of every modified test file.
PHASE1_RE = re.compile(
    r"//\s*PHASE1_HARDENING:\s*"
    r"skip=(?P<skip>\d+)\s+"
    r"expect_ge=(?P<expect_ge>\d+)\s+"
    r"no_throw=(?P<no_throw>\d+)"
)

# Round-trip test files added by phases 3-5.
ROUND_TRIP_FILES = [
    "test_annotation_roundtrip.cpp",
    "test_form_roundtrip.cpp",
    "test_xfdf_encode_decode.cpp",
    "test_encryption_roundtrip.cpp",
    "test_signature_roundtrip.cpp",
    "test_pdfa_roundtrip.cpp",
    "test_pkcs7_roundtrip.cpp",
    "test_redaction_roundtrip.cpp",
    "test_image_roundtrip.cpp",
    "test_metadata_roundtrip.cpp",
    "test_realworld_corpus.cpp",
]

TEST_F_RE = re.compile(
    r"^\s*TEST(?:_F|_P)?\s*\(\s*[A-Za-z_]\w*\s*,\s*([A-Za-z_]\w*)",
    re.MULTILINE,
)


def collect_phase1_headers() -> dict:
    """Walk tests/test_*.cpp, find every PHASE1_HARDENING header,
    sum the per-file counts."""
    files: list[dict] = []
    totals = {"skip": 0, "expect_ge": 0, "no_throw": 0}
    for cpp in sorted(TESTS.glob("test_*.cpp")):
        text = cpp.read_text(encoding="utf-8", errors="replace")
        m = PHASE1_RE.search(text)
        if not m:
            continue
        skip = int(m.group("skip"))
        ge = int(m.group("expect_ge"))
        nt = int(m.group("no_throw"))
        files.append({
            "path": str(cpp.relative_to(REPO).as_posix()),
            "skip": skip,
            "expect_ge": ge,
            "no_throw": nt,
        })
        totals["skip"] += skip
        totals["expect_ge"] += ge
        totals["no_throw"] += nt
    return {"files": files, "totals": totals}


def collect_round_trip_tests() -> dict:
    """Count TEST_F / TEST_P bodies in each round-trip file (excluding
    DISABLED_ tests, which are intentionally not active)."""
    files: list[dict] = []
    total = 0
    disabled = 0
    for stem in ROUND_TRIP_FILES:
        path = TESTS / stem
        if not path.exists():
            files.append({"path": stem, "test_count": 0,
                          "missing": True})
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        names = TEST_F_RE.findall(text)
        active = [n for n in names if not n.startswith("DISABLED_")]
        dis    = [n for n in names if     n.startswith("DISABLED_")]
        files.append({
            "path": str(path.relative_to(REPO).as_posix()),
            "test_count": len(active),
            "disabled_count": len(dis),
        })
        total    += len(active)
        disabled += len(dis)
    return {
        "files": files,
        "total_tests": total,
        "total_disabled": disabled,
    }


def collect_realworld_fixtures() -> dict:
    if not REALWORLD.exists():
        return {"pdfs": 0, "sidecars": 0}
    pdfs = list(REALWORLD.rglob("*.pdf"))
    sidecars = list(REALWORLD.rglob("*.expected.json"))
    return {
        "pdfs": len(pdfs),
        "sidecars": len(sidecars),
        "pdf_paths": [
            str(p.relative_to(REPO).as_posix()) for p in sorted(pdfs)
        ],
    }


def parse_known_weak_tests() -> dict:
    """Read KNOWN_WEAK_TESTS.txt and return the entry list. Format is
    free-form text — each non-blank, non-comment line is an entry."""
    if not KNOWN_WEAK.exists():
        return {"count": 0, "entries": []}
    entries: list[str] = []
    for line in KNOWN_WEAK.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        entries.append(s)
    return {"count": len(entries), "entries": entries}


def run_lint() -> dict:
    """Invoke the phase-1 lint script and capture exit code + finding
    count."""
    lint = SCRIPTS / "lint_save_without_verify.py"
    if not lint.exists():
        return {"exit_code": -1, "findings": -1,
                "error": "lint script missing"}
    try:
        proc = subprocess.run(
            [sys.executable, str(lint), str(TESTS), "--quiet"],
            capture_output=True, text=True, timeout=60,
        )
    except Exception as ex:
        return {"exit_code": -1, "findings": -1, "error": str(ex)}
    # The lint script's last summary line is e.g.
    # "[lint_save_without_verify] 35 finding(s) in 1 path(s)."
    findings = -1
    for line in proc.stdout.splitlines():
        m = re.match(r"\s*\[lint_save_without_verify\]\s+"
                     r"(\d+)\s+finding", line)
        if m:
            findings = int(m.group(1))
            break
    return {
        "exit_code": proc.returncode,
        "findings": findings,
        "stdout_tail": proc.stdout[-500:],
    }


# ─── Pipeline a7c2e541 phase-18 closeout collectors ──────────────────


def collect_ui_catalog_completeness() -> dict:
    """Read tests/docs/ui_element_catalog.json and report the
    pipeline-a7c2e541 phase-1 inventory totals plus objectName /
    accessibleName coverage percentages.

    Schema::

        {
          "catalog_path": "tests/docs/ui_element_catalog.json",
          "widget_count": int,
          "action_count": int,
          "dialog_count": int,
          "menu_count": int,
          "toolbar_count": int,
          "objectName_coverage_pct": float,
          "accessibleName_coverage_pct": float,
          "total_setObjectName_calls": int,
          "total_setAccessibleName_calls": int,
          "duplicate_objectName_warnings": int
        }
    """
    if not UI_CATALOG.exists():
        return {
            "catalog_path": str(UI_CATALOG.relative_to(REPO).as_posix()),
            "missing": True,
        }
    try:
        catalog = json.loads(UI_CATALOG.read_text(encoding="utf-8"))
    except Exception as ex:  # noqa: BLE001
        return {
            "catalog_path": str(UI_CATALOG.relative_to(REPO).as_posix()),
            "parse_error": str(ex),
        }
    widgets = catalog.get("widgets", [])
    actions = catalog.get("actions", [])
    summary = catalog.get("summary", {})

    widget_count = len(widgets)
    action_count = len(actions)
    obj_with = sum(1 for w in widgets if w.get("objectName"))
    acc_with = sum(1 for w in widgets if w.get("accessibleName"))
    obj_pct = (100.0 * obj_with / widget_count) if widget_count else 0.0
    acc_pct = (100.0 * acc_with / widget_count) if widget_count else 0.0
    dup_warnings = summary.get("duplicate_objectName_warnings", [])

    return {
        "catalog_path": str(UI_CATALOG.relative_to(REPO).as_posix()),
        "widget_count": widget_count,
        "action_count": action_count,
        "dialog_count": len(catalog.get("dialogs", [])),
        "menu_count": len(catalog.get("menus", [])),
        "toolbar_count": len(catalog.get("toolbars", [])),
        "objectName_coverage_pct": round(obj_pct, 2),
        "accessibleName_coverage_pct": round(acc_pct, 2),
        "total_setObjectName_calls": int(
            summary.get("total_setObjectName_calls", 0)
        ),
        "total_setAccessibleName_calls": int(
            summary.get("total_setAccessibleName_calls", 0)
        ),
        "duplicate_objectName_warnings": (
            len(dup_warnings) if isinstance(dup_warnings, list) else 0
        ),
    }


def collect_fixture_diversity_count() -> dict:
    """Walk tests/fixtures/ and report the PDF + sidecar count plus a
    per-category breakdown.  Categories are inferred from the
    immediate parent directory under ``tests/fixtures/`` (the existing
    phase-3 layout); unknown / loose files land under "misc".

    Schema::

        {
          "total_pdfs": int,
          "total_sidecars": int,
          "categories": {<dirname>: int, ...},
          "realworld_pdfs": int
        }
    """
    if not FIXTURES.exists():
        return {"total_pdfs": 0, "total_sidecars": 0, "categories": {}}
    pdfs = list(FIXTURES.rglob("*.pdf"))
    sidecars = list(FIXTURES.rglob("*.expected.json"))
    categories: dict[str, int] = {}
    for pdf in pdfs:
        try:
            rel = pdf.relative_to(FIXTURES)
        except ValueError:
            continue
        # First path component identifies the bucket; loose files at
        # the top of tests/fixtures/ go to "root".
        parts = rel.parts
        bucket = parts[0] if len(parts) > 1 else "root"
        # Strip extension for top-level loose .pdfs (so the bucket name
        # isn't the PDF filename itself).
        if bucket.lower().endswith(".pdf"):
            bucket = "root"
        categories[bucket] = categories.get(bucket, 0) + 1
    realworld_pdfs = sum(
        1 for p in pdfs
        if "realworld" in p.relative_to(FIXTURES).parts
    )
    return {
        "total_pdfs": len(pdfs),
        "total_sidecars": len(sidecars),
        "categories": dict(sorted(categories.items())),
        "realworld_pdfs": realworld_pdfs,
    }


def collect_visual_baseline_count() -> dict:
    """Count the screenshot baselines committed under
    tests/fixtures/screenshots/ and tests/fixtures/ui_screenshots/,
    and any SSIM reference JSONs.

    Schema::

        {
          "screenshot_baselines": int,
          "ui_screenshot_baselines": int,
          "ssim_reference_jsons": int,
          "total_pngs": int
        }
    """
    scr = list(SCREENSHOTS.glob("*.png")) if SCREENSHOTS.exists() else []
    ui_scr = (
        list(UI_SCREENSHOTS.rglob("*.png"))
        if UI_SCREENSHOTS.exists() else []
    )
    # SSIM thresholds / reference json under tests/visual/
    visual_jsons = list(VISUAL.glob("*.json")) if VISUAL.exists() else []
    return {
        "screenshot_baselines": len(scr),
        "ui_screenshot_baselines": len(ui_scr),
        "ssim_reference_jsons": len(visual_jsons),
        "total_pngs": len(scr) + len(ui_scr),
    }


def _count_pytest_tests(test_files: list[Path]) -> int:
    """Run ``pytest --collect-only -q`` on the given files and parse
    out the "N tests collected" footer.  Returns 0 if pytest is not
    invokable or no tests collect; a negative value indicates an
    error.

    This is invoked once per E2E subsystem at summary-collection time
    so that the closeout JSON always reflects the current count of
    collected tests (parametrize expansions included), not a stale
    hand-curated number.
    """
    if not test_files:
        return 0
    args = [sys.executable, "-m", "pytest"]
    for tf in test_files:
        args.append(str(tf))
    args += ["--collect-only", "-q"]
    try:
        proc = subprocess.run(
            args, capture_output=True, text=True, timeout=180,
            cwd=str(REPO),
        )
    except Exception:  # noqa: BLE001
        return -1
    # Footer format: "<N> tests collected in <T>s" or
    # "<N> tests collected / <M> deselected in <T>s".
    m = re.search(
        r"^\s*(\d+)\s+tests?\s+collected\b",
        proc.stdout, flags=re.MULTILINE,
    )
    if m:
        return int(m.group(1))
    return 0


def collect_e2e_test_count_per_subsystem() -> dict:
    """Run pytest --collect-only on each subsystem's test files (per
    ``E2E_SUBSYSTEM_FILES``) and return the per-subsystem count.

    The returned dict has one entry per subsystem plus a "total" key.
    Phase-18 AC2 requires every subsystem entry to be a positive
    integer.
    """
    out: dict[str, int] = {}
    total = 0
    for subsystem, names in E2E_SUBSYSTEM_FILES.items():
        files = [TESTS / n for n in names]
        existing = [f for f in files if f.exists()]
        n = _count_pytest_tests(existing) if existing else 0
        out[subsystem] = n
        if n > 0:
            total += n
    out["total"] = total
    return out


def collect_methodology_lint_results() -> dict:
    """Invoke each of the three methodology-lint scripts and capture
    their (exit_code, findings) tuple.  Phase-18 AC6 requires all
    three to exit 0.

    Schema::

        {
          "save_without_verify": {"findings": int, "exit_code": int},
          "qt_test_gaps":        {
              "total": int, "fixed": int,
              "allowlisted": int, "unresolved": int,
              "exit_code": int
          },
          "python_ui_gaps":     {"findings": int, "exit_code": int}
        }
    """
    out: dict[str, dict] = {}

    def _run(script: Path, label: str, finding_re: str) -> dict:
        if not script.exists():
            return {"exit_code": -1, "findings": -1,
                    "error": "lint script missing"}
        try:
            proc = subprocess.run(
                [sys.executable, str(script), str(TESTS), "--quiet"],
                capture_output=True, text=True, timeout=60,
            )
        except Exception as ex:  # noqa: BLE001
            return {"exit_code": -1, "findings": -1, "error": str(ex)}
        findings = -1
        m = re.search(finding_re, proc.stdout, flags=re.MULTILINE)
        if m:
            findings = int(m.group(1))
        return {
            "exit_code": proc.returncode,
            "findings": findings,
            "label": label,
        }

    # save_without_verify (does NOT accept --quiet; pass --quiet only if
    # the script supports it.  It does; same convention as run_lint().)
    out["save_without_verify"] = _run(
        SCRIPTS / "lint_save_without_verify.py",
        "save_without_verify",
        r"\[lint_save_without_verify\]\s+(\d+)\s+finding",
    )

    # qt_test_gaps reports a richer footer.
    qt = {"exit_code": -1}
    qt_script = SCRIPTS / "lint_qt_test_gaps.py"
    if qt_script.exists():
        try:
            proc = subprocess.run(
                [sys.executable, str(qt_script), str(TESTS), "--quiet"],
                capture_output=True, text=True, timeout=60,
            )
            qt["exit_code"] = proc.returncode
            m = re.search(
                r"\[lint_qt_test_gaps\]\s+(\d+)\s+finding\(s\)\s+total,"
                r"\s+(\d+)\s+FIXED,\s+(\d+)\s+ALLOWLISTED,\s+"
                r"(\d+)\s+UNRESOLVED",
                proc.stdout,
            )
            if m:
                qt["total"]       = int(m.group(1))
                qt["fixed"]       = int(m.group(2))
                qt["allowlisted"] = int(m.group(3))
                qt["unresolved"]  = int(m.group(4))
        except Exception as ex:  # noqa: BLE001
            qt["error"] = str(ex)
    out["qt_test_gaps"] = qt

    out["python_ui_gaps"] = _run(
        SCRIPTS / "lint_python_ui_gaps.py",
        "python_ui_gaps",
        r"\[lint_python_ui_gaps\]\s+(\d+)\s+finding",
    )
    return out


def collect_stress_perf_baseline() -> dict:
    """Read tests/docs/stress_perf_baseline.json and report per-
    scenario budget vs measured wall-clock summary.

    Schema::

        {
          "baseline_path": "...",
          "captured_at": "...",
          "scenarios_recorded": int,
          "scenarios_within_budget": int,
          "scenarios_over_budget": int,
          "all_within_budget": bool,
          "over_budget_scenarios": [{"name": ..., "budget_s": ..., "max_s": ...}]
        }
    """
    if not STRESS_BASELINE.exists():
        return {
            "baseline_path": str(STRESS_BASELINE.relative_to(REPO).as_posix()),
            "missing": True,
        }
    try:
        data = json.loads(STRESS_BASELINE.read_text(encoding="utf-8"))
    except Exception as ex:  # noqa: BLE001
        return {
            "baseline_path": str(STRESS_BASELINE.relative_to(REPO).as_posix()),
            "parse_error": str(ex),
        }
    scenarios = data.get("scenarios", {}) or {}
    within = 0
    over: list[dict] = []
    for name, s in scenarios.items():
        budget = float(s.get("budget_s", 0) or 0)
        wmax   = float(s.get("wall_clock_s_max", 0) or 0)
        if budget > 0 and wmax <= budget:
            within += 1
        elif budget > 0:
            over.append({
                "name": name, "budget_s": budget, "max_s": wmax,
            })
    return {
        "baseline_path": str(STRESS_BASELINE.relative_to(REPO).as_posix()),
        "captured_at": data.get("captured_at"),
        "scenarios_recorded": len(scenarios),
        "scenarios_within_budget": within,
        "scenarios_over_budget": len(over),
        "all_within_budget": len(over) == 0 and len(scenarios) > 0,
        "over_budget_scenarios": over,
    }


def main(argv: Iterable[str]) -> int:
    # Reconfigure stdout/stderr to UTF-8 so output containing non-ASCII
    # characters (e.g., the U+2192 arrow used in the cap-history docstring,
    # or non-ASCII identifiers harvested from KNOWN_WEAK_TESTS.txt) does
    # not raise UnicodeEncodeError when the default Windows console
    # encoding is cp1252.  Required by CLAUDE.md "Always add UTF-8 unicode
    # support explicitly to python scripts."  Falls back gracefully on
    # older Pythons where reconfigure isn't available.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, Exception):  # noqa: BLE001
            pass

    ap = argparse.ArgumentParser(
        description=("Pipeline c4e71a39 phase-6 remediation summary."))
    ap.add_argument("--strict", action="store_true",
                    help="Exit 1 if any of the AC-derived thresholds "
                         "are violated (phase-1 totals below floor, "
                         "round-trip count below AC sum, known-weak "
                         "above cap, lint non-zero exit).")
    ap.add_argument("--out", type=Path, default=None,
                    help="Optional path to write the JSON report. "
                         "Default: stdout.")
    args = ap.parse_args(list(argv))

    summary = {
        "pipeline_id":                  PIPELINE_ID,
        "current_pipeline_id":          CURRENT_PIPELINE_ID,
        "phase1_hardening":             collect_phase1_headers(),
        "round_trip_tests":             collect_round_trip_tests(),
        "realworld_fixtures":           collect_realworld_fixtures(),
        "known_weak_tests":             parse_known_weak_tests(),
        "lint":                         run_lint(),
        # Pipeline a7c2e541 phase-18 closeout extensions.  These cover
        # the E2E expansion work (UI catalog, fixture diversity,
        # visual baselines, per-subsystem E2E counts, methodology lint
        # results, and stress perf budgets) that landed across
        # phases 1-17.
        "ui_catalog_completeness":      collect_ui_catalog_completeness(),
        "fixture_diversity_count":      collect_fixture_diversity_count(),
        "visual_baseline_count":        collect_visual_baseline_count(),
        "e2e_test_count_per_subsystem": collect_e2e_test_count_per_subsystem(),
        "methodology_lint_results":     collect_methodology_lint_results(),
        "stress_perf_baseline":         collect_stress_perf_baseline(),
    }

    text = json.dumps(summary, indent=2, ensure_ascii=False) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)

    if args.strict:
        problems: list[str] = []
        p1 = summary["phase1_hardening"]["totals"]
        if p1["skip"] < 68:
            problems.append(f"phase1 skip total {p1['skip']} < 68")
        if p1["expect_ge"] < 20:
            problems.append(f"phase1 expect_ge {p1['expect_ge']} < 20")
        if p1["no_throw"] < 15:
            problems.append(f"phase1 no_throw {p1['no_throw']} < 15")
        rt_total = summary["round_trip_tests"]["total_tests"]
        # AC floor is 75 round-trip TEST_F bodies (per pipeline.json
        # phase-6 acceptance criterion). The TEST_P expansions in
        # test_realworld_corpus.cpp aren't counted here — those are
        # tracked separately via the realworld_fixtures section.
        if rt_total < 75:
            problems.append(f"round-trip tests {rt_total} < 75")
        rw_pdfs = summary["realworld_fixtures"]["pdfs"]
        if rw_pdfs < 12:
            problems.append(f"realworld fixtures {rw_pdfs} < 12")
        kw = summary["known_weak_tests"]["count"]
        # Cap history: phase-6 (c4e71a39) set 20 → phase-12 (a7c2e541)
        # raised to 25 → phase-15 raised to 30 → phase-16 karen-approved
        # cap relaxation from 20 to 24 with explicit phase-17 closure
        # obligation for 4+ entries (see header of
        # tests/KNOWN_WEAK_TESTS.txt).  Phase-16 closed 6 total entries
        # (3 legacy: signature CN / unicode metadata / redaction text;
        # 3 karen-prescribed: P8-BUG-2b, P13-INFRA-5, P14-INFRA-2)
        # leaving 24 = 23 P-prefixed phase 7+ entries + 1 legacy
        # DISABLED.  Phase-17 closed 5 more entries (P10-INFRA-1,
        # P11-INFRA-1, P11-BUG-2, P12-INFRA-2, P15-INFRA-2), leaving 19.
        # Phase-18 hard-asserts the original phase-6 cap of <= 20.
        if kw > 20:
            problems.append(f"known-weak tests {kw} > 20 cap")
        lint = summary["lint"]
        if lint.get("exit_code", -1) != 0:
            problems.append(f"lint exit_code {lint.get('exit_code')}")

        # ─── Pipeline a7c2e541 phase-18 acceptance criteria ───────────

        # AC6: all three methodology lints must exit 0.
        mlr = summary["methodology_lint_results"]
        for lint_name in ("save_without_verify", "qt_test_gaps",
                          "python_ui_gaps"):
            entry = mlr.get(lint_name, {}) or {}
            ec = entry.get("exit_code", -1)
            if ec != 0:
                problems.append(
                    f"methodology lint {lint_name} exit_code {ec}"
                )

        # AC2: e2e_test_count_per_subsystem populated for all 11
        # subsystems (every count must be > 0).
        e2e = summary["e2e_test_count_per_subsystem"]
        required_subs = list(E2E_SUBSYSTEM_FILES.keys())
        for sub in required_subs:
            n = int(e2e.get(sub, 0) or 0)
            if n <= 0:
                problems.append(f"e2e subsystem {sub} count={n}")

        # AC1: ensure the six new sections are present (defensive
        # against future code edits accidentally dropping one).
        new_sections = [
            "ui_catalog_completeness",
            "fixture_diversity_count",
            "visual_baseline_count",
            "e2e_test_count_per_subsystem",
            "methodology_lint_results",
            "stress_perf_baseline",
        ]
        for sec in new_sections:
            if sec not in summary:
                problems.append(f"missing closeout section: {sec}")

        if problems:
            sys.stderr.write("[remediation_summary] STRICT failures:\n")
            for p in problems:
                sys.stderr.write(f"  - {p}\n")
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
