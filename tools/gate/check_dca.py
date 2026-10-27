#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_dca.py — enforce the escaped-defect -> DCA -> class-test loop.

Pipeline e5b9c231 Phase 5, criterion 4. Validates every Defect Causal Analysis
record under ``docs/audit/dca/`` against the mandatory contract so a DCA record
cannot be a prose essay that skips the parts that matter:

  * all mandatory ``## `` sections are present;
  * the **Reproducing test** and **Class-level test** fields each name a test
    file that ACTUALLY EXISTS on disk (path before the ``::Suite.Case``);
  * the cited ``Suite.Case`` ACTUALLY EXISTS INSIDE that file -- resolved by
    parsing the GTest macro calls (``TEST`` / ``TEST_F`` / ``TEST_P`` /
    ``TYPED_TEST`` / ``TYPED_TEST_P``) out of comment/string-blanked source,
    or, for a ``.py`` path, the pytest ``def``/``class`` the nodeid names;
  * the **Harness property that missed it** field is filled;
  * no unfilled ``<...>`` / ``TODO`` / ``TBD`` placeholder tokens remain;
  * every citation inside ``## Prevention / standing control`` RESOLVES -- see
    "Prevention-section resolution" below (GitHub #203).

Follow-up phase 41 closed the blind spot in the third bullet: before it, the
checker verified only that the FILE existed, so a record could cite
``tests/test_foo.cpp::Suite.TotallyMadeUpCase`` and pass a CI step whose name
claims it "names real tests" (R-26). Resolution now FAILS CLOSED -- a citation
with no ``::Suite.Case``, an unreadable file, an unsupported file type, an
unresolved case, or an ambiguous (duplicated) case all exit 1.

``TEMPLATE.md`` and ``README.md`` are excluded (the template legitimately holds
placeholders). Exit 0 = all records conform (or none exist yet); 1 = a record
violates the contract; 2 = usage/IO error.

Usage:
  python tools/gate/check_dca.py [--dca-dir docs/audit/dca] [--repo-root .]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List, Optional, Set, Tuple

# Reuse -- do NOT re-implement -- the length-preserving comment/string blanker
# added in F37.  A third variant of this logic is a third place for it to be
# subtly wrong.
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))
from check_magnitude_guard import blank_comments_and_strings  # noqa: E402

EXCLUDED = {"TEMPLATE.md", "README.md"}

REQUIRED_SECTIONS = [
    "## Escaped defect",
    "## Reproducing test (instance)",
    "## Causal analysis",
    "## Class-level test",
    "## Prevention / standing control",
]

# Machine-checked bold-label fields. Use [ \t]* (not \s*) before the value so an
# EMPTY field cannot let the match cross the newline and greedily capture the
# next section heading as if it were the value.
FIELD_REPRO = re.compile(r"\*\*Reproducing test:\*\*[ \t]*`([^`]+)`")
FIELD_CLASS = re.compile(r"\*\*Class-level test:\*\*[ \t]*`([^`]+)`")
FIELD_PROP = re.compile(r"\*\*Harness property that missed it:\*\*[ \t]*(\S.*)")

# GitHub #252. TEMPLATE.md has carried **Severity** and **Escaped to** since the
# first record, and this checker validated neither -- so a record could omit
# both and still report OK. Three did. "Escaped to" is the more load-bearing of
# the pair: it is the difference between a defect a test caught and one a USER
# caught, which is the whole point of keeping these records.
FIELD_SEVERITY = re.compile(r"\*\*Severity:\*\*[ \t]*(\S.*)")
FIELD_ESCAPED = re.compile(r"\*\*Escaped to:\*\*[ \t]*(\S.*)")

# Enforced vocabulary, so a record grading itself "Cosmetic" cannot sail
# through. TEMPLATE.md lists only Critical/High/Medium, but "Low" is used by
# SEVEN committed records -- 0004, 0007, 0010, 0049, 0074, 0075, 0077 -- and has
# been since early on. The template was narrower than the practice; widening the
# checker to match is the honest reconciliation, because rewriting historical
# severities to satisfy a list nothing had ever enforced would be editing the
# record to fit the tool. TEMPLATE.md is updated to say so.
#
# This comment said "ten" until the phase-4 claim-check gate counted. Ten was
# the number of records this checker FAILED before the fix: seven using Low,
# plus 0083/0084/0085 which omitted the fields entirely. Two different
# populations, one number -- the same denominator-with-the-wrong-scope error
# that docs/audit/test-pdfs-corpus-facts.md was written to stop.
SEVERITIES = frozenset(("Critical", "High", "Medium", "Low"))

