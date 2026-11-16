# -*- coding: utf-8 -*-
"""The Linux CI lane's recipe and docs/building-linux.md must not drift apart.

linux-validation.yml says its recipe follows docs/building-linux.md (GitHub
#390). That sentence was prose with nothing behind it, so this pins the parts a
reader would copy: the preset, the configuration, the target list, the -j cap,
the headless platform, the opt-in slow-save tests and the shared parity filter.
Each property is matched by its own pattern on each side, because YAML and a
shell snippet spell the same setting differently (`QT_QPA_PLATFORM: offscreen`
versus `QT_QPA_PLATFORM=offscreen`).
"""
from __future__ import annotations

import io
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "linux-validation.yml"
DOC = REPO / "docs" / "building-linux.md"


def _strip_yaml_comments(text: str) -> str:
    # Comment-only lines describe the recipe; they must not satisfy it.
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))


def _read(p: Path) -> str:
    return io.open(p, encoding="utf-8").read()


# (property, pattern in the workflow, pattern in the doc)
PROPERTIES = [
    ("CMake preset",
     r"cmake --preset linux-gcc\b", r"cmake --preset linux-gcc\b"),
    ("Release configuration",
     r"--config Release\b", r"--config Release\b"),
    ("the three targets",
     r"--target pdfe pdfe_preflight pdfe_tests\b",
     r"--target pdfe pdfe_preflight pdfe_tests\b"),
    ("-j3 memory cap",
     r"pdfe_tests -j3\b", r"pdfe_tests -j3\b"),
    ("headless offscreen platform",
     r"QT_QPA_PLATFORM:\s*offscreen\b", r"QT_QPA_PLATFORM=offscreen\b"),
    ("opt-in slow save tests",
     r"PDFE_RUN_SLOW_SAVE_TESTS:\s*\"1\"", r"PDFE_RUN_SLOW_SAVE_TESTS=1\b"),
    ("shared parity filter",
     r"tools/gate/parity_filter\.py", r"tools/gate/parity_filter\.py"),
    ("Release binary path",
     r"bin/Release/pdfe_tests\b", r"bin/Release/pdfe_tests\b"),
]


@pytest.mark.parametrize("name,wf_pat,doc_pat", PROPERTIES,
                         ids=[p[0] for p in PROPERTIES])
def test_workflow_and_doc_agree(name, wf_pat, doc_pat):
    wf = _strip_yaml_comments(_read(WORKFLOW))
    doc = _read(DOC)
    assert re.search(wf_pat, wf), f"linux-validation.yml no longer has: {name}"
    assert re.search(doc_pat, doc), f"docs/building-linux.md no longer has: {name}"


def test_workflow_does_not_reintroduce_the_old_recipe():
    wf = _strip_yaml_comments(_read(WORKFLOW))
    assert "xvfb-run" not in wf, "the lane went back to xvfb-run"
    assert not re.search(r"-j\$\(nproc\)", wf), "the lane dropped the -j3 cap"
    assert "QT_QPA_PLATFORM: xcb" not in wf


def test_comment_lines_do_not_count_as_the_recipe():
    text = "# cmake --preset linux-gcc\nrun: echo hi\n"
    assert not re.search(PROPERTIES[0][1], _strip_yaml_comments(text))
