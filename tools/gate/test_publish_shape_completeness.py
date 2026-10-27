# -*- coding: utf-8 -*-
"""Can check_publish_shape_completeness.py actually go red? (GitHub #254)

This corpus has a documented failure mode where a guard is added, observed to be
green, and never shown to be capable of anything else -- "a guard no mutation can
redden is decoration". A COMPLETENESS guard is especially prone to it, because it
is green by default: it demands a shape only for reference APIs that appear in the
scanned tree, and a clean tree therefore produces the same output as a broken
regex, an aborted walk or an empty reference set.

So each test here drives the guard to a specific verdict from a probe tree it
builds, and asserts on the NAMED finding rather than just the exit code.

One of these checks caught a real defect while being written: the first version of
the instance-spelling shape was `(?<![A-Za-z0-9_])\\.\\s*rename\\s*\\(`, whose
lookbehind can never be satisfied -- a `.` in `f.rename(` is always preceded by
an identifier character. It matched nothing, which the sweep reported as PASS.
test_every_shape_matches_its_own_probe is the check that would have caught it.
"""
from __future__ import annotations

import io
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools" / "gate" / "check_publish_shape_completeness.py"
SWEEP = ROOT / "tools" / "gate" / "check_win87_publish_shapes.py"

sys.path.insert(0, str(ROOT / "tools" / "gate"))
import check_publish_shape_completeness as C  # noqa: E402
import check_win87_publish_shapes as W  # noqa: E402


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-u", str(CHECKER), *args],
        cwd=str(ROOT), capture_output=True, text=True,
        encoding="utf-8", errors="replace")


def probe_tree(tmp_path: Path, name: str, body: str) -> Path:
    src = tmp_path / "probe"
    src.mkdir(parents=True, exist_ok=True)
    with io.open(src / name, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)
    return src


# ---------------------------------------------------------------------------
# 1. The guard is green on the real tree -- the control for everything below.
# ---------------------------------------------------------------------------

def test_green_on_the_real_tree():
    r = run()
    assert r.returncode == 0, (
        "the committed tree should satisfy the completeness guard.\n"
        "stdout:\n%s\nstderr:\n%s" % (r.stdout, r.stderr))
    assert "OK --" in r.stdout


# ---------------------------------------------------------------------------
# 2. COVERAGE -- the property #254 asked for, in the words it asked for them:
#    "adding a new publish call to the tree without adding its shape is
#    detectable".
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("api,call", [
    ("CreateHardLink", "::CreateHardLinkW(b, a, nullptr);"),
    ("CreateSymbolicLink", "::CreateSymbolicLinkW(b, a, 0);"),
    ("SHFileOperation", "::SHFileOperationW(&op);"),
    ("CopyFile2", "::CopyFile2(a, b, nullptr);"),
    ("MoveFileWithProgress", "::MoveFileWithProgressW(a, b, 0, 0, 0);"),
])
def test_uncovered_publish_call_reddens_the_guard(tmp_path, api, call):
    """Each of these is a real publish API that SHAPES does not cover.

    They are in the reference surface precisely so that the day one is used, it
    fails rather than passing silently. If this test ever goes green for an api,
    somebody added the shape -- which is the correct response, and the fix is to
    move that row out of this list, not to delete the assertion.
    """
    src = probe_tree(tmp_path, "Publish.cpp",
                     "#include <windows.h>\nvoid f(const wchar_t* a, "
                     "const wchar_t* b) {\n    %s\n}\n" % call)
    r = run("--root", str(src))
    assert r.returncode == 1, (
        "%s went unreported. stdout:\n%s" % (api, r.stdout))
    assert "UNCOVERED: %s" % api in r.stdout, r.stdout
    # The message has to say what to do, or the next reader argues with it.
    assert "check_win87_publish_shapes.py" in r.stderr


def test_a_covered_publish_call_does_not_redden(tmp_path):
    """The converse. Without this, the test above is satisfied by a guard that
    fails on every input, which proves nothing about coverage."""
    src = probe_tree(tmp_path, "Covered.cpp",
                     "#include <windows.h>\nvoid f(const wchar_t* a, "
                     "const wchar_t* b) {\n"
                     "    ::MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);\n}\n")
    r = run("--root", str(src))
    assert r.returncode == 0, (
        "MoveFileEx IS in SHAPES, so it must not be reported uncovered.\n%s"
        % r.stdout)


def test_a_comment_is_not_a_publish_call(tmp_path):
    """The sweep strips comments and literals before matching; the completeness
    checker reuses that same stripper, and must keep doing so. A reference API
    named in prose is how src/core/AtomicFileReplace.h discusses the shapes it
    replaced -- counting those would make the guard permanently red."""
    src = probe_tree(tmp_path, "Prose.cpp",
                     "// This used to call ::CreateHardLinkW(b, a, nullptr);\n"
                     "const char* s = \"::SHFileOperationW(&op);\";\n")
    r = run("--root", str(src))
    assert r.returncode == 0, (
        "a commented-out or quoted call was counted as real usage:\n%s" % r.stdout)


