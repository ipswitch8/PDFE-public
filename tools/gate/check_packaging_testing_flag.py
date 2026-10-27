#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Every packaging path must configure PDFE_TESTING=OFF (GitHub #182).

WHAT THIS CLOSES
================
``PDFE_TESTING`` gates the test-only introspection seams (``PDFE_TEST_PIPES``):
a hidden pipe that activates any tool by ``objectName``, and a readback of a
dialog's whole child inventory.  GitHub #55 added the flag so a shipped binary
would not carry that control channel -- any local process that can set one
environment variable gets to drive the UI.

GitHub #182 found the mitigation had never once applied.  ``option(PDFE_TESTING
... ON)`` defaults ON, and *no build path in the repository ever passed it OFF*
-- not a preset, not a workflow, not ``scripts/sign/Build-Release-Signed.ps1``,
which is the script that produced the signed v0.2.0 installer.  The only
occurrence of the flag in the off position was a COMMENT.  The installer was
packaged out of ``build/vs2022``: the very tree ``pdfe_tests.exe`` is configured
for, and therefore the one tree guaranteed to have the seam compiled in.

WHY A CHECKER AND NOT JUST THE FIX
==================================
The fix is a preset and three call-site edits.  The drift is invisible: nothing
about a PDFE_TESTING=ON binary looks different, no test fails, the installer
installs and runs, and the flag reads as an implementation detail to anyone
adding a packaging step later.  The failure mode is somebody wiring up a new
release job, or a `-BuildDir build/vs2022`, and shipping the seam again --
which is exactly what happened between #55 and #182 with the fix *already
written down in a comment*.  So the guard is the deliverable; the flag is the
easy part.

DERIVED, NOT HARDCODED
======================
The packaging preset's name and binary directory are read out of
``CMakePresets.json`` (the unique configure preset that sets PDFE_TESTING OFF),
not baked in here.  Renaming the tree therefore updates every gate at once
instead of silently making them vacuous.

