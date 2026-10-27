# -*- coding: utf-8 -*-
"""Apply phase-5 gate corrections (karen + validator):
 - karen: add the 2 dropped phase-4 process findings (macOS thin-subset, no flaky
   quarantine ledger) as register entries; make the provenance accounting honest.
 - validator: re-sort the register by severity (Critical->High->Med->Low); close
   the orphan R-34 via SC-a and fix the standing-controls claim; recompute totals.
Regenerates flaws-register.md from the sorted JSON. Docs-only."""
import json, io, os, collections
HERE = os.path.dirname(os.path.abspath(__file__))

reg = json.load(io.open(os.path.join(HERE, "flaws-register.json"), encoding="utf-8"))
entries = reg["register"]

# --- karen: add the two dropped phase-4 process findings ---------------------
new = [
 {"id": "R-36", "phase": "P4", "class": "process",
  "location": "CI: .github/workflows/macos-validation.yml:118-120",
  "description": "The macOS validation lane runs only PreflightOpen.*/PreflightXPlatform.* (a handful of isolation-boundary tests), not the full suite, so one of three advertised platforms provides almost no regression coverage.",
  "exemplar_escape": "A macOS-specific regression outside the preflight subset ships green because the macOS lane never exercises the rest of the suite.",
  "failure_missed": "Any macOS-only defect in the ~4360 tests outside the preflight subset is invisible on the macOS lane.",
  "severity": "Med",
  "evidence": "phase-4 process-critique.json finding 'macOS validation is a thin subset' (macos-validation.yml:118-120)",
  "remediation": "Run the full (or a representative core) suite on the macOS lane, or drop the platform's advertised support claim to match reality."},
 {"id": "R-37", "phase": "P4", "class": "process",
  "location": "CI: census.json:451; .github/workflows/windows-validation.yml:291-304",
  "description": "Rerun-on-failure exists but known-flaky tests (PerfStressTest.TenThousandAnnotationsOnePage, MainWindowIntegrationTest.ZoomComboExists) are tracked only in inline comments — there is no machine-readable quarantine/xfail registry, so a test degrading from reliable to flaky is not a first-class signal and reruns can mask a slowly-worsening test.",
  "exemplar_escape": "A test that silently drifts from reliable to intermittently-failing is absorbed by rerun-on-failure and never flagged as degrading.",
  "failure_missed": "A slowly-worsening test (eventually a real regression) is masked by reruns with no quarantine ledger to surface the trend.",
  "severity": "Low",
  "evidence": "phase-4 process-critique.json finding 'No formal flaky quarantine ledger' (census.json:451; windows-validation.yml:291-304)",
  "remediation": "Add a machine-readable flaky-quarantine/xfail registry with an entry+owner+expiry per known-flaky test; fail CI when an unregistered test flakes."},
]
existing_ids = {e["id"] for e in entries}
for n in new:
    if n["id"] not in existing_ids:
        entries.append(n)

# --- validator: severity-rank the register (stable by numeric ID within tier) -
SEV = {"Critical": 0, "High": 1, "Med": 2, "Medium": 2, "Low": 3}
def idnum(e):
    try: return int(e["id"].split("-")[1])
    except Exception: return 999
entries.sort(key=lambda e: (SEV.get(e.get("severity"), 4), idnum(e)))
reg["register"] = entries

# --- recompute totals + honest provenance note -------------------------------
bysev = collections.Counter(e["severity"] for e in entries)
# normalize Medium->Med for the tally
med = bysev.get("Med", 0) + bysev.get("Medium", 0)
byclass = collections.Counter(e["class"] for e in entries)
reg["totals"]["entries"] = len(entries)
reg["totals"]["by_severity"] = {"Critical": bysev.get("Critical", 0), "High": bysev.get("High", 0),
                                "Med": med, "Low": bysev.get("Low", 0)}