# ---------------------------------------------------------------------------
# 3. LIVENESS -- a SHAPE that matches nothing in the reference surface.
# ---------------------------------------------------------------------------

def test_a_dead_shape_reddens_the_guard(monkeypatch):
    """A regex that matches nothing is indistinguishable from a covered API, so
    the list has to be checked for entries that have stopped meaning anything.
    This is the check that caught the unsatisfiable-lookbehind shape."""
    import re
    shapes = dict(W.SHAPES)
    shapes["nonexistent::publish"] = re.compile(r"\bthis_matches_no_reference_api\s*\(")
    monkeypatch.setattr(W, "SHAPES", shapes)
    rc = C.main([])
    assert rc == 1, "a shape matching no reference API was accepted as live"


# ---------------------------------------------------------------------------
# 4. The guard's own vacuity -- every assertion above is about the guard's
#    OUTPUT, so a guard whose regexes silently match nothing would satisfy the
#    green tests and the red ones would be the only thing holding it up.
# ---------------------------------------------------------------------------

def test_every_shape_matches_its_own_probe():
    """Each SHAPES entry must match at least one canonical call.

    An entry that matches no probe is either dead or malformed, and a malformed
    one is invisible: the sweep reports PASS whether the regex is precise or
    unsatisfiable. The first instance-spelling shape added for #254 had exactly
    this defect.
    """
    probes = list(C.PROBE.values())
    unmatched = [name for name, rx in W.SHAPES.items()
                 if not any(rx.search(p) for p in probes)]
    assert not unmatched, (
        "these SHAPES match none of the %d canonical probes, so they cannot "
        "distinguish a real publish from no publish: %s"
        % (len(probes), unmatched))


def test_every_reference_api_has_a_probe():
    """A reference entry with no probe can never be found covered, so it would
    read as a permanent gap -- or, worse, the `probe and ...` guard in the
    checker makes it silently uncoverable. Keep the two tables in step."""
    missing = sorted(set(C.REFERENCE) - set(C.PROBE))
    assert not missing, "reference APIs with no probe call: %s" % missing


def test_every_probe_matches_its_own_reference_regex():
    """And the probes have to actually BE calls to the API they are filed
    under, or the coverage computation is comparing unrelated things."""
    import re
    wrong = []
    for name, pattern in C.REFERENCE.items():
        probe = C.PROBE.get(name)
        if probe and not re.compile(pattern).search(probe):
            wrong.append(name)
    assert not wrong, (
        "these reference regexes do not match their own probe call: %s" % wrong)


def test_report_only_entries_are_real_reference_apis():
    """A typo in REPORT_ONLY silently stops excusing anything -- or silently
    excuses nothing while looking like it does."""
    unknown = sorted(C.REPORT_ONLY - set(C.REFERENCE))
    assert not unknown, "REPORT_ONLY names non-reference APIs: %s" % unknown


def test_the_walk_actually_reaches_files(tmp_path):
    """The failure this guards against: a walk that yields nothing reports
    'OK -- every reference publish API used in the tree is covered', which is
    vacuously true of the empty set. --list prints the per-API usage counts, so
    a tree with a known call must show a non-zero one."""
    src = probe_tree(tmp_path, "Seen.cpp",
                     "void f() { ::MoveFileExW(a, b, 0); }\n")
    r = run("--root", str(src), "--list")
    assert r.returncode == 0, r.stderr
    line = [ln for ln in r.stdout.splitlines() if ln.strip().startswith("MoveFileEx ")]
    assert line, "MoveFileEx absent from --list output:\n%s" % r.stdout
    assert "files_using=1" in line[0], (
        "the walk did not see the probe file, so a green verdict would be "
        "vacuous: %s" % line[0])


# ---------------------------------------------------------------------------
# 5. The fix this guard found, pinned. LogHandler's log rotation used
#    QFile::remove + QFile::rename -- delete the previous generation, then
#    publish over it -- which loses the old generation if the publish fails.
# ---------------------------------------------------------------------------

def test_log_rotation_does_not_delete_before_publishing():
    body = (ROOT / "src" / "app" / "LogHandler.cpp").read_text(
        encoding="utf-8", errors="replace")
    stripped = W.strip_comments_and_literals(body)
    assert "atomicReplaceFile(" in stripped, (
        "LogHandler no longer publishes the rotated generation through the "
        "sanctioned atomic primitive")
    assert "QFile::rename" not in stripped, (
        "QFile::rename is back in LogHandler -- it does not replace an existing "
        "destination on Windows, which is what forced the remove-first shape")
    assert "QFile::remove(backup)" not in stripped, (
        "the previous generation is being deleted before the publish that is "
        "meant to replace it; a failed publish then leaves no generation at all")
