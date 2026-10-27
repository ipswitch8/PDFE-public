# -*- coding: utf-8 -*-
"""The inventory producer must never write a machine path into a tracked file.

`coverage_inventory.py` regenerates `coverage_inventory.json`, which is
COMMITTED. It used to write `str(path)` -- the absolute path of whichever
machine ran it -- so the build account name went into a tracked artifact on
every regeneration, and the committed copy had to be hand-scrubbed afterwards.
This repository had its history rewritten across 1,290 commits to remove that
string and is intended to become public; a manual scrub step only has to be
forgotten once.

`tests/test_ui_suite_path_hygiene.cpp` catches the leak after the fact, by
scanning tracked files. These tests catch it in the producer, and pin the
specific spelling that already slipped through once: MuPDF's open-failure
message quotes the filename with DOUBLED backslashes, so the first fix -- a
literal string replace of the plain and forward-slashed root -- silently
missed it and one occurrence survived into the tracked JSON.

Deliberately importable without PyMuPDF: the fast-checks CI lane installs only
pytest and pyyaml, and a guard that cannot run in the lane that runs it is not
a guard. `coverage_inventory` therefore defers its `fitz` import.
"""
import sys
from pathlib import Path

import pytest

FIXTURES_DIR = Path(__file__).resolve().parent
if str(FIXTURES_DIR) not in sys.path:
    sys.path.insert(0, str(FIXTURES_DIR))

import coverage_inventory as ci  # noqa: E402

BACKSLASH = chr(92)
ROOT = str(ci.REPO_ROOT)


def _account_name():
    """The user-directory component of the repo root.

    Derived from the path rather than hardcoded, so this test does not itself
    become a place the account name is written down.

    That sentence was true of the code and false of the docstring above it,
    which spelled the account name out as an "e.g." -- and this file was
    committed that way, because the tracked-path guard scans TRACKED files and
    this one was still untracked when the evidence run for that commit ran.
    Do not put an example here.
    """
    parts = ci.REPO_ROOT.parts
    return parts[2] if len(parts) > 2 else ""


@pytest.mark.parametrize(
    "label,text",
    [
        ("single backslash", ROOT + BACKSLASH + "tests" + BACKSLASH + "x.pdf"),
        ("forward slash", ROOT.replace(BACKSLASH, "/") + "/tests/x.pdf"),
        # The form that actually escaped: MuPDF's error text after escaping.
        ("doubled backslash",
         ROOT.replace(BACKSLASH, BACKSLASH * 2) + BACKSLASH * 2 + "tests"
         + BACKSLASH * 2 + "x.pdf"),
        ("uppercased", ROOT.upper() + BACKSLASH + "tests" + BACKSLASH + "x.pdf"),
        ("mixed separators",
         ROOT.replace(BACKSLASH, "/") + BACKSLASH + "tests/x.pdf"),
    ],
)
def test_scrub_removes_the_repo_root_in_every_spelling(label, text):
    account = _account_name()
    if not account:
        pytest.fail("could not derive the account component from the repo root")
    scrubbed = ci.scrub_repo_paths(text)
    assert account.lower() not in scrubbed.lower(), (
        "%s form leaked the account name: %r" % (label, scrubbed)
    )
    assert "<repo>" in scrubbed


def test_scrub_leaves_unrelated_text_alone():
    # A scrubber that eats everything is as useless as one that eats nothing.
    text = "open failed: no such file 'relative/path/to/x.pdf'"
    assert ci.scrub_repo_paths(text) == text


def test_repo_relative_never_returns_an_absolute_path():
    inside = ci.REPO_ROOT / "tests" / "fixtures" / "x.pdf"
    assert ci.repo_relative(inside) == "tests/fixtures/x.pdf"


def test_repo_relative_falls_back_to_the_bare_name_outside_the_repo():
    # A fixture reached through a path outside the repo must still not put a
    # machine-specific prefix into the tracked JSON.
    outside = Path(ROOT).anchor and Path(ci.REPO_ROOT.anchor) / "elsewhere" / "y.pdf"
    assert ci.repo_relative(outside) == "y.pdf"


def test_the_committed_inventory_carries_no_absolute_paths():
    """The artifact itself, not just the function that writes it."""
    import json

    inv = FIXTURES_DIR / "coverage_inventory.json"
    if not inv.exists():
        pytest.fail("coverage_inventory.json is missing; run coverage_inventory.py")
    with open(inv, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    offenders = []
    for rec in data.get("fixtures", []):
        for key in ("path", "relPath", "error"):
            value = rec.get(key)
            if not isinstance(value, str):
                continue
            # Any drive-letter root or UNC prefix is machine-specific.
            if ":" + BACKSLASH in value or ":/" in value or value.startswith(BACKSLASH * 2):
                offenders.append("%s -> %s: %r" % (rec.get("relPath"), key, value))

    assert not offenders, (
        "coverage_inventory.json carries %d absolute path(s):\n  %s"
        % (len(offenders), "\n  ".join(offenders[:10]))
    )
