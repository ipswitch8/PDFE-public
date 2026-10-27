# -*- coding: utf-8 -*-
"""Phase-4 synthesis: stitch ui-critique + process-critique into a single
headline view. Docs-only."""
import json, io, os
HERE = os.path.dirname(os.path.abspath(__file__))
def load(n): return json.load(io.open(os.path.join(HERE, n), encoding="utf-8"))
ui = load("ui-critique.json"); pr = load("process-critique.json")

sk = ui.get("skip_audit", {}); mx = ui.get("matrix_reconciliation", {})
vo = ui.get("visual_oracle_critique", {}); ut = ui.get("totals", {})

syn = {
    "phase": "phase-4-ui-and-process-critique",
    "generated_note": "Synthesis of ui-critique.json + process-critique.json. FIND-AND-DOCUMENT; no code changed.",
    "ui": {
        "total_skip_sites": sk.get("total_skip_sites"),
        "legitimate_env_guard": sk.get("legitimate_env_guard"),
        "silent_zero_protection": sk.get("silent_zero_protection"),
        "ui_test_files": ut.get("ui_test_files"),
        "effectively_never_run_in_ci": ut.get("effectively_never_run_in_ci"),
        "matrix_pass_is": mx.get("matrix_pass_is_runtime_or_static"),
        "over_claimed_row_classes": len(mx.get("over_claimed", [])),
        "visual_verification_exists": vo.get("any_visual_verification_exists"),
        "visual_mechanisms": vo.get("mechanisms_found"),
        "invisible_regression_classes": vo.get("invisible_regression_classes"),
    },
    "process": {
        "gate_verdict_source": pr.get("gate_integrity", {}).get("verdict_source"),
        "forgery_allowlist_present": pr.get("gate_integrity", {}).get("forgery_allowlist_present"),
        "gap_a15_junit_parsed_in_ci": pr.get("gap_a15_ci", {}).get("junit_parsed_in_ci"),
        "mutation_gate": pr.get("mutation_coverage_floors", {}).get("mutation_gate"),
        "mutation_mutant_count": pr.get("mutation_coverage_floors", {}).get("mutant_count"),
        "mutation_kill_floor_enforced": pr.get("mutation_coverage_floors", {}).get("kill_floor_enforced"),
        "coverage_floor_enforced": pr.get("mutation_coverage_floors", {}).get("coverage_floor_enforced"),
        "flaky_policy": pr.get("flaky_dod_dca", {}).get("flaky_policy"),
        "dod": pr.get("flaky_dod_dca", {}).get("dod"),
        "dca_loop": pr.get("flaky_dod_dca", {}).get("dca_loop"),
    },
    "process_findings": pr.get("findings", []),
    "headline_findings": [
        "GOOD: the gate-reliability fix is REAL — verdicts derive from artifacts + parsed JUnit XML, not prose; no forgery allow-list; GAP-A15 defended in CI (XML sum-of-failures, not exit code).",
        "HOLLOW STANDING GATES: the CI mutation-kill gate is green-by-construction (%s curated mutants, all pre-fixed) while phase-3's real 16-mutant run found 10 survivors incl. a redaction confidentiality regression; coverage floor unenforced; DCA loop decorative; DoD absent." % pr.get("mutation_coverage_floors", {}).get("mutant_count"),
        "META: every quality gate is decoupled from the merge — all CI is nightly/dispatch-only since 2026-07-20, so nothing blocks a bad commit at commit time.",
        "UI PERMASKIP WALL: ~%s of the ~%s UI skip sites are silent-zero-protection (gap_a14 UIA-hang), gating an estimated ~300 of ~450 UI test functions from running ANY behavioral assertion in CI." % (sk.get("silent_zero_protection"), sk.get("total_skip_sites")),
        "MATRIX OVER-CLAIM: UI-matrix 'pass' is a runtime CSV rebuilt from a one-off interactive run; 236 dialog rows are green on objectName-present-in-.cpp alone, 131 act_* rows green on pipe-fired-no-effect.",
        "VISUAL ORACLE EXISTS BUT SHALLOW: PrintWindow/SSIM/pixel_diff/golden exist but goldens self-seed, thresholds relaxed, and all of it sits behind the gap_a14 skip wall — so mirrored/wrong-zoom canvas, tofu glyphs, wrong icon, garbled dialog, white/half-size edited text stay invisible (the shipped content-stream bug class, one level up from phase-1).",
    ],
    "cross_validation": "Phase-1 found widget-exists-only dominating 77 files; phase-4 confirms it one level up at the E2E/matrix layer AND shows the visual oracles that could catch the real bug class are skip-walled. Phase-3's real mutation survivors (esp. redaction confidentiality) are absent from the standing 3-mutant CI gate.",
}
io.open(os.path.join(HERE, "phase-4-synthesis.json"), "w", encoding="utf-8").write(json.dumps(syn, indent=1))