PLACEHOLDER = re.compile(r"<[^>\n]+>|\bTODO\b|\bTBD\b")

# Code spans and fenced blocks are stripped before the placeholder scan.
#
# WHY: the placeholder rule looks for `<...>` left over from TEMPLATE.md, but
# `<...>` is also ordinary C++ -- `std::vector<char>`, `<size_t>`, `Vec<T>` --
# and a DCA record about C++ is going to contain some. Two real records
# (0032, 0033) were rejected for `<size_t>`, `<char>` and `<int>` appearing
# inside backticks, which reddened `fast-checks` on correct content.
#
# A guard that fires on correct input is worse than no guard: it trains people
# to ignore it, and it makes the one genuine failure indistinguishable from the
# noise. An unfilled template placeholder always sits in PROSE (that is what
# copying TEMPLATE.md leaves behind), so ignoring code is precise rather than
# permissive -- it removes false positives without weakening the real check.
_FENCED = re.compile(r"^```.*?^```", re.S | re.M)
_INLINE_CODE = re.compile(r"``[^`]*``|`[^`\n]*`")


def strip_code(text: str) -> str:
    """Blank out fenced blocks and inline code spans, preserving line count."""
    def blank(m: "re.Match[str]") -> str:
        return re.sub(r"[^\n]", " ", m.group(0))
    return _INLINE_CODE.sub(blank, _FENCED.sub(blank, text))


def _test_path(field_value: str) -> str:
    """Return just the file path from a ``path::Suite.Case`` test reference."""
    return field_value.split("::", 1)[0].strip()


# ---------------------------------------------------------------------------
# GTest case resolution
# ---------------------------------------------------------------------------
#
# Macro survey across tests/**/*.cpp (phase 41): TEST_F (3420), TEST (1678),
# TEST_P (18), INSTANTIATE_TEST_SUITE_P (13).  No TYPED_TEST/TYPED_TEST_P and no
# test-defining wrapper macros exist in this repo today; the typed forms are
# accepted anyway so that adding one does not silently produce a FALSE FAILURE
# on an honest record -- a checker that cannot see a legitimate macro is worse
# than the blind spot being closed here.
#
# INSTANTIATE_TEST_SUITE_P is deliberately NOT a definition site: it names a
# suite, never a case.  A TEST_P case runs under the instantiated name
# ``Prefix/Suite.Case/0``, but the DCA field records the SOURCE-LEVEL identity
# of the test, which is what a reader greps for -- so ``Suite.Case`` resolves
# against a TEST_P, and a citation written in the runtime form is normalised to
# the source form before matching (see _split_case).
_GTEST_DEF = re.compile(
    r"\b(TEST_F|TEST_P|TYPED_TEST_P|TYPED_TEST|TEST)\s*\(\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
    re.S,
)

_CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".inl"}

# pytest nodeid tail: a ``def name(`` or ``class name`` at any indentation.
_PY_DEF = re.compile(r"^[ \t]*(?:async[ \t]+)?(?:def|class)[ \t]+([A-Za-z_]\w*)",
                     re.M)


def _read_source(path: Path) -> str:
    """Read a source file, normalising CRLF (core.autocrlf=true in this repo).

    Raises OSError/UnicodeDecodeError to the caller -- resolution FAILS CLOSED
    rather than treating an unreadable file as 'no problems found'.
    """
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def parse_gtest_cases(path: Path) -> Set[str]:
    """Return every ``Suite.Case`` defined by a GTest macro in ``path``.

    Comments and string/char literals are blanked first (length-preserving), so
    a commented-out or quoted ``TEST(A, B)`` does not resolve a citation.
    Whitespace and newlines inside the macro call are tolerated -- ``TEST_F(\\n
    Suite,\\n Case)`` is legal C++ and must resolve.
    """
    blanked = blank_comments_and_strings(_read_source(path))
    return {f"{m.group(2)}.{m.group(3)}" for m in _GTEST_DEF.finditer(blanked)}


def _count_gtest_case(path: Path, suite_case: str) -> int:
    """How many macro definitions in ``path`` produce ``suite_case``."""
    blanked = blank_comments_and_strings(_read_source(path))
    return sum(1 for m in _GTEST_DEF.finditer(blanked)
               if f"{m.group(2)}.{m.group(3)}" == suite_case)


