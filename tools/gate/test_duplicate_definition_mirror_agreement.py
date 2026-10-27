# -*- coding: utf-8 -*-
"""GitHub #275 C5 — the Python detector must agree with the shared corpus.

TWO IMPLEMENTATIONS, ONE QUESTION
=================================
"Does this source DEFINE <name> at column 0?" is answered twice:

  * here, by ``check_duplicate_test_definitions.definition_sizes()``
    (TYPE_DEF / FUNC_DEF / NOT_A_DEF plus ``_opens_a_body``);
  * in C++, by ``definesHelperAtColumnZero`` in tests/test_oracle_standard.cpp.

Nothing compared them. #275 is the issue filed because a ~155-line helper was
defined twice and the guard could not see it — and its deeper point is that two
copies of a thing drift. That applies to the DETECTOR as much as to the helper:
a drift here leaves both sides green, each against its own tests.

So both sides read ``tests/fixtures/column_zero_definition_corpus.txt`` and must
agree with it. This file is the Python half; ``OracleStandard.
TheColumnZeroDetectorAgreesWithTheSharedCorpus`` is the C++ half. The corpus is
the single source of truth — neither implementation is — so "they disagree with
each other" is detected as "at least one disagrees with the corpus", without
either language needing to call the other.

WHAT THIS FOUND
===============
``multiline-function-declaration``. The Python side hit that shape on its first
run against the real tree (tests/support/SaveFailureInjection.h:312-316, whose
``;`` sits four lines below the name), wrongly reported a DECLARATION as a
second definition, and was fixed by balancing the parameter list. The C++ mirror
checks only the first line for ``;`` and never got that fix.

Run:  python -m pytest tools/gate/test_duplicate_definition_mirror_agreement.py -q
"""

from __future__ import annotations

import importlib.util
import io
import sys
from pathlib import Path

import pytest

#: parents[2], not [1]: this file is <root>/tools/gate/<name>.py, so two levels
#: up is <root>/tools. Asserted rather than assumed — the same off-by-one has
#: now appeared three times in this phase (the new #288 guard, the four proof
#: scripts, and here), and in every case a wrong root silently resolves to
#: "nothing to check" rather than to an error.
REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
CORPUS = REPO_ROOT / "tests" / "fixtures" / "column_zero_definition_corpus.txt"
DETECTOR = REPO_ROOT / "tools" / "gate" / "check_duplicate_test_definitions.py"
CPP_MIRROR = REPO_ROOT / "tests" / "test_oracle_standard.cpp"

#: The C++ test that must consume the SAME corpus. Asserted below, because a
#: corpus only one side reads is not an agreement mechanism — it is a second
#: copy of one side's tests, which is the defect #275 is about.
CPP_TEST_NAME = "TheColumnZeroDetectorAgreesWithTheSharedCorpus"


def _load_detector():
    spec = importlib.util.spec_from_file_location("dup_defs", DETECTOR)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["dup_defs"] = mod
    spec.loader.exec_module(mod)
    return mod


def parse_corpus(text: str) -> list[dict]:
    """Parse the shared corpus. Deliberately strict — a silently dropped case
    would weaken the corpus while leaving it green, which is the failure mode
    this whole file exists to prevent."""
    cases: list[dict] = []
    cur: dict | None = None
    in_src = False
    for lineno, raw in enumerate(text.splitlines(), 1):
        if in_src:
            if raw == "@end":
                assert cur is not None
                cur["src"] = "\n".join(cur["src_lines"]) + "\n"
                cases.append(cur)
                cur, in_src = None, False
                continue
            if raw.startswith("@"):
                raise AssertionError(
                    f"{CORPUS.name}:{lineno}: a source line may not begin with "
                    f"'@' (it would be read as a directive and truncate the "
                    f"case): {raw!r}")
            cur["src_lines"].append(raw)
            continue
        if not raw.strip() or raw.startswith("#"):
            continue
        if raw.startswith("@case "):
            cur = {"id": raw[len("@case "):].strip(), "src_lines": []}
            continue
        if raw.startswith("@name "):
            cur["name"] = raw[len("@name "):].strip()
            continue
        if raw.startswith("@expect "):
            v = raw[len("@expect "):].strip()
            assert v in ("0", "1"), f"{CORPUS.name}:{lineno}: @expect must be 0 or 1"
            cur["expect"] = v == "1"
            continue
        if raw.startswith("@why "):
            cur["why"] = raw[len("@why "):].strip()
            continue
        if raw == "@src":
            in_src = True
            continue
        raise AssertionError(f"{CORPUS.name}:{lineno}: unrecognised line {raw!r}")
    assert cur is None and not in_src, "corpus ends inside an unterminated @case"
    return cases


