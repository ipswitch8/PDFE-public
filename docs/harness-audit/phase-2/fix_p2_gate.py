# -*- coding: utf-8 -*-
"""Apply phase-2 gate corrections (karen + validator findings), then recompute
rtm totals from the actual array. Docs-only."""
import json, io, os, collections
HERE = os.path.dirname(os.path.abspath(__file__))
p = os.path.join(HERE, "rtm.json")
d = json.load(io.open(p, encoding="utf-8"))
caps = d["capabilities"]

# --- karen fix: Permission flags row over-claimed the gap -------------------
for c in caps:
    if c.get("capability", "").startswith("Permission flags"):
        c["coverage_class"] = "has-negative-or-boundary"
        c["oracle_note"] = (
            "CORRECTED (karen phase-2 gate): test_permission_enforcement.cpp DOES contain genuine "
            "behavioral-enforcement negatives that invoke product code and assert refusal, at both "
            "the engine boundary (PlainTextExporterRefusesCopyDenied:215 asserts write() returns false "
            "AND no output file created; PrintEngineRefusesPrintDeniedWithValidPrinter:257 asserts "
            "print() returns false before rasterization) and the UI-slot boundary "
            "(OnPrintRefuses*/OnExportPlainTextRefusesCopyDenied/OnCopyRefusesCopyDeniedEvenIfToggleCleared/"
            "TextEditCopyCutRefusesCopyDenied:312-385 drive real MainWindow slots and assert refusal). "
            "These are mutation-killable. RESIDUAL (narrower) gap: the flag-only check "
            "test_encryption_roundtrip.cpp::CopyFalsePermissionEnforcedOnReopen:410 only reads copyAllowed(); "
            "and saveEncrypted's own re-encryption path / a hypothetical NEW downstream consumer that ignores "
            "PermissionFlags are not covered by an enforcement test. Main surface is covered; residual is narrow."
        )
        c["user_impact"] = "Med"

# --- karen fix: propagate to uncovered_ranked -------------------------------
for u in d.get("uncovered_ranked", []):
    if u.get("capability", "").startswith("Permission enforcement"):
        u["capability"] = "Permission enforcement — residual (re-encryption path / new downstream consumers)"
        u["coverage_class"] = "has-negative-or-boundary (main surface covered; residual narrow)"
        u["user_impact"] = "Med"
        u["why"] = (
            "CORRECTED (karen phase-2 gate): the main copy/print restriction surface IS enforced by real "
            "behavioral tests in test_permission_enforcement.cpp (engine + UI refusal, mutation-killable). "
            "Residual gap is narrow: test_encryption_roundtrip.cpp::CopyFalsePermissionEnforcedOnReopen is "
            "flag-read-only, and saveEncrypted's re-encryption path / future downstream consumers ignoring "
            "PermissionFlags have no enforcement test. Re-scoped from High to Med; not a rewrite-existing-tests item."
        )
# re-sort uncovered_ranked by impact (High, Med, Low)
PRI = {"High": 0, "Med": 1, "Medium": 1, "Low": 2, "": 3}
d["uncovered_ranked"].sort(key=lambda u: PRI.get(u.get("user_impact", ""), 3))

# --- validator fix: recompute totals from the actual capabilities array ------
dist = collections.Counter(c.get("coverage_class") for c in caps)
d["totals"] = {
    "capabilities": len(caps),
    "none": dist.get("none", 0),
    "happy_path_only": dist.get("happy-path-only", 0),
    "has_negative_or_boundary": dist.get("has-negative-or-boundary", 0),
    "strong_visual": dist.get("strong-with-visual-oracle", 0),
    "note": (
        "%d enumerated user-facing capability rows (some rows bundle sub-features). coverage_class distribution "
        "recomputed from the actual array (validator phase-2 gate fix): strong-with-visual-oracle=%d (redaction, "
        "and even that is conditionally GTEST_SKIP-gated); has-negative-or-boundary=%d; happy-path-only=%d; none=%d "
        "(every shipped feature has at least one test file -- the gap is oracle STRENGTH not test EXISTENCE). "
        "Roadmap/out-of-scope items (public-key encryption, scanner input, Office/HTML import, Read Out Loud, "
        "spell-check, reading-order editor, Portfolio, Action-wizard) are NOT counted as gaps (unimplemented by "
        "design). Phase-1 classified 147/402 files (36.6%%) / 1712/4365 tests (39.2%%) weak-oracle; this RTM "
        "localizes that weakness onto VISUAL and SECURITY-ENFORCEMENT capabilities."
        % (len(caps), dist.get("strong-with-visual-oracle", 0), dist.get("has-negative-or-boundary", 0),
           dist.get("happy-path-only", 0), dist.get("none", 0))
    ),
}
io.open(p, "w", encoding="utf-8").write(json.dumps(d, indent=1))
assert (d["totals"]["none"] + d["totals"]["happy_path_only"]
        + d["totals"]["has_negative_or_boundary"] + d["totals"]["strong_visual"]) == len(caps), "totals must sum"
print("rtm.json fixed. totals:", {k: v for k, v in d["totals"].items() if k != "note"})
print("sum check OK ==", len(caps))
