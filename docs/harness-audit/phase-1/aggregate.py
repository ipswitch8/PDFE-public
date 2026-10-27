# -*- coding: utf-8 -*-
"""Phase-1 oracle-audit synthesis: aggregate the 6 cluster scorecards into a
single per-subsystem scorecard + a global severity-ranked weak-oracle list."""
import json, io, os, glob, re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

ORACLE_CATS = ["true", "pseudo", "metamorphic", "heuristic", "proxy", "no-oracle-smoke"]
SMELLS = ["assertion-free", "tautological", "EXPECT_NO_THROW-only",
          "over-tolerant-threshold", "extraction-only-for-visual",
          "widget-exists-only", "reopens-without-error-only"]

# --- smell-string normalization (karen phase-1 gate fix) ---------------------
# The per-file scorecards encode smells with location suffixes
# ("extraction-only-for-visual:659", "reopens-without-error-only@250") and with
# free-text variants. Exact-string matching silently dropped 35-95% of matches.
# Normalize: strip location suffixes, map synonyms to the canonical 7, surface a
# genuine NOVEL smell class explicitly, and separate oracle-category tokens that
# leaked into the smells[] field (they are NOT smells and must not be counted as such).
ORACLE_TOKENS = {"true", "pseudo", "metamorphic", "heuristic", "proxy", "no-oracle-smoke"}
SYNONYM_TO_CANON = {
    "heuristic-threshold": "over-tolerant-threshold",
    "over-tolerant": "over-tolerant-threshold",
    "no-throw-only": "EXPECT_NO_THROW-only",
    "expect_no_throw": "EXPECT_NO_THROW-only",
    "dimension-echo": "tautological",          # asserts echoed inputs -> can't fail
    "state-only-apply": "reopens-without-error-only",
    "counter-only-apply": "reopens-without-error-only",
    "verifier-as-oracle": "tautological",
    "description-string-only": "widget-exists-only",
}
# Novel smell class this audit surfaced but that is NOT one of the original 7.
# Reported as a named 8th+ tally alongside the canonical 7 (never dropped).
NOVEL_CANON = {
    "skips-hide-coverage": "skip-hidden-coverage",
    "gtest-skip-guards": "skip-hidden-coverage",
    "gtest-skip-guard": "skip-hidden-coverage",
    "conditional-skip-on-confidentiality": "skip-hidden-coverage",
    "platform-skip-on-confidentiality": "skip-hidden-coverage",
    "conditional-skip-on-only-real-oracle": "skip-hidden-coverage",
    "hardcoded-private-path-skip": "skip-hidden-coverage",
    "proxy-source-grep": "source-grep-proxy",
    "source-grep-proxy": "source-grep-proxy",
    "no-content-removal-check": "no-removal-check",
    "no-removal-check": "no-removal-check",
    "unasserted-half-of-stated-contract": "partial-contract-assert",
    "weak-positive-detection": "weak-positive-detection",
    "misnamed-test": "misnamed-test",
    "mislabeled-test": "misnamed-test",
}

def normalize_smell(s):
    """Return (kind, canonical) where kind in {'canon','novel','oracle-token','unmapped'}."""
    s = (s or "").strip()
    s = re.sub(r"[@:]\s*[0-9].*$", "", s)      # strip trailing @N / :N location
    s = re.sub(r"\s*\([^)]*\)\s*$", "", s)      # strip trailing (...)
    s = s.strip()
    if s in SMELLS:
        return ("canon", s)
    if s in SYNONYM_TO_CANON:
        return ("canon", SYNONYM_TO_CANON[s])
    if s in NOVEL_CANON:
        return ("novel", NOVEL_CANON[s])
    if s in ORACLE_TOKENS:
        return ("oracle-token", s)
    return ("unmapped", s)

# load census per_file for subsystem mapping
census = json.load(io.open(os.path.join(ROOT, "docs/harness-audit/census.json"), encoding="utf-8"))
file_subsys = {f["file"]: f["subsystem"] for f in census["per_file"]}
file_tests = {f["file"]: f["tests"] for f in census["per_file"]}

card_paths = sorted(glob.glob(os.path.join(HERE, "C*_scorecard.json")))

