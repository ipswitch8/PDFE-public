# -*- coding: utf-8 -*-
"""Phase-3 C++ mutation-testing harness for PDFE.

For each mutant in mutants.json: apply a single precise source edit, incrementally
rebuild pdfe_tests, run the mutant's gtest filter, classify KILLED (a targeted test
failed) vs SURVIVED (all green), then ALWAYS revert via `git checkout`. A baseline
run confirms each filter is green before mutation. Product-code safety: the working
tree is asserted clean before the next mutant; a failed revert aborts the whole run.

Usage:
  python docs/harness-audit/phase-3/run_mutations.py            # all mutants
  python docs/harness-audit/phase-3/run_mutations.py --baseline # baseline filters only
  python docs/harness-audit/phase-3/run_mutations.py --only M01,M05
"""
import json, io, os, sys, subprocess, xml.etree.ElementTree as ET, time

ROOT = r"C:\Users\dev\source\PDFE"
HERE = os.path.join(ROOT, "docs", "harness-audit", "phase-3")
CMAKE = r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
BUILD_DIR = os.path.join(ROOT, "build", "vs2022")
TEST_EXE = os.path.join(BUILD_DIR, "bin", "Debug", "pdfe_tests.exe")
LOGDIR = os.path.join(HERE, "runs")
os.makedirs(LOGDIR, exist_ok=True)

def run(cmd, cwd=ROOT, timeout=1800):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       shell=False, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace")

def git_clean_for(path):
    rc, out = run(["git", "status", "--porcelain", "--", path])
    return out.strip() == ""

def git_revert(path):
    run(["git", "checkout", "--", path])

def build():
    t0 = time.time()
    rc, out = run([CMAKE, "--build", BUILD_DIR, "--config", "Debug",
                   "--target", "pdfe_tests"], timeout=2400)
    return rc, out, round(time.time() - t0, 1)

def run_filter(gfilter, tag):
    xml = os.path.join(LOGDIR, "res_%s.xml" % tag)
    if os.path.exists(xml):
        os.remove(xml)
    rc, out = run([TEST_EXE, "--gtest_filter=" + gfilter,
                   "--gtest_output=xml:" + xml], timeout=1800)
    io.open(os.path.join(LOGDIR, "log_%s.txt" % tag), "w", encoding="utf-8").write(out)
    tests = fails = errors = skipped = 0
    parsed = False
    if os.path.exists(xml):
        try:
            r = ET.parse(xml).getroot()
            tests = int(r.get("tests", 0)); fails = int(r.get("failures", 0))
            errors = int(r.get("errors", 0)); skipped = int(r.get("disabled", 0))
            parsed = True
        except Exception:
            pass
    return {"exit": rc, "tests": tests, "failures": fails, "errors": errors,
            "parsed_xml": parsed, "xml": xml, "ran_nonzero": tests > 0}

def apply_mutant(m):
    """Return (ok, message). Applies find->replace, requires exactly 1 occurrence."""
    fp = os.path.join(ROOT, m["file"].replace("/", os.sep))
    src = io.open(fp, encoding="utf-8").read()
    n = src.count(m["find"])
    if n != 1:
        return False, "anchor count=%d (need exactly 1)" % n
    io.open(fp, "w", encoding="utf-8").write(src.replace(m["find"], m["replace"], 1))
    return True, "applied"

