#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GitHub #192 ratchet -- the Qt6Svg module DLL must be staged for BOTH targets.

Why this guard exists, and why it reads SOURCE rather than the build tree
--------------------------------------------------------------------------
`qsvgd.dll` / `qsvgicond.dll` import `Qt6Svg[d].dll` directly, but that import
lives inside a plugin loaded at runtime via QPluginLoader -- not in the
executable's own import table. vcpkg's applocal deployment walks only a
target's direct imports, so it never stages the module, and for months nothing
else did either.

The bug was invisible locally the whole time because a stale `Qt6Svgd.dll` sat
in the output directory from an unrelated earlier build. That is the important
part: **the three `UiStructureBugs` icon tests only fail when the DLL is
genuinely absent.** They detect "the DLL is missing from the output directory",
which any leftover artifact satisfies -- they do NOT detect "the CMake copy
step was removed".

That distinction is not academic here. The only CI lane that runs those tests,
`windows-validation.yml`, is schedule-only (no push/PR trigger) and sets
`clean: false` to preserve the build tree between runs -- which is precisely
the masking condition that hid #192 originally. So a revert of either copy
block would very likely go unnoticed: not checked on the PR that introduced it,
and then masked by a leftover DLL on the next nightly.

This guard therefore asserts the copy COMMAND exists in the CMake source, for
both targets and both configurations. It never looks at `build/`, so no stale
artifact can satisfy it, and it runs on the automatic lane where the
regression would actually be introduced.

Found by the phase-1 `claim-check` gate of pipeline p87-open-issues, which
failed the phase for exactly this gap.

Exit 0 = both deployments present. Exit 1 = at least one missing.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

# (cmake file, the TARGET_FILE_DIR target the copy must land in)
REQUIRED = [
    ("tests/CMakeLists.txt", "pdfe_tests"),
    ("src/app/CMakeLists.txt", "pdfe"),
]


def check_one(rel_path: str, target: str) -> list[str]:
    """Return a list of problems for one (file, target) pair; empty == ok."""
    path = REPO_ROOT / rel_path
    problems: list[str] = []

    if not path.is_file():
        return [f"{rel_path}: file does not exist"]

    text = path.read_text(encoding="utf-8", errors="replace")

    # Find every copy_if_different command and keep the ones mentioning Qt6Svg.
    # Splitting on COMMAND keeps each command's arguments together, so a
    # Qt6Svg source path cannot be credited to a different command's
    # destination.
    chunks = text.split("COMMAND ")
    svg_chunks = [c for c in chunks if re.search(r"Qt6Svgd?\.dll", c)]

    if not svg_chunks:
        problems.append(
            f"{rel_path}: no command references Qt6Svg[d].dll at all -- the "
            f"#192 deployment for target '{target}' is gone"
        )
        return problems

    # At least one of them must be a copy landing in THIS target's dir --
    # at its ROOT, not merely somewhere beneath it. Windows resolves a
    # plugin's DLL imports against the executable's own directory, so a copy
    # retargeted into a subdirectory (say `iconengines/`) stages the file
    # where nothing will load it and reproduces #192 exactly. An earlier
    # revision of this guard matched `TARGET_FILE_DIR:<target>>` anywhere in
    # the command and let that mutant through; the phase-1 claim-check gate
    # caught it. Anchor the destination to end at the trailing slash.
    dest_pat = re.compile(
        r"TARGET_FILE_DIR:" + re.escape(target) + r"\s*>\s*/?\s*\"\s*$",
        re.IGNORECASE | re.MULTILINE,
    )
    good = [
        c for c in svg_chunks
        if "copy_if_different" in c and dest_pat.search(c)
    ]
    if not good:
        problems.append(
            f"{rel_path}: Qt6Svg[d].dll is mentioned but not copy_if_different'd "
            f"into $<TARGET_FILE_DIR:{target}>/ -- the #192 deployment for "
            f"'{target}' is not wired to that target"
        )
        return problems

    # Both configurations must be covered. A Release-side typo is invisible on
    # this box (only Debug is exercised) and would surface only at packaging.
    blob = "\n".join(good)
    if not re.search(r"Qt6Svgd\.dll", blob):
        problems.append(
            f"{rel_path}: the Qt6Svg copy for '{target}' does not name the "
            f"Debug module Qt6Svgd.dll"
        )
    if not re.search(r"(?<!Qt6Svg)(?<!d)Qt6Svg\.dll", blob):
        problems.append(
            f"{rel_path}: the Qt6Svg copy for '{target}' does not name the "
            f"Release module Qt6Svg.dll"
        )
    return problems


def main() -> int:
    all_problems: list[str] = []
    for rel_path, target in REQUIRED:
        found = check_one(rel_path, target)
        if found:
            all_problems.extend(found)
        else:
            print(f"[qt6svg-deploy] ok   {rel_path} -> {target}")

    if all_problems:
        print()
        print("[qt6svg-deploy] FAIL: the GitHub #192 deployment has regressed.")
        for p in all_problems:
            print(f"  - {p}")
        print()
        print("  Without this copy, a CLEAN build tree ships without the Qt6Svg")
        print("  module and the UiStructureBugs icon tests fail with 0 visible")
        print("  icons. A dirty tree hides it -- which is why this guard reads")
        print("  the CMake source and never the build output.")
        return 1

    print("[qt6svg-deploy] PASS: Qt6Svg[d].dll staged for both targets, "
          "both configurations.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
