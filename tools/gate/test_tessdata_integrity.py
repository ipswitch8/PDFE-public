#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #177 — prove the tessdata integrity check detects a TAMPERED FILE.

WHY THIS FILE EXISTS, and what was already true before it.

`tools/gate/check_third_party_notices.py` already hashes every attributed
artifact and compares against a recorded SHA-256 (its `check()`, the
`actual != expected` branch). The three committed tessdata blobs
(`ara`/`eng`/`osd` `.traineddata`) are in its `ENTRIES` table with real hashes,
added when phase-18 gave them the provenance record they lacked.

So the MECHANISM was there. What was missing is the control over it:

  * `tools/gate/test_check_third_party_notices.py` mentions `tessdata`
    **zero** times. Its only fixture is `test_PDFs/crazyones-pdfa.pdf`.
  * Its `test_a_stale_hash_in_the_checker_is_caught` mutates the **expected**
    hash in the `ENTRIES` dict. That proves the comparison fires, which is
    worth having — but it does not exercise the file-read path at all. A
    checker that read the wrong path, read a cached copy, or skipped hashing
    above some size would pass it.

GitHub #177 asks for a check "with a test proving it detects a tampered /
mismatched tessdata archive". This supplies the missing direction: the bytes on
disk are changed, nothing else is, and the checker must notice.

