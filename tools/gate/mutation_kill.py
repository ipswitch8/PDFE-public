#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mutation_kill.py -- targeted C++ mutation harness for PDFE.

The audit (docs/audit/census-and-mutation-feasibility.md) established that a
*targeted* mutant harness is feasible for this codebase even though a full
mutation run of a C++/Qt project is not: pick a specific source edit that
inverts a real behavior, rebuild the one affected target, run the test that is
supposed to catch it, and assert the test now FAILS. A test that still passes
against the mutated code is a false-green oracle (a "surviving mutant").

kill-rate = killed_mutants / total_mutants.

This script is the reusable engine behind the CI `mutation-kill-rate` job and
the local Phase-5 floor. It is deliberately conservative: each mutant is a
literal search/replace in one file, applied, built, tested, then REVERTED
(the original file is always restored, even on error/interrupt).

Manifest schema (JSON):
{
  "build": {
    "cmake": "<path to cmake.exe>",
    "build_preset": "debug-vs2022",
    "test_exe": "build/vs2022/bin/Debug/pdfe_tests.exe"
  },
  "mutants": [
    {
      "id": "M2-coordmapper-rot90",
      "file": "src/core/CoordinateTransform.cpp",
      "find": "<exact source substring>",
      "replace": "<mutated substring>",
      "target": "pdfe_tests",             # cmake target to rebuild
      "killing_test": "CoordMapperTest.RoundTripRotation90"  # gtest filter
    }
  ]
}

A mutant is KILLED iff, after applying it and rebuilding, the killing_test
FAILS (or the build breaks -- a mutant that won't compile is trivially killed
and reported as such). It SURVIVES iff the killing_test still passes.

Usage:
  python tools/gate/mutation_kill.py --manifest tools/gate/mutants.json \
      [--floor 1.0] [--dry-run] [--json-out mutation-report.json] \
      [--sweep-json manifest-sweep.json]

--dry-run validates the manifest and confirms every `find` string is present
exactly once in its file WITHOUT building (fast, safe, no toolchain needed).

That validation IS the manifest staleness sweep (F31/R-24): sweep_manifest()
enumerates every mutant, locates its find-string in the cited source file and
reports a PASS/FAIL row per mutant; --sweep-json persists those rows. There is
deliberately no separate sweep script -- a second checker could drift from the
one CI runs, and the whole point of the sweep is that it cannot.

Exit 0 = kill-rate >= floor (or dry-run OK); 1 = below floor / survivor;
2 = manifest or setup error.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys


def _read(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def _write(path, text, newline=""):
    with open(path, "w", encoding="utf-8", newline=newline) as fh:
        fh.write(text)


def _detect_newline(path):
    """Return the line ending the file on disk actually uses.

    F31: `_read` opens in universal-newline mode, so a CRLF source file arrives
    as "\\n"-joined text. Writing it straight back with newline="" rewrote every
    tracked source file this harness touched to LF -- invisible to `git diff`
    under core.autocrlf=true (git normalises on read) but a real, unrequested
    change to the worktree bytes, i.e. mutation residue that survives the
    revert. Detect and restore the original ending instead.
    """
    with open(path, "rb") as fh:
        raw = fh.read()
    return "\r\n" if b"\r\n" in raw else "\n"


def sweep_manifest(repo, mutants):
    """Mechanical staleness sweep over every mutant in a manifest.

    F31 (R-24): this IS the manifest staleness sweep -- there is deliberately no
    second, separate sweep script, because a duplicate checker can drift from
    the one CI actually runs. Every mutant is enumerated, its find-string is
    located in the cited source file, and a per-mutant PASS/FAIL row is
    returned. `--dry-run` reports these rows and `--sweep-json` persists them as
    evidence.

    Returns a list of dicts: {id, file, status ("PASS"/"FAIL"), detail}.
    """
    rows = []
    for m in mutants:
        ok, msg = validate_mutant(repo, m)
        rows.append({
            "id": m.get("id", "?"),
            "file": m.get("file", ""),
            "status": "PASS" if ok else "FAIL",
            "detail": msg,
        })
    return rows


def validate_mutant(repo, m):
    """Return (ok, message). Confirms find-string occurs exactly once."""
    fpath = os.path.join(repo, m["file"])
    if not os.path.isfile(fpath):
        return False, "file not found: %s" % m["file"]
    src = _read(fpath)
    n = src.count(m["find"])
    if n == 0:
        return False, "find-string absent in %s (mutant stale?)" % m["file"]
    if n > 1:
        return False, "find-string ambiguous in %s (%d matches)" % (m["file"], n)
    return True, "ok"


# Where cmake actually lives on a stock Windows dev box for this project. The
# repo's own instructions use this full path because plain `cmake` is NOT on
# PATH there; VS ships it inside the IDE tree and never adds it.
_VS_CMAKE = (
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
    r"\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")


def resolve_cmake(configured):
    """Resolve the cmake executable, falling back rather than failing obscurely.

    WHY THIS EXISTS (karen gate, phase 9). `build()` used the manifest's
    ``cmake`` value verbatim. The test manifest says ``"cmake"``, and on a stock
    Windows dev box that is not on PATH, so ``subprocess.run`` raised
    ``FileNotFoundError: [WinError 2] The system cannot find the file
    specified`` -- three tests in ``test_mutation_kill_floor.py`` failing with an
    error naming no file at all.

    That is not a cosmetic annoyance. It made
    ``tools/gate/run_fast_checks_locally.py`` -- the DCA 0051 control whose whole
    point is that "the guards are green" becomes checkable -- report a failure
    on a correct tree unless the operator had already put cmake on PATH by hand.
    A control that only works after undocumented environment setup is one people
    stop running, which is precisely the failure mode DCA 0047 records.

    Order: an explicit PDFE_CMAKE override, then the configured value if it
    resolves, then the VS-bundled path this project documents, then the bare
    name so the original error surfaces on a machine where none of these apply.
    """
    override = os.environ.get("PDFE_CMAKE")
    if override and (os.path.isfile(override) or shutil.which(override)):
        return override
    if configured:
        if os.path.isfile(configured):
            return configured
        found = shutil.which(configured)
        if found:
            return found
    if os.path.isfile(_VS_CMAKE):
        return _VS_CMAKE
    return configured or "cmake"


class ToolchainError(RuntimeError):
    """cmake could not be LAUNCHED -- a setup failure, not a build result.

    THIS DISTINCTION IS LOAD-BEARING, and getting it wrong is worse than the
    problem it was introduced to fix. `apply_and_test` reads any non-zero build
    exit as "mutant did not compile (trivially killed)" -- correct, because a
    mutant that breaks the compile really is killed. But a cmake that cannot be
    launched at all returns non-zero for EVERY mutant, so the harness would
    report a 100% kill rate having built and tested nothing, and the floor gate
    would pass vacuously.

    That is precisely the false-green-oracle class this harness exists to
    detect, reproduced inside the detector. So a launch failure raises instead,
    and `main` turns it into exit 2 ("manifest or setup error"), which the
    module docstring already reserves.
    """


def build(build_cfg, target):
    cmake = resolve_cmake(build_cfg.get("cmake"))
    preset = build_cfg["build_preset"]
    cmd = [cmake, "--build", "--preset", preset, "--target", target, "--parallel", "4"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as e:
        # Name the executable. The bare OSError did not, which is why the
        # original failure read as a mystery rather than "cmake is not here".
        raise ToolchainError(
            "could not launch cmake (%r): %s\n"
            "Set PDFE_CMAKE to the cmake executable, or put it on PATH. "
            "This is a SETUP failure -- it is deliberately not reported as a "
            "killed mutant, because that would make the kill-rate vacuously "
            "100%%." % (cmake, e)) from e
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def run_test(test_exe, gtest_filter):
    if not os.path.isfile(test_exe):
        return None, "test exe not found: %s" % test_exe
    cmd = [test_exe, "--gtest_filter=%s" % gtest_filter, "--gtest_brief=1"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def apply_and_test(repo, build_cfg, m):
    """Apply one mutant, build, run killing_test, always revert.
    Returns dict with keys: id, killed(bool), reason."""
    fpath = os.path.join(repo, m["file"])
    original = _read(fpath)
    # Preserve the file's real line endings across apply+revert (see
    # _detect_newline): otherwise the "always restore" guarantee restores the
    # CONTENT but not the BYTES.
    newline = _detect_newline(fpath)
    mutated = original.replace(m["find"], m["replace"], 1)
    result = {"id": m["id"], "killed": False, "reason": ""}
    try:
        _write(fpath, mutated, newline)
        bcode, blog = build(build_cfg, m.get("target", "pdfe_tests"))
        if bcode != 0:
            result["killed"] = True
            result["reason"] = "mutant did not compile (trivially killed)"
            return result
        test_exe = os.path.join(repo, build_cfg["test_exe"])
        tcode, tlog = run_test(test_exe, m["killing_test"])
        if tcode is None:
            result["reason"] = tlog
            return result
        if tcode != 0:
            result["killed"] = True
            result["reason"] = "killing_test FAILED against mutant (mutant killed)"
        else:
            result["killed"] = False
            result["reason"] = ("SURVIVOR: killing_test still passed against mutated "
                                "code -- the test does not actually catch this defect")
    finally:
        _write(fpath, original, newline)  # ALWAYS restore, byte-for-byte
        # F69(b) IS NOT CHECKED HERE, AND THE REASON IS WORTH RECORDING.
        #
        # An earlier version asserted `binary_postdates_sources` right here.
        # That check ALWAYS FIRED, on healthy runs and pathological ones alike,
        # because at this instant it is asking a question with only one possible
        # answer: the binary was built BEFORE the restore, and `_write()` above
        # stamps a fresh mtime on the source, so the source is newer by
        # construction. A guard that can only fail is as useless as one that can
        # never fail, and it appended a false "[F69b] STALE BINARY" to every
        # mutant record. The `karen` gate caught it.
        #
        # The property only becomes a real question once a rebuild has been
        # ATTEMPTED since the restore, so it is checked after the mutant loop
        # instead — see `_assert_tree_rebuilt_after_restore` in main().
    return result


def _assert_tree_rebuilt_after_restore(repo, build_cfg, touched_files):
    """F69(b) — the tree this harness LEAVES BEHIND must be self-consistent.

    Every mutant restores its own source in a `finally`, so the sources are
    correct when the loop ends. The binary is not: the last thing built was the
    final mutant. Anything run afterwards against that binary measures a tree
    that does not exist — source saying one thing, binary another.

    So rebuild once from the restored sources and then assert the artifact
    actually postdates them. That is the moment the question has two possible
    answers, which is what makes it a check rather than noise.

    Reported, not raised: this is a diagnostic about the tree left behind, and
    it must not change the mutation verdict the caller is about to return.
    """
    try:
        from rebuild_assert import binary_postdates_sources
    except Exception as exc:
        print("[mutation-kill] rebuild assertion unavailable: %s" % exc,
              flush=True)
        return True

    print("\n[mutation-kill] rebuilding from the restored sources (F69b) ...",
          flush=True)
    bcode, _ = build(build_cfg, build_cfg.get("target", "pdfe_tests"))
    if bcode != 0:
        print("[mutation-kill] REBUILD FAILED after restore -- the tree left "
              "behind does not compile, which is worse than a stale binary.",
              flush=True)
        return False

    exe = os.path.join(repo, build_cfg["test_exe"])
    ok, msg = binary_postdates_sources(exe, sorted(set(touched_files)))
    print("[mutation-kill] %s" % msg, flush=True)
    return ok


def main(argv):
    ap = argparse.ArgumentParser(description="Targeted C++ mutation harness for PDFE.")
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--floor", type=float, default=1.0,
                    help="minimum acceptable kill-rate (default 1.0 = all killed)")
    ap.add_argument("--dry-run", action="store_true",
                    help="validate manifest + find-strings only; no build/test")
    ap.add_argument("--json-out", default="")
    ap.add_argument("--sweep-json", default="",
                    help="write the per-mutant staleness-sweep rows (PASS/FAIL "
                         "per mutant) to this path as JSON evidence")
    args = ap.parse_args(argv)

    repo = os.getcwd()
    try:
        manifest = json.loads(_read(args.manifest))
    except (OSError, ValueError) as exc:
        print("manifest error: %s" % exc)
        return 2

    mutants = manifest.get("mutants", [])
    if not mutants:
        print("manifest has no mutants")
        return 2

    # Always sweep first: per-mutant PASS/FAIL for the find-string anchor.
    rows = sweep_manifest(repo, mutants)
    print("staleness sweep: %d mutant(s) in %s" % (len(rows), args.manifest))
    for r in rows:
        print("  %-4s %-40s %s" % (r["status"], r["id"],
                                   r["file"] if r["status"] == "PASS" else r["detail"]))
    passed = sum(1 for r in rows if r["status"] == "PASS")
    print("  sweep result: %d PASS / %d FAIL" % (passed, len(rows) - passed))
    if args.sweep_json:
        _write(args.sweep_json, json.dumps(
            {"manifest": args.manifest, "total": len(rows), "passed": passed,
             "failed": len(rows) - passed, "mutants": rows}, indent=2) + "\n")

    stale = [(r["id"], r["detail"]) for r in rows if r["status"] == "FAIL"]
    if stale:
        print("MANIFEST VALIDATION FAILED:")
        for mid, msg in stale:
            print("  %-32s %s" % (mid, msg))
        return 2
    print("manifest: %d mutant(s), all find-strings present exactly once." % len(mutants))

    if args.dry_run:
        print("DRY RUN OK (no build/test performed).")
        return 0

    if "build" not in manifest:
        print("manifest missing 'build' section; cannot build/test.")
        return 2

    results = []
    for m in mutants:
        print("\n=== mutant %s (%s) ===" % (m["id"], m["file"]))
        r = apply_and_test(repo, manifest["build"], m)
        status = "KILLED " if r["killed"] else "SURVIVED"
        print("  %s -- %s" % (status, r["reason"]))
        results.append(r)

    # F69(b): the sources are restored but the binary is still the LAST MUTANT's.
    # Rebuild from the restored tree and prove the artifact followed, so nothing
    # downstream measures a half-mutated binary. See DCA 0045.
    _assert_tree_rebuilt_after_restore(
        repo, manifest["build"],
        [os.path.join(repo, m["file"]) for m in mutants])

    killed = sum(1 for r in results if r["killed"])
    rate = killed / len(results)
    print("\n================ mutation summary ================")
    print("  killed %d / %d   kill-rate = %.3f   floor = %.3f" %
          (killed, len(results), rate, args.floor))
    survivors = [r["id"] for r in results if not r["killed"]]
    if survivors:
        print("  SURVIVORS: " + ", ".join(survivors))

    if args.json_out:
        _write(args.json_out, json.dumps(
            {"killed": killed, "total": len(results), "kill_rate": rate,
             "floor": args.floor, "results": results}, indent=2))

    return 0 if rate >= args.floor else 1


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except ToolchainError as e:
        # Exit 2 = setup error, per the module docstring. Deliberately
        # NOT 0 and NOT a kill-rate verdict: the harness established
        # nothing, and saying so is the whole point.
        print("[mutation-kill] SETUP ERROR: %s" % e, file=sys.stderr)
        sys.exit(2)
