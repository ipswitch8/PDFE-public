# -*- coding: utf-8 -*-
"""Companion test for `check_fz_try_throw_free.py` (GitHub #287).

The checker's `--self-test` exercises its judgement against synthetic snippets.
That is necessary and not sufficient: a parser can pass every fixture its author
wrote and still be blind to the real codebase's formatting. The guard currently
reports **0 findings** across 148 blocks, and a clean sweep is evidence about
the sweep until proven otherwise.

So this file adds the two things the self-test cannot:

  * it runs the block parser against the REAL tree and requires it to find the
    measured population -- a parser that has gone blind reports clean, and
    "0 findings" is exactly what blindness looks like;
  * it MUTATES real tracked files, injecting a `throw` into each real `fz_try`
    body in turn, and requires every mutant to be caught at the right line.
    Mutating the guard's own fixtures would only prove the fixtures match the
    fixtures.

No tracked file is modified: the mutation is applied to the file's text
in memory and scanned from there.
"""
from __future__ import annotations

import importlib.util
import io
import pathlib
import subprocess
import sys

import pytest

_HERE = pathlib.Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
_MOD_PATH = _HERE / "check_fz_try_throw_free.py"

#: Measured 2026-10-15. A ratchet, not a pin: the parser finding materially
#: fewer blocks than this means it has stopped seeing real code, which is the
#: failure mode that makes "0 findings" meaningless.
MIN_EXPECTED_BLOCKS = 120

#: Files with enough real blocks to be a worthwhile mutation corpus, chosen for
#: formatting variety (brace-on-same-line, brace-on-next-line, deeply nested,
#: and inside execute() lambdas).
MUTATION_CORPUS = (
    "src/core/Document.cpp",
    "src/compliance/XmpMetadataWriter.cpp",
    "src/reflow/AlignmentInjector.cpp",
    "src/annotations/AnnotationManager.cpp",
    "src/core/MuPdfContext.cpp",
)


def _load():
    spec = importlib.util.spec_from_file_location("fztt", _MOD_PATH)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["fztt"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def mod():
    return _load()


def test_the_checkers_own_self_test_passes():
    r = subprocess.run([sys.executable, str(_MOD_PATH), "--self-test"],
                       capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-4000:]


def test_the_real_tree_is_clean():
    """The product state: no C++ throw inside any fz_try body."""
    r = subprocess.run([sys.executable, str(_MOD_PATH)],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    assert r.returncode == 0, out[-4000:]
    assert "findings: 0" in out, out[-2000:]


def test_the_parser_still_sees_the_real_population(mod):
    """Non-vacuity. 'findings: 0' from a parser that matched nothing is not news.

    The checker itself refuses to pass with zero blocks, which covers total
    blindness. This covers PARTIAL blindness -- a brace-matching or macro
    regression that still finds some blocks but silently stops seeing most of
    them would otherwise read as a clean tree.
    """
    total = 0
    files = 0
    for f in mod.iter_sources([str(_REPO / "src")]):
        text = io.open(f, encoding="utf-8", errors="replace").read()
        if "fz_try" not in text and "fz_always" not in text:
            continue
        n = len(mod.fz_try_bodies(mod.strip_comments_and_strings(text)))
        if n:
            files += 1
            total += n
    assert total >= MIN_EXPECTED_BLOCKS, (
        f"the block parser found only {total} fz_try/fz_always bodies across "
        f"{files} file(s); at least {MIN_EXPECTED_BLOCKS} were measured on "
        f"2026-10-15. A parser that has stopped seeing real blocks reports "
        f"'findings: 0', which is indistinguishable from a clean tree.")


@pytest.mark.parametrize("rel", MUTATION_CORPUS)
def test_every_real_fz_try_body_is_mutation_sensitive(mod, rel):
    """Inject a `throw` into each real fz_try body; all must be caught.

    This is the assertion that makes the clean sweep mean something. It uses
    REAL tracked files rather than the checker's fixtures, because a guard that
    only ever sees its author's formatting is vouched for by its own test.

    The line attribution is asserted too, not just the count. A finding that
    cites the wrong line sends the reader to innocent code, and a guard whose
    citations drift is worse than none.
    """
    path = _REPO / rel
    if not path.is_file():
        pytest.fail(f"mutation corpus file {rel} is missing; update "
                    f"MUTATION_CORPUS rather than letting this skip")

    text = io.open(path, encoding="utf-8", errors="replace").read()
    bodies = mod.fz_try_bodies(mod.strip_comments_and_strings(text))
    assert bodies, f"{rel} has no fz_try bodies, so it cannot host a mutant"

    baseline = mod.scan_text(text, rel)
    assert baseline == [], (
        f"{rel} already has findings {baseline}; the mutation proof below "
        f"needs a clean baseline to attribute its kills to the mutation")

    survivors = []
    for i, (_opener, start, _end) in enumerate(bodies):
        injected = '\n        throw std::runtime_error("MUTANT");'
        mutant = text[:start] + injected + text[start:]
        expected_line = mutant.count("\n", 0, start) + 2
        got = mod.scan_text(mutant, rel)
        if len(got) != 1 or got[0]["line"] != expected_line:
            survivors.append((i, expected_line,
                              [(g["line"], g["depth"]) for g in got]))

    assert not survivors, (
        f"{len(survivors)} of {len(bodies)} injected throws in {rel} were "
        f"missed or mis-attributed: {survivors}. Each survivor is a real "
        f"fz_try body this guard cannot protect.")


def test_a_throw_outside_the_block_is_not_a_false_positive(mod):
    """The other direction, on real code.

    A guard that fired on every `throw` anywhere in a file containing `fz_try`
    would also report 'clean tree' as impossible and get switched off. These
    files contain plenty of legitimate throws outside fz_try bodies -- the
    clean baseline above already depends on that, and this states it.
    """
    throws_outside = 0
    for rel in MUTATION_CORPUS:
        path = _REPO / rel
        if not path.is_file():
            continue
        text = io.open(path, encoding="utf-8", errors="replace").read()
        stripped = mod.strip_comments_and_strings(text)
        bodies = mod.fz_try_bodies(stripped)
        for m in mod._THROW_RE.finditer(stripped):
            at = m.start(0)
            while at < len(stripped) and not stripped.startswith("throw", at):
                at += 1
            if not any(s <= at < e for _o, s, e in bodies):
                throws_outside += 1
    assert throws_outside > 0, (
        "no `throw` was found outside an fz_try body anywhere in the mutation "
        "corpus, so the clean baseline does not demonstrate that this guard "
        "discriminates by LOCATION rather than merely failing to match "
        "`throw` at all")
    print(f"[#287] legitimate throws outside fz_try bodies in the corpus: "
          f"{throws_outside}")