# --- Pass 0: clean each cluster in place so it is self-consistent -------------
# (karen + validator phase-1 gate findings): strip location suffixes from
# per_file smells, drop oracle-category tokens that leaked into smells[], map
# synonyms to the canonical 7, split out the novel-smell class, dedupe per file,
# then recompute each cluster's own totals.by_smell / totals.by_novel from the
# cleaned per_file and standardize the key name to `by_smell` across all 6.
def clean_cluster(path):
    d = json.load(io.open(path, encoding="utf-8"))
    csm = {s: 0 for s in SMELLS}
    cnov, cleak, cunm = {}, {}, {}
    for pf in d.get("per_file", []):
        # Always normalize from the preserved original so the pass is idempotent.
        if "smells_raw" not in pf:
            pf["smells_raw"] = list(pf.get("smells", []) or [])
        raw = pf.get("smells_raw", []) or []
        canon_set, novel_set = [], []
        seen_c, seen_n = set(), set()
        for sm in raw:
            kind, val = normalize_smell(sm)
            if kind == "canon":
                if val not in seen_c:
                    seen_c.add(val); canon_set.append(val); csm[val] += 1
            elif kind == "novel":
                if val not in seen_n:
                    seen_n.add(val); novel_set.append(val)
                    cnov[val] = cnov.get(val, 0) + 1
            elif kind == "oracle-token":
                cleak[val] = cleak.get(val, 0) + 1
            else:
                cunm[val] = cunm.get(val, 0) + 1
        pf["smells"] = canon_set                   # cleaned canonical smells
        if novel_set:
            pf["novel_smells"] = novel_set
    t = d.setdefault("totals", {})
    t.pop("by_smell_files", None)                  # standardize key name
    t["by_smell"] = csm
    if cnov:
        t["by_novel"] = cnov
    if cleak:
        t["oracle_token_leak_removed_from_smells"] = cleak
    if cunm:
        t["unmapped_smell_tokens"] = cunm
    io.open(path, "w", encoding="utf-8").write(json.dumps(d, indent=1))
    return d

cards = [clean_cluster(p) for p in card_paths]

# ---- global oracle + smell totals (by file and by test) ----
global_oracle_files = {c: 0 for c in ORACLE_CATS}
global_oracle_tests = {c: 0 for c in ORACLE_CATS}
global_smell_files = {s: 0 for s in SMELLS}
global_novel_files = {}          # novel-smell -> file count
global_unmapped = {}             # unmapped raw token -> count (must be empty/surfaced, never silent)
oracle_token_leak = {}           # oracle-category token seen in smells[] -> count (reported, not a smell)
per_subsys = {}   # subsystem -> {files, tests, by_oracle_files, by_oracle_tests, by_smell}

def blank_sub():
    return {"files": 0, "tests": 0,
            "by_oracle_files": {c: 0 for c in ORACLE_CATS},
            "by_oracle_tests": {c: 0 for c in ORACLE_CATS},
            "by_smell_files": {s: 0 for s in SMELLS},
            "by_novel_files": {}}

missing_subsys = set()
# novel-smell + leakage tallies come from the cleaned cluster totals (Pass 0
# already moved them out of per_file.smells), so aggregate them from there.
for c in cards:
    t = c.get("totals", {})
    for k, v in (t.get("by_novel", {}) or {}).items():
        global_novel_files[k] = global_novel_files.get(k, 0) + v
    for k, v in (t.get("oracle_token_leak_removed_from_smells", {}) or {}).items():
        oracle_token_leak[k] = oracle_token_leak.get(k, 0) + v
    for k, v in (t.get("unmapped_smell_tokens", {}) or {}).items():
        global_unmapped[k] = global_unmapped.get(k, 0) + v
for c in cards:
    for pf in c["per_file"]:
        fn = pf["file"].replace("\\", "/")
        # normalize path to census key
        key = fn if fn in file_subsys else ("tests/" + os.path.basename(fn))
        sub = file_subsys.get(key, file_subsys.get("tests/" + os.path.basename(fn)))
        if sub is None:
            missing_subsys.add(fn); sub = pf.get("subsystem", "other/uncategorized")
        oracle = pf.get("oracle", "proxy")
        if oracle not in ORACLE_CATS:
            oracle = "proxy"
        ntests = file_tests.get(key, pf.get("tests", 0)) or 0
        global_oracle_files[oracle] += 1
        global_oracle_tests[oracle] += ntests
        s = per_subsys.setdefault(sub, blank_sub())
        s["files"] += 1; s["tests"] += ntests
        s["by_oracle_files"][oracle] += 1
        s["by_oracle_tests"][oracle] += ntests
        seen_canon, seen_novel = set(), set()   # dedupe per file (files-exhibiting count)
        for sm in pf.get("smells", []) or []:
            kind, canon = normalize_smell(sm)
            if kind == "canon":
                if canon not in seen_canon:
                    seen_canon.add(canon)
                    global_smell_files[canon] += 1
                    s["by_smell_files"][canon] += 1
            elif kind == "novel":
                if canon not in seen_novel:
                    seen_novel.add(canon)
                    global_novel_files[canon] = global_novel_files.get(canon, 0) + 1
                    s["by_novel_files"][canon] = s["by_novel_files"].get(canon, 0) + 1
            elif kind == "oracle-token":
                oracle_token_leak[canon] = oracle_token_leak.get(canon, 0) + 1
            else:
                global_unmapped[canon] = global_unmapped.get(canon, 0) + 1