def _split_case(field_value: str) -> Optional[str]:
    """Return the ``Suite.Case`` part of a citation, or None if absent.

    A parameterised test's RUNTIME name is ``Prefix/Suite.Case/0``; that form is
    normalised back to ``Suite.Case`` so both spellings resolve identically.
    """
    if "::" not in field_value:
        return None
    tail = field_value.split("::", 1)[1].strip()
    if not tail:
        return None
    return tail


def _normalise_case(tail: str) -> str:
    """Strip an instantiation prefix and/or a ``/N`` parameter index."""
    core = tail.split("/")
    if len(core) > 1:
        # ``Prefix/Suite.Case`` or ``Prefix/Suite.Case/0`` -> the dotted part.
        dotted = [seg for seg in core if "." in seg]
        if len(dotted) == 1:
            return dotted[0]
    return tail


def resolve_case(label: str, field_value: str, repo_root: Path) -> List[str]:
    """Verify ``path::Suite.Case`` names a test that really exists.

    Returns a list of problems (empty == resolved).  Every abnormal outcome --
    missing ``::``, unreadable file, unsupported file type, unresolved case,
    duplicate case -- is a problem, never a silent pass.
    """
    problems: List[str] = []
    rel = _test_path(field_value)
    target = repo_root / rel
    if not target.exists():
        return [f"'{label}' names a test file that does not exist: {rel}"]

    tail = _split_case(field_value)
    if tail is None:
        return [f"'{label}' cites `{field_value}` with no `::Suite.Case` -- the "
                f"field must name the test, not just the file "
                f"(expected: `{rel}::Suite.Case`)"]

    suffix = target.suffix.lower()
    if suffix == ".py":
        try:
            src = _read_source(target)
        except (OSError, UnicodeDecodeError) as exc:
            return [f"'{label}' cites {rel} but it could not be read: {exc}"]
        # pytest nodeid: file::[Class::]test_name -- check the last segment.
        name = tail.split("::")[-1].split("[")[0].strip()
        if name not in set(_PY_DEF.findall(src)):
            problems.append(
                f"'{label}' cites `{name}` in {rel} but no `def {name}` / "
                f"`class {name}` exists in that file")
        return problems

    if suffix not in _CPP_SUFFIXES:
        return [f"'{label}' cites {rel}, whose type ({suffix or 'no suffix'}) "
                f"is not one this checker can resolve a `Suite.Case` in; cite a "
                f"C++ GTest file or a pytest .py file"]

    suite_case = _normalise_case(tail)
    if not re.fullmatch(r"[A-Za-z_]\w*\.[A-Za-z_]\w*", suite_case):
        return [f"'{label}' cites `{tail}` in {rel}, which is not a parseable "
                f"`Suite.Case` (expected e.g. `MySuite.MyCase`)"]

    try:
        n = _count_gtest_case(target, suite_case)
    except (OSError, UnicodeDecodeError) as exc:
        return [f"'{label}' cites {rel} but it could not be read: {exc}"]

    if n == 0:
        problems.append(
            f"'{label}' cites `{suite_case}` but no TEST/TEST_F/TEST_P/"
            f"TYPED_TEST defining it exists in {rel}")
    elif n > 1:
        problems.append(
            f"'{label}' cites `{suite_case}` but {n} macro definitions in "
            f"{rel} produce that name -- ambiguous")
    return problems


