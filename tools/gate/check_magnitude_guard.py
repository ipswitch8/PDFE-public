#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mechanised sweep for the "magnitude guard" test anti-pattern (PERF-02 / F37).

WHAT THIS IS
------------
F27 found two tests whose only comparative assertion sat inside a timing guard
that is permanently false on modern hardware::

    if (small > 0.02) {              // 50-page baseline measures ~0.0145s
        EXPECT_LT(big / small, 15.0);
    }

The assertion never executed. The suite reported a passing scaling test that
measured nothing. Both instances were fixed in d85ec1d; the sweep that found
the second one was described in prose and thrown away. This file is that sweep,
committed, so the anti-pattern cannot silently return.

THE CRITERION, verbatim from
docs/audit/meta-audit-2026-07/followup/deferred-backlog.md (row PERF-02):

    a timing or resource threshold that can be permanently false on fast
    hardware, gating a comparative assertion.

    Implemented as a brace-aware scanner over tests/**/*.cpp (comments and
    string literals blanked, `if (...)` conditions matched for a numeric-literal
    comparison, guarded block searched for EXPECT_/ASSERT_) -- shape, not
    phrasing, because a phrasing grep would have missed both instances (their
    comments differ).

That is stage 1: SHAPE. Shape alone over-collects -- F27 recorded 19 shape
matches of which 2 were the real pattern -- so stage 2 applies the rule the
backlog states immediately below the row:

    A guard on measurement AVAILABILITY is legitimate -- the alternative is
    asserting on a number you do not have. A guard on measurement MAGNITUDE
    silently disables the oracle on fast hardware, and gets faster every year.

Mechanised as two exemptions, both textual and both checkable:

  * ZERO-THRESHOLD. Every numeric literal taking part in a comparison in the
    condition is zero (`if (baselineMB > 0)`, `if (idx != 0)`). "Do I have a
    measurement at all" is availability, not magnitude, and cannot become
    permanently false by hardware getting faster.
  * BOTH-PATHS. An `else` branch exists and also contains an EXPECT_/ASSERT_
    (`tests/test_high_contrast.cpp` `if (ratio >= 4.5) ... else ...`). No path
    is silent, so nothing is disabled.

Anything else is SUSPECT and must be adjudicated by a human, once, into the
committed baseline (magnitude_guard_baseline.json) with a written reason. That
is what makes the "recorded as checked, not fixed" disposition durable instead
of a sentence in a document.

EXIT CODES
----------
  0  no unreviewed suspect guard
  1  a suspect guard is not in the baseline, or a baseline entry is stale
  2  usage / IO error

The baseline is keyed on (file, normalised condition, ordinal) -- NOT on line
numbers and NOT on raw bytes. `core.autocrlf=true` in this repo rewrites line
endings on checkout, so every comparison here runs on CRLF-normalised text; and
line numbers move whenever anything above a guard is edited, which would make
the baseline rot for reasons that have nothing to do with the anti-pattern.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Dict, Iterable, List, Optional, Tuple

# THE GUARD CRASHED WHILE REPORTING A VIOLATION. Found 2026-10-07 while making
# this scanner skip-aware (#274): a suspect condition containing a non-ASCII
# character (U+2265, the '>=' sign, in a test's own source) raised
# UnicodeEncodeError from cp1252 on the `print("VIOLATION ...")` line. The sweep
# therefore died with a traceback at the exact moment it had something to say,
# and the non-zero exit came from the crash rather than from the finding -- a
# guard that cannot print its own result is a guard that does not report.
# Same idiom as audit_gate_artifacts.py:85.
try:  # pragma: no cover
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

# --------------------------------------------------------------------------
# Stage 0 -- blanking
# --------------------------------------------------------------------------