# ---- merged severity-ranked findings ----
SEV_RANK = {"critical": 0, "high": 1, "med": 2, "medium": 2, "low": 3, "": 4}
def norm_sev(s):
    s = (s or "").strip().lower()
    if s in ("medium",): return "Med"
    return {"critical": "Critical", "high": "High", "med": "Med",
            "low": "Low", "": "unrated"}.get(s, s.capitalize() or "unrated")
findings = []
for c in cards:
    cl = c["cluster"]
    for f in c.get("ranked_weak_findings", []):
        f = dict(f); f["cluster"] = cl
        f["severity"] = norm_sev(f.get("severity"))
        findings.append(f)
findings.sort(key=lambda f: (SEV_RANK.get(f.get("severity", "").lower(), 4),
                             0 if f.get("confidence") == "confirmed" else 1))

sev_counts = {}
for f in findings:
    sev = f.get("severity", "unrated") or "unrated"
    sev_counts[sev] = sev_counts.get(sev, 0) + 1

total_files = sum(s["files"] for s in per_subsys.values())
total_tests = sum(s["tests"] for s in per_subsys.values())
weak_files = global_oracle_files["proxy"] + global_oracle_files["no-oracle-smoke"]
weak_tests = global_oracle_tests["proxy"] + global_oracle_tests["no-oracle-smoke"]

synthesis = {
    "phase": "phase-1-oracle-audit",
    "generated_note": "Synthesis of 6 cluster scorecards (C1-C6) covering all 402 C++ test files. FIND-AND-DOCUMENT only; no product/test code modified.",
    "totals": {
        "files": total_files, "tests": total_tests,
        "by_oracle_files": global_oracle_files,
        "by_oracle_tests": global_oracle_tests,
        "by_smell_files": global_smell_files,
        "by_novel_smell_files": global_novel_files,
        "oracle_token_leak_excluded_from_smells": oracle_token_leak,
        "unmapped_smell_tokens": global_unmapped,
        "weak_oracle_files_proxy_plus_smoke": weak_files,
        "weak_oracle_files_pct": round(100.0 * weak_files / total_files, 1),
        "weak_oracle_tests_proxy_plus_smoke": weak_tests,
        "weak_oracle_tests_pct": round(100.0 * weak_tests / total_tests, 1) if total_tests else 0,
    },
    "per_subsystem": per_subsys,
    "findings_severity_counts": sev_counts,
    "ranked_weak_findings": findings,
    "notes": {
        "missing_subsys_paths": sorted(missing_subsys),
        "systemic_pattern": "extraction-only-for-visual is the dominant cross-cluster disease (content-edit/TRE/fonts/redaction/image): a visual feature verified only by text-extraction round-trip, structurally blind to mirror/invisible/white/wrong-glyph/half-size/off-page — the exact class that shipped the mirror/half-size/color-loss content-stream defects.",
        "tautological_pattern": "Multiple comment-labelled 'critical'/'regression-lock' tests re-implement the production formula in-test and assert against the copy, never invoking the product code path (Y-flip coord mapper, width-scaling, CID CMap loop, tile/HiDPI math) -> a revert of the real fix cannot fail them.",
        "security_theatre": "cve_replay + malicious-PDF hostile-JS tests share a no-throw / wall-clock-timing oracle around a catch-all; signature trust-chain / DocMDP / OCSP-CRL rejections are masked by permanently-untrusted or self-signed fixtures; a live operator-precedence bug (&b==0) can no-op a tamper test.",
        "conditionality_escape": "The strongest redaction byte/pixel-absence oracles GTEST_SKIP under secret-fixture absence / on Windows and STILL report green -> silent degradation to mark-count proxies; box-only redaction leaving Tj operators in the saved stream would ship undetected.",
    },
}

io.open(os.path.join(HERE, "oracle-audit-synthesis.json"), "w", encoding="utf-8").write(
    json.dumps(synthesis, indent=1))

# ---- markdown ----
def bar(n, tot):
    return "" if not tot else "#" * int(round(20.0 * n / tot))

md = []
md.append("# Phase 1 — Oracle Audit Synthesis\n")
md.append("Adversarial classification of **every** C++ test file's oracle strength "
          "across 6 subsystem clusters (C1–C6). The harness is the defendant: a test "
          "counts only if it asserts a user-visible truth AND fails when that truth breaks. "
          "FIND-AND-DOCUMENT only — no product or test code was modified.\n")
md.append(f"## Global totals — {total_files} files / {total_tests} tests\n")
md.append("### Oracle strength (by file)\n")
md.append("| Oracle category | Files | Tests |")
md.append("|---|---:|---:|")
for c in ORACLE_CATS:
    md.append(f"| {c} | {global_oracle_files[c]} | {global_oracle_tests[c]} |")