GATES
=====
G1  ``CMakePresets.json`` declares EXACTLY ONE configure preset that sets
    ``PDFE_TESTING`` OFF, and its ``binaryDir`` is not the standard build tree.
    Zero means no OFF path exists (the #182 state).  Two or more means "the
    packaging tree" is ambiguous and every gate below would be guessing.
G2  ``CMakeLists.txt`` still declares ``option(PDFE_TESTING ... ON)``.  The
    default MUST stay ON -- ``pdfe_tests.exe`` needs the seams -- so a flip to
    OFF is a silent loss of the test seams, and deleting the option makes every
    other gate here moot.
G3  The ordinary build preset does NOT pin ``PDFE_TESTING``.  If it ever
    acquires ``ON`` explicitly, someone has begun treating the two trees as
    interchangeable.
G4  Every ``cpack ... --config <...>CPackConfig.cmake`` in tracked text reads
    that config from the packaging tree.
G5  No tracked packaging-relevant path (``CPackConfig.cmake``, ``installer/``,
    ``_CPack_Packages``) is rooted in the standard build tree.  This is the
    catch-all for a new packaging site that G4's ``cpack`` pattern misses.
G6  ``scripts/sign/Build-Release-Signed.ps1`` -- the script that actually
    produces signed releases -- defaults ``$BuildDir`` to the packaging tree AND
    carries a runtime refusal that reads ``PDFE_TESTING:BOOL=`` out of that
    tree's ``CMakeCache.txt`` and throws unless it is OFF.  The default alone is
    not enough: ``-BuildDir build/vs2022`` would walk straight past it.  The
    refusal reads the CACHE rather than trusting the preset name, because the
    preset is a claim about how a tree was configured and the cache is the tree.
G7  ``.github/workflows/windows-validation.yml`` configures the packaging preset
    before packaging, verifies ``PDFE_TESTING:BOOL=`` from the resulting cache,
    and gates the installer step on that step's outcome.
G8  ``scripts/verify-installer-deps.ps1`` -- the clean-machine smoke test --
    defaults to the packaging tree's installer, so it exercises the bundle that
    actually ships.

Usage::

    python tools/gate/check_packaging_testing_flag.py [--repo-root .]
    python tools/gate/check_packaging_testing_flag.py --self-test
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import subprocess
import sys

TAG = "[packaging-testing-flag]"

CMAKELISTS = "CMakeLists.txt"
PRESETS = "CMakePresets.json"
SIGN_SCRIPT = "scripts/sign/Build-Release-Signed.ps1"
SMOKE_SCRIPT = "scripts/verify-installer-deps.ps1"
WORKFLOW = ".github/workflows/windows-validation.yml"

# The tree pdfe_tests.exe is configured for. Packaging must never come from it.
STANDARD_TREE = "build/vs2022"

# Dated audit records quote historical commands verbatim; rewriting them to
# quiet a sweep falsifies history (the F32/F39/F40/F43 precedent). They are
# excluded from the text sweeps, never from the live-path gates above.
SWEEP_EXCLUDE_PREFIXES = (
    "docs/audit/",
    "tests/_tmp/",
    # Pinned BEFORE-copies of workflows, kept verbatim so other checkers can
    # test themselves against the shape they were written to catch. Rewriting
    # them would break those checkers and falsify the record of what the
    # workflow looked like.
    "tools/gate/fixtures/",
)

# Dated observations that quote the OLD packaging location because that is
# where the artifact was at the time. LIVE instructions get rewritten; a record
# of "verified on 2026-08-01 against <path>" does not, because rewriting it
# would make the record false. Same LIVE/HISTORICAL split the F43 citation
# checker enforces. A file that is NOT in this map and NOT rewritten FAILS --
# the classification has to be made, not defaulted.
HISTORICAL_RECORDS = {
    "docs/security/unsigned-binary-risk-acceptance.md":
        "dated 2026-08-01 signature verification, and a dated observation "
        "contrasting the stale root-directory installer with the CPack one; "
        "both are records of what was true then, when packaging did come out "
        "of build/vs2022.",
}
# Extensions a packaging command could plausibly live in. Deliberately NOT
# .json/.txt/.xml: those are dominated in this repo by multi-megabyte captured
# artifacts and registers, which cannot invoke cpack and whose bulk turned this
# checker into a minute-long scan. The files the gates read by name
# (CMakePresets.json, CMakeLists.txt) are loaded explicitly regardless of this
# list, so narrowing it cannot make a named gate vacuous.
SWEEP_EXTENSIONS = (".yml", ".yaml", ".ps1", ".psm1", ".md", ".bat", ".cmd",
                    ".sh", ".py", ".cmake")

# A packaging invocation is a command line. Nothing over this is one, and the
# repo carries generated documents far larger.
MAX_SWEEP_BYTES = 1024 * 1024

# This file itself quotes the forbidden strings while explaining them.
SWEEP_EXCLUDE_FILES = (
    "tools/gate/check_packaging_testing_flag.py",
    "tools/gate/test_check_packaging_testing_flag.py",
)


# ---------------------------------------------------------------------------
# reading
# ---------------------------------------------------------------------------

def read_text(path):
    with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def tracked_files(repo_root):
    """Tracked text files worth sweeping. git ls-files, so an untracked scratch
    copy of a workflow cannot fail the gate and a NEW file cannot dodge it."""
    try:
        out = subprocess.check_output(
            ["git", "-C", repo_root, "ls-files"],
            stderr=subprocess.DEVNULL).decode("utf-8", "replace")
    except Exception:
        return []
    keep = []
    for rel in out.splitlines():
        rel = rel.strip().replace("\\", "/")
        if not rel or rel in SWEEP_EXCLUDE_FILES:
            continue
        if rel.startswith(SWEEP_EXCLUDE_PREFIXES):
            continue
        if not rel.endswith(SWEEP_EXTENSIONS):
            continue
        keep.append(rel)
    return keep


def load_repo(repo_root):
    """path -> text for everything the gates read."""
    files = {}
    for rel in set(tracked_files(repo_root)) | {
            CMAKELISTS, PRESETS, SIGN_SCRIPT, SMOKE_SCRIPT, WORKFLOW}:
        full = os.path.join(repo_root, rel.replace("/", os.sep))
        if not os.path.isfile(full):
            continue
        if os.path.getsize(full) > MAX_SWEEP_BYTES and rel not in (
                CMAKELISTS, PRESETS, SIGN_SCRIPT, SMOKE_SCRIPT, WORKFLOW):
            continue
        files[rel] = read_text(full)
    return files


# ---------------------------------------------------------------------------
# gates
# ---------------------------------------------------------------------------

def _preset_flag(preset):
    """The preset's PDFE_TESTING setting as 'ON'/'OFF'/None. CMakePresets allows
    either a bare string or a {type,value} object; both appear in this repo."""
    cv = preset.get("cacheVariables") or {}
    if "PDFE_TESTING" not in cv:
        return None
    v = cv["PDFE_TESTING"]
    if isinstance(v, dict):
        v = v.get("value", "")
    return str(v).strip().upper()


def _norm_dir(binary_dir):
    d = str(binary_dir or "").replace("\\", "/")
    d = d.replace("${sourceDir}/", "")
    return d.rstrip("/")


def find_packaging_preset(files, problems):
    """G1. Returns (name, dir) or (None, None)."""
    raw = files.get(PRESETS)
    if raw is None:
        problems.append("G1 %s is missing" % PRESETS)
        return None, None
    try:
        doc = json.loads(raw)
    except ValueError as exc:
        problems.append("G1 %s is not valid JSON: %s" % (PRESETS, exc))
        return None, None

    off = []
    for p in doc.get("configurePresets", []):
        if _preset_flag(p) == "OFF":
            off.append((p.get("name", "<unnamed>"), _norm_dir(p.get("binaryDir"))))

    if not off:
        problems.append(
            "G1 no configure preset in %s sets PDFE_TESTING=OFF. That is the "
            "GitHub #182 state exactly: the flag defaults ON and nothing turns "
            "it off, so every packaged installer carries the PDFE_TEST_PIPES "
            "control channel." % PRESETS)
        return None, None
    if len(off) > 1:
        problems.append(
            "G1 %d configure presets set PDFE_TESTING=OFF (%s). 'The packaging "
            "tree' must be unambiguous or every gate below is guessing which "
            "one it means." % (len(off), ", ".join(n for n, _ in off)))
        return None, None

    name, bdir = off[0]
    if not bdir:
        problems.append("G1 packaging preset '%s' declares no binaryDir" % name)
        return name, None
    if bdir == STANDARD_TREE:
        problems.append(
            "G1 packaging preset '%s' uses binaryDir %s -- the SAME tree "
            "pdfe_tests.exe is configured for. PDFE_TESTING is configure-scoped, "
            "so one tree cannot hold both postures; the OFF build needs its own "
            "binaryDir." % (name, bdir))
        return name, None
    return name, bdir


def check_option_default_on(files, problems):
    """G2."""
    raw = files.get(CMAKELISTS)
    if raw is None:
        problems.append("G2 %s is missing" % CMAKELISTS)
        return
    m = re.search(r"option\s*(\()\s*PDFE_TESTING\b", raw)
    if not m:
        problems.append(
            "G2 %s no longer declares option(PDFE_TESTING ...). Without the "
            "option there is no seam gate at all and every gate here is moot."
            % CMAKELISTS)
        return
    # Scan to the MATCHING close paren, tracking depth. A lazy `.*?\)` stops at
    # the first ')' -- which in this declaration sits inside the help string
    # ("...seams (PDFE_TEST_PIPES). Turn OFF..."), so it captured a body with no
    # ON/OFF in it and reported a false failure. Caught by running the checker
    # against the real tree before wiring it up.
    depth = 0
    end = None
    for i in range(m.start(1), len(raw)):
        ch = raw[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is None:
        problems.append("G2 option(PDFE_TESTING ...) is unterminated in %s"
                        % CMAKELISTS)
        return
    body = raw[m.end():end]
    # The help string legitimately contains the words ON/OFF; the DEFAULT is
    # the bare token outside any quotes. Strip quoted spans first.
    body = re.sub(r'"(?:[^"\\]|\\.)*"', " ", body, flags=re.S)
    # Last bare word before the closing paren is the default.
    words = re.findall(r"\b(ON|OFF)\b", body)
    if not words:
        problems.append("G2 option(PDFE_TESTING ...) has no ON/OFF default")
        return
    if words[-1] != "ON":
        problems.append(
            "G2 option(PDFE_TESTING ...) now defaults %s. It must stay ON: "
            "pdfe_tests.exe needs the introspection seams compiled in, and a "
            "silent flip to OFF would disable the UI-dialog test layer without "
            "failing anything. The OFF posture belongs to the packaging preset, "
            "not to the default." % words[-1])


def check_standard_preset_unpinned(files, problems, packaging_preset):
    """G3."""
    raw = files.get(PRESETS)
    if raw is None:
        return
    try:
        doc = json.loads(raw)
    except ValueError:
        return
    for p in doc.get("configurePresets", []):
        name = p.get("name", "")
        if name == packaging_preset:
            continue
        if _norm_dir(p.get("binaryDir")) != STANDARD_TREE:
            continue
        flag = _preset_flag(p)
        if flag is not None:
            problems.append(
                "G3 preset '%s' (binaryDir %s) pins PDFE_TESTING=%s. The "
                "standard tree must ride the option() default; pinning it here "
                "is how the two trees start being treated as interchangeable."
                % (name, STANDARD_TREE, flag))


CPACK_CONFIG_RE = re.compile(
    r"cpack\b[^\n]*?--config\s+\"?([^\s\"']+CPackConfig\.cmake)", re.I)


def check_cpack_sites(files, problems, packaging_dir):
    """G4."""
    if not packaging_dir:
        return
    pkg = packaging_dir.lower()
    for rel in sorted(files):
        for m in CPACK_CONFIG_RE.finditer(files[rel]):
            cfg = m.group(1).replace("\\", "/").lower()
            if pkg not in cfg:
                problems.append(
                    "G4 %s invokes cpack with --config %s, which is not under "
                    "the packaging tree %s. A tree configured PDFE_TESTING=ON "
                    "packages the test-seam control channel into the installer "
                    "(GitHub #182)." % (rel, m.group(1), packaging_dir))


FORBIDDEN_PACKAGING_PATHS = (
    STANDARD_TREE + "/CPackConfig.cmake",
    STANDARD_TREE + "/installer",
    STANDARD_TREE + "/_CPack_Packages",
)


def check_no_packaging_from_standard_tree(files, problems):
    """G5 -- catch-all for a packaging site G4's cpack pattern would miss."""
    for rel in sorted(files):
        if rel in HISTORICAL_RECORDS:
            continue
        text = files[rel].replace("\\", "/")
        low = text.lower()
        for bad in FORBIDDEN_PACKAGING_PATHS:
            if bad.lower() in low:
                problems.append(
                    "G5 %s references %s. Packaging artifacts must come from "
                    "the PDFE_TESTING=OFF tree only; %s is the test tree."
                    % (rel, bad, STANDARD_TREE))


def check_sign_script(files, problems, packaging_dir):
    """G6."""
    raw = files.get(SIGN_SCRIPT)
    if raw is None:
        problems.append("G6 %s is missing" % SIGN_SCRIPT)
        return
    m = re.search(r"\[string\]\s*\$BuildDir\s*=\s*[\"']([^\"']+)[\"']", raw)
    if not m:
        problems.append("G6 %s has no literal $BuildDir default to check"
                        % SIGN_SCRIPT)
    elif packaging_dir and _norm_dir(m.group(1)) != packaging_dir:
        problems.append(
            "G6 %s defaults $BuildDir to '%s', not the packaging tree '%s'. "
            "This is the script that produced the signed v0.2.0 installer; its "
            "default IS the release path." % (SIGN_SCRIPT, m.group(1), packaging_dir))

    reads_cache = "PDFE_TESTING:BOOL=" in raw and "CMakeCache.txt" in raw
    refuses = bool(re.search(r"throw", raw, re.I)) and "PDFE_TESTING:BOOL=OFF" in raw
    if not (reads_cache and refuses):
        problems.append(
            "G6 %s does not read PDFE_TESTING:BOOL= out of the build tree's "
            "CMakeCache.txt and throw unless it is OFF. The default alone is "
            "not a control: `-BuildDir %s` walks straight past it, and the "
            "preset NAME is a claim about how a tree was configured while the "
            "cache is the tree." % (SIGN_SCRIPT, STANDARD_TREE))


def check_workflow(files, problems, packaging_preset, packaging_dir):
    """G7."""
    raw = files.get(WORKFLOW)
    if raw is None:
        problems.append("G7 %s is missing" % WORKFLOW)
        return
    if not packaging_preset:
        return

    if ("cmake --preset %s" % packaging_preset) not in raw:
        problems.append(
            "G7 %s never runs `cmake --preset %s`. Without configuring the "
            "packaging tree the release job has nothing to package but the test "
            "tree." % (WORKFLOW, packaging_preset))
        return

    # The configure step must READ THE CACHE BACK. A configure that trusts the
    # preset proves the command was typed, not that the tree carries the flag.
    if "PDFE_TESTING:BOOL=" not in raw:
        problems.append(
            "G7 %s configures %s but never verifies PDFE_TESTING:BOOL= in the "
            "resulting CMakeCache.txt. Read the cache back -- a preset is a "
            "claim, the cache is the tree." % (WORKFLOW, packaging_preset))

    # The installer step must be gated on the configure step, not on the
    # build of the OTHER tree (the F44/F46 producer-guard rule).
    if not re.search(r"^\s*id:\s*configure_package\s*$", raw, re.M):
        problems.append(
            "G7 %s has no step with `id: configure_package`; the installer step "
            "cannot name it as its producer." % WORKFLOW)
        return
    if "steps.configure_package.outcome == 'success'" not in raw:
        problems.append(
            "G7 %s does not gate the installer step on "
            "steps.configure_package.outcome == 'success'. An installer built "
            "when the packaging configure failed or was skipped would be "
            "packaged from whatever tree happens to be on disk." % WORKFLOW)

    if packaging_dir and ("cpack" in raw) and (packaging_dir not in raw.replace("\\", "/")):
        problems.append(
            "G7 %s invokes cpack but never mentions the packaging tree %s"
            % (WORKFLOW, packaging_dir))


def check_smoke_script(files, problems, packaging_dir):
    """G8."""
    raw = files.get(SMOKE_SCRIPT)
    if raw is None:
        problems.append("G8 %s is missing" % SMOKE_SCRIPT)
        return
    if not packaging_dir:
        return
    m = re.search(r"Get-ChildItem\s*\(Join-Path\s+\$repoRoot\s+'([^']+)'\)", raw)
    if not m:
        problems.append(
            "G8 %s no longer resolves a default installer directory that this "
            "gate can read" % SMOKE_SCRIPT)
        return
    if not m.group(1).replace("\\", "/").startswith(packaging_dir):
        problems.append(
            "G8 %s defaults to installers under '%s', not the packaging tree "
            "'%s'. The clean-machine smoke test would then vouch for a bundle "
            "that is not the one shipped." % (SMOKE_SCRIPT, m.group(1), packaging_dir))


def check_all(files):
    problems = []
    preset, pkgdir = find_packaging_preset(files, problems)
    check_option_default_on(files, problems)
    check_standard_preset_unpinned(files, problems, preset)
    check_cpack_sites(files, problems, pkgdir)
    check_no_packaging_from_standard_tree(files, problems)
    check_sign_script(files, problems, pkgdir)
    check_workflow(files, problems, preset, pkgdir)
    check_smoke_script(files, problems, pkgdir)
    return problems


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _mutate(files, rel, old, new):
    """Apply one mutation, insisting the marker is really there.

    A mutation whose marker silently failed to match is INDISTINGUISHABLE from
    a surviving mutant, so this raises rather than returning the tree unchanged.
    Separator-tolerant: the workflow writes Windows paths with backslashes and
    the scripts write them with forward slashes.
    """
    out = dict(files)
    text = out[rel]
    for o, n in ((old, new),
                 (old.replace("/", "\\"), new.replace("/", "\\"))):
        if o in text:
            out[rel] = text.replace(o, n, 1)
            return out
    raise AssertionError(
        "self-test mutation marker absent from %s: %r" % (rel, old[:80]))


def self_test(repo_root):
    """Prove the guard both FIRES and STAYS QUIET, against the real tree.

    The clean baseline is the repository as it stands, so a self-test that
    passed while the real check failed would be impossible. Each mutation
    re-introduces one concrete way #182 comes back, and must be caught by the
    gate that owns it -- a mutation caught by the WRONG gate is not a kill.
    """
    files = load_repo(repo_root)
    failures = []

    base = check_all(files)
    if base:
        failures.append("clean tree is not clean: %s" % "; ".join(base))

    preset, pkgdir = find_packaging_preset(files, [])
    if not preset or not pkgdir:
        print("%s self-test cannot run: no unambiguous packaging preset" % TAG,
              file=sys.stderr)
        return 1

    def expect(label, mutated, gate):
        probs = check_all(mutated)
        hit = [p for p in probs if p.startswith(gate)]
        if not hit:
            failures.append(
                "mutation %r did not trip %s (got: %s)"
                % (label, gate, "; ".join(probs) or "NOTHING"))

    # M1 -- the packaging preset stops setting the flag (the #182 state).
    m1 = dict(files)
    doc = json.loads(m1[PRESETS])
    for p in doc.get("configurePresets", []):
        if p.get("name") == preset:
            p.get("cacheVariables", {}).pop("PDFE_TESTING", None)
    m1[PRESETS] = json.dumps(doc, indent=2)
    expect("packaging preset drops PDFE_TESTING", m1, "G1")

    # M1b -- the packaging preset points at the test tree.
    m1b = dict(files)
    doc = json.loads(m1b[PRESETS])
    for p in doc.get("configurePresets", []):
        if p.get("name") == preset:
            p["binaryDir"] = "${sourceDir}/" + STANDARD_TREE
    m1b[PRESETS] = json.dumps(doc, indent=2)
    expect("packaging preset reuses the test tree", m1b, "G1")

    # M2 -- the option default flips, silently disarming the test seams.
    m2 = _mutate(files, CMAKELISTS,
                 "for release/packaging builds so the seam is statically dead.\"\n       ON)",
                 "for release/packaging builds so the seam is statically dead.\"\n       OFF)")
    expect("option default flipped to OFF", m2, "G2")

    # M3 -- the standard preset starts pinning the flag.
    m3 = dict(files)
    doc = json.loads(m3[PRESETS])
    for p in doc.get("configurePresets", []):
        if _norm_dir(p.get("binaryDir")) == STANDARD_TREE and p.get("name") != preset:
            p.setdefault("cacheVariables", {})["PDFE_TESTING"] = "ON"
    m3[PRESETS] = json.dumps(doc, indent=2)
    expect("standard preset pins PDFE_TESTING=ON", m3, "G3")

    # M4 -- a cpack call reverts to the test tree.
    m4 = _mutate(files, WORKFLOW,
                 "%s/CPackConfig.cmake" % pkgdir,
                 "%s/CPackConfig.cmake" % STANDARD_TREE)
    expect("cpack --config points at the test tree", m4, "G4")
    expect("cpack --config points at the test tree", m4, "G5")

    # M5 -- the signing script's default reverts.
    m5 = _mutate(files, SIGN_SCRIPT,
                 '[string] $BuildDir = "%s"' % pkgdir,
                 '[string] $BuildDir = "%s"' % STANDARD_TREE)
    expect("signing script defaults back to the test tree", m5, "G6")

    # M6 -- the signing script keeps the default but loses the refusal, so
    # `-BuildDir build/vs2022` would sail through.
    m6 = _mutate(files, SIGN_SCRIPT, "PDFE_TESTING:BOOL=OFF", "PDFE_TESTING:BOOL=ANY")
    expect("signing script loses its cache refusal", m6, "G6")

    # M7 -- the workflow stops configuring the packaging tree.
    m7 = _mutate(files, WORKFLOW,
                 "cmake --preset %s" % preset,
                 "cmake --preset windows-vs2022")
    expect("workflow drops the packaging configure", m7, "G7")

    # M7b -- the installer step stops naming the configure step as producer,
    # so a failed packaging configure would leave it packaging anything.
    m7b = _mutate(files, WORKFLOW,
                  "steps.configure_package.outcome == 'success'",
                  "steps.build_release.outcome == 'success'")
    expect("installer step re-gated on the wrong producer", m7b, "G7")

    # M8 -- the clean-machine smoke test vouches for the wrong bundle.
    m8 = _mutate(files, SMOKE_SCRIPT,
                 "'%s/installer'" % pkgdir,
                 "'%s/installer'" % STANDARD_TREE)
    expect("smoke test defaults to the test tree's installer", m8, "G8")

    if failures:
        print("%s self-test FAILED:" % TAG, file=sys.stderr)
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        return 1
    print("%s self-test: clean tree passes; 10 mutations each tripped their "
          "own gate" % TAG)
    return 0


# ---------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    repo_root = os.path.abspath(args.repo_root)
    if args.self_test:
        return self_test(repo_root)

    problems = check_all(load_repo(repo_root))
    if problems:
        print("%s FAIL -- a packaging path can ship the PDFE_TEST_PIPES seam "
              "(GitHub #182):" % TAG, file=sys.stderr)
        for p in problems:
            print("  - %s" % p, file=sys.stderr)
        return 1
    print("%s OK -- every packaging path configures PDFE_TESTING=OFF" % TAG)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
