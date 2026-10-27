#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Flag comments and user-facing strings that name a trust source PDFE does
not consult.

GitHub #212 recorded the decision: PDFE does NOT read the Windows certificate
store. Its only trust sources are the user's persisted anchors and, where
``useOpenSslDefaultPaths`` is set, ``X509_STORE_set_default_paths()`` --
OPENSSLDIR/certs plus SSL_CERT_FILE / SSL_CERT_DIR, POSIX conventions that
contribute nothing on Windows.

GitHub #233 is the SECOND time the phrase has had to be chased through the
tree, and it suggested this guard by name. The chase has a consistent shape:

  * the flag was renamed useSystemStore -> useOpenSslDefaultPaths, and prose
    describing it was not, because a rename tool follows identifiers;
  * a hand audit for the literal ``system store`` walked straight past
    ``system TRUST store`` -- a real string this product shipped, and a false
    ASSURANCE (it told the user the OS had vouched for a certificate the OS
    was never asked about);
  * a third variant, ``system CA store``, sat in a test comment.

So the rule is NOT "ban the phrase". Several correct uses exist and must
survive -- most importantly tests/test_cert_store_trust_policy.cpp, which
opens the Windows store with CertOpenSystemStoreW precisely so it can prove
the product ignores it. A guard that drove the phrase to zero would delete the
evidence that the decision holds.