# ---------------------------------------------------------------------------
# Prevention-section resolution (GitHub #203)
# ---------------------------------------------------------------------------
#
# BEFORE THIS: `## Prevention / standing control` was required to be PRESENT and
# nothing inside it was resolved against anything -- not a `Suite.Case`, not a
# `tools/gate/*.json` key. A Prevention section is the ONE place in this repo
# that only describes STANDING CONTROLS, so an unresolvable citation there is a
# control the reader believes exists and does not. DCA 0001 named a mutation
# seed in `tools/gate/mutants.json` that has never existed; that is the shape
# this closes.
#
# PRECISION IS THE WHOLE PROBLEM. Prose contains backticked things that are not
# test names -- `Document.cpp`, `MANIFEST.sha256`, `Qt6Svg.dll`, `CMakeLists.txt`,
# `std::vector<char>`, `LtvStatus::LtvValidated`, `pdf_dict_get`. A guard that
# fires on correct input is worse than no guard (see strip_code()'s note), so a
# token is treated as a CITATION only when it is unambiguously one:
#
#   (a) `path::Suite.Case` -- and only when `path` ends in a source suffix this
#       checker can actually parse. `Qt::CaseInsensitive` and
#       `pdfe::core::detail::foo` are NOT citations and are ignored.
#   (b) a bare `Suite.Case` where BOTH halves begin with an upper-case letter.
#       That single rule removes every file name in the corpus (`.cpp`, `.h`,
#       `.md`, `.txt`, `.dll`, `.sha256` all start lower-case) without removing
#       any real GTest name, whose halves are CamelCase by house convention.
#       A known-extension denylist backs it up for a hypothetical `Foo.PDF`.
#
# A bare `Suite.Case` is resolved against the GTest macro definitions present
# anywhere under `tests/` (the same _GTEST_DEF parse, over comment/string-blanked
# source, that the Reproducing/Class-level fields use). Unlike those fields it is
# NOT an error for the same name to be defined twice: the Prevention claim is
# "this control exists", and existence is what is checked. Zero definitions is
# the failure.
#
# HISTORICAL ADDENDA ARE EXEMPT. A dated `**Correction, 2026-08-09 ...**` /
# `**Corrected 2026-08-14 ...**` paragraph is narrative ABOUT THE PAST: it
# deliberately quotes test names as they were spelled then. DCA 0014 does
# exactly this for four CRL tests that have since been renamed. Resolving those
# would redden an honest record for saying true things about history, so a
# blank-line-separated block whose leading bold run names a correction/addendum
# AND carries an ISO date is skipped. The date is required so the exemption
# cannot be claimed by writing the word "Note" in front of a live claim.

PREVENTION_HEADING = "## Prevention / standing control"

# Tokens that are deliberately generic -- the format being described, not a test.
_METASYNTACTIC = {
    "Suite.Case", "Suite.Instance", "Suite.Class",
    "MySuite.MyCase", "Foo.Bar", "Prefix.Case",
}

# Belt-and-braces behind the "both halves CamelCase" rule.
_NOT_A_CASE_SUFFIX = {
    "cpp", "cc", "cxx", "c", "h", "hpp", "hxx", "inl", "py", "md", "json",
    "txt", "yml", "yaml", "xml", "dll", "exe", "pdb", "sha256", "cmake",
    "ps1", "sh", "bat", "ini", "log", "csv", "pdf", "png", "svg", "qrc",
}

_BACKTICKED = re.compile(r"`([^`\n]+)`")
_BARE_CASE = re.compile(r"[A-Z][A-Za-z0-9_]*\.[A-Z][A-Za-z0-9_]*")
_CITABLE_PATH = re.compile(r"\.(?:cpp|cc|cxx|c|h|hpp|hxx|inl|py)$", re.I)
_GATE_JSON = re.compile(r"tools/gate/[A-Za-z0-9_.\-]+\.json")
_PLAIN_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_\-]*")
_ISO_DATE = re.compile(r"\b20\d\d-\d\d-\d\d\b")
_ADDENDUM_LEAD = re.compile(
    r"^[ \t>]*(?:(?:[-*+]|\d+\.)[ \t]+)?\*\*("
    r"Correction|Corrected|Addendum|Update|Updated|Superseded|Revised|"
    r"Revision|History|Historical)\b([^*]*)\*\*", re.I)
_BULLET_START = re.compile(r"^[ \t]*(?:[-*+]|\d+\.)[ \t]+")

# Directories under tests/ that are scratch/output, not sources.
_SKIP_DIRS = {"_tmp", "__pycache__", "build", ".git", "node_modules"}

# repo_root -> {Suite.Case}. Built once; the scan walks 80 records.
_CASE_INDEX_CACHE: dict = {}


def tests_case_index(repo_root: Path) -> Set[str]:
    """Every ``Suite.Case`` defined by a GTest macro anywhere under ``tests/``.

    Deterministic and offline: a filesystem walk, comment/string-blanked with
    the same helper the field resolver uses. Unreadable files are SKIPPED here
    (not fatal) because this index is a corpus, not a citation -- a citation
    that fails to resolve because of an unreadable file still fails closed, it
    just fails with "no definition found".
    """
    key = str(repo_root)
    cached = _CASE_INDEX_CACHE.get(key)
    if cached is not None:
        return cached
    cases: Set[str] = set()
    tests_dir = repo_root / "tests"
    if tests_dir.is_dir():
        for p in sorted(tests_dir.rglob("*")):
            if p.suffix.lower() not in _CPP_SUFFIXES or not p.is_file():
                continue
            if any(part in _SKIP_DIRS for part in p.relative_to(tests_dir).parts):
                continue
            try:
                blanked = blank_comments_and_strings(_read_source(p))
            except (OSError, UnicodeDecodeError):
                continue
            for m in _GTEST_DEF.finditer(blanked):
                cases.add("%s.%s" % (m.group(2), m.group(3)))
    _CASE_INDEX_CACHE[key] = cases
    return cases


