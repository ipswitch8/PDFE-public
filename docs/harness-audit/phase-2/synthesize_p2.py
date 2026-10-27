# -*- coding: utf-8 -*-
"""Phase-2 synthesis: stitch RTM + fixture-catalog + hostile-input-matrix into a
single headline view + consolidated adversarial-fixture backlog. Docs-only."""
import json, io, os

HERE = os.path.dirname(os.path.abspath(__file__))
def load(n): return json.load(io.open(os.path.join(HERE, n), encoding="utf-8"))
rtm = load("rtm.json"); fix = load("fixture-catalog.json"); hos = load("hostile-input-matrix.json")

rt = rtm["totals"]; ft = fix["totals"]; ht = hos["totals"]

# Consolidated, priority-sorted adversarial-fixture backlog (hostile-input backlog
# is the spine; annotate items that a zero/weak RTM row or danger-zone path corroborates).
PRI = {"High": 0, "Med": 1, "Medium": 1, "Low": 2, "": 3}
backlog = sorted(hos.get("backlog", []), key=lambda b: PRI.get(b.get("priority", ""), 3))

synthesis = {
    "phase": "phase-2-rtm-and-input-domain",
    "generated_note": "Synthesis of rtm.json + fixture-catalog.json + hostile-input-matrix.json. FIND-AND-DOCUMENT; no product/test code changed.",
    "rtm_totals": rt,
    "fixture_totals": ft,
    "hostile_input_totals": ht,
    "headline": {
        "capabilities": rt.get("capabilities"),
        "coverage_none": rt.get("none"),
        "coverage_happy_path_only": rt.get("happy_path_only"),
        "coverage_has_negative_or_boundary": rt.get("has_negative_or_boundary"),
        "coverage_strong_visual": rt.get("strong_visual"),
        "central_finding": "none=0: every shipped feature has >=1 test file; the gap is oracle STRENGTH not test EXISTENCE. Only %s of %s capabilities have a strong visual/artifact oracle; %s are happy-path-only. This localizes Phase-1's 36.6%%-weak-oracle result onto the visual + security-enforcement capabilities where a user or attacker would actually notice." % (
            rt.get("strong_visual"), rt.get("capabilities"), rt.get("happy_path_only")),
        "fixtures_benign": ft.get("benign"),
        "fixtures_adversarial": ft.get("adversarial"),
        "fixtures_orphan": ft.get("orphan"),
        "hostile_classes_zero": ht.get("zero"),
        "hostile_classes_partial": ht.get("partial"),
        "hostile_classes_nonzero": ht.get("nonzero"),
    },
    "zero_coverage_hostile_classes": hos.get("zero_coverage_classes", []),
    "top_uncovered_capabilities": rtm.get("uncovered_ranked", [])[:12],
    "consolidated_adversarial_fixture_backlog": backlog,
    "cross_validation": [
        "RTM none=0 + strong_visual=1 corroborates Phase-1 (147/402 files weak-oracle); the weakness is concentrated on visual + security-enforcement features.",
        "Fixture danger-zones (paths fed ONLY benign inputs: OCR/TRE, scanned-render, PDF/A tagging, save/rewrite) match the hostile-input PARTIAL/ZERO classes and the RTM happy-path-only rows.",
        "Sheared/skewed-CTM = ZERO fixture coverage is the direct untested sibling of the -100 Tz mirror bug this audit exists for; the adversarial-ambient corpus has half-SCALE cm but no shear.",
        "flaw_corpus (47 adversarial fixtures) all feed ONE open-only smoke test -> adversarial breadth exists but adversarial DEPTH per product-path is thin (an open-parse pass is not a save/redact/render pass).",
    ],
}
io.open(os.path.join(HERE, "phase-2-synthesis.json"), "w", encoding="utf-8").write(json.dumps(synthesis, indent=1))

md = []
md.append("# Phase 2 — RTM & Input-Domain Adversarial-Fixture Audit (Synthesis)\n")
md.append("Two-way requirements traceability + fixture benign/adversarial catalog + "
          "hostile-input EP/BVA/FMEA coverage matrix. FIND-AND-DOCUMENT only — no product/test code changed.\n")
md.append("## Headline\n")
md.append(f"- **RTM: {rt.get('capabilities')} user-facing capabilities** — "
          f"none=**{rt.get('none')}**, happy-path-only=**{rt.get('happy_path_only')}**, "
          f"has-negative/boundary=**{rt.get('has_negative_or_boundary')}**, "
          f"strong-with-visual-oracle=**{rt.get('strong_visual')}**.")
md.append(f"  - **Central finding:** every feature has a test; the gap is oracle *strength*, not test *existence*. "
          f"Only {rt.get('strong_visual')}/{rt.get('capabilities')} capabilities carry a strong visual/artifact oracle. "
          "This is Phase-1's weak-oracle result localized onto exactly the visual + security-enforcement features a user/attacker would notice.")
md.append(f"- **Fixtures: {ft.get('fixtures')}** — {ft.get('benign')} benign / {ft.get('adversarial')} adversarial; "
          f"{ft.get('orphan')} orphan (consumed by no test), {ft.get('missing_referenced')} referenced-but-missing. "
          f"Prior on-disk inventory ~60% stale ({fix.get('reconciliation_vs_prior_inventory','').strip()[:80]}...).")
md.append(f"- **Hostile-input classes: {ht.get('classes')}** — {ht.get('nonzero')} nonzero, "
          f"{ht.get('partial')} partial, **{ht.get('zero')} zero-coverage**.")
md.append("")
md.append("## Zero-coverage hostile-input classes (ranked by risk)\n")
for c in hos.get("zero_coverage_classes", []):
    md.append(f"- **{c}**")
md.append("")
md.append("## Top uncovered / weakly-covered user-visible capabilities\n")
for i, u in enumerate(rtm.get("uncovered_ranked", [])[:12], 1):
    md.append(f"{i}. **[{u.get('user_impact','')}] {u.get('capability','')}** "
              f"(coverage={u.get('coverage_class','')}) — {u.get('why','')}")
md.append("")
md.append("## Consolidated adversarial-fixture backlog (priority-ordered)\n")
md.append("| Pri | Class | Fixture to add | Product path | Oracle | Failure it would catch |")
md.append("|---|---|---|---|---|---|")
for b in backlog:
    md.append(f"| {b.get('priority','')} | {b.get('class','')} | `{b.get('fixture_to_add','')}` | "
              f"{b.get('product_path','')} | {b.get('oracle','')} | {b.get('failure_caught','')} |")
md.append("")
md.append("## Cross-validation with Phase-1\n")
for x in synthesis["cross_validation"]:
    md.append(f"- {x}")
io.open(os.path.join(HERE, "phase-2-synthesis.md"), "w", encoding="utf-8").write("\n".join(md))
print("wrote phase-2 synthesis.")
print("RTM totals:", rt)
print("fixture totals:", ft)
print("hostile totals:", ht)
print("backlog items:", len(backlog))