@pytest.fixture(scope="module")
def corpus() -> list[dict]:
    assert CORPUS.exists(), f"shared corpus missing: {CORPUS}"
    cases = parse_corpus(io.open(CORPUS, encoding="utf-8").read())
    assert cases, "the corpus parsed to zero cases — a vacuous pass"
    return cases


@pytest.fixture(scope="module")
def detector():
    return _load_detector()


def _python_defines(mod, tmp_path: Path, src: str, name: str) -> bool:
    """The Python side's verdict, through its REAL entry point.

    `definition_sizes` takes a path, so the case is written to a file rather
    than the regexes being poked directly — testing the patterns in isolation
    would skip `_opens_a_body`, which is exactly where the two sides differ.
    """
    p = tmp_path / "case.cpp"
    io.open(p, "w", encoding="utf-8", newline="\n").write(src)
    return name in mod.definition_sizes(str(p))


def test_the_corpus_is_well_formed(corpus):
    """Every case carries all four fields, and ids are unique."""
    seen = set()
    for c in corpus:
        for field in ("id", "name", "expect", "why"):
            assert field in c, f"case {c.get('id')!r} is missing @{field}"
        assert c["src"].strip(), f"case {c['id']!r} has an empty @src"
        assert c["id"] not in seen, f"duplicate @case id {c['id']!r}"
        seen.add(c["id"])


def test_the_corpus_pins_both_verdicts(corpus):
    """A corpus of all-positives or all-negatives measures only one direction.

    Both detectors are trivially satisfied by `return true` or `return false`
    if the corpus leans entirely one way, so the corpus itself has to be
    checked for balance — the population is part of the spec.
    """
    pos = [c["id"] for c in corpus if c["expect"]]
    neg = [c["id"] for c in corpus if not c["expect"]]
    assert len(pos) >= 4, f"only {len(pos)} positive case(s): {pos}"
    assert len(neg) >= 4, f"only {len(neg)} negative case(s): {neg}"


def test_the_cpp_side_reads_the_same_corpus():
    """The agreement mechanism requires BOTH sides to consume this file.

    If only Python reads it, the corpus is not a cross-language control at all
    — it is a second copy of the Python tests, which is the shape GitHub #275
    was filed about. So the C++ mirror is required to name both the corpus and
    the test that drives it.
    """
    src = io.open(CPP_MIRROR, encoding="utf-8", errors="replace").read()
    assert "column_zero_definition_corpus.txt" in src, (
        "tests/test_oracle_standard.cpp does not reference the shared corpus, "
        "so nothing holds the C++ detector to it and the two implementations "
        "can still drift apart silently (GitHub #275 C5)")
    assert CPP_TEST_NAME in src, (
        f"the C++ half ({CPP_TEST_NAME}) is missing from "
        f"tests/test_oracle_standard.cpp")


def test_python_detector_agrees_with_every_corpus_case(corpus, detector,
                                                       tmp_path):
    """THE criterion. Every case, with the disagreements named."""
    wrong = []
    for i, c in enumerate(corpus):
        d = tmp_path / f"c{i}"
        d.mkdir()
        got = _python_defines(detector, d, c["src"], c["name"])
        if got != c["expect"]:
            wrong.append(
                f"  {c['id']}: corpus says defines={c['expect']}, "
                f"check_duplicate_test_definitions says {got}\n"
                f"      why: {c['why']}")
    assert not wrong, (
        "the Python detector disagrees with the shared corpus on "
        f"{len(wrong)} case(s) — which means it also disagrees with the C++ "
        "mirror wherever that one agrees with the corpus:\n" + "\n".join(wrong))
