#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Revert-and-observe for WIN-87 phase-56, acceptance criterion 5.

WHAT IT DOES
------------
Reverts Document::save()'s full-rewrite branch (WIN-87 site #1) to the
pre-phase-56, PATH-BASED primitive -- write by path, flush by path-reopen,
publish by MoveFileExW-by-path -- rebuilds, and runs the phase-56 hostile
filesystem tests against it. The tests are discriminating only if they go RED
there and GREEN again after the restore.

WHY IT VERIFIES THE REBUILD INSTEAD OF ASSUMING IT
--------------------------------------------------
Earlier in this pipeline a revert-and-observe run produced a complete false
finding (F69): shutil.copy2 preserved the source mtime, MSBuild concluded the
object file was up to date and skipped the compile, and five deterministic runs
measured code that was never built. So every build here is checked twice:

  * the restored/patched source is force-touched (os.utime to now), AND
  * Document.obj's mtime must be strictly newer after the build than before,
    AND the build log must contain a compile line for Document.cpp.

If either check fails the script ABORTS rather than reporting a measurement.
"""

import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src" / "core" / "Document.cpp"
OBJ = REPO / "build" / "vs2022" / "src" / "core" / "pdfe_core.dir" / "Debug" / "Document.obj"
EXE = REPO / "build" / "vs2022" / "bin" / "Debug" / "pdfe_tests.exe"
CMAKE = Path(
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
    r"\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

# The tests that must be discriminating. Site #1 only: criterion 5 asks for at
# least one site, and reverting one site keeps the patch small enough to be
# auditable by reading it.
FILTER = "FullRewriteSaveTocttou.*"

# (find, replace) pairs turning the phase-56 code back into the phase-55 code.
# Each `find` must occur EXACTLY once, or the script aborts -- a silently
# unapplied hunk is how a revert-and-observe measures the fixed code and calls
# the test decorative.
REVERT = [
    (
        """    std::error_code stageEc;
    std::unique_ptr<StagingFile> staging =
        StagingFile::createFullRewriteTempFor(path, stageEc);
    if (!staging) {
        throw PdfeException(
            "Could not create the staging file for " + path.string()
            + " (original left intact): " + stageEc.message());
    }
""",
        """    const auto tempPath = fullRewriteTempPathFor(path);
    const std::string tempPathStr = mupdfPathUtf8(tempPath);
    struct RevertTempGuard {
        const std::filesystem::path* p;
        bool armed = true;
        void disarm() { armed = false; }
        ~RevertTempGuard() {
            if (!armed || !p) return;
            std::error_code e;
            std::filesystem::remove(*p, e);
        }
    } tempGuard{&tempPath};
""",
    ),
    (
        """        writeFullRewriteThroughStagingHandle(*pImpl->context, pImpl->pdfDoc,
                                             *staging, opts);
""",
        """        pImpl->context->execute([this, &tempPathStr, &opts](fz_context* ctx) {
            pdf_save_document(ctx, pImpl->pdfDoc, tempPathStr.c_str(), &opts);
        });
""",
    ),
    (
        """    {
        std::error_code flushEc;
        staging->flushToStableStorage(flushEc);  // best-effort, as before
    }
""",
        """    flushFileToStableStorage(tempPath);
""",
    ),
    (
        """    const bool replaced = staging->commitOverWithRetry(path, ec);
""",
        """    const bool replaced = atomicReplaceFileWithRetry(tempPath, path, ec);
""",
    ),
    (
        """    // The replace succeeded: the staging file no longer exists under its own
    // name (it IS `path` now) and commitOver has already marked itself
    // committed, so the destructor removes nothing.
""",
        """    tempGuard.disarm();
