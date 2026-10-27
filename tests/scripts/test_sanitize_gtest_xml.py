#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""The sanitizer must produce XML that is both clean AND parseable.

GitHub #263. ``sanitize_gtest_xml.py`` replaced user-profile paths with a
literal ``<user>``. That token is not valid inside an XML attribute, so whenever
a redacted path landed in one -- or a test id legitimately contained ``<`` --
the output stopped being well-formed. Its post-check asked only whether the
account name was gone and then printed "account name absent: verified", so the
file read as certified while no XML parser could open it.

Found by the phase-5 karen gate, which could not parse the evidence artifact the
script had just certified clean: a pytest junit report with a test id containing
an angle bracket.

The fixtures below use the POSIX ``/c/Users/...`` spelling, which this script
handles alongside the Windows one, purely so the test bodies carry no backslash
escaping of their own.
"""
from __future__ import annotations

import importlib.util
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "sanitize_gtest_xml.py"

_spec = importlib.util.spec_from_file_location("_sanitize_under_test", SCRIPT)
san = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(san)

_NOWHERE = "/nowhere/else"


def _doc(inner: str) -> str:
    return '<?xml version="1.0"?>\n<testsuites>%s</testsuites>\n' % inner


def test_a_redacted_path_leaves_the_document_parseable():
    """The regression: a user path inside an ATTRIBUTE is the failing shape."""
    raw = _doc('<testcase name="t" file="/c/Users/someone/x.cpp"/>')
    out = san.sanitize(raw, repo_root=_NOWHERE, account="someone")
    ET.fromstring(out)                      # must not raise
    assert "someone" not in out


def test_the_token_is_escaped_not_literal():
    raw = _doc('<testcase name="t" file="/c/Users/someone/x.cpp"/>')
    out = san.sanitize(raw, repo_root=_NOWHERE, account="")
    assert "<user>" not in out, "a literal <user> makes the document ill-formed"
    assert "&lt;user&gt;" in out


def test_the_redacted_token_still_reads_as_user():
    """Escaping must not change what a human sees."""
    raw = _doc('<testcase name="t" file="/c/Users/someone/x.cpp"/>')
    out = san.sanitize(raw, repo_root=_NOWHERE, account="")
    root = ET.fromstring(out)
    assert "<user>" in root.find("testcase").get("file")


def test_a_test_id_already_containing_an_angle_bracket_survives():
    """The exact artifact that exposed this: `<` already present in a name."""
    raw = _doc('<testcase name="t[&lt;user&gt;-carol]" '
               'file="/c/Users/someone/y.cpp"/>')
    out = san.sanitize(raw, repo_root=_NOWHERE, account="someone")
    root = ET.fromstring(out)
    assert root.find("testcase").get("name") == "t[<user>-carol]"


def test_the_writer_refuses_an_unparseable_result(tmp_path, monkeypatch):
    """The post-check must cover well-formedness, not only the account name.

    Without this the script prints "verified" about one property while another
    is broken, which is how the defect survived.
    """
    src = tmp_path / "in.xml"
    src.write_text(_doc('<testcase name="t" file="/c/Users/someone/x.cpp"/>'),
                   encoding="utf-8")
    dst = tmp_path / "out.xml"

    # Re-break it exactly as the old version did.
    monkeypatch.setattr(san, "_USER_TOKEN", "<user>")
    monkeypatch.setattr(san, "_ACCOUNT_TOKEN", "<account>")
    rc = san.main([str(src), str(dst), "--repo-root", _NOWHERE,
                   "--account", "someone"])
    assert rc == 1, "an ill-formed result was written and reported as clean"
    assert not dst.exists(), "the unparseable file was written anyway"


def test_a_clean_input_still_writes(tmp_path):
    """The control: the refusal above must not reject everything."""
    src = tmp_path / "in.xml"
    src.write_text(_doc('<testcase name="t" file="/c/Users/someone/x.cpp"/>'),
                   encoding="utf-8")
    dst = tmp_path / "out.xml"
    rc = san.main([str(src), str(dst), "--repo-root", _NOWHERE,
                   "--account", "someone"])
    assert rc == 0
    ET.parse(dst)


def test_the_account_name_check_still_refuses(tmp_path, monkeypatch):
    """Well-formedness must not have displaced the original guarantee."""
    src = tmp_path / "in.xml"
    src.write_text(_doc('<testcase name="someone" file="/tmp/x.cpp"/>'),
                   encoding="utf-8")
    dst = tmp_path / "out.xml"
    # Neutralise the scrubbing so the name survives into the output.
    monkeypatch.setattr(san, "sanitize", lambda text, repo_root, account="": text)
    rc = san.main([str(src), str(dst), "--repo-root", _NOWHERE,
                   "--account", "someone"])
    assert rc == 1
    assert not dst.exists()
