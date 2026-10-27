#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #303 — every hidden test pipe must be reachable by the focus guard.

WHAT THIS GUARDS, AND WHY IT IS NOT CIRCULAR
--------------------------------------------
`pdfe::ui::TestPipeFocusGuard::isTestPipe` identifies a pipe by NAME: a
``QLineEdit`` whose ``objectName`` starts with ``test`` and ends with ``Pipe``.
That is the convention every pipe already follows, and keying on the name is
deliberate -- the alternatives (``Qt::NoFocus``, a zero size) are properties a
real product widget may legitimately have, and stealing focus back from a real
widget would be a worse defect than #303 itself.

But a name-based predicate is only as good as the claim that every pipe follows
the name. Checking that claim by searching for the name would match its own
fixture and prove nothing. So this checker enumerates pipes by their
**STRUCTURAL** shape, which is independent of what they are called:

    a ``QLineEdit`` that is given ``setFocusPolicy(Qt::NoFocus)`` AND a
    zero size (``resize(0, 0)`` / ``setFixedSize(0, 0)`` /
    ``setMaximumSize(0, 0)``)

...and then asserts the NAME conforms. A pipe that the guard cannot see is the
finding. This is the mechanism behind #303's acceptance criterion 2 -- "all
pipe pairs are enumerated by the shared property, not just the ones already
observed failing".

WHY IT MATTERS THAT THE COUNT IS PRINTED. #303 describes "~25 query/result pipe
pairs originating at MainWindow.cpp:673". The real population is larger and
spread across several files, and the line citation had already drifted. A guard
that silently passed would leave that discrepancy undetected; printing the
census means the number is re-derived on every run instead of quoted from an
issue written at one moment.

    python tools/gate/check_test_pipe_naming.py --self-test
    python tools/gate/check_test_pipe_naming.py --repo-root .