Run:  python -m pytest tools/gate/test_tessdata_integrity.py -q
"""

from __future__ import annotations

import hashlib
import importlib.util
import shutil
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
GUARD = REPO_ROOT / "tools" / "gate" / "check_third_party_notices.py"

TESSDATA = [
    "tests/fixtures/tessdata/ara.traineddata",
    "tests/fixtures/tessdata/eng.traineddata",
    "tests/fixtures/tessdata/osd.traineddata",
]

# The smallest of the three, so the byte-flip test copies 1.4 MB rather than 10.
SMALLEST = "tests/fixtures/tessdata/ara.traineddata"


def _load():
    spec = importlib.util.spec_from_file_location("tpn_guard", GUARD)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["tpn_guard"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def mod():
    return _load()


# --------------------------------------------------------------------------
# The three blobs are actually in the table, and their hashes are real.
# --------------------------------------------------------------------------

@pytest.mark.parametrize("rel", TESSDATA)
def test_the_blob_is_in_the_guards_table(mod, rel):
    """Without this, every test below could pass while covering nothing.

    The population of a guard is part of its specification: a hash check over
    an empty table reports success. #177's first half was exactly this shape --
    `check_third_party_notices.py` reported "OK -- 9 artifacts attributed"
    while these three were not in its table at all.
    """
    assert rel in mod.ENTRIES, (
        f"{rel} has no entry in check_third_party_notices.ENTRIES, so nothing "
        f"hashes it and the tests below would be vacuous")
    assert len(mod.ENTRIES[rel]["sha256"]) == 64


@pytest.mark.parametrize("rel", TESSDATA)
def test_the_recorded_hash_is_the_real_hash(mod, rel):
    """Recorded == on-disk, right now. Guards against a paste error."""
    raw = (REPO_ROOT / rel).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == mod.ENTRIES[rel]["sha256"], (
        f"{rel}'s recorded SHA-256 does not match the file in the tree")


def test_the_live_tree_passes(mod):
    """Control. If this is red the tamper tests below prove nothing."""
    problems = []
    mod.check(problems)
    tess = [p for p in problems if "tessdata" in p]
    assert not tess, "tessdata problems on an unmodified tree: %r" % tess


# --------------------------------------------------------------------------
# THE missing direction: tamper the FILE, not the expectation.
# --------------------------------------------------------------------------

def _real_notices() -> str:
    """The live THIRD-PARTY-NOTICES.md text.

    A stub of just "<path> <hash>" is NOT enough: `check()` also requires the
    recorded SIZE, the upstream project, its URL and the licence to appear in
    the notices text. My first version of this harness used a stub, and the
    green direction below caught it immediately -- every tamper test was
    reporting four unrelated attribution problems alongside the hash mismatch,
    so a harness bug would have looked like successful detection.

    Using the real text means the ONLY thing varying across these tests is the
    bytes on disk, which is the whole point.
    """
    return (REPO_ROOT / "THIRD-PARTY-NOTICES.md").read_text(
        encoding="utf-8", errors="replace")


def _tampered_tree(tmp_path, mod, rel, mutate):
    """A minimal tree with one tessdata blob mutated, pointed at by REPO_ROOT.

    Only the one file is copied, plus a notices text supplied directly, so the
    10 MB `osd` blob is never duplicated. `_tracked()` shells out to
    `git ls-files` with `cwd=REPO_ROOT`; pointing REPO_ROOT at a non-repo would
    make it report UNTRACKED and mask the hash mismatch with a different
    problem, so it is stubbed to True for the duration. That stub is the reason
    `test_untracked_is_still_reported_separately` exists below -- stubbing a
    predicate is only safe if something else still covers it.
    """
    dest = tmp_path / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(REPO_ROOT / rel, dest)
    data = bytearray(dest.read_bytes())
    mutate(data)
    dest.write_bytes(bytes(data))
    return dest


@pytest.mark.parametrize("label,mutate", [
    ("one byte flipped in the middle",
     lambda d: d.__setitem__(len(d) // 2, d[len(d) // 2] ^ 0xFF)),
    ("one byte flipped in the header",
     lambda d: d.__setitem__(0, d[0] ^ 0xFF)),
    ("truncated by one byte", lambda d: d.__delitem__(-1)),
    ("one byte appended", lambda d: d.append(0x00)),
])
def test_a_tampered_file_is_caught(mod, tmp_path, monkeypatch, label, mutate):
    """The direction #177 asks for: the bytes change, nothing else does.

    Four shapes rather than one, because "tampered" is not a single thing: a
    flipped byte deep inside, a flipped byte in the header, a truncation and an
    append all have to be caught, and a length-only check would miss the first
    two while a header-only check would miss all but the second.
    """
    _tampered_tree(tmp_path, mod, SMALLEST, mutate)
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(mod, "_tracked", lambda rel: True)

    entries = {SMALLEST: mod.ENTRIES[SMALLEST]}
    notices = _real_notices()

    problems = []
    mod.check(problems, notices_text=notices, entries=entries)

    assert problems, (
        "a tampered %s (%s) produced NO problem. The integrity check did not "
        "read the file on disk." % (SMALLEST, label))
    assert any("SHA-256" in p for p in problems), (
        "something was reported, but not a hash mismatch: %r" % problems)


def test_an_untampered_copy_is_NOT_caught(mod, tmp_path, monkeypatch):
    """The green direction, and it is not ceremonial.

    Every assertion above expects a PROBLEM, so they only establish that the
    check is SENSITIVE. This one establishes that it is SPECIFIC -- that the
    copy-and-redirect harness itself does not manufacture the mismatch the
    tamper tests then "detect". Without it, a bug in `_tampered_tree` (copying
    the wrong file, say) would make all four tamper tests pass for the wrong
    reason.
    """
    _tampered_tree(tmp_path, mod, SMALLEST, lambda d: None)
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(mod, "_tracked", lambda rel: True)

    entries = {SMALLEST: mod.ENTRIES[SMALLEST]}
    notices = _real_notices()

    problems = []
    mod.check(problems, notices_text=notices, entries=entries)
    assert not problems, (
        "an UNMODIFIED copy was reported as a problem, so the tamper tests "
        "above may be detecting the harness rather than the tampering: %r"
        % problems)


def test_untracked_is_still_reported_separately(mod, tmp_path, monkeypatch):
    """`_tracked` is stubbed above; this keeps the real branch covered.

    A file present but untracked means a clean checkout will not have it, so
    the attribution describes nothing. That is a different failure from a hash
    mismatch and must stay distinguishable.
    """
    _tampered_tree(tmp_path, mod, SMALLEST, lambda d: None)
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(mod, "_tracked", lambda rel: False)

    entries = {SMALLEST: mod.ENTRIES[SMALLEST]}
    notices = _real_notices()

    problems = []
    mod.check(problems, notices_text=notices, entries=entries)
    assert any("NOT TRACKED" in p for p in problems), (
        "an untracked artifact was not reported as untracked: %r" % problems)


def test_a_missing_file_is_caught(mod, tmp_path, monkeypatch):
    """Deletion, as distinct from mutation."""
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(mod, "_tracked", lambda rel: True)

    entries = {SMALLEST: mod.ENTRIES[SMALLEST]}
    notices = _real_notices()

    problems = []
    mod.check(problems, notices_text=notices, entries=entries)
    assert any("not in the tree" in p for p in problems), (
        "a deleted attributed artifact was not reported: %r" % problems)