md.append(f"\n**Weak-oracle (proxy + no-oracle-smoke): {weak_files}/{total_files} files "
          f"({synthesis['totals']['weak_oracle_files_pct']}%), "
          f"{weak_tests}/{total_tests} tests "
          f"({synthesis['totals']['weak_oracle_tests_pct']}%)** — i.e. roughly a third of "
          "the suite asserts something merely *correlated* with correctness, not correctness itself.\n")
md.append("### Test-smell prevalence (files exhibiting)\n")
md.append("| Smell (canonical 7) | Files |")
md.append("|---|---:|")
for s in sorted(SMELLS, key=lambda x: -global_smell_files[x]):
    md.append(f"| {s} | {global_smell_files[s]} |")
md.append("")
if global_novel_files:
    md.append("### Novel smell class surfaced by this audit (not one of the original 7)\n")
    md.append("| Novel smell | Files |")
    md.append("|---|---:|")
    for s in sorted(global_novel_files, key=lambda x: -global_novel_files[x]):
        md.append(f"| {s} | {global_novel_files[s]} |")
    md.append("\n`skip-hidden-coverage` = an oracle that is real but GTEST_SKIP-gated on a "
              "fixture/platform, so it silently degrades to a weaker proxy (or zero coverage) "
              "and still reports green; `source-grep-proxy` = asserts by grepping source/decoded "
              "text rather than the rendered/saved artifact. These are the conditionality escapes.\n")
if oracle_token_leak:
    md.append(f"*Note: {sum(oracle_token_leak.values())} oracle-category tokens "
              f"({', '.join(sorted(oracle_token_leak))}) had leaked into per-file `smells[]` lists "
              "in C3/C5; they are excluded from smell counts (they are oracle categories, not smells) "
              "and the cluster files were cleaned.*\n")
if global_unmapped:
    md.append(f"*WARNING: {len(global_unmapped)} unmapped smell tokens remain: {global_unmapped}*\n")
else:
    md.append("*All raw smell tokens mapped to a canonical or named-novel smell — zero silently dropped.*\n")
md.append("## Per-subsystem scorecard\n")
md.append("| Subsystem | Files | Tests | true | pseudo | meta | heur | **proxy** | **smoke** | weak% (files) |")
md.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for sub in sorted(per_subsys, key=lambda k: -(per_subsys[k]["by_oracle_files"]["proxy"] + per_subsys[k]["by_oracle_files"]["no-oracle-smoke"])):
    s = per_subsys[sub]; o = s["by_oracle_files"]
    w = o["proxy"] + o["no-oracle-smoke"]
    pct = round(100.0 * w / s["files"], 0) if s["files"] else 0
    md.append(f"| {sub} | {s['files']} | {s['tests']} | {o['true']} | {o['pseudo']} | "
              f"{o['metamorphic']} | {o['heuristic']} | {o['proxy']} | {o['no-oracle-smoke']} | {pct:.0f}% |")
md.append("")
md.append("## Findings by severity\n")
for sev in ["Critical", "High", "Med", "Medium", "Low", "unrated"]:
    if sev in sev_counts:
        md.append(f"- **{sev}: {sev_counts[sev]}**")
md.append(f"\nTotal ranked weak-oracle findings: **{len(findings)}**\n")
md.append("## Top ranked weak-oracle findings (Critical + High)\n")
md.append("Each names the specific test and the concrete user-visible failure it would miss.\n")
n = 0
for f in findings:
    if f.get("severity") not in ("Critical", "High"):
        continue
    n += 1
    if n > 45:
        break
    loc = f.get("file", "?")
    ln = f.get("line", "")
    loc = f"{loc}:{ln}" if ln else loc
    md.append(f"**{n}. [{f.get('severity')}] `{loc}` — {f.get('test','')}** "
              f"({f.get('cluster','')}, oracle={f.get('oracle','')}, "
              f"smells={','.join(f.get('smells',[]) or [])}, conf={f.get('confidence','')})  ")
    md.append(f"   Misses: {f.get('user_visible_failure_missed','')}\n")
md.append("## Systemic patterns\n")
for k, v in synthesis["notes"].items():
    if k == "missing_subsys_paths":
        continue
    md.append(f"- **{k}**: {v}\n")
io.open(os.path.join(HERE, "oracle-audit-synthesis.md"), "w", encoding="utf-8").write("\n".join(md))
print("wrote synthesis. weak files:", weak_files, "/", total_files,
      "| weak tests:", weak_tests, "/", total_tests)
print("severity counts:", sev_counts)
print("missing subsys paths:", sorted(missing_subsys)[:10])
print("total findings:", len(findings))