md = []
md.append("# Phase 4 — UI/E2E & Process/Governance Critique (Synthesis)\n")
md.append("Audit of the native-UI/E2E layer (skip-guard, matrix over-claim, visual-oracle blindness) "
          "and the process/governance layer (gate integrity, GAP-A15, mutation/coverage floors, "
          "flaky/DoD/DCA). FIND-AND-DOCUMENT only.\n")
md.append("## Headline findings\n")
for h in syn["headline_findings"]:
    md.append(f"- {h}")
md.append("")
md.append("## UI/E2E layer\n")
md.append(f"- **Skip sites: {sk.get('total_skip_sites')}** — {sk.get('legitimate_env_guard')} legitimate env-guard, "
          f"**{sk.get('silent_zero_protection')} silent-zero-protection** (green never runs).")
md.append(f"- **~{ut.get('effectively_never_run_in_ci')}** of the UI test surface effectively never runs a behavioral assertion in CI.")
md.append(f"- **Matrix 'pass' = {mx.get('matrix_pass_is_runtime_or_static')}** — over-claimed row classes: {len(mx.get('over_claimed', []))}.")
md.append(f"- **Visual verification exists: {vo.get('any_visual_verification_exists')}** ({', '.join(vo.get('mechanisms_found', []) or [])}) — but shallow (self-seeding goldens, relaxed thresholds, skip-walled).")
md.append(f"  - Invisible regression classes: {'; '.join(vo.get('invisible_regression_classes', []) or [])}.")
md.append("")
md.append("## Process/Governance layer\n")
p = syn["process"]
md.append(f"- **Gate verdict source: {p['gate_verdict_source']}** (forgery allow-list present: {p['forgery_allowlist_present']}) — the reliability fix is real.")
md.append(f"- **GAP-A15 JUnit parsed in CI: {p['gap_a15_junit_parsed_in_ci']}** — defended where CI runs.")
md.append(f"- **Mutation gate: {p['mutation_gate']}** with {p['mutation_mutant_count']} mutants, kill-floor enforced={p['mutation_kill_floor_enforced']} — but green-by-construction vs phase-3's 10 real survivors. **Coverage floor enforced: {p['coverage_floor_enforced']}**.")
md.append(f"- **Flaky policy: {p['flaky_policy']} · DoD: {p['dod']} · DCA loop: {p['dca_loop']}**.")
md.append("")
md.append("## Ranked process findings\n")
sev = {"High": 0, "Med": 1, "Medium": 1, "Low": 2}
for f in sorted(pr.get("findings", []), key=lambda x: sev.get(x.get("severity", ""), 3)):
    md.append(f"- **[{f.get('severity')}] {f.get('area')}** — {f.get('finding')} ({f.get('evidence')})")
md.append("")
md.append("## Cross-validation\n")
md.append(syn["cross_validation"])
io.open(os.path.join(HERE, "phase-4-synthesis.md"), "w", encoding="utf-8").write("\n".join(md))
print("wrote phase-4 synthesis.")
print("UI:", syn["ui"]["total_skip_sites"], "skips /", syn["ui"]["silent_zero_protection"], "zero-protection")
print("process findings:", len(pr.get("findings", [])))