def prevention_section(text: str) -> str:
    """Return the body of the Prevention section ('' when absent).

    CRLF is normalised FIRST. ``core.autocrlf=true`` in this repo, so a CI
    checkout on Windows hands these records ``\\r\\n`` on disk. The block split
    below is on a blank line, and ``\\r\\n\\r\\n`` does not match
    ``\\n[ \\t]*\\n``: unnormalised, the whole section collapses into ONE block,
    the historical-addendum exemption stops applying, and DCA 0014 reddens.

    Stated honestly: ``check_record`` reads with Python's UNIVERSAL NEWLINES, so
    the ``scan()`` path never actually hands this function a ``\\r``. This line
    is therefore defence for the DIRECT callers of ``check_prevention`` /
    ``prevention_section`` (the self-tests, and anything that reads bytes or
    passes text through), not a fix for a live scan failure -- and
    ``test_prevention_resolution_survives_crlf`` exercises it on that path,
    where removing this line does redden it. Do not "prove" it through
    ``scan()``: it cannot fail there, which would make the test vacuous.
    """
    text = text.replace("\r\n", "\n")
    i = text.find(PREVENTION_HEADING)
    if i < 0:
        return ""
    body = text[i + len(PREVENTION_HEADING):]
    m = re.search(r"^## ", body, re.M)
    return body[:m.start()] if m else body


def _is_historical_block(block: str) -> bool:
    """True for a DATED correction/addendum paragraph (narrative about the past)."""
    m = _ADDENDUM_LEAD.match(block.lstrip("\n"))
    return bool(m) and bool(_ISO_DATE.search(m.group(2)))


def _blocks(section: str) -> List[str]:
    return [b for b in re.split(r"\n[ \t]*\n", section) if b.strip()]


def _bullets(block: str) -> List[str]:
    """Split one block into list items (leading prose is its own chunk)."""
    chunks: List[str] = []
    cur: List[str] = []
    for line in block.split("\n"):
        if _BULLET_START.match(line) and cur:
            chunks.append("\n".join(cur))
            cur = [line]
        else:
            cur.append(line)
    if cur:
        chunks.append("\n".join(cur))
    return chunks


def _looks_like_case(tok: str) -> bool:
    if tok in _METASYNTACTIC:
        return False
    if not _BARE_CASE.fullmatch(tok):
        return False
    return tok.split(".", 1)[1].lower() not in _NOT_A_CASE_SUFFIX


def check_prevention(text: str, repo_root: Path) -> List[str]:
    """Resolve the citations inside ``## Prevention / standing control``."""
    section = prevention_section(text)
    if not section.strip():
        return []
    problems: List[str] = []
    index: Optional[Set[str]] = None

    for block in _blocks(section):
        if _is_historical_block(block):
            continue
        for bullet in _bullets(block):
            toks = _BACKTICKED.findall(bullet)

            # (a) test citations
            for tok in toks:
                tok = tok.strip()
                if "::" in tok:
                    head = tok.split("::", 1)[0].strip()
                    if not _CITABLE_PATH.search(head):
                        continue  # `Qt::CaseInsensitive`, `ns::func` -- not a citation
                    problems.extend(
                        resolve_case("Prevention", tok, repo_root))
                    continue
                if not _looks_like_case(tok):
                    continue
                if index is None:
                    index = tests_case_index(repo_root)
                if tok not in index:
                    problems.append(
                        "'Prevention / standing control' cites `%s` but no "
                        "TEST/TEST_F/TEST_P/TYPED_TEST defining it exists "
                        "anywhere under tests/ -- the standing control it "
                        "promises does not exist" % tok)

            # (b) tools/gate/*.json seed ids / keys
            jsons = _GATE_JSON.findall(bullet)
            if not jsons:
                continue
            named = set(jsons)
            idents = [t.strip() for t in toks
                      if _PLAIN_IDENT.fullmatch(t.strip())
                      and t.strip() not in named]
            if not idents:
                continue
            for rel in sorted(named):
                target = repo_root / rel
                if not target.exists():
                    problems.append(
                        "'Prevention / standing control' names %s, which does "
                        "not exist" % rel)
                    continue
                try:
                    blob = _read_source(target)
                except (OSError, UnicodeDecodeError) as exc:
                    problems.append(
                        "'Prevention / standing control' names %s but it could "
                        "not be read: %s" % (rel, exc))
                    continue
                for ident in idents:
                    if ident not in blob:
                        problems.append(
                            "'Prevention / standing control' cites `%s` "
                            "alongside %s, but that identifier does not appear "
                            "in that file -- the control it names is not "
                            "registered" % (ident, rel))
    return problems