THE RULE: a line may name a system/Windows/machine trust store provided its
own comment block (or the string's immediate neighbourhood) also carries a
disambiguating token -- naming OpenSSL's default paths, or saying plainly that
the store is not consulted. Naming the thing and explaining it is fine. Naming
it and leaving the reader to resolve it is the defect.

Usage:
    python tools/gate/check_trust_source_claims.py [--repo-root .]
    python tools/gate/check_trust_source_claims.py --self-test
"""

from __future__ import annotations

import argparse
import io
import os
import re
import sys

# Phrasings that resolve, for a Windows reader, to the OS certificate store.
# Deliberately broader than the literal that was wrong first -- both misses in
# this defect's history were variants, not the original.
# NOTE THE ABSENCE OF \n IN THE GAP CLASS, AND WHY IT MATTERS.
#
# The first version of this guard used `[^.\n"]{0,25}?`, i.e. it refused to
# match across a line break. That made it structurally unable to see a wrapped
# comment -- and the phase-7 validator gate then found TWO surviving false
# claims that were invisible for exactly that reason:
#
#     // ... when the anchors, the system
#     // store and the revocation state all differ from today's.
#
#     //      QSettings-backed TrustAnchorManager singleton plus the machine's real
#     //      store.
#
# A line-oriented regex cannot see either, and neither can `grep`. That is the
# SAME blind-spot shape as the defect this guard exists to catch: the original
# audit missed "system TRUST store" by searching for the literal "system
# store". Third occurrence of one bug class, so the gap class is what gets
# closed here, not the two instances.
#
# The gap now permits newlines and the `//` / `///` / `*` continuation markers
# that come with them. It still stops at a full stop or a quote, so a match
# cannot run across a sentence boundary.
CLAIM_RE = re.compile(
    r"(system|machine|windows|os|platform|native)"
    r"[^.\"]{0,40}?"
    r"\bstore\b",
    re.IGNORECASE,
)

# Tokens that make a mention safe: they either name the real mechanism or
# state the negative outright.
EXCULPATORY_RE = re.compile(
    r"openssl"
    r"|default\s+(certificate\s+)?(cert\s+)?paths?"
    r"|X509_STORE_set_default_paths"
    r"|OPENSSLDIR"
    r"|SSL_CERT_(FILE|DIR)"
    r"|useOpenSslDefaultPaths"
    r"|does\s+not\s+consult"
    r"|never\s+(reads?|consult)"
    r"|not\s+consulted"
    r"|deliberately\s+not"
    r"|is\s+NOT\s+the\s+Windows"
    r"|are\s+NOT\s+the\s+Windows"
    r"|no\s+system\s+trust\s+store\s+integration"
    r"|#212|GitHub\s*#212",
    re.IGNORECASE,
)

# Identifier-ish uses that are not prose claims at all.
IDENT_RE = re.compile(
    r"X509_STORE"
    r"|HCERTSTORE"
    r"|CertOpenStore|CertOpenSystemStore|CertCloseStore|CertEnumCertificates"
    r"|loadIntoStore|emptyStore|StoreCert|->store|\bstore\s*=|_store\b",
    re.IGNORECASE,
)

SCAN_DIRS = ("src/signatures",)
SCAN_FILES = (
    "tests/test_ltv_validator.cpp",
    "tests/test_cert_chain_trust.cpp",
    "tests/test_pkcs7_verifywithchain_trust.cpp",
    "tests/test_cert_store_trust_policy.cpp",
    "tests/test_signature_roundtrip.cpp",
)

# How far to look for the disambiguating token. A doc comment block is the
# natural unit, but the tokens often sit a couple of lines off (a @param tag,
# a wrapped sentence), so the window is generous on purpose: this guard is
# meant to catch an UNEXPLAINED mention, not to police comment layout.
CONTEXT_BEFORE = 8
CONTEXT_AFTER = 8


# A regex literal that MATCHES the bad phrasing is not a claim -- it is the
# guard. Without this, the message guards in test_ltv_validator.cpp flag
# themselves, and the obvious "fix" would be to weaken them.
REGEX_LITERAL_RE = re.compile(r'R"\(|QRegularExpression|re\.compile')


def is_store_touching_file(text: str) -> bool:
    """True when the file genuinely CALLS into the Windows store.

    tests/test_cert_store_trust_policy.cpp must describe the OS store in
    plain terms: it enumerates the live ROOT store precisely so it can prove
    the product ignores what it finds there. Exempting it BY NAME would let a
    real false claim hide behind the filename, so the exemption is earned by
    evidence instead -- the file has to actually call the Win32 API. A file
    that stops opening the store stops being exempt.

    A MENTION IS NOT A CALL, and the first version of this function could not
    tell the difference. It tested `"CertOpenSystemStore" in text`, which
    exempted `src/signatures/CertChainValidator.h` -- because line 33 of that
    header says:

        // No file under src/ calls CertOpenStore, CertOpenSystemStore or ...

    i.e. prose DENYING the call bought the whole file an exemption, and the one
    header carrying the RECORDED DECISION became the one file where a false
    claim about that decision could never be caught. The worst possible file to
    exempt, exempted by the sentence that makes the decision explicit.

    Same shape as every other miss in this defect's history: a rule written
    against one example (the test that really opens the store) generalising
    exactly as far as that example and no further.

    So the token must appear on a line that is not a comment.
    """
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith(("//", "///", "*", "/*", "#")):
            continue
        if "CertOpenSystemStore" in stripped:
            return True
    return False


def _is_comment_line(s: str) -> bool:
    s = s.lstrip()
    return s.startswith(("//", "///", "*", "/*"))


def _enclosing_comment_block(lines: list[str], i: int) -> str:
    """The contiguous run of comment lines containing line `i`, or "".

    A doc block is the unit a human reads, so it is the unit the exculpatory
    rule should judge. Without this, a long and perfectly accurate block gets
    judged eight lines at a time and trips on its own subject matter.
    """
    if i >= len(lines) or not _is_comment_line(lines[i]):
        return ""
    lo = i
    while lo > 0 and _is_comment_line(lines[lo - 1]):
        lo -= 1
    hi = i
    while hi + 1 < len(lines) and _is_comment_line(lines[hi + 1]):
        hi += 1
    return "\n".join(lines[lo:hi + 1])


def scan_text(text: str, path: str) -> list[tuple[int, str]]:
    """Return [(lineno, line)] for unexplained trust-store claims.

    Scans the WHOLE text rather than line by line, so a claim split across a
    line wrap is still found, then maps each match back to the line it starts
    on. Scanning per line is what made the first version of this guard miss
    two real false claims; see the comment on CLAIM_RE.
    """
    if is_store_touching_file(text):
        return []

    lines = text.splitlines()
    # Start offset of each line, for mapping a match offset back to a line.
    starts: list[int] = []
    pos = 0
    for line in lines:
        starts.append(pos)
        pos += len(line) + 1

    def line_index_of(offset: int) -> int:
        lo, hi = 0, len(starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if starts[mid] <= offset:
                lo = mid
            else:
                hi = mid - 1
        return lo

    out: list[tuple[int, str]] = []
    seen: set[int] = set()
    for m in CLAIM_RE.finditer(text):
        i = line_index_of(m.start())
        if i in seen:
            continue
        line = lines[i]
        span = m.group(0)
        # An identifier or a regex literal is not a prose claim. Check both the
        # matched span and the line it starts on, since a wrapped match can
        # begin on an ordinary line and run into a declaration.
        if IDENT_RE.search(span) or IDENT_RE.search(line):
            continue
        if REGEX_LITERAL_RE.search(span) or REGEX_LITERAL_RE.search(line):
            continue
        lo = max(0, i - CONTEXT_BEFORE)
        hi = min(len(lines), i + CONTEXT_AFTER + 1)
        window = "\n".join(lines[lo:hi])
        # ...plus the WHOLE enclosing comment block, which is what this guard's
        # docstring has always promised ("its own comment block") and what the
        # ±8-line window alone does not deliver. The RECORDED DECISION block in
        # CertChainValidator.h is ~96 lines and explains at length why the OS
        # root set is the wrong trust source -- accurate, load-bearing security
        # rationale. Judged 8 lines at a time it trips the guard; judged as the
        # block it is, it clears on its own text.
        #
        # That direction matters: the guard bends to correct documentation, it
        # does not make correct documentation bend to it. Rewriting that
        # rationale to appease a matcher is the precise failure the #233
        # criterion warns about.
        window += "\n" + _enclosing_comment_block(lines, i)
        if EXCULPATORY_RE.search(window):
            continue
        seen.add(i)
        out.append((i + 1, line.strip()))
    return out


def iter_targets(root: str):
    for d in SCAN_DIRS:
        full = os.path.join(root, d)
        for dirpath, _dirnames, filenames in os.walk(full):
            for fn in filenames:
                if fn.endswith((".h", ".cpp", ".hpp")):
                    yield os.path.join(dirpath, fn)
    for f in SCAN_FILES:
        p = os.path.join(root, f)
        if os.path.exists(p):
            yield p


def self_test() -> int:
    """Prove the guard can both fire and stay quiet.

    A guard that cannot redden is decoration; one that can ONLY redden is
    equally useless. Both directions are asserted here, using the exact
    strings this product actually shipped.
    """
    bad_cases = [
        # The false denial that shipped.
        'r.message = tr("not trusted by your anchors or the system store.");',
        # The false assurance that shipped, and that a literal search for the
        # line above does not match.
        'result.message = tr("Validated against system trust store");',
        # The third variant, from a test comment.
        "// its root is not in the system CA store, so we must trust it",
        # The doc-comment shape.
        "/// No /DSS dict. Chain validated against the system store.",
        # BOTH OF THESE ARE REAL. They survived the hand audit AND the first
        # version of this guard, because the phrase wraps across a line and a
        # line-oriented regex cannot see it. Verbatim from the tree, so a
        # regression to per-line matching reddens on the actual text rather
        # than on an invented example.
        "// CRLs and OCSP responses a validator will need LATER —\n"
        "// possibly years later, when the anchors, the system\n"
        "// store and the revocation state all differ from today's.",
        "//   1. There was no way to name the anchor set. Validation read the\n"
        "//      QSettings-backed TrustAnchorManager singleton plus the "
        "machine's real\n"
        "//      store. A test could only mutate the user's persisted trust "
        "list and",
    ]
    good_cases = [
        # Naming it AND explaining it.
        "// NOT the Windows store -- OpenSSL default certificate paths only.",
        "/// anchors plus OpenSSL's default certificate paths; never the "
        "Windows store.",
        # The test that genuinely opens the store, with its justification.
        "// self-signed root pulled from the live Windows ROOT store; the\n"
        "// test can see the OS store precisely so it can prove the product\n"
        "// does not consult it.",
        # Pure identifier use.
        "HCERTSTORE store = ::CertOpenSystemStoreW(0, L\"ROOT\");",
        "X509_STORE* store = X509_STORE_new();",
        # The guard's own pattern is not a claim.
        'QRegularExpression(R"(system\\s+(trust\\s+)?store)")',
    ]

    # The file-level exemption must be EARNED, not granted. Same prose twice;
    # the only difference is whether the file really opens the OS store. If
    # both of these landed the same way the exemption would be a blanket one,
    # and a false claim could hide behind it.
    claim_line = "/// One certificate lifted out of a Windows system store.\n"
    earned = claim_line + "HCERTSTORE s = ::CertOpenSystemStoreW(0, L\"ROOT\");"
    unearned = claim_line + "HCERTSTORE s = someOtherThing();"

    # A MENTION IN A COMMENT MUST NOT EXEMPT. This is the case the first
    # version got wrong, and it was not hypothetical: CertChainValidator.h
    # says "No file under src/ calls CertOpenStore, CertOpenSystemStore or
    # any other" -- prose DENYING the call -- which exempted the entire
    # header carrying the RECORDED DECISION. The `security-audit` gate
    # independently built this same proof-of-concept, so it is pinned here
    # with a real false claim underneath it.
    mention_only = (
        "// No file under src/ calls CertOpenStore, CertOpenSystemStore or\n"
        "// any other Win32 certificate API.\n"
        'result.message = tr("Validated against system trust store");'
    )

    failures = []
    if scan_text(earned, "<self-test>"):
        failures.append(
            "EXEMPTION NOT APPLIED: a file that really opens the OS store "
            "was flagged."
        )
    if not scan_text(unearned, "<self-test>"):
        failures.append(
            "EXEMPTION UNEARNED: the same prose passed WITHOUT the file "
            "opening the store -- the exemption is a blanket, not evidence."
        )
    if not scan_text(mention_only, "<self-test>"):
        failures.append(
            "EXEMPTION BOUGHT BY A COMMENT: a file that only NAMES "
            "CertOpenSystemStore -- in prose denying the call -- suppressed a "
            "real false claim. A mention is not a call."
        )

    for src in bad_cases:
        if not scan_text(src, "<self-test>"):
            failures.append("MISSED (should have flagged): %s" % src)
    for src in good_cases:
        hits = scan_text(src, "<self-test>")
        if hits:
            failures.append("FALSE POSITIVE: %s -> %r" % (src, hits))

    if failures:
        sys.stdout.write("[trust-claims] SELF-TEST FAILED\n")
        for f in failures:
            sys.stdout.write("  %s\n" % f)
        return 1
    sys.stdout.write(
        "[trust-claims] self-test OK: %d claim shapes flagged, "
        "%d correct usages left alone.\n" % (len(bad_cases), len(good_cases))
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = os.path.abspath(args.repo_root)
    findings = []
    scanned = 0
    for path in iter_targets(root):
        scanned += 1
        with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        rel = os.path.relpath(path, root).replace("\\", "/")
        for lineno, line in scan_text(text, rel):
            findings.append((rel, lineno, line))

    if scanned == 0:
        sys.stdout.write(
            "[trust-claims] FAIL: scanned 0 files. The scan roots are wrong, "
            "so a clean result here would mean nothing.\n"
        )
        return 1

    if findings:
        sys.stdout.write(
            "[trust-claims] FAIL: %d unexplained trust-store claim(s) "
            "(GitHub #212/#233).\n\n" % len(findings)
        )
        for rel, lineno, line in findings:
            sys.stdout.write("  %s:%d\n      %s\n" % (rel, lineno, line))
        sys.stdout.write(
            "\nPDFE does not consult the Windows certificate store. Either "
            "name the real\nmechanism (OpenSSL's default certificate paths / "
            "useOpenSslDefaultPaths) or say\nplainly that the store is not "
            "consulted. Citing #212 in the same comment\nblock also "
            "satisfies this guard.\n"
        )
        return 1

    sys.stdout.write(
        "[trust-claims] OK: %d files scanned, every trust-store mention "
        "either names the real mechanism or states the negative.\n" % scanned
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