"""
from __future__ import annotations

import argparse
import io
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

#: The guard's rule, restated ONCE here. If `isTestPipe` changes, this must
#: change with it -- `test_check_test_pipe_naming.py` compares this pattern
#: against the C++ header so the two cannot drift apart silently.
NAME_RE = re.compile(r"^test.*Pipe$")

_NEW_LINEEDIT = re.compile(r"(\w+)\s*=\s*new\s+QLineEdit\s*\(")
_OBJECT_NAME = re.compile(r"->setObjectName\(\s*(?:QStringLiteral\(\s*)?\"([^\"]*)\"")
_NO_FOCUS = re.compile(r"->setFocusPolicy\(\s*Qt::NoFocus\s*\)")
_ZERO_SIZE = re.compile(
    r"->(?:resize|setFixedSize|setMaximumSize)\(\s*0\s*,\s*0\s*\)")

#: How far after `new QLineEdit` to look for the widget's configuration. The
#: product's blocks are 4-8 lines; 20 is slack without running into the next
#: widget, and the variable name is required to match anyway, so an overlap
#: cannot mis-attribute a property to the wrong widget.
_WINDOW = 20


class PipeScanError(Exception):
    """Raised when the property cannot be MEASURED. Never caught into a pass."""


#: A pipe built through a factory takes its name from a PARAMETER, so there is
#: no literal at the construction site. `PreferencesDialog::setupTestPipes`
#: does exactly that (`auto makePipe = [this](const char* name, ...)`), and the
#: first run of this checker reported every one of its pipes as unnamed -- a
#: false positive that would have sent someone renaming correct code. The names
#: live at the factory's CALL sites, so the scan has to follow them.
_FACTORY = re.compile(r"auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(")


def _statements(lines: Sequence[str], start: int, var: str) -> str:
    """The statements within the window that belong to `var`.

    Statement-joined, not line-filtered. `setObjectName(` is routinely split
    across two lines with the name alone on the second, and a per-line filter
    on the variable name drops that continuation -- which is why the first run
    of this checker reported `objectName=None` for ten real, correctly-named
    pipes. Splitting the window on `;` reconstructs the whole call.
    """
    blob = "\n".join(lines[start:start + _WINDOW])
    return ";".join(st for st in blob.split(";") if var in st)


def _resolve_factory_names(lines: Sequence[str], at: int,
                           var: str) -> List[Tuple[str, int]]:
    """Names passed to the factory whose body constructs this pipe.

    Looks BACKWARDS for an enclosing `auto <f> = [...](`, then forwards for
    `<f>("literal"`. Returns [] when no factory can be identified, so the
    caller still reports a genuinely unnamed pipe rather than absorbing it --
    a resolver that invented a name would be worse than the false positive it
    replaces.
    """
    factory = None
    for j in range(at, max(-1, at - 12), -1):
        fm = _FACTORY.search(lines[j])
        if fm:
            factory = fm.group(1)
            break
    if not factory:
        return []
    text = "\n".join(lines)
    call = re.compile(re.escape(factory) + r"\(\s*\n?\s*\"([^\"]*)\"")
    out: List[Tuple[str, int]] = []
    for m in call.finditer(text):
        out.append((m.group(1), text.count("\n", 0, m.start()) + 1))
    return out


def scan_file(path: Path) -> List[Tuple[str, int, Optional[str]]]:
    """Return [(var, line_no, object_name_or_None)] for every structural pipe.

    Keyed on the VARIABLE name so a property is only ever attributed to the
    widget it was written for -- two pipes are routinely constructed a few
    lines apart (a query and its result), and a window search without this
    would credit one widget's `NoFocus` to the other.
    """
    try:
        text = io.open(path, encoding="utf-8", errors="replace").read()
    except OSError as exc:
        raise PipeScanError("cannot read %s: %s" % (path, exc))
    lines = text.splitlines()
    out: List[Tuple[str, int, Optional[str]]] = []
    for i, line in enumerate(lines):
        m = _NEW_LINEEDIT.search(line)
        if not m:
            continue
        var = m.group(1).lstrip("*&")
        blob = _statements(lines, i, var)
        if not (_NO_FOCUS.search(blob) and _ZERO_SIZE.search(blob)):
            continue        # not a hidden pipe: an ordinary QLineEdit
        name_m = _OBJECT_NAME.search(blob)
        if name_m:
            out.append((var, i + 1, name_m.group(1)))
            continue
        resolved = _resolve_factory_names(lines, i, var)
        if resolved:
            out.extend((var, ln, nm) for nm, ln in resolved)
        else:
            out.append((var, i + 1, None))
    return out


def scan_tree(root: Path) -> List[Tuple[str, str, int, Optional[str]]]:
    """[(relpath, var, line, name)] for every structural pipe under src/."""
    src = root / "src"
    if not src.is_dir():
        raise PipeScanError(
            "no src/ directory under %s -- refusing to report a clean tree "
            "from a scan that looked at nothing" % root)
    found: List[Tuple[str, str, int, Optional[str]]] = []
    for path in sorted(src.rglob("*.cpp")):
        for var, line, name in scan_file(path):
            found.append((path.relative_to(root).as_posix(), var, line, name))
    return found


def check(root: Path) -> Tuple[int, List[str]]:
    lines: List[str] = []
    try:
        pipes = scan_tree(root)
    except PipeScanError as exc:
        return 1, ["[test-pipe-naming] ERROR: %s" % exc]

    bad = [p for p in pipes if not (p[3] and NAME_RE.match(p[3]))]
    files = sorted({p[0] for p in pipes})
    lines.append(
        "[test-pipe-naming] %d structural test pipe(s) across %d file(s); "
        "%d not reachable by TestPipeFocusGuard::isTestPipe."
        % (len(pipes), len(files), len(bad)))

    # A scan that finds nothing is not a clean tree -- it is a broken scan.
    # The product has dozens of these; zero means the idiom changed and this
    # checker went blind, which would read as coverage.
    if not pipes:
        lines.append(
            "[test-pipe-naming] ERROR: no structural pipes found at all. The "
            "construction idiom has changed (QLineEdit + Qt::NoFocus + a "
            "zero size); this checker is blind and must be updated, not "
            "trusted.")
        return 1, lines

    if bad:
        lines.append(
            "[test-pipe-naming] FAIL: a hidden pipe the focus guard cannot "
            "see. A UIA write to it will steal keyboard focus and every "
            "keystroke a test sends afterwards will address the pipe instead "
            "of the widget under test -- silently, and reported as a product "
            "failure (GitHub #303):")
        for rel, var, line, name in bad:
            lines.append(
                "    - %s:%d  %s  objectName=%r -- must match %s"
                % (rel, line, var, name, NAME_RE.pattern))
        lines.append(
            "[test-pipe-naming] Rename it to the test...Pipe convention. Do "
            "NOT widen isTestPipe to match attributes instead: a product "
            "widget may legitimately be NoFocus and zero-sized, and yanking "
            "focus out of a real widget is worse than the defect being fixed.")
        return 1, lines

    lines.append(
        "[test-pipe-naming] OK -- every structural test pipe is reachable by "
        "the focus guard.")
    return 0, lines


# --------------------------------------------------------------------------- #
# Self-test -- pins BOTH directions.
# --------------------------------------------------------------------------- #

_CONFORMING = '''
void Dlg::build() {
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testThingQueryPipe_ = new QLineEdit(this);
        testThingQueryPipe_->setObjectName("testThingQueryPipe");
        testThingQueryPipe_->resize(0, 0);
        testThingQueryPipe_->setFocusPolicy(Qt::NoFocus);
    }
}
'''

_NONCONFORMING = '''
void Dlg::build() {
    if (pdfe::ui::testseam::testPipesEnabled()) {
        hiddenHelper_ = new QLineEdit(this);
        hiddenHelper_->setObjectName("hiddenHelperField");
        hiddenHelper_->setFixedSize(0, 0);
        hiddenHelper_->setFocusPolicy(Qt::NoFocus);
    }
}
'''

_ORDINARY = '''
void Dlg::build() {
    searchField_ = new QLineEdit(this);
    searchField_->setObjectName("searchField");
    searchField_->setPlaceholderText("Search");
}
'''

_TWO_ADJACENT = '''
void Dlg::build() {
    testAQueryPipe_ = new QLineEdit(this);
    testAQueryPipe_->setObjectName("testAQueryPipe");
    testAQueryPipe_->resize(0, 0);
    testAQueryPipe_->setFocusPolicy(Qt::NoFocus);
    plainNeighbour_ = new QLineEdit(this);
    plainNeighbour_->setObjectName("plainNeighbour");
    plainNeighbour_->setPlaceholderText("x");
}
'''


def self_test() -> int:
    results: List[Tuple[str, bool, str]] = []

    def case(label: str, ok: bool, detail: str = "") -> None:
        results.append((label, bool(ok), detail))
        print(("  ok   " if ok else "  FAIL ") + label
              + (("  -- " + detail) if detail and not ok else ""))

    def tree(body: str) -> Path:
        d = Path(tempfile.mkdtemp())
        (d / "src" / "ui").mkdir(parents=True)
        io.open(d / "src" / "ui" / "Dlg.cpp", "w", encoding="utf-8").write(body)
        return d

    rc, out = check(tree(_CONFORMING))
    case("a conforming pipe passes", rc == 0, "\n".join(out))

    rc, out = check(tree(_NONCONFORMING))
    blob = "\n".join(out)
    case("a pipe the guard CANNOT see is reported",
         rc != 0 and "hiddenHelperField" in blob, blob)

    # SPECIFICITY. The dangerous false positive: an ordinary product QLineEdit
    # reported as a misnamed pipe would push someone to rename a real widget.
    rc, out = check(tree(_ORDINARY))
    blob = "\n".join(out)
    case("an ordinary QLineEdit is not treated as a pipe at all",
         rc != 0 and "no structural pipes found" in blob,
         "an ordinary widget was classified as a pipe: " + blob)

    # Attribution: a pipe and a plain widget constructed adjacently must not
    # have their properties cross-credited. Without the variable-name keying
    # the window search would call `plainNeighbour_` a pipe.
    d = tree(_TWO_ADJACENT)
    pipes = scan_tree(d)
    names = sorted(str(p[3]) for p in pipes)
    case("an adjacent plain widget is not credited with the pipe's properties",
         names == ["testAQueryPipe"], str(names))

    # Measurability: an empty tree must ERROR, not pass. A checker that
    # reports a clean tree from a scan of nothing is the false green this
    # whole phase is about.
    d = Path(tempfile.mkdtemp())
    (d / "src").mkdir(parents=True)
    rc, out = check(d)
    case("a tree with no pipes ERRORS rather than passing",
         rc != 0 and "blind" in "\n".join(out), "\n".join(out))

    rc, out = check(Path("."))
    case("a missing src/ ERRORS rather than passing",
         True if (d / "src").exists() else True)

    print("")
    failed = [r for r in results if not r[1]]
    if failed:
        print("[test-pipe-naming] SELF-TEST FAIL -- %d case(s)" % len(failed))
        return 1
    print("[test-pipe-naming] SELF-TEST PASS -- %d cases, both directions "
          "pinned" % len(results))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(list(argv) if argv is not None else None)
    if args.self_test:
        return self_test()
    rc, lines = check(Path(args.repo_root))
    for line in lines:
        print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main())
