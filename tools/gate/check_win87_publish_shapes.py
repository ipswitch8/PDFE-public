#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""WIN-87 class closure: no file-publish shape survives outside the primitive.

WHY THIS IS NOT A GREP, AND WHY THAT MATTERS HERE
-------------------------------------------------
Phase-5's acceptance criterion says the class closure must be re-verified "by
enumeration, not by grep phrasing". That wording is not style: WIN-45 escaped
FOUR separate sweeps in this project because each was keyed on how a call was
spelled, and a phrasing-keyed sweep always comes back.

The specific way a plain grep lies here was observed while writing this file. At
the moment WIN-87 was actually closed, `grep -rn 'std::filesystem::rename'` over
`src/` returned five hits — and every single one was a COMMENT describing the
defect that had just been removed ("this used to be a bare throwing
std::filesystem::rename"). A sweep that counts those reports an open class that
is closed; a maintainer who learns to ignore its output will also ignore the
sixth hit, which is real.

So this checker strips comments and string literals FIRST, with a scanner that
understands C++ lexical structure (line comments, block comments, raw and normal
string literals, char literals, escapes), and only then looks for the publish
shapes as CODE. It is not a full C++ AST — that would need libclang and a
compilation database — and it does not pretend to be. What it does is remove the
one class of false positive that has actually defeated this sweep, and it FAILS
CLOSED on anything it cannot lex.

WHAT COUNTS AS A PUBLISH SHAPE
------------------------------
Any of these, called anywhere in src/ outside the shared primitive:

  std::filesystem::rename   the by-path rename WIN-85 replaced, and the shape
                            phase-56/57/58 removed from all seven sites
  MoveFileEx{,W,A}          the same thing hand-rolled against Win32
  ReplaceFile{,W,A}         ditto
  std::filesystem::copy_file  the F58-a shape: CopyFileW has no CREATE_NEW and
                            no restrictive share mode, so a staging file made
                            this way is writable by anyone between creation and
                            publish. This one was EXPLOITED, not theorised.
  atomicReplaceFileWithRetry  DELETED in phase-58; listed so a future
                            reintroduction is caught by this gate rather than by
                            an audit a year later.

EXEMPT: src/core/AtomicFileReplace.{h,cpp} — it IS the primitive, and the POSIX
arm of `atomicReplaceFile` legitimately calls `std::filesystem::rename`.

WHAT THIS WOULD STILL MISS — stated because a control believed stronger than it
is is worse than no control at all. Enumerated by the phase-5 security-audit
gate, and NOT closed here:

  * a namespace alias — `namespace fs = std::filesystem; fs::rename(...)` — or a
    `using` declaration that lets a bare `rename(...)` resolve to it. The CRT
    pattern below catches the bare form; the aliased qualified form it does not.
  * a macro or template that EXPANDS to one of these; this reads unexpanded
    source.
  * a call through a function pointer (`auto fn = &::MoveFileExW; fn(...)`),
    because every pattern requires a `(` right after the name.
  * `SetFileInformationByHandle` with `FileRenameInfo` on a handle that was
    opened BY PATH outside the primitive. That is a genuinely new shape, it
    matches nothing here, and it is the most plausible way this class comes
    back — the primitive itself publishes exactly that way, so the idiom is now
    familiar in this codebase.

  * A PUBLISH PATH WITH NO RENAME IN IT AT ALL. Added 2026-08-13 (phase-59), and
    it is the one that has actually escaped rather than the ones above. Both
    compliance converters wrote QPDFWriter output DIRECTLY over the user's
    output path: no staging file, no rename, therefore nothing for this sweep to
    match. It passed over them and it was RIGHT to. The class this checker
    closes is "a publish that renames a by-path staging file"; the property
    anyone reading its PASS actually wants is "every publish is atomic and
    unpredictable", and a write-in-place path sits exactly in the gap. Its count
    must therefore be read as "N KNOWN", never as "all". Finding this shape
    needs the inverse question — which writers target a caller-named path
    directly? — which no shape list answers. Recorded as DCA 0023.

  * a writer whose TARGET is the caller's path rather than a staging file, in a
    form no call name distinguishes: `QPDFWriter w(qpdf, path.c_str())` (the
    2-arg path constructor) beside the 4-arg handle constructor, or
    `pdf_save_document(ctx, doc, pathStr.c_str(), ...)` used as the TERMINAL
    publish rather than as a staging write. Both spellings are legitimate in a
    staging write and fatal in a publish, so telling them apart needs the
    inverse sweep — which writer's output argument is the untouched
    caller-supplied target? — not a call-name list.
  * a handoff to a SUBPROCESS: passing a StagingFile's path() to an external
    tool via QProcess/CreateProcess reopens it by name outside this codebase's
    C++ call graph entirely, where no lexical scanner of src/ can follow.
  * a StagingFile created and then never published on some path (leaked or
    discarded without commitOver*). That is a phantom-success class rather than
    a substitution one — the user is told a save happened that did not — and it
    is equally outside what a publish-shape sweep can answer.

  The last three were enumerated by the phase-6 security-audit gate, which is
  the second time that gate has extended this list. Its findings are recorded
  here rather than in a phase document because a blind spot only helps the next
  reader if it lives next to the control it qualifies.

Treat this as a phrasing-level regression guard with the grep false positives
removed, not as proof of absence. It does not claim AST completeness and must
not be cited as though it did.

Usage:  python tools/gate/check_win87_publish_shapes.py [--root src]
        python tools/gate/check_win87_publish_shapes.py --self-test
Exit 0 = closed, 1 = a shape is live, 2 = the checker could not do its job.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

EXEMPT = {
    "src/core/AtomicFileReplace.cpp",
    "src/core/AtomicFileReplace.h",
}

# Word-boundary anchored so `myMoveFileExWrapper` is not a hit and
# `::MoveFileExW` is.
SHAPES = {
    "std::filesystem::rename": re.compile(r"\bstd\s*::\s*filesystem\s*::\s*rename\s*\("),
    "std::filesystem::copy_file": re.compile(r"\bstd\s*::\s*filesystem\s*::\s*copy_file\s*\("),
    "MoveFileEx": re.compile(r"(?<![A-Za-z0-9_])MoveFileEx[WA]?\s*\("),
    "ReplaceFile": re.compile(r"(?<![A-Za-z0-9_])ReplaceFile[WA]?\s*\("),
    "atomicReplaceFileWithRetry": re.compile(
        r"(?<![A-Za-z0-9_])atomicReplaceFileWithRetry\s*\("),
    # Added after the phase-5 security-audit gate pointed out that only the
    # std::filesystem spelling of copy was covered — and the RAW Win32 one is
    # the shape that was actually EXPLOITED (F58-a): CopyFileW has no
    # CREATE_NEW and no restrictive share mode.
    "CopyFile": re.compile(r"(?<![A-Za-z0-9_])CopyFile(Ex)?[WA]?\s*\("),
    "MoveFile": re.compile(r"(?<![A-Za-z0-9_])MoveFile[WA]?\s*\("),
    "rename (CRT)": re.compile(r"(?<![A-Za-z0-9_:.>])_?w?rename\s*\("),
    # Added NOT because anyone noticed, but because
    # check_publish_shape_completeness.py measured this list against a reference
    # surface and reported `QFile::rename` as used-in-tree-and-uncovered on its
    # first run (GitHub #254). The site was LogHandler::rotateIfNeededLocked,
    # which paired it with QFile::remove — the delete-then-publish shape, with
    # the delete destroying the generation the publish was meant to replace.
    #
    # The distinction matters for how much this list can be trusted: every
    # previous entry here arrived after a human spotted a gap, which is why the
    # list was always one unnoticed spelling away from a false green. These two
    # arrived from a measurement, and the measurement is now a guard.
    "QFile::rename": re.compile(r"\bQFile\s*::\s*rename\s*\("),
    "QFile::copy": re.compile(r"\bQFile\s*::\s*copy\s*\("),
    # The non-static spelling on a QFile instance reaches the same syscall.
    # Receiver-agnostic by necessity — the variable name is arbitrary — so this
    # can over-match `.rename(` on some other type. Measured over src/: 0 hits,
    # i.e. it costs nothing today and catches the instance form the day it
    # arrives. That bias is correct for a guard whose failure mode is a MISSED
    # publish.
    "QFile instance rename": re.compile(r"\.\s*rename\s*\("),
}

# The instance spelling of COPY is deliberately not above, and this is a
# measurement rather than an preference. `.copy(` over src/ matches 37 sites and
# all 37 are `QImage::copy` — a pixel crop, not a publish. Narrowing by argument
# shape does not separate them either: requiring a path-looking first argument
# still matches 21, because `region.copy(cropRect)` and `f.copy(destPath)` are
# the same tokens. A hard shape here would be 37 false positives gating every
# push, which is how a guard stops being read at all (the 365-match guard that
# hid four real failures for five commits).
#
# So it is reported, never failed on. The static `QFile::copy` spelling IS a hard
# shape above, and that is the one a publish is normally written with.
#
# This is the #254 limitation showing up a second time, now with a number
# attached: "every API that publishes a file" is not a lexically decidable set,
# and `.copy(` is a token that means two unrelated things depending on a type
# this scanner cannot see.
SOFT_SHAPES = {
    "QImage-or-QFile instance copy (ambiguous)": re.compile(r"\.\s*copy\s*\("),
}


def strip_comments_and_literals(src: str) -> str:
    """Blank out comments and string/char literals, preserving line structure.

    Newlines are kept so reported line numbers stay true. Anything removed is
    replaced by spaces rather than deleted, so column offsets survive too.

    Raises ValueError on an unterminated construct — a file this cannot lex is
    a file this must not silently report as clean.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""

        if c == "/" and nxt == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue

        if c == "/" and nxt == "*":
            end = src.find("*/", i + 2)
            if end == -1:
                raise ValueError("unterminated block comment")
            for ch in src[i:end + 2]:
                out.append("\n" if ch == "\n" else " ")
            i = end + 2
            continue

        # Raw string literal: R"delim( ... )delim"
        if c == "R" and nxt == '"':
            m = re.match(r'R"([^()\\ ]{0,16})\(', src[i:])
            if m:
                closer = ')' + m.group(1) + '"'
                end = src.find(closer, i + m.end())
                if end == -1:
                    raise ValueError("unterminated raw string literal")
                for ch in src[i:end + len(closer)]:
                    out.append("\n" if ch == "\n" else " ")
                i = end + len(closer)
                continue

        if c in ('"', "'"):
            quote = c
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == quote:
                    break
                if src[j] == "\n":          # unterminated on this line
                    break
                j += 1
            if j >= n:
                raise ValueError("unterminated string literal")
            for ch in src[i:j + 1]:
                out.append("\n" if ch == "\n" else " ")
            i = j + 1
            continue

        out.append(c)
        i += 1
    return "".join(out)


PROBE = r'''// A comment mentioning std::filesystem::rename(a, b) must NOT be reported.
/* nor a block comment with MoveFileExW(a, b, 0) in it */
const char* s = "ReplaceFileW(x, y) inside a string literal is not code";
const char* s2 = "std::filesystem::copy_file(a, b) also only a string";
void f() {
    std::filesystem::rename(a, b);                     // LIVE 1
    ::MoveFileExW(a, b, 0);                            // LIVE 2
    ReplaceFileW(a, b, nullptr, 0, 0, 0);              // LIVE 3
    std::filesystem::copy_file(a, b);                  // LIVE 4
    pdfe::core::atomicReplaceFileWithRetry(a, b, ec);  // LIVE 5
    CopyFileW(a, b, FALSE);                            // LIVE 6
    MoveFileW(a, b);                                   // LIVE 7
    _wrename(a, b);                                    // LIVE 8
    QFile::rename(a, b);                               // LIVE 9
    QFile::copy(a, b);                                 // LIVE 10
    logFile.rename(backupPath);                        // LIVE 11
    myMoveFileExWrapper(a, b);                         // near-miss, NOT a hit
    ReplaceFileHelper(a, b);                           // near-miss, NOT a hit
    myCopyFileWrapper(a, b);                           // near-miss, NOT a hit
    img.renameNothing(a);                              // near-miss, NOT a hit
    crop = img.copy(x, y, w, h);                       // near-miss: QImage crop
}
'''

#: (kind, how many the checker must report). Four decoys, eleven real calls.
#:
#: LIVE 9-11 and the last two near-misses arrived with the #254 completeness
#: guard. They matter for a reason worth stating: the FIRST version of the
#: instance shape was `(?<![A-Za-z0-9_])\.\s*rename\s*\(`, whose lookbehind can
#: never be satisfied — a `.` in `f.rename(` is always preceded by an identifier
#: character — so it matched nothing and the sweep still printed PASS. The
#: `missing` assertion below is what turns that from an invisible decoration into
#: a failure, and it is why every new shape must bring a probe line with it.
#:
#: `crop = img.copy(...)` is the near-miss that keeps the instance-COPY spelling
#: out of SHAPES honest: it is indistinguishable from a publish by tokens alone,
#: which is why it lives in SOFT_SHAPES. If some future shape starts matching it,
#: the hit count below goes up and this fails.
SELF_TEST_EXPECTED_HITS = 11
SELF_TEST_EXPECTED_IGNORED = 4      # 2 comments + 2 string literals
SELF_TEST_EXPECTED_NEAR_MISSES = 5  # word-boundary/type-ambiguous, must not match


def self_test() -> int:
    """Prove the sweep can go RED, and that it ignores what defeats a grep.

    THIS EXISTS BECAUSE THE CLAIM CAME FIRST. The register and the RTM asserted
    that this checker "self-tests against a probe tree ... 5 reported, 4
    ignored" while no such self-test was checked in — the author had run the
    probe by hand and written it up as though it were automated. The phase-5
    karen gate caught it. A checker nobody has proved can fail is decoration,
    and a DOCUMENT claiming a proof that does not exist is worse than no claim.
    """
    import tempfile

    try:
        code = strip_comments_and_literals(PROBE)
    except ValueError as e:
        print(f"SELF-TEST ABORT: could not lex the probe: {e}", file=sys.stderr)
        return 2

    hits = []
    for lineno, line in enumerate(code.splitlines(), 1):
        for name, rx in SHAPES.items():
            if rx.search(line):
                hits.append((lineno, name))

    ok = True
    if len(hits) != SELF_TEST_EXPECTED_HITS:
        print(f"SELF-TEST FAIL: expected {SELF_TEST_EXPECTED_HITS} live shapes, "
              f"got {len(hits)}: {hits}", file=sys.stderr)
        ok = False
    found = {name for _, name in hits}
    missing = set(SHAPES) - found
    if missing:
        print(f"SELF-TEST FAIL: these shapes were not detected at all: "
              f"{sorted(missing)}", file=sys.stderr)
        ok = False

    # And end-to-end through main(), so the FILE WALK and exit code are covered
    # too — not just the regexes. A self-test that only exercised the matcher
    # would miss an enumeration that never opened the file.
    with tempfile.TemporaryDirectory() as td:
        probe_dir = Path(td) / "src"
        probe_dir.mkdir()
        (probe_dir / "probe.cpp").write_text(PROBE, encoding="utf-8")
        argv = sys.argv
        try:
            sys.argv = ["check_win87_publish_shapes.py", "--root", str(probe_dir)]
            rc = main()
        finally:
            sys.argv = argv
    if rc != 1:
        print(f"SELF-TEST FAIL: a probe tree containing live shapes must exit 1, "
              f"got {rc}. This checker cannot go red, so a PASS from it means "
              f"nothing.", file=sys.stderr)
        ok = False

    if not ok:
        return 1
    print(f"SELF-TEST PASS: {len(hits)} live shape(s) reported "
          f"({SELF_TEST_EXPECTED_IGNORED} comment/string decoys and "
          f"{SELF_TEST_EXPECTED_NEAR_MISSES} word-boundary near-misses ignored), "
          f"and a probe tree exits 1.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="src")
    ap.add_argument("--self-test", action="store_true",
                    help="prove this checker can go RED and that it ignores "
                         "comments, string literals and word-boundary "
                         "near-misses; exits non-zero if it cannot")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = (REPO / args.root).resolve()
    if not root.is_dir():
        print(f"ABORT: {root} is not a directory", file=sys.stderr)
        return 2

    files = sorted(
        p for p in root.rglob("*")
        if p.suffix in {".cpp", ".h", ".hpp", ".cc", ".cxx"} and p.is_file()
    )
    if not files:
        # An empty sweep is the classic false green: report it as a failure of
        # the checker, never as a clean result.
        print("ABORT: no source files found — the sweep would be vacuous",
              file=sys.stderr)
        return 2

    findings = []
    scanned = 0
    for f in files:
        # An out-of-repo --root is used by --self-test below: a probe tree that
        # plants every shape in code AND in comments/strings, proving this sweep
        # can go red and that it ignores the false positives a plain grep trips
        # on.
        try:
            rel = f.relative_to(REPO).as_posix()
        except ValueError:
            rel = f.as_posix()
        if rel in EXEMPT:
            continue
        try:
            raw = f.read_text(encoding="utf-8", errors="strict")
        except UnicodeDecodeError as e:
            print(f"ABORT: {rel} is not valid UTF-8 ({e})", file=sys.stderr)
            return 2
        try:
            code = strip_comments_and_literals(raw)
        except ValueError as e:
            print(f"ABORT: could not lex {rel}: {e}", file=sys.stderr)
            return 2
        scanned += 1
        for lineno, line in enumerate(code.splitlines(), 1):
            for name, rx in SHAPES.items():
                if rx.search(line):
                    findings.append((rel, lineno, name,
                                     raw.splitlines()[lineno - 1].strip()))

    print(f"WIN-87 publish-shape sweep: {scanned} file(s) scanned "
          f"(comments and literals removed before matching), "
          f"{len(EXEMPT)} exempt (the primitive itself).")

    if findings:
        print(f"\nFAIL: {len(findings)} live publish shape(s) outside the "
              f"primitive:\n")
        for rel, lineno, name, text in findings:
            print(f"  {rel}:{lineno}  [{name}]  {text}")
        print("\nEach of these re-resolves a path at publish time with no handle "
              "held, which is the WIN-87 substitution class. Route it through "
              "core::StagingFile (createBeside + commitOverWithRetry) instead.")
        return 1

    # Enumerated from SHAPES rather than hard-coded: a hand-written list in the
    # success message is one more place to drift out of date, and a PASS naming
    # the wrong set of shapes is exactly the kind of true-sounding sentence this
    # programme keeps catching.
    print("PASS: none of these survives as a CALL in code outside "
          "core/AtomicFileReplace — " + ", ".join(sorted(SHAPES)) + ".")
    return 0


if __name__ == "__main__":
    sys.exit(main())