def main():
    args = sys.argv[1:]
    baseline_only = "--baseline" in args
    only = None
    for a in args:
        if a.startswith("--only"):
            only = set((a.split("=", 1)[1] if "=" in a else args[args.index(a) + 1]).split(","))
    cat = json.load(io.open(os.path.join(HERE, "mutants.json"), encoding="utf-8"))
    mutants = cat["mutants"]
    if only:
        mutants = [m for m in mutants if m["id"].split("-")[0] in only or m["id"] in only]

    # Pre-flight: tree must be clean for every file we will touch.
    files = sorted(set(m["file"] for m in mutants))
    for f in files:
        if not git_clean_for(f):
            print("ABORT: working tree not clean for", f); sys.exit(2)

    results = {"baseline": {}, "mutants": [], "started": time.strftime("%Y-%m-%d %H:%M:%S")}

    # Baseline: run each distinct filter once, unmutated — must be green.
    filters = sorted(set(m["test_filter"] for m in mutants))
    print("== BASELINE: %d distinct filters ==" % len(filters))
    for i, gf in enumerate(filters):
        r = run_filter(gf, "baseline_%02d" % i)
        results["baseline"][gf] = r
        status = "GREEN" if (r["ran_nonzero"] and r["failures"] == 0 and r["errors"] == 0) else "NOT-GREEN"
        print("  [%s] tests=%d fail=%d  %s" % (status, r["tests"], r["failures"], gf[:70]))
    io.open(os.path.join(HERE, "mutation-results.json"), "w", encoding="utf-8").write(json.dumps(results, indent=1))
    if baseline_only:
        print("baseline-only done."); return

    # Mutants
    print("== MUTANTS: %d ==" % len(mutants))
    for idx, m in enumerate(mutants):
        tag = m["id"].replace("/", "_")
        rec = {"id": m["id"], "subsystem": m.get("subsystem"), "file": m["file"],
               "prediction": m.get("prediction"), "test_filter": m["test_filter"]}
        print("[%d/%d] %s (predict %s)" % (idx + 1, len(mutants), m["id"], m.get("prediction")))
        try:
            ok, msg = apply_mutant(m)
            if not ok:
                rec["result"] = "ERROR-ANCHOR"; rec["detail"] = msg
                print("   ANCHOR-FAIL:", msg); results["mutants"].append(rec); continue
            brc, bout, bsec = build()
            rec["build_seconds"] = bsec
            if brc != 0:
                rec["result"] = "BUILD-FAIL"
                io.open(os.path.join(LOGDIR, "build_%s.txt" % tag), "w", encoding="utf-8").write(bout[-8000:])
                print("   BUILD-FAIL (%ss)" % bsec); results["mutants"].append(rec); continue
            rr = run_filter(m["test_filter"], tag)
            rec["run"] = rr
            killed = rr["ran_nonzero"] and (rr["failures"] > 0 or rr["errors"] > 0)
            rec["result"] = "KILLED" if killed else ("SURVIVED" if rr["ran_nonzero"] else "NO-TESTS-RAN")
            rec["matches_prediction"] = (rec["result"].lower().startswith(str(m.get("prediction", "")).lower()[:6]))
            print("   %s  tests=%d fail=%d (build %ss)" % (rec["result"], rr["tests"], rr["failures"], bsec))
        finally:
            git_revert(m["file"])
            if not git_clean_for(m["file"]):
                rec["result"] = rec.get("result", "?") + "+REVERT-FAILED"
                results["mutants"].append(rec)
                io.open(os.path.join(HERE, "mutation-results.json"), "w", encoding="utf-8").write(json.dumps(results, indent=1))
                print("   ABORT: revert failed for", m["file"]); sys.exit(3)
        results["mutants"].append(rec)
        io.open(os.path.join(HERE, "mutation-results.json"), "w", encoding="utf-8").write(json.dumps(results, indent=1))

    results["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    # summary
    k = sum(1 for r in results["mutants"] if r.get("result") == "KILLED")
    s = sum(1 for r in results["mutants"] if r.get("result") == "SURVIVED")
    other = [r for r in results["mutants"] if r.get("result") not in ("KILLED", "SURVIVED")]
    results["summary"] = {"killed": k, "survived": s, "other": len(other),
                          "kill_rate": round(100.0 * k / max(1, k + s), 1)}
    io.open(os.path.join(HERE, "mutation-results.json"), "w", encoding="utf-8").write(json.dumps(results, indent=1))
    print("== DONE: killed=%d survived=%d other=%d kill_rate=%s%% ==" % (k, s, len(other), results["summary"]["kill_rate"]))

if __name__ == "__main__":
    main()