""",
    ),
]


def run(cmd, log_path=None, timeout=3600):
    proc = subprocess.run(cmd, cwd=str(REPO), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)
    out = (proc.stdout or "") + (proc.stderr or "")
    if log_path:
        io.open(log_path, "w", encoding="utf-8").write(out)
    return proc.returncode, out


def build_and_verify(tag, outdir):
    """Build pdfe_tests and PROVE Document.cpp was actually recompiled."""
    before = OBJ.stat().st_mtime if OBJ.exists() else 0.0
    # Force-touch: shutil.copy2 preserves mtime, and MSBuild would then skip.
    now = time.time()
    os.utime(SRC, (now, now))
    log = outdir / f"build_{tag}.log"
    rc, out = run([str(CMAKE), "--build", str(REPO / "build" / "vs2022"),
                   "--config", "Debug", "--target", "pdfe_tests"], log)
    if rc != 0:
        sys.exit(f"BUILD FAILED ({tag}); see {log}")
    after = OBJ.stat().st_mtime if OBJ.exists() else 0.0
    compiled = "Document.cpp" in out
    if not (after > before and compiled):
        sys.exit(
            f"ABORT: the {tag} build did not recompile Document.cpp "
            f"(obj mtime {before} -> {after}, compile line seen: {compiled}). "
            "Measuring now would repeat the F69 false finding."
        )
    return {"obj_mtime_before": before, "obj_mtime_after": after,
            "compile_line_seen": compiled, "log": str(log)}


def run_tests(tag, outdir):
    xml = outdir / f"gtest_{tag}.xml"
    rc, out = run([str(EXE), f"--gtest_filter={FILTER}",
                   f"--gtest_output=xml:{xml}"], outdir / f"tests_{tag}.log")
    import xml.etree.ElementTree as ET
    tree = ET.parse(str(xml))
    root = tree.getroot()
    total = int(root.get("tests", 0))
    failures = int(root.get("failures", 0))
    errors = int(root.get("errors", 0))
    disabled = int(root.get("disabled", 0))
    failed_names = []
    for case in root.iter("testcase"):
        if case.find("failure") is not None:
            failed_names.append(f"{case.get('classname')}.{case.get('name')}")
    return {"exit_code": rc, "tests": total, "failures": failures,
            "errors": errors, "disabled": disabled,
            "failed": failed_names, "xml": str(xml)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default=str(REPO / "build" / "p56_revert"))
    args = ap.parse_args()
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    original = io.open(SRC, encoding="utf-8", newline="").read()
    backup = outdir / "Document.cpp.orig"
    io.open(backup, "w", encoding="utf-8", newline="").write(original)

    # core.autocrlf=true, so the working-tree file is CRLF while the hunks
    # above are written LF. Normalise the HUNKS to the file, never the file to
    # the hunks: rewriting the whole file's line endings would show up as a
    # 3000-line diff if the restore ever failed.
    crlf = "\r\n" in original
    if crlf:
        REVERT[:] = [(f.replace("\n", "\r\n"), r.replace("\n", "\r\n"))
                     for f, r in REVERT]

    patched = original
    for find, replace in REVERT:
        n = patched.count(find)
        if n != 1:
            sys.exit(f"ABORT: revert hunk matched {n} times, expected 1:\n"
                     f"{find[:120]}…")
        patched = patched.replace(find, replace)

    result = {"filter": FILTER}
    try:
        io.open(SRC, "w", encoding="utf-8", newline="").write(patched)
        result["reverted_build"] = build_and_verify("reverted", outdir)
        result["reverted_tests"] = run_tests("reverted", outdir)
    finally:
        io.open(SRC, "w", encoding="utf-8", newline="").write(original)

    result["restored_build"] = build_and_verify("restored", outdir)
    result["restored_tests"] = run_tests("restored", outdir)

    rev = result["reverted_tests"]
    res = result["restored_tests"]
    result["discriminating"] = (rev["failures"] + rev["errors"] > 0
                                and res["failures"] + res["errors"] == 0)
    io.open(outdir / "phase56_revert_and_observe.json", "w",
            encoding="utf-8").write(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    if not result["discriminating"]:
        sys.exit("NOT DISCRIMINATING — the tests do not distinguish the "
                 "path-based primitive from the handle-based one.")


if __name__ == "__main__":
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    main()
