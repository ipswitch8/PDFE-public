#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_qt6svg_deploy.py (GitHub #192, DCA 0076).

Both directions are pinned deliberately. A guard no mutation can redden is
decoration; a guard that rejects legitimate reformatting gets deleted by the
next person it annoys. The negative cases below are therefore not padding --
they are what stops the anchored destination regex being tightened into
something unusable.

These drive `check_one()` against CMake fragments in a temp tree, so nothing
here touches the real repository.
"""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_qt6svg_deploy as guard  # noqa: E402

# The real shape, as it appears in tests/CMakeLists.txt and src/app/CMakeLists.txt.
GOOD = """\
add_custom_command(TARGET pdfe_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/bin,bin>/$<IF:$<CONFIG:Debug>,Qt6Svgd.dll,Qt6Svg.dll>"
        "$<TARGET_FILE_DIR:pdfe_tests>/"
)
"""


def _check(text: str, target: str = "pdfe_tests") -> list[str]:
    """Run the guard's per-file check over `text`, in an isolated temp tree."""
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        rel = "tests/CMakeLists.txt"
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
        original = guard.REPO_ROOT
        try:
            guard.REPO_ROOT = root
            return guard.check_one(rel, target)
        finally:
            guard.REPO_ROOT = original


# --------------------------------------------------------------------------
# Control
# --------------------------------------------------------------------------

def test_the_real_shape_passes():
    assert _check(GOOD) == []


# --------------------------------------------------------------------------
# POSITIVE -- each of these is a way #192 comes back
# --------------------------------------------------------------------------

def test_removing_the_tests_destination_is_caught():
    """DCA 0076 reproducing test: the copy's destination line is deleted."""
    mutated = GOOD.replace('        "$<TARGET_FILE_DIR:pdfe_tests>/"\n', "")
    assert _check(mutated), "a destination-less copy must be reported"


def test_retargeting_the_destination_into_a_subdirectory_is_caught():
    """DCA 0076 class-level test: the copy lands one directory down.

    Windows resolves a plugin's DLL imports against the executable's own
    directory, so a module staged in a subdirectory is never loaded and #192
    recurs verbatim. An earlier revision of the guard matched the target
    anywhere in the command and let this through.
    """
    mutated = GOOD.replace(
        '"$<TARGET_FILE_DIR:pdfe_tests>/"',
        '"$<TARGET_FILE_DIR:pdfe_tests>/' + "icon" + 'engines/"',
    )
    assert _check(mutated), "a copy into a subdirectory must be reported"


def test_release_side_name_typo_is_caught():
    """Invisible on a box that only builds Debug; surfaces first at packaging."""
    mutated = GOOD.replace("Qt6Svgd.dll,Qt6Svg.dll", "Qt6Svgd.dll,Qt6Svgg.dll")
    assert _check(mutated), "a Release-side name typo must be reported"


def test_debug_side_name_typo_is_caught():
    mutated = GOOD.replace("Qt6Svgd.dll,Qt6Svg.dll", "Qt6Svgdd.dll,Qt6Svg.dll")
    assert _check(mutated), "a Debug-side name typo must be reported"


def test_dropping_the_copy_entirely_is_caught():
    assert _check("add_custom_command(TARGET pdfe_tests POST_BUILD)\n"), (
        "a file with no Qt6Svg copy at all must be reported"
    )


def test_a_copy_wired_to_a_different_target_is_caught():
    """The command exists but stages into the other target's directory."""
    mutated = GOOD.replace("TARGET_FILE_DIR:pdfe_tests", "TARGET_FILE_DIR:pdfe")
    assert _check(mutated, target="pdfe_tests"), (
        "a copy landing in another target's dir must be reported"
    )


# --------------------------------------------------------------------------
# NEGATIVE -- legitimate variation must NOT be rejected
# --------------------------------------------------------------------------

def test_destination_without_a_trailing_slash_still_passes():
    variant = GOOD.replace(
        '"$<TARGET_FILE_DIR:pdfe_tests>/"', '"$<TARGET_FILE_DIR:pdfe_tests>"'
    )
    assert _check(variant) == [], "omitting the trailing slash is legitimate"


def test_trailing_whitespace_after_the_destination_still_passes():
    variant = GOOD.replace(
        '"$<TARGET_FILE_DIR:pdfe_tests>/"', '"$<TARGET_FILE_DIR:pdfe_tests>/"   '
    )
    assert _check(variant) == [], "trailing whitespace is legitimate"


def _main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"[qt6svg-selftest] ok   {t.__name__}")
        except AssertionError as exc:
            failed += 1
            print(f"[qt6svg-selftest] FAIL {t.__name__}: {exc}")
    print()
    if failed:
        print(f"[qt6svg-selftest] {failed} of {len(tests)} failed")
        return 1
    print(f"[qt6svg-selftest] all {len(tests)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
