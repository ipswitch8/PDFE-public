# -*- coding: utf-8 -*-
"""Phase-1 testing-methodology lint: flag tests that mutate a PDF
(save / saveAs / applyRedactions / convertAndSave / writeMetadata /
encrypt / decryptInPlace) without a follow-up "open the result and
verify" assertion.

Background
----------
A May-2026 audit across the PDFE test suite found the dominant
testing-methodology gap is **internal-state-only assertions**: a test
calls one of the mutation APIs above, asserts the call succeeded, but
never re-opens the output and verifies the observable property. Bugs
in serialization, permissions, structure, and incremental writes slip
through.

This lint enforces "if you mutate, you must verify."

Detection
---------
Two passes on each test_*.cpp file:

1. **Mutation-call detection**: regex match against a curated list
   of mutation method calls (see ``MUTATION_PATTERNS``). For each
   match, record the file, line, the matched call, and the test
   function name (the enclosing TEST/TEST_F/TEST_P body, derived
   by tracking brace depth).

2. **Verification-call detection**: regex match against
   ``VERIFICATION_PATTERNS`` (Document::open, RoundTripHelper helpers,
   pixel-render helpers, std::filesystem::file_size, in-memory readback
   accessors like readMetadata/getAnnotations/getRedactionMarks,
   byte-level file inspection like bytes.find / readAll, etc.). For
   each match, record the file + line + enclosing test.

A mutation in test T is "verified" iff there is at least one matching
verification call in the same test T at a line >= the mutation's line
AND <= the test's closing brace. If no verification is found, the
mutation is flagged.

3. **Save-target taint (added for GitHub #163)**: the name list in
   pass 2 is a *curated* one, and the suite has outgrown it. Nearly
   every test file grows its own purpose-built readback helper
   (``ReadSavedRotation``, ``savedPageContent``, ``readDssPayload``,
   ``inspectStruct``, ``daclSddl``, ``extractFontProgram``,
   ``allDecodedStreams``, ...), and none of them can be in a list
   written before they existed. That produced 75 findings of which 64
   were tests that verify perfectly well — the linter was reporting the
   staleness of its own vocabulary, not a methodology gap.

   So a mutation is ALSO verified when its **save target is read back**:
   if the mutation's first argument is a plain identifier ``A``, and
   somewhere at or after the mutation (and inside the same test body)
   ``A`` is passed as an argument to some call that is neither another
   mutation nor a known cleanup sink (``remove`` / ``remove_all`` /
   ``rename`` / ``CloseHandle`` / ...), then the artifact was re-opened
   or inspected and the mutation is verified.

   This is strictly a *generalisation* of pass 2 — it only ever accepts
   more, never fewer — so nothing that the name list caught stops being
   caught. What it removes is the false-positive class, without the
   linter having to be taught every helper name in the suite forever.
   A mutation whose target is a temporary expression (``doc->saveAs(
   tempPath("x"))``) has no identifier to taint and falls back to
   pass 2 alone.

Allowlist
---------
Some tests legitimately mutate without a re-open verification (e.g.
they're testing the mutation API itself returns the expected value,
not its byte-level effect). Add the test name to ``ALLOWLIST`` to
suppress the warning. Each allowlist entry must include a one-line
justification and an issue/PR reference.

Output
------
The script prints one line per finding in the form

    file:line  test=NAME  mutation=CALL  hint=...

and exits 0 if there are zero findings (lint clean) or 1 if there are
any findings.

A ``--list-allowlist`` flag dumps the allowlist for review.
A ``--quiet`` flag suppresses the per-finding lines and prints only
the summary count.
A ``--advisory`` flag forces exit 0 regardless of finding count
(used historically during phase-1 rollout; default is hard-exit-1).

Usage
-----
    python tests/scripts/lint_save_without_verify.py tests/
    python tests/scripts/lint_save_without_verify.py --quiet tests/

CI wiring
---------
Phase-16 (pipeline a7c2e541 closeout) flips this script to hard-exit-1
on any finding so the regression cannot be re-introduced silently.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


# ─── Patterns ────────────────────────────────────────────────────────

# Mutation calls: method invocations that change the PDF on disk or
# the in-memory document in a way that must be verified by re-opening
# the result OR by reading back the mutated state in-memory. The
# leading ``\b`` anchors avoid false-matches inside longer identifier
# names.
MUTATION_PATTERNS = [
    re.compile(r"\b(\w+)->saveAs\s*\("),
    re.compile(r"\b(\w+)->save\s*\("),
    re.compile(r"\b(\w+)->applyRedactions\s*\("),
    re.compile(r"\b(\w+)->applyPageRedactions\s*\("),
    re.compile(r"\b(\w+)->applyAllRedactions\s*\("),
    re.compile(r"\b(\w+)->convertAndSave\s*\("),
    re.compile(r"\b(\w+)->writeMetadata\s*\("),
    re.compile(r"\b(\w+)->encryptDocument\s*\("),
    re.compile(r"\b(\w+)->decryptInPlace\s*\("),
    re.compile(r"\b(\w+)->signDocument\s*\("),
    re.compile(r"\b(\w+)->writeXMP\s*\("),
    re.compile(r"\b(\w+)->writeStructureTree\s*\("),
]

# Verification calls / patterns. A test is "verified" if any of these
# appears at or after the mutation in the same test body.
#
# This list intentionally covers both disk-roundtrip patterns
# (Document::open, file_size, ifstream-style byte reads) AND
# in-memory-readback patterns (readMetadata, getRedactionMarks,
# getAnnotations, result.marksApplied, pageContainsSpanText, etc.).
# The audit's intent is "the mutation produced an observable effect
# we can verify" — both paths satisfy that intent.
VERIFICATION_PATTERNS = [
    # Disk re-open
    re.compile(r"\bDocument::open\s*\("),
    re.compile(r"\bcore::Document::open\s*\("),
    re.compile(r"\bRoundTripHelper::SaveAndReopen\s*\("),
    re.compile(r"\bSaveAndReopen\s*\("),
    re.compile(r"\bRenderPage\s*\("),
    # File-property checks
    re.compile(r"\bstd::filesystem::file_size\s*\("),
    re.compile(r"\bstd::filesystem::exists\s*\("),
    re.compile(r"\bfilesystem::file_size\s*\("),
    re.compile(r"\bfilesystem::exists\s*\("),
    # Raw-byte inspection of the saved file
    re.compile(r"\breadAll\s*\("),
    re.compile(r"\bcountOccurrences\s*\("),
    re.compile(r"\bbytes\.find\s*\("),
    re.compile(r"std::ifstream"),
    re.compile(r"\bifstream\s+\w+"),
    # In-memory readback after mutation
    re.compile(r"\breadMetadata\s*\("),
    re.compile(r"\breadField\s*\("),
    re.compile(r"\bgetRedactionMarks?\s*\("),
    re.compile(r"\bgetRedactionMarkAt\s*\("),
    re.compile(r"\bgetAllRedactionMarks\s*\("),
    re.compile(r"\bpendingMarkCount\s*\("),
    re.compile(r"\bgetAnnotations?\s*\("),
    re.compile(r"\bgetLinkAction\s*\("),
    re.compile(r"\bgetSignatureDetails\s*\("),
    re.compile(r"\bgetFormField\s*\("),
    # Render-and-compare helpers
    re.compile(r"\bpageContainsSpanText\s*\("),
    re.compile(r"\brenderPage\s*\("),
    # Verifier classes — purpose-built post-mutation validators
    re.compile(r"\bRedactionVerifier\b"),
    re.compile(r"\bverify\s*\([^)]*Path"),
    # Result-object inspection: the mutation API returns a struct
    # whose fields encode the effect; checking those fields IS the
    # verification (e.g., result.marksApplied, result.success).
    re.compile(r"\.marksApplied\b"),
    re.compile(r"\.pagesAffected\b"),
    re.compile(r"\.documentRewritten\b"),
    # Event callback assertions: tests that wire willSave/didSave
    # callbacks and assert they fired ARE verifying the mutation
    # had its expected event-dispatch side-effect.
    re.compile(r"\bwillSave\b"),
    re.compile(r"\bdidSave\b"),
    re.compile(r"\bsetDocumentEventCallback\s*\("),
]

# ─── Save-target taint (GitHub #163) ─────────────────────────────────

# The method names that MUTATION_PATTERNS matches. A second mutation
# call consuming the same target is not a verification of the first, so
# these are excluded when looking for readbacks.
MUTATION_METHOD_NAMES = {
    "saveAs", "save", "applyRedactions", "applyPageRedactions",
    "applyAllRedactions", "convertAndSave", "writeMetadata",
    "encryptDocument", "decryptInPlace", "signDocument", "writeXMP",
    "writeStructureTree",
}

# Calls that consume the save target but observe NOTHING about it.
# Deleting the artifact, renaming it, or closing a handle to it is
# housekeeping, not an oracle — a test whose only post-save use of the
# target is one of these has still not verified the mutation.
CLEANUP_SINK_NAMES = {
    "remove", "remove_all", "rename", "create_directories",
    "create_directory", "CloseHandle", "DeleteFileW", "DeleteFileA",
}

# One call site found in a stripped block of code.
_IDENT_TAIL_RE = re.compile(r"([A-Za-z_]\w*)\s*$")


def _strip_noise(line: str) -> str:
    """Blank out line comments and string/char literals so tokens inside
    them cannot be mistaken for code."""
    out = re.sub(r"//.*$", "", line)
    out = re.sub(r'"(?:[^"\\]|\\.)*"', '""', out)
    out = re.sub(r"'(?:[^'\\]|\\.)*'", "''", out)
    return out


def mutation_target(lines: list[str], mut_line: int) -> str | None:
    """Return the first argument of the mutation call at ``mut_line`` when
    it is a plain identifier, else None (temporaries have no identity to
    follow)."""
    # Join up to 5 lines so a wrapped argument list is still parsed.
    blob = "".join(_strip_noise(l) + " "
                   for l in lines[mut_line - 1:mut_line + 4])
    for pat in MUTATION_PATTERNS:
        m = pat.search(blob)
        if not m:
            continue
        i = blob.index("(", m.end() - 1)
        depth = 0
        for j in range(i, len(blob)):
            if blob[j] == "(":
                depth += 1
            elif blob[j] == ")":
                depth -= 1
                if depth == 0:
                    args = blob[i + 1:j]
                    first = args.split(",")[0].strip()
                    if re.fullmatch(r"[A-Za-z_]\w*", first):
                        return first
                    return None
        return None
    return None


def _blank_mutation_args(block: str) -> str:
    """Replace every mutation call's argument list with empty parens, so
    ``ASSERT_NO_THROW(doc->save(target))`` does not read as a call that
    observes ``target``."""
    out = block
    for pat in MUTATION_PATTERNS:
        while True:
            m = pat.search(out)
            if not m:
                break
            i = out.index("(", m.end() - 1)
            depth = 0
            end = None
            for j in range(i, len(out)):
                if out[j] == "(":
                    depth += 1
                elif out[j] == ")":
                    depth -= 1
                    if depth == 0:
                        end = j
                        break
            if end is None:
                break
            # Keep the length stable-ish; neuter both the callee (so the
            # loop terminates) and the args.
            out = out[:m.start()] + "__mut__" + " " * (i - m.start() - 7) \
                + "(" + " " * (end - i - 1) + ")" + out[end + 1:]
    return out


def target_is_read_back(target: str, lines: list[str],
                        mut_line: int, end_line: int) -> bool:
    """True iff ``target`` is passed to some non-mutation, non-cleanup
    call at or after ``mut_line`` and within the test body."""
    block = "\n".join(_strip_noise(l)
                      for l in lines[mut_line - 1:end_line])
    block = _blank_mutation_args(block)
    tok = re.compile(r"\b" + re.escape(target) + r"\b")
    if not tok.search(block):
        return False
    # Walk every '(' and treat the identifier immediately before it as
    # the callee; if the target appears anywhere in that call's argument
    # span, the artifact was consumed by that call.
    for i, ch in enumerate(block):
        if ch != "(":
            continue
        head = _IDENT_TAIL_RE.search(block[:i])
        if not head:
            continue
        callee = head.group(1)
        if callee in MUTATION_METHOD_NAMES or callee in CLEANUP_SINK_NAMES:
            continue
        depth = 0
        for j in range(i, len(block)):
            if block[j] == "(":
                depth += 1
            elif block[j] == ")":
                depth -= 1
                if depth == 0:
                    if tok.search(block[i + 1:j]):
                        return True
                    break
    return False


# Test-function start: TEST_F(Suite, Name) { ... } and friends.
TEST_FN_RE = re.compile(
    r"^\s*TEST(?:_F|_P)?\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)"
)

# Allowlist of test function names where mutation-without-verification
# is intentional. Format: 'Suite.Name'  -> 'reason / issue ref'.
ALLOWLIST: dict[str, str] = {
    # GitHub #76 / p84 phase-1. This is a CHILD-PROCESS role, not a test: it
    # performs the save and reports the outcome over a result file. The
    # verification deliberately lives in the PARENT
    # (ConcurrentInstanceSave.TwoInstances*), which is the only side that can
    # see the surviving file after both children exit -- it checks raw bytes
    # (header, final %%EOF, nothing after it, startxref in range landing on a
    # real xref section), then reopens, then round-trips. Verifying inside the
    # child would assert on a file the other child may still be replacing.
    "ConcurrentInstanceSaveChild.RunRole":
        "child-process save role; byte-level verification is in the parent test (#76)",
    # Tests that exist explicitly to verify *that* a method throws on
    # bad input — re-opening would be nonsensical.
    "RedactionManagerTest.CreateRedactionMark_NullPage":
        "tests an exception path; no save artifact to re-open",
    "RedactionManagerTest.DeleteNonExistentMark":
        "tests an exception path; no save artifact to re-open",
    # Null-argument no-throw contracts — there is no artifact and no
    # in-memory state to inspect (the mutation is a no-op by design).
    "RedactionManagerEdgeCaseTest.ApplyPageRedactions_NullPage":
        "null-arg exception contract; no artifact produced",
    "RedactionManagerEdgeCaseTest.ApplyAllRedactions_NullDocument":
        "null-arg exception contract; no artifact produced",
    "RedactionManagerEdgeCaseTest.ApplyOnCleanPage":
        "no-op apply on clean page; EXPECT_NO_THROW IS the contract",
    "MetadataManagerTest.WriteMetadata_NullDoc":
        "null-arg no-throw contract; no doc to read back",
    # Error-path: save to bogus directory should throw OR auto-create;
    # the test exists to lock either behavior, not to verify file output.
    "SaveOperationsTest.SaveToNonExistentDirectory":
        "error-path test; no successful save artifact to re-open",
    # SaveAs path-validation tests — assert saveAs() throws on bad
    # input paths. No artifact produced; re-opening doesn't apply.
    "SaveTest.SaveAs_RejectsAdsSelectorInFilename":
        "EXPECT_THROW on NTFS ADS selector ':' in filename; no artifact",
    "SaveTest.SaveAs_RejectsEmptyPath":
        "EXPECT_THROW on empty path; no artifact",
    "SavePathSecurityTest.EmptyPathRejected":
        "EXPECT_THROW on empty path; no artifact",
    "SavePathSecurityTest.NullByteInPathRejected":
        "EXPECT_THROW on embedded NUL byte; no artifact",
    "SavePathSecurityTest.WindowsADSRejected":
        "EXPECT_THROW on Windows ADS selector; no artifact",
    "SavePathSecurityTest.UNCPathInSaveTarget":
        "EXPECT_THROW on UNC \\\\server\\share path; no artifact",
    "SavePathSecurityTest.RelativePathTraversalAllowed":
        "EXPECT_NO_THROW contract that ../ is intentionally permitted; "
        "test follows with std::filesystem::remove(target) which proves "
        "the file was created, but lint doesn't recognize remove as verify",
    "SavePathSecurityTest.LongPathHandledGracefully":
        "EXPECT_NO_THROW contract on 300-char filename; either succeeds "
        "or filesystem rejects — both acceptable, no specific artifact "
        "property to assert",
    # Stress/timing tests — assertion is wall-clock duration, not output
    # content. Re-opening to verify isn't relevant.
    "FormFieldSaveStressTest.SaveTimeAt100Fields":
        "stress test measuring save wall-clock time at scale; "
        "re-open verify isn't the contract",
    "FormFieldSaveStressTest.SaveTimeAt250Fields":
        "stress test measuring save wall-clock time at scale; "
        "re-open verify isn't the contract",
    # ── GitHub #163 ────────────────────────────────────────────────
    # Two sites where the verification genuinely exists but cannot be
    # seen by a "same test body, at or after the mutation line" scan.
    # Neither is a blanket suppression: both name one test, and both
    # state where the oracle actually is.
    "RedactionApplyTest.NoFilesystemResidueFromAnyApplyPath":
        "the mutation runs inside a lambda handed to expectNoResidue(), "
        "whose post-run directory scan IS the oracle — and that call "
        "lexically PRECEDES the lambda body, so an at-or-after-the-"
        "mutation scan can never reach it (GitHub #163)",
    "SaveKillWindowVictim.SavesUntilKilled":
        "victim-child body: saves in a loop until the parent kills the "
        "process. The oracle is in the PARENT (it inspects the file "
        "state left behind after the kill), so by construction it "
        "cannot appear in this process's test body (GitHub #163)",
}


# ─── Data types ──────────────────────────────────────────────────────


@dataclass
class TestSpan:
    """One TEST/TEST_F/TEST_P body in a file."""
    suite: str
    name: str
    start_line: int
    end_line: int  # 1-based, inclusive

    @property
    def qualified(self) -> str:
        return f"{self.suite}.{self.name}"


@dataclass
class Mutation:
    file: Path
    line: int
    call: str
    test: TestSpan


# ─── File parsing ────────────────────────────────────────────────────


def find_test_spans(lines: list[str]) -> list[TestSpan]:
    """Walk a file's lines and identify each TEST/TEST_F/TEST_P body
    by tracking brace depth from the line of the test header.
    """
    spans: list[TestSpan] = []
    i = 0
    n = len(lines)
    while i < n:
        m = TEST_FN_RE.match(lines[i])
        if not m:
            i += 1
            continue
        suite, name = m.group(1), m.group(2)
        # Find the opening brace — it may be on the same line or the
        # next non-blank line.
        depth = 0
        j = i
        opened = False
        while j < n:
            line = lines[j]
            # Strip line comments and string literals roughly so braces
            # inside them don't perturb depth. Cheap heuristic — a
            # full C++ parser would be overkill here.
            stripped = re.sub(r"//.*$", "", line)
            stripped = re.sub(r'"(?:[^"\\]|\\.)*"', '""', stripped)
            stripped = re.sub(r"'(?:[^'\\]|\\.)*'", "''", stripped)
            for ch in stripped:
                if ch == "{":
                    depth += 1
                    opened = True
                elif ch == "}":
                    depth -= 1
                    if opened and depth == 0:
                        spans.append(TestSpan(
                            suite=suite, name=name,
                            start_line=i + 1,
                            end_line=j + 1,
                        ))
                        i = j + 1
                        break
            if opened and depth == 0:
                break
            j += 1
        else:
            i += 1
            continue
        if j >= n:
            break
    return spans


def find_mutations(file: Path, lines: list[str],
                   spans: list[TestSpan]) -> list[Mutation]:
    out: list[Mutation] = []
    span_by_line = {}
    for s in spans:
        for ln in range(s.start_line, s.end_line + 1):
            span_by_line[ln] = s
    for ln_idx, line in enumerate(lines, start=1):
        # Skip comment lines quickly.
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue
        for pat in MUTATION_PATTERNS:
            m = pat.search(line)
            if m:
                span = span_by_line.get(ln_idx)
                if span is None:
                    # Mutation outside a TEST body (e.g. in a helper).
                    # Skip — not a test-author oversight.
                    continue
                out.append(Mutation(
                    file=file, line=ln_idx,
                    call=m.group(0).strip(),
                    test=span,
                ))
                break
    return out


def is_verified(mut: Mutation, lines: list[str]) -> bool:
    """Look for any verification pattern in the same test body, on a
    line >= the mutation's line.
    """
    end = mut.test.end_line
    start = mut.line
    for ln in range(start, end + 1):
        line = lines[ln - 1]
        for pat in VERIFICATION_PATTERNS:
            if pat.search(line):
                return True
    return False


# ─── Driver ──────────────────────────────────────────────────────────


def lint_file(file: Path) -> list[Mutation]:
    text = file.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    spans = find_test_spans(lines)
    muts = find_mutations(file, lines, spans)
    findings: list[Mutation] = []
    for mut in muts:
        if mut.test.qualified in ALLOWLIST:
            continue
        if is_verified(mut, lines):
            continue
        # Save-target taint (#163): the artifact was read back by a
        # helper this script has never heard of.
        target = mutation_target(lines, mut.line)
        if target and target_is_read_back(target, lines, mut.line,
                                          mut.test.end_line):
            continue
        findings.append(mut)
    return findings


def lint_path(path: Path) -> list[Mutation]:
    findings: list[Mutation] = []
    if path.is_file() and path.suffix == ".cpp":
        return lint_file(path)
    if path.is_dir():
        for cpp in sorted(path.rglob("test_*.cpp")):
            findings.extend(lint_file(cpp))
    return findings


def format_finding(mut: Mutation) -> str:
    return (f"{mut.file}:{mut.line}  test={mut.test.qualified}  "
            f"mutation={mut.call}  hint=add a re-open or property check "
            f"after the mutation")


def main(argv: Iterable[str]) -> int:
    ap = argparse.ArgumentParser(
        description=("Flag test mutations of PDF state that lack a "
                     "follow-up verification."))
    ap.add_argument("paths", nargs="+", type=Path,
                    help="Test files or directories to lint.")
    ap.add_argument("--quiet", action="store_true",
                    help="Print only the summary count, not findings.")
    ap.add_argument("--list-allowlist", action="store_true",
                    help="Dump the in-script allowlist and exit.")
    ap.add_argument("--advisory", action="store_true",
                    help="Force exit 0 regardless of findings "
                         "(historic phase-1 behavior; default is "
                         "hard-exit-1 since pipeline a7c2e541 phase-16).")
    args = ap.parse_args(list(argv))

    if args.list_allowlist:
        for k, v in sorted(ALLOWLIST.items()):
            print(f"  {k}  -- {v}")
        return 0

    findings: list[Mutation] = []
    for p in args.paths:
        if not p.exists():
            print(f"ERROR: path does not exist: {p}", file=sys.stderr)
            return 2
        findings.extend(lint_path(p))

    if not args.quiet:
        for mut in findings:
            print(format_finding(mut))

    print(f"\n[lint_save_without_verify] {len(findings)} finding(s) "
          f"in {len(args.paths)} path(s).")

    # Pipeline a7c2e541 phase-16: hard-exit-1 on any finding. Pass
    # --advisory to suppress (historic behavior) — used only for
    # local iteration during cleanup.
    if args.advisory:
        return 0
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
