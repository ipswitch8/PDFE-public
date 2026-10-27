# -*- coding: utf-8 -*-
"""SC-b enforcement (meta-audit gap A3-41): the oracle standard, made lint-checkable.

`docs/harness-audit/standing-controls.md` §SC-b proposed "a written,
lint-enforceable standard for what counts as an oracle". The meta-audit found the
control ASPIRATIONAL: no standard document and no linter existed, which is why
R-03 / R-14 / R-16 / R-18 / R-19 / R-20 / R-21 -- every register row that depends
on SC-b -- had no recurrence prevention. This script is the enforcement point.
The standard it enforces is `docs/audit/oracle-standard.md`.

Rules (each maps to a smell class registered in that document):

  OS-1  zero-assertion file
        A `tests/test_*.cpp` that contains no `EXPECT_`/`ASSERT_` at all.
        Allowlist: `test_main.cpp` (the harness entry point, no tests).

  OS-2  self-negating tautology
        `EXPECT_TRUE(x || !x)` and friends -- true for every value of x.

  OS-3  unsigned-lower-bound tautology  [registered here as A3-55's
        `over-tolerant-threshold` class, which was dropped between the phase-1
        oracle audit and the phase-5 capstone and had no register entry at all]
        `EXPECT_GE(container.size(), 0)` -- an unsigned quantity is always >= 0.

  OS-4  SUCCEED-only test body
        A `TEST`/`TEST_F`/`TEST_P` whose body calls `SUCCEED()` and contains no
        `EXPECT_`/`ASSERT_`. The test cannot fail whatever the product does.

  OS-5  the standard document must exist and must register every smell class
        this script knows about, including the five NOVEL classes the phase-1
        audit found and the capstone dropped (gap A3-56): `misnamed-test`,
        `source-grep-proxy`, `partial-contract-assert`, `no-removal-check`,
        `weak-positive-detection`.

Comments and string literals are stripped before matching, so a rule quoted in a
comment (this file's own C++ siblings quote several) is not counted as a
violation.

Exit 0 only when there are ZERO violations. There is no allowlist budget for
OS-2/OS-3/OS-4: the tree is at zero, and the point of the control is to keep it
there.

Usage:
    python tests/scripts/lint_oracle_standard.py tests/
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Files exempt from OS-1 (no test cases by construction).
OS1_ALLOWLIST = {"test_main.cpp"}

# OS-4 RATCHET BASELINE.
#
# OS-2 and OS-3 are enforced at ZERO: every self-negating tautology and every
# unsigned-lower-bound tautology in the suite was repaired when this control
# landed, so there is no allowlist for them and a new one is a hard failure.
#
# OS-4 could not be taken to zero in the same change: 38 pre-existing test bodies
# assert nothing but SUCCEED(). Deleting or rewriting all of them blind would be
# worse than recording them, so they are pinned here as a SHRINK-ONLY baseline
# and the rule is enforced in BOTH directions:
#
#   * a SUCCEED-only body that is not on this list fails the lint (no new debt);
#   * a name on this list that no longer violates ALSO fails the lint, with an
#     instruction to delete the entry (the baseline cannot rot into a permanent
#     amnesty, which is how allowlists usually die).
#
# The count is quoted in docs/audit/oracle-standard.md and must match.
OS4_BASELINE = {
    "AnnotationToolsTest.ToolActivateDeactivate",
    "AutoTaggerTest.ConstructDestroy",
    "BatchEngineTest.ConstructDestroy",
    "ComplianceDialogTest.ConstructDestroy",
    "ComplianceManagerTest.ConstructDestroy",
    "ContentStreamEditorTest.ConstructorDestructor",
    "ContentStreamTaggerTest.ConstructDestroy",
    "DialogWidgetAudit.AltTextDialog_ClosesCleanly",
    "DialogWidgetAudit.AttachmentPanel_ClosesCleanly",
    "DialogWidgetAudit.CalculationOrderDialog_ClosesCleanly",
    "DialogWidgetAudit.HeaderFooterDialog_ClosesCleanly",
    "DialogWidgetAudit.PageLabelsDialog_ClosesCleanly",
    "DialogWidgetAudit.PreferencesDialog_ClosesCleanly",
    "DialogWidgetAudit.PropertiesPanel_ClosesCleanly",
    "DiffEngineTest.ConstructDestroy",
    "FontComplianceCheckerTest.ConstructDestroy",
    "FormDataIOTest.MoveAssignment",
    "FormDataIOTest.MoveConstruction",
    "HeaderFooterApplyTest.MarkersPresentInOutputStream",
    "IccProfileManagerTest.ConstructDestroy",
    "LayerManagerTest.ConstructDestroy",
    "LinkToolTest.SetDefaultUri",
    "PdfAConverterTest.ConstructDestroy",
    "PdfAValidatorTest.ConstructDestroy",
    "PdfContentWriterTest.GetContentStreamDataReturnsNonEmpty",
    "PdfContentWriterTest.GetImageDataFromScannedPageReturnsNonEmpty",
    "PdfUAConverterTest.ConstructDestroy",
    "Pkcs7BridgeTest.ConstructDestroy",
    "Pkcs7BridgeTest.DropNullSigner",
    "Pkcs7BridgeTest.DropNullVerifier",
    "ProcessMitigationsTest.NoOpOnNonWindows",
    "ProhibitedFeatureScannerTest.ConstructDestroy",
    "SignatureCommandsTest.Destructor",
    "StampToolTest.SetColor",
    "StructureTreeBuilderTest.ConstructDestroy",
    "StructureTreeBuilderTest.SetDocumentLanguage",
    "UncoveredDialogs.TrustAnchorDialogRefreshIsIdempotent",
    "XmpMetadataWriterTest.ConstructDestroy",
}

NOVEL_SMELL_CLASSES = [
    "misnamed-test",
    "source-grep-proxy",
    "partial-contract-assert",
    "no-removal-check",
    "weak-positive-detection",
]
REGISTERED_SMELL_CLASSES = [
    "zero-assertion-file",
    "self-negating-tautology",
    "over-tolerant-threshold",
    "succeed-only-body",
] + NOVEL_SMELL_CLASSES

ASSERTION_RE = re.compile(r"\b(?:EXPECT|ASSERT)_")
TEST_RE = re.compile(
    r"\bTEST(?:_F|_P)?\s*\(\s*([A-Za-z_0-9]+)\s*,\s*([A-Za-z_0-9]+)\s*\)\s*\{")
TAUTOLOGY_RE = re.compile(
    r"EXPECT_(?:TRUE|THAT)\(\s*([A-Za-z_0-9.\->()\[\]]+)\s*\|\|\s*!\s*\1\s*\)")
UNSIGNED_GE_RE = re.compile(
    r"(?:EXPECT|ASSERT)_GE\(\s*([^,;]*?(?:\.size\(\)|\.length\(\)|\.count\(\))"
    r"[^,;]*?)\s*,\s*0u?\s*\)")

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_STRING_LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')
# Raw string literals: R"delim( ... )delim". Their content is NOT escaped, so a
# regex literal containing a bare `"` splits into two halves under
# _STRING_LITERAL and leaves the remainder unblanked -- the mirror image of the
# C++ side's failure, where the same input desynchronises the state machine and
# blanks real code instead. Measured on this tree: 76 raw strings, 18 of them
# containing a `"` across 8 files. Both implementations were wrong; the C++ one
# was wrong LOUDLY (an OS-1 false positive) and this one was wrong QUIETLY,
# which is the worse of the two. Matched FIRST, before the plain-string pattern
# can bite into one. See tests/test_oracle_standard.cpp::stripNoise.
_RAW_STRING = re.compile(r'R"([^\s()\\]{0,16})\(.*?\)\1"', re.S)


def strip_noise(src: str) -> str:
    """Blank out comments and string literals, preserving line numbering."""
    def blank(m: "re.Match[str]") -> str:
        return re.sub(r"[^\n]", " ", m.group(0))
    # Comments first (a raw-string opener quoted inside a comment must not
    # swallow code), then RAW strings, then plain ones.
    src = _BLOCK_COMMENT.sub(blank, src)
    src = _LINE_COMMENT.sub(blank, src)
    src = _RAW_STRING.sub(blank, src)
    src = _STRING_LITERAL.sub(blank, src)
    return src


def line_of(src: str, pos: int) -> int:
    return src.count("\n", 0, pos) + 1


def body_of(src: str, open_brace_end: int) -> str:
    """Text of a brace-balanced body starting just after its opening `{`."""
    depth = 1
    i = open_brace_end
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[open_brace_end:i]


def scan(tests_dir: Path):
    violations = []
    seen_os4 = set()
    for path in sorted(tests_dir.glob("test_*.cpp")):
        raw = path.read_text(encoding="utf-8", errors="replace")
        src = strip_noise(raw)
        rel = path.as_posix()

        if path.name not in OS1_ALLOWLIST and not ASSERTION_RE.search(src):
            violations.append({
                "rule": "OS-1", "class": "zero-assertion-file",
                "file": rel, "line": 1,
                "detail": "file contains no EXPECT_/ASSERT_ at all"})

        for m in TAUTOLOGY_RE.finditer(src):
            violations.append({
                "rule": "OS-2", "class": "self-negating-tautology",
                "file": rel, "line": line_of(src, m.start()),
                "detail": "EXPECT_TRUE(%s || !%s) is true for every value"
                          % (m.group(1), m.group(1))})

        for m in UNSIGNED_GE_RE.finditer(src):
            violations.append({
                "rule": "OS-3", "class": "over-tolerant-threshold",
                "file": rel, "line": line_of(src, m.start()),
                "detail": "%s is unsigned, so >= 0 always holds" % m.group(1)})

        for m in TEST_RE.finditer(src):
            body = body_of(src, m.end())
            if "SUCCEED(" in body and not ASSERTION_RE.search(body):
                name = "%s.%s" % (m.group(1), m.group(2))
                seen_os4.add(name)
                if name not in OS4_BASELINE:
                    violations.append({
                        "rule": "OS-4", "class": "succeed-only-body",
                        "file": rel, "line": line_of(src, m.start()),
                        "detail": "%s asserts nothing but SUCCEED() and is not "
                                  "on the shrink-only baseline" % name})

    for stale in sorted(OS4_BASELINE - seen_os4):
        violations.append({
            "rule": "OS-4", "class": "stale-baseline-entry",
            "file": "tests/scripts/lint_oracle_standard.py", "line": 1,
            "detail": "%s is on the OS-4 baseline but no longer violates OS-4 — "
                      "delete it from OS4_BASELINE so the ratchet keeps its "
                      "grip" % stale})
    return violations


def check_standard_doc(repo_root: Path):
    doc = repo_root / "docs" / "audit" / "oracle-standard.md"
    if not doc.exists():
        return [{"rule": "OS-5", "class": "missing-standard",
                 "file": doc.as_posix(), "line": 1,
                 "detail": "the SC-b standard document does not exist"}]
    text = doc.read_text(encoding="utf-8")
    missing = [c for c in REGISTERED_SMELL_CLASSES if c not in text]
    if missing:
        return [{"rule": "OS-5", "class": "unregistered-smell",
                 "file": doc.as_posix(), "line": 1,
                 "detail": "smell classes absent from the standard: %s"
                           % ", ".join(missing)}]
    return []


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tests_dir", type=Path, nargs="?", default=Path("tests"))
    ap.add_argument("--json", type=Path, default=None,
                    help="also write the findings as JSON to this path")
    args = ap.parse_args()

    tests_dir = args.tests_dir
    repo_root = tests_dir.resolve().parent

    violations = scan(tests_dir) + check_standard_doc(repo_root)

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(
            {"tool": "tests/scripts/lint_oracle_standard.py",
             "standard": "docs/audit/oracle-standard.md",
             "registered_smell_classes": REGISTERED_SMELL_CLASSES,
             "violations": violations,
             "violation_count": len(violations)}, indent=2), encoding="utf-8")

    if not violations:
        print("lint_oracle_standard: 0 violations "
              "(%d smell classes registered)" % len(REGISTERED_SMELL_CLASSES))
        return 0

    print("lint_oracle_standard: %d violation(s) of docs/audit/oracle-standard.md"
          % len(violations))
    for v in violations:
        print("  %s:%d  [%s %s] %s"
              % (v["file"], v["line"], v["rule"], v["class"], v["detail"]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
