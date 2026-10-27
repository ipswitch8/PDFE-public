#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-tests for check_tracking_banners.py.

The guard's job is to fail when a confusable document is unlabelled. These tests
exercise BOTH directions, because a guard that can only ever pass is decoration
and a guard that reddens on correct documents is worse than none — it teaches
people to bypass it.
"""
import os
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_tracking_banners as C  # noqa: E402

CHECKER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "check_tracking_banners.py")

WORKLIST = """# Some Gap Register

| id | area | status |
|---|---|---|
""" + "\n".join("| G-%02d | core | OPEN |" % i for i in range(40))

HIST_BANNER = """<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
"""


# ---- the pure decision function -------------------------------------------

def test_unlabelled_worklist_is_flagged():
    verdict, score = C.classify(WORKLIST, "docs/some-register.md", 12)
    assert verdict == "missing"
    assert score >= 12


def test_banded_worklist_passes():
    verdict, _ = C.classify(HIST_BANNER + WORKLIST, "docs/some-register.md", 12)
    assert verdict == "ok"


def test_prose_banner_passes_for_plain_text_registries():
    """KNOWN_WEAK_TESTS.txt cannot carry an HTML comment; it says it in prose."""
    text = "# registry\n# ... the canonical tracker is GitHub Issues ...\n" + WORKLIST
    verdict, _ = C.classify(text, "tests/KNOWN_WEAK_TESTS.txt", 12)
    assert verdict == "ok"


def test_exemption_with_a_reason_passes():
    text = "<!-- tracking-banner: exempt -- a schema doc, not a work list -->\n" + WORKLIST
    verdict, _ = C.classify(text, "docs/x.md", 12)
    assert verdict == "ok"


def test_exemption_without_a_reason_is_rejected():
    """An empty exemption is a bypass with no audit trail."""
    text = "<!-- tracking-banner: exempt --  -->\n" + WORKLIST
    verdict, _ = C.classify(text, "docs/x.md", 12)
    assert verdict == "blank-exemption"


def test_ordinary_prose_is_not_flagged():
    """The false-positive direction. A guard that fires here is worse than none."""
    text = ("# Architecture notes\n\nThe canvas renders tiles. The renderer is "
            "asynchronous and the cache is keyed by zoom.\n")
    verdict, score = C.classify(text, "docs/architecture-notes.md", 12)
    assert verdict == "ok"
    assert score < 12


def test_single_investigation_record_is_discounted():
    """'Here is what I found on Tuesday' is not a work list even when it says
    OPEN. NOTE the example is a mutation-evidence file, not a dispositions file
    -- 'dispositions' was deliberately removed from the discount, see
    test_dispositions_files_are_NOT_discounted."""
    plain = C.score_text(WORKLIST, "docs/audit/some-register.md")
    record = C.score_text(WORKLIST, "docs/audit/area-sign-mutation-evidence-2026-08-31.md")
    assert record < plain
    assert record == plain // 3


def test_dispositions_files_are_NOT_discounted():
    """Regression: 'dispositions' used to sit in the single-record discount, and
    that exempted the largest confusable class in the repo -- 21 of 70 real
    phase-NN-dispositions.md files scored >=12 raw but fell under the threshold
    only because of it. They were banded by hand, so the tree looked clean while
    the ratchet did not actually hold for them. A discount that exempts the
    biggest population is a hole, not a false-positive control."""
    verdict, score = C.classify(WORKLIST, "docs/audit/phase-90-dispositions.md", 12)
    assert verdict == "missing", (
        "an unbanded phase-NN-dispositions.md must be flagged; got %r at score %d"
        % (verdict, score))
    # and it must not be discounted relative to a plainly-named register
    assert (C.score_text(WORKLIST, "docs/audit/phase-90-dispositions.md")
            == C.score_text(WORKLIST, "docs/audit/some-register.md"))


def test_crlf_is_normalised():
    """core.autocrlf=true here: a guard comparing raw bytes passes in the session
    that wrote the file and fails after a fresh checkout."""
    lf = C.classify(HIST_BANNER + WORKLIST, "docs/x.md", 12)
    crlf_text = (HIST_BANNER + WORKLIST).replace("\n", "\r\n")
    crlf = C.classify(crlf_text, "docs/x.md", 12)
    assert lf == crlf


# ---- the CLI, end to end ---------------------------------------------------

def _run(root):
    p = subprocess.run([sys.executable, CHECKER, "--repo-root", str(root)],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def test_cli_passes_on_a_labelled_tree(tmp_path):
    d = tmp_path / "docs"
    d.mkdir()
    (d / "register.md").write_text(HIST_BANNER + WORKLIST, encoding="utf-8")
    rc, out = _run(tmp_path)
    assert rc == 0, out


def test_cli_fails_on_an_unlabelled_tracker_and_names_it(tmp_path):
    d = tmp_path / "docs"
    d.mkdir()
    (d / "register.md").write_text(WORKLIST, encoding="utf-8")
    rc, out = _run(tmp_path)
    assert rc == 1, out
    assert "register.md" in out
    # The failure must tell the reader how to fix it, not just that it failed.
    assert "tracking-banner" in out and "TRACKING.md" in out


def test_cli_fails_when_a_banner_is_REMOVED(tmp_path):
    """The drift case this guard exists for: someone deletes a banner."""
    d = tmp_path / "docs"
    d.mkdir()
    f = d / "register.md"
    f.write_text(HIST_BANNER + WORKLIST, encoding="utf-8")
    assert _run(tmp_path)[0] == 0
    f.write_text(WORKLIST, encoding="utf-8")          # banner removed
    rc, out = _run(tmp_path)
    assert rc == 1, out


def test_cli_fails_on_a_NEW_unlabelled_tracker(tmp_path):
    """The other drift case: a new work list appears next month."""
    d = tmp_path / "docs"
    d.mkdir()
    (d / "old.md").write_text(HIST_BANNER + WORKLIST, encoding="utf-8")
    assert _run(tmp_path)[0] == 0
    (d / "new-backlog.md").write_text(WORKLIST, encoding="utf-8")
    rc, out = _run(tmp_path)
    assert rc == 1, out
    assert "new-backlog.md" in out


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