def blank_comments_and_strings(src: str) -> str:
    """Replace comment and string/char-literal bodies with spaces.

    Length and newline positions are preserved exactly, so offsets and line
    numbers computed on the blanked text are valid for the original text.
    Handles // , /* */, "..." (with \\ escapes), '...', and C++11 raw strings
    R"delim( ... )delim".
    """
    out = list(src)
    i = 0
    n = len(src)

    def blank(start: int, end: int) -> None:
        for k in range(start, min(end, n)):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif c in ('"', "'"):
            # Raw string?  R"delim( ... )delim"
            if c == '"' and i > 0 and src[i - 1] == "R":
                m = re.match(r'"([^()\\ ]{0,16})\(', src[i:])
                if m:
                    delim = m.group(1)
                    close = ')' + delim + '"'
                    j = src.find(close, i + m.end())
                    j = n if j < 0 else j + len(close)
                    blank(i + 1, j - 1)
                    i = j
                    continue
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == c:
                    j += 1
                    break
                if src[j] == "\n":  # unterminated; bail at EOL
                    break
                j += 1
            blank(i + 1, j - 1)
            i = j
        else:
            i += 1
    return "".join(out)


# --------------------------------------------------------------------------
# Stage 1 -- shape
# --------------------------------------------------------------------------

_IF_RE = re.compile(r"\bif\b")
_IDENT_TAIL = re.compile(r"[A-Za-z0-9_]")

# A numeric literal adjacent to a relational/equality operator, on either side.
# `<<` and `>>` (stream/shift) are excluded by the surrounding-character checks
# in `_condition_has_numeric_comparison`, not here.
_NUM = r"(?:0[xX][0-9A-Fa-f]+|(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?[fFuUlL]*)"
_CMP_THEN_NUM = re.compile(r"(<=|>=|==|!=|<|>)\s*[-+]?\s*(" + _NUM + r")\b")
_NUM_THEN_CMP = re.compile(r"\b(" + _NUM + r")\s*(<=|>=|==|!=|<|>)")