def check_record(path: Path, repo_root: Path) -> List[str]:
    """Return a list of problems for one DCA record (empty == conforms)."""
    problems: List[str] = []
    text = path.read_text(encoding="utf-8", errors="replace")

    # 1. Mandatory sections.
    for sec in REQUIRED_SECTIONS:
        if sec not in text:
            problems.append(f"missing mandatory section '{sec}'")

    # 2. Reproducing + class-level test fields must exist, point at real files,
    #    AND name a Suite.Case that really exists inside those files.
    for label, rx in (("Reproducing test", FIELD_REPRO),
                      ("Class-level test", FIELD_CLASS)):
        m = rx.search(text)
        if not m:
            problems.append(f"missing or malformed '{label}' field "
                            f"(expected: - **{label}:** `path::Suite.Case`)")
            continue
        problems.extend(resolve_case(label, m.group(1), repo_root))

    # 3. Harness-property field must be filled.
    mp = FIELD_PROP.search(text)
    if not mp or not mp.group(1).strip():
        problems.append("missing or empty 'Harness property that missed it' field")

    # 3b. Severity and Escaped-to must be present (GitHub #252). Both are in
    #     TEMPLATE.md and neither was checked, so three records omitted them
    #     and still reported OK.
    ms = FIELD_SEVERITY.search(text)
    if not ms or not ms.group(1).strip():
        problems.append(
            "missing or empty 'Severity' field "
            "(expected: - **Severity:** %s)" % " | ".join(sorted(SEVERITIES)))
    else:
        got = ms.group(1).strip().split()[0].strip("*_`.,;")
        if got not in SEVERITIES:
            problems.append(
                "Severity %r is not one of %s"
                % (got, ", ".join(sorted(SEVERITIES))))

    me = FIELD_ESCAPED.search(text)
    if not me or not me.group(1).strip():
        problems.append(
            "missing or empty 'Escaped to' field (expected: - **Escaped to:** "
            "build / user / soak-run -- where it surfaced instead of a test)")

    # 4. Citations inside the Prevention section must resolve (GitHub #203).
    problems.extend(check_prevention(text, repo_root))

    # 5. No leftover placeholders — in PROSE. See strip_code().
    for ph in PLACEHOLDER.findall(strip_code(text)):
        problems.append(f"unfilled placeholder present: {ph!r}")

    return problems


def scan(dca_dir: Path, repo_root: Path) -> Tuple[bool, List[str]]:
    msgs: List[str] = []
    if not dca_dir.exists():
        msgs.append(f"[check-dca] DCA dir not found: {dca_dir} (no records yet)")
        return True, msgs
    records = sorted(p for p in dca_dir.glob("*.md") if p.name not in EXCLUDED)
    if not records:
        msgs.append(f"[check-dca] no DCA records under {dca_dir} yet (OK)")
        return True, msgs

    ok = True
    for rec in records:
        problems = check_record(rec, repo_root)
        if problems:
            ok = False
            msgs.append(f"[check-dca] FAIL {rec.name}:")
            msgs.extend(f"    - {p}" for p in problems)
        else:
            msgs.append(f"[check-dca] OK   {rec.name}")
    return ok, msgs


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo-root", type=Path, default=Path("."))
    ap.add_argument("--dca-dir", type=Path, default=None,
                    help="DCA records dir (default <repo-root>/docs/audit/dca).")
    args = ap.parse_args(argv)

    repo_root = args.repo_root.resolve()
    dca_dir = args.dca_dir or (repo_root / "docs" / "audit" / "dca")
    if not dca_dir.is_absolute():
        dca_dir = repo_root / dca_dir

    ok, msgs = scan(dca_dir, repo_root)
    for m in msgs:
        print(m)
    if not ok:
        print("[check-dca] one or more DCA records violate the contract "
              "(see docs/audit/dca/README.md).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