reg["totals"]["by_class"] = dict(byclass)
reg["totals"]["source_findings_consolidated"] = {
    "phase1_ranked_weak_findings": 181, "phase1_weak_oracle_files": 147,
    "phase2_capabilities_happy_path_only": 68, "phase2_hostile_classes_zero": 4,
    "phase3_surviving_mutants": 10, "phase4_process_findings_consolidated": 7,
    "note": ("All 7 phase-4 process-critique findings are now consolidated: CI-trigger-posture->R-25, "
             "mutation-green-by-construction->R-24, DCA-loop-decorative->R-26, coverage-floor-unenforced->R-32, "
             "judgment-gate-honor-system->R-31, macOS-thin-subset->R-36 (added, karen phase-5 gate), "
             "no-flaky-quarantine-ledger->R-37 (added, karen phase-5 gate). R-28 (UI-matrix over-claim) and "
             "R-30 (GAP-A15 historical class, now DEFENDED in CI per phase-4) are additional process-class "
             "register entries from other sources, NOT among the 7 phase-4 process findings — the earlier "
             "'7 process entries == 7 phase-4 findings' equality was the scope-narrowing-behind-a-matching-total "
             "defect karen caught; corrected here."),
}
io.open(os.path.join(HERE, "flaws-register.json"), "w", encoding="utf-8").write(json.dumps(reg, indent=1))

# --- validator: close R-34 via SC-a; wire R-36/R-37 into SC-h ----------------
sc = json.load(io.open(os.path.join(HERE, "standing-controls.json"), encoding="utf-8"))
for c in sc["controls"]:
    if c["id"] == "SC-a" and "R-34" not in c["closes_register_ids"]:
        c["closes_register_ids"].append("R-34")   # adversarial fixture corpus wires orphan fixtures to consumers
    if c["id"] == "SC-h":
        for rid in ("R-36", "R-37"):
            if rid not in c["closes_register_ids"]:
                c["closes_register_ids"].append(rid)
io.open(os.path.join(HERE, "standing-controls.json"), "w", encoding="utf-8").write(json.dumps(sc, indent=1))

# --- regenerate flaws-register.md as a severity-ranked table -----------------
md = []
md.append("# PDFE Test-Harness — Flaws & Gaps Register\n")
md.append("Consolidated, severity-ranked register of the adversarial harness audit "
          "(phases 0-4). FIND-AND-DOCUMENT: no product/test code was changed. Each entry "
          "clusters related prior-phase findings by class; remediation is the separate gated "
          "program in `remediation-plan.json`.\n")
t = reg["totals"]
md.append(f"**{t['entries']} entries** — Critical {t['by_severity']['Critical']}, "
          f"High {t['by_severity']['High']}, Med {t['by_severity']['Med']}, Low {t['by_severity']['Low']}. "
          f"By class: " + ", ".join(f"{k} {v}" for k, v in sorted(t['by_class'].items())) + ".\n")
md.append("Ranked entries (Critical → Low):\n")
md.append("| ID | Sev | Class | Phase | Location | Description | Failure missed | Remediation |")
md.append("|---|---|---|---|---|---|---|---|")
def cell(s): return str(s).replace("|", "\\|").replace("\n", " ").strip()
for e in entries:
    md.append("| {id} | {sev} | {cls} | {ph} | {loc} | {desc} | {fm} | {rem} |".format(
        id=e["id"], sev=e["severity"], cls=e["class"], ph=e["phase"],
        loc=cell(e["location"])[:90], desc=cell(e["description"])[:220],
        fm=cell(e["failure_missed"])[:160], rem=cell(e["remediation"])[:160]))
md.append("\n## Full detail per entry\n")
for e in entries:
    md.append(f"### {e['id']} [{e['severity']}] — {e['class']} ({e['phase']})")
    md.append(f"- **Location:** {e['location']}")
    md.append(f"- **Description:** {e['description']}")
    md.append(f"- **Exemplar escape:** {e['exemplar_escape']}")
    md.append(f"- **Failure missed:** {e['failure_missed']}")
    md.append(f"- **Evidence:** {e['evidence']}")
    md.append(f"- **Remediation:** {e['remediation']}\n")
io.open(os.path.join(HERE, "flaws-register.md"), "w", encoding="utf-8").write("\n".join(md))

print("register entries:", len(entries), "| totals:", reg["totals"]["by_severity"], reg["totals"]["by_class"])
print("severity order OK:", all(SEV.get(entries[i]["severity"], 4) <= SEV.get(entries[i+1]["severity"], 4) for i in range(len(entries)-1)))
print("R-34 in SC-a:", "R-34" in [rid for c in sc["controls"] if c["id"] == "SC-a" for rid in c["closes_register_ids"]])
print("R-36/R-37 in SC-h:", all(rid in [x for c in sc["controls"] if c["id"] == "SC-h" for x in c["closes_register_ids"]] for rid in ("R-36", "R-37")))