def _match_paren(text: str, open_idx: int) -> Optional[int]:
    """Index just past the ')' matching the '(' at open_idx, or None."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return None


def _match_brace(text: str, open_idx: int) -> Optional[int]:
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return None


def _skip_ws(text: str, i: int) -> int:
    n = len(text)
    while i < n and text[i].isspace():
        i += 1
    return i


def _guarded_block(text: str, after_cond: int) -> Tuple[int, int]:
    """Brace-aware extent of the statement guarded by an `if`.

    A braced body is taken to its matching `}`; an unbraced body is taken to
    the first `;` at paren/brace depth 0, so
    `if (elapsedMs > 100) EXPECT_LT(a, b);` is captured too.
    """
    i = _skip_ws(text, after_cond)
    n = len(text)
    if i < n and text[i] == "{":
        end = _match_brace(text, i)
        return i, (end if end is not None else n)
    depth = 0
    j = i
    while j < n:
        ch = text[j]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == ";" and depth <= 0:
            return i, j + 1
        j += 1
    return i, n


def _else_block(text: str, after_body: int) -> Optional[Tuple[int, int]]:
    i = _skip_ws(text, after_body)
    if not text.startswith("else", i):
        return None
    if i + 4 < len(text) and _IDENT_TAIL.match(text[i + 4]):
        return None
    return _guarded_block(text, i + 4)


def _cmp_ok(cond: str, m: re.Match, op_span: Tuple[int, int]) -> bool:
    """Reject `<<`, `>>`, `->`, `<=>`-adjacent noise around the operator."""
    s, e = op_span
    before = cond[s - 1] if s > 0 else ""
    after = cond[e] if e < len(cond) else ""
    op = cond[s:e]
    if op in ("<", ">"):
        if before in "<>-" or after in "<>":
            return False
    return True


def _condition_numeric_literals(cond: str) -> Optional[List[str]]:
    """Literals participating in a comparison, or None if there is no
    numeric-literal comparison at all."""
    lits: List[str] = []
    for m in _CMP_THEN_NUM.finditer(cond):
        if _cmp_ok(cond, m, m.span(1)):
            lits.append(m.group(2))
    for m in _NUM_THEN_CMP.finditer(cond):
        if _cmp_ok(cond, m, m.span(2)):
            lits.append(m.group(1))
    return lits or None


def _literal_is_zero(lit: str) -> bool:
    t = lit.rstrip("fFuUlL")
    try:
        if t.lower().startswith("0x"):
            return int(t, 16) == 0
        return float(t) == 0.0
    except ValueError:
        return False


def normalise_condition(cond: str) -> str:
    return re.sub(r"\s+", " ", cond).strip()


ASSERT_RE = re.compile(r"\b(?:EXPECT_|ASSERT_)")

# GitHub #274 -- THE DOOR THIS SCANNER COULD NOT SEE.
#
# The criterion is "a check that does not run". Until now that was read as "a
# conditional ASSERTION", so a threshold gating a GTEST_SKIP was structurally
# invisible:
#
#     if (user.size() < 3) {
#         GTEST_SKIP() << "the build account name is unavailable or too short";
#     }
#
# Same defect, different door. Under this project's SKIP=FAIL rule it is the
# WORSE form, because it announces "not run" and is then counted as a non-failure
# by every consumer -- the gtest summary, the merged XML, and the skip baseline.
# The instance that mattered was the only tree-wide account-name leak check, and
# it stopped running exactly when the account name was undeterminable.
#
# Why this does not drown the report in the 635 known skip sites: the dominant
# shape is `if (!std::filesystem::exists(fixture)) GTEST_SKIP()`, whose condition
# contains NO numeric literal, so stage 1 never matches it. Only a skip gated on
# a NUMBER reaches classification, which is the magnitude case this file exists
# for.
SKIP_RE = re.compile(r"\bGTEST_SKIP\b")


_COMPOUND_RE = re.compile(r"&&|\|\|")
_RELATIONAL_RE = re.compile(r"(<=|>=|<|>)\s*[-+]?\s*" + _NUM)


class Match:
    __slots__ = ("path", "line", "condition", "ordinal", "classification",
                 "reason", "assert_snippet", "compound", "relational")

    def __init__(self, path, line, condition, ordinal, classification, reason,
                 assert_snippet, compound=False, relational=True):
        self.path = path
        self.line = line
        self.condition = condition
        self.ordinal = ordinal
        self.classification = classification
        self.reason = reason
        self.assert_snippet = assert_snippet
        # Reading-sensitivity attributes.  The recorded criterion says only
        # "`if (...)` conditions matched for a numeric-literal comparison",
        # which admits narrower readings; these let anyone re-measure how much
        # of the count each narrowing costs instead of arguing about it.
        self.compound = compound          # condition contains && or ||
        self.relational = relational      # has < <= > >= against a literal
                                          # (False => only == / != matched)

    def key(self) -> str:
        return "%s::%s::%d" % (self.path, self.condition, self.ordinal)

    def to_json(self) -> Dict[str, object]:
        return {
            "file": self.path,
            "condition": self.condition,
            "ordinal": self.ordinal,
            "classification": self.classification,
            "why": self.reason,
            "line_at_scan_time": self.line,
            "guarded_assert": self.assert_snippet,
        }


def scan_text(path: str, raw: str) -> List[Match]:
    # CRLF normalisation: core.autocrlf=true means the same tracked file has
    # different bytes in different checkouts.  Every offset below is computed
    # on the normalised text so the baseline keys are checkout-independent.
    src = raw.replace("\r\n", "\n").replace("\r", "\n")
    blanked = blank_comments_and_strings(src)

    results: List[Match] = []
    seen_conditions: Dict[str, int] = {}

    for m in _IF_RE.finditer(blanked):
        s = m.start()
        if s > 0 and _IDENT_TAIL.match(blanked[s - 1]):
            continue
        p = _skip_ws(blanked, m.end())
        if p >= len(blanked) or blanked[p] != "(":
            continue
        cond_end = _match_paren(blanked, p)
        if cond_end is None:
            continue
        cond = blanked[p + 1:cond_end - 1]
        lits = _condition_numeric_literals(cond)
        if lits is None:
            continue
        body_start, body_end = _guarded_block(blanked, cond_end)
        body = blanked[body_start:body_end]
        body_asserts = ASSERT_RE.search(body)
        body_skips = SKIP_RE.search(body)
        if not (body_asserts or body_skips):
            continue

        # -- shape matched.  Classify.
        if all(_literal_is_zero(x) for x in lits):
            cls, why = "benign", "zero-threshold: availability guard, not magnitude"
        elif body_skips and not body_asserts:
            # #274. BOTH-PATHS CANNOT EXEMPT A SKIP, and that is the whole point:
            # the exemption means "no path is silent", but the skip path IS the
            # silent one -- it abandons the rest of the test body, not just this
            # branch. So a non-zero threshold gating a skip is suspect whether or
            # not an else exists.
            cls, why = ("suspect",
                        "non-zero threshold gating a GTEST_SKIP: the test stops "
                        "and is counted as a non-failure")
        else:
            eb = _else_block(blanked, body_end)
            if eb is not None and ASSERT_RE.search(blanked[eb[0]:eb[1]]):
                cls, why = "benign", "both-paths: the else branch also asserts"
            else:
                cls, why = "suspect", "non-zero threshold gating the only assertion"

        norm = normalise_condition(cond)
        ordinal = seen_conditions.get(norm, 0)
        seen_conditions[norm] = ordinal + 1

        # Quote whichever disabler actually matched, so a skip-gated finding
        # shows the GTEST_SKIP rather than an unrelated later assertion.
        am = body_asserts or body_skips
        snippet = normalise_condition(
            src[body_start + am.start():body_start + am.start() + 80])

        results.append(Match(path, src.count("\n", 0, s) + 1, norm, ordinal,
                             cls, why, snippet,
                             compound=bool(_COMPOUND_RE.search(cond)),
                             relational=bool(_RELATIONAL_RE.search(cond))))
    return results


def reading_variants(matches: List["Match"]) -> Dict[str, int]:
    """How many shape matches survive each narrower reading of the criterion.

    Reported, never used for pass/fail.  See the F37 dispositions: the F27
    prose recorded "19 shape matches" without recording which reading produced
    it, and the narrowest defensible reading (relational operators only, single
    un-compounded condition) is the one that lands near that number.
    """
    return {
        "as_written": len(matches),
        "relational_only": sum(1 for m in matches if m.relational),
        "simple_condition_only": sum(1 for m in matches if not m.compound),
        "relational_and_simple": sum(1 for m in matches
                                     if m.relational and not m.compound),
    }


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def iter_sources(roots: Iterable[str], repo_root: str) -> List[str]:
    files: List[str] = []
    for root in roots:
        abs_root = root if os.path.isabs(root) else os.path.join(repo_root, root)
        if os.path.isfile(abs_root):
            files.append(abs_root)
            continue
        for dirpath, dirnames, filenames in os.walk(abs_root):
            # `_tmp` is pytest's --basetemp (pytest.ini), which fills with COPIES
            # of real test sources whenever a self-test snapshots the tree. Walking
            # it makes this guard's verdict depend on whether pytest happened to
            # run recently -- the baseline gains phantom entries like
            # `tests/_tmp/<testname>/snap/tests/test_x.cpp` and the
            # baseline-covers-live check reddens. Found by F46: a standing control
            # that is a function of scratch state is not a control.
            # Anchored on the ONE basetemp path, not on the name `_tmp`
            # anywhere: a blanket name prune would silently hide a tracked
            # directory that happened to be called `_tmp` (karen, F46).
            basetemp = os.path.join(repo_root, "tests", "_tmp")
            dirnames[:] = sorted(
                d for d in dirnames
                if d != "__pycache__"
                and os.path.normcase(os.path.join(dirpath, d))
                != os.path.normcase(basetemp))
            for fn in sorted(filenames):
                if fn.endswith(".cpp"):
                    files.append(os.path.join(dirpath, fn))
    return files


def rel(path: str, repo_root: str) -> str:
    try:
        return os.path.relpath(path, repo_root).replace(os.sep, "/")
    except ValueError:
        return path.replace(os.sep, "/")


def load_baseline(path: str) -> Dict[str, Dict[str, object]]:
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    out: Dict[str, Dict[str, object]] = {}
    for entry in data.get("entries", []):
        key = "%s::%s::%d" % (entry["file"], entry["condition"],
                              int(entry.get("ordinal", 0)))
        out[key] = entry
    return out


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--paths", nargs="*", default=["tests"],
                    help="roots to scan (default: tests). Note the default "
                         "deliberately excludes tools/gate/fixtures, which "
                         "contains planted control guards.")
    ap.add_argument("--baseline", default=None,
                    help="committed JSON of adjudicated shape matches")
    ap.add_argument("--allow-stale-baseline", action="store_true",
                    help="do not fail when a baseline entry matches nothing")
    ap.add_argument("--list", action="store_true",
                    help="emit every shape match as JSON and exit 0")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    repo_root = os.path.abspath(args.repo_root)
    matches: List[Match] = []
    for f in iter_sources(args.paths, repo_root):
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                raw = fh.read()
        except OSError as exc:
            sys.stderr.write("error: cannot read %s: %s\n" % (f, exc))
            return 2
        matches.extend(scan_text(rel(f, repo_root), raw))

    if args.list:
        json.dump({"shape_matches": len(matches),
                   "reading_variants": reading_variants(matches),
                   "entries": [m.to_json() for m in matches]},
                  sys.stdout, indent=2, sort_keys=False)
        sys.stdout.write("\n")
        return 0

    suspects = [m for m in matches if m.classification == "suspect"]
    benign = [m for m in matches if m.classification == "benign"]

    baseline: Dict[str, Dict[str, object]] = {}
    if args.baseline:
        bpath = args.baseline
        if not os.path.isabs(bpath):
            bpath = os.path.join(repo_root, bpath)
        try:
            baseline = load_baseline(bpath)
        except (OSError, ValueError, KeyError) as exc:
            sys.stderr.write("error: bad baseline %s: %s\n" % (bpath, exc))
            return 2

    live_keys = {m.key() for m in matches}
    unreviewed = [m for m in suspects if m.key() not in baseline]
    stale = sorted(k for k in baseline if k not in live_keys)

    if not args.quiet:
        print("magnitude-guard sweep (PERF-02 / F37)")
        print("  roots            : %s" % ", ".join(args.paths))
        print("  shape matches    : %d" % len(matches))
        print("    suspect        : %d" % len(suspects))
        print("    benign         : %d" % len(benign))
        rv = reading_variants(matches)
        print("  reading variants : relational_only=%d simple_condition_only=%d"
              " relational_and_simple=%d  (informational; not pass/fail)"
              % (rv["relational_only"], rv["simple_condition_only"],
                 rv["relational_and_simple"]))
        if args.baseline:
            print("  baseline entries : %d" % len(baseline))
        for m in unreviewed:
            print("VIOLATION %s:%d  if (%s)  ->  %s"
                  % (m.path, m.line, m.condition, m.assert_snippet))
        for k in stale:
            print("STALE-BASELINE %s" % k)

    failed = bool(unreviewed) or (bool(stale) and not args.allow_stale_baseline)
    if failed and not args.quiet:
        if unreviewed:
            print("\nFAIL: %d guard(s) gate the only assertion on a non-zero "
                  "threshold and are not in the baseline." % len(unreviewed))
            print("Either fix the test (make the assertion unconditional, or "
                  "assert on both paths), or -- if the guard really is about "
                  "fixture shape rather than measurement magnitude -- add it "
                  "to %s with a written reason." % (args.baseline or
                                                    "the baseline"))
        if stale and not args.allow_stale_baseline:
            print("\nFAIL: %d baseline entry(ies) match nothing in the tree. "
                  "The guard was removed or rewritten: prune the baseline so "
                  "it cannot silently re-accept a different guard later."
                  % len(stale))
    elif not args.quiet:
        print("OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
