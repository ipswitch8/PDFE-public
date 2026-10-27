#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""An oracle that did not run must not read as green (GitHub #189).

THE SITUATION, MEASURED
-----------------------
`test_PDFs/` is gitignored (`.gitignore:235`, `test_PDFs/*`). 64 PDFs sit there
on the machine that produced them; TWO are re-included and tracked --
`crazyones-pdfa.pdf` (16 KB, `.gitignore:249`) and `multicolumn.pdf` (77 KB,
`.gitignore:273`), leaving 62 untracked. The directory is 332 MB of third-party
documents, so committing it is not on the table: it is a size problem and a
licensing problem at once, and neither improves by ignoring it.

(This paragraph said "exactly ONE" until 2026-09-25. That was true when written;
`multicolumn.pdf` was re-included later, for the same reason and on the same
terms. Corrected in place -- the growth-detection logic below was NOT touched.
Re-derived facts: docs/audit/test-pdfs-corpus-facts.md.)

WHAT THE HEADLINE OVERSTATES, AND WHAT IS REALLY LEFT
-----------------------------------------------------
Issue #189 names seven dependent test files. Re-derived from the sources
2026-09-14, five of them are already safe:

  * test_conformance_independent.cpp and test_fidelity_r3_19_pdfa_independent.cpp
    resolve `crazyones-pdfa.pdf`, which IS tracked (F45 re-included it for
    exactly this reason) and is pinned in the CMake fixture floor.
  * test_extraction_layout_realworld.cpp deliberately does NOT use the corpus --
    it was written for #27 against the committed tests/fixtures/realworld/
    arxiv_bert.pdf, whose absence is a CONFIGURE-time failure.
  * test_fixture_corpus.cpp only mentions the path; it is the guard, not a
    consumer.
  * test_pdf_open_robustness.py already exits 1 on a missing directory.

The live exposure is narrower and sharper: TWO GTEST_SKIP sites, both gated on
`test_PDFs/multicolumn.pdf`, in test_fidelity_r3_07_text_extraction.cpp and
test_layout_extraction_corpus.cpp. Both are honestly worded ("fixture missing
(not a pass)") -- and honesty in the reason string does not change the RESULT,
which is what a gate reads. On every clean checkout, including every CI run,
those two column-order oracles do not execute and the suite is green anyway.

WHY A RATCHET RATHER THAN A CONVERSION
---------------------------------------
Turning both into hard failures would redden every CI run today for an input CI
is never going to have, which trains people to ignore the red. Fetching the
corpus makes the build depend on a host nobody controls. So:

  * the class is FROZEN. Every corpus-gated skip site must be listed in the
    baseline with a written reason. A new one fails this check on the commit
    that adds it -- which is the failure mode that actually occurs, a new
    oracle quietly joining the silent set.
  * the baseline cannot go STALE. A listed site that no longer exists also
    fails, so the file cannot accumulate entries for code that is gone.
  * the skips are made LOUD at run time. `--xml` reads a JUnit artifact and
    prints, by name, every oracle that did not execute for want of the corpus,
    and fails if the count exceeds the baseline. tests/scripts/run_gtest.py
    calls this after every run, so "N oracles did not execute" appears next to
    the pass count instead of being inferrable only by reading 1270 skip sites.

Usage:
    python tools/gate/check_external_corpus_skips.py --repo-root .
    python tools/gate/check_external_corpus_skips.py --xml gtest_full.xml
    python tools/gate/check_external_corpus_skips.py --self-test

Exit 0 = the silent set is exactly the baseline, 1 = it grew or went stale.
"""
from __future__ import annotations

import argparse
import glob
import io
import json
import os
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BASELINE = os.path.join(HERE, "external_corpus_skip_baseline.json")

#: The external corpus, however it is spelled in a test.
CORPUS_REF = re.compile(r"testPdfsDir\s*\(\s*\)|test_PDFs")
SKIP_CALL = re.compile(r"\bGTEST_SKIP\s*\(")

#: How far above a GTEST_SKIP the corpus reference may sit. The real sites put
#: the path one or two lines up (`const fs::path pdf = testPdfsDir() / ...;` then
#: `if (!fs::exists(pdf)) GTEST_SKIP()`).
SKIP_WINDOW = 4


def _is_comment(line):
    s = line.lstrip()
    return s.startswith("//") or s.startswith("*") or s.startswith("/*")


#: The enclosing gtest macro, so a site is identified by the ORACLE it silences
#: rather than merely by the file it lives in.
TEST_MACRO = re.compile(
    r"^\s*TEST(?:_F|_P)?\s*\(\s*([A-Za-z_][\w]*)\s*,\s*([A-Za-z_][\w]*)\s*\)")


def enclosing_test(lines, index):
    """`Suite.Name` of the TEST containing line `index` (0-based), or ''.

    WHY THE TEST NAME AND NOT JUST THE FILE. Caught by mutation: keying the
    baseline on the FILE meant a baselined file could absorb any number of NEW
    corpus-gated oracles without the ratchet noticing -- adding a second silent
    oracle to test_layout_extraction_corpus.cpp went through green. A baseline
    that excuses a file excuses everything anyone later puts in it.
    """
    for i in range(index, -1, -1):
        m = TEST_MACRO.match(lines[i])
        if m:
            return "%s.%s" % (m.group(1), m.group(2))
    return ""


def scan_sources(tests_dir):
    """Every live GTEST_SKIP gated on the external corpus.

    Comment lines are excluded on BOTH sides: several files discuss the corpus
    and the skip in prose right next to real code, and crediting prose would
    both inflate this census and let a real site hide behind a paragraph.
    """
    found = []
    for path in sorted(glob.glob(os.path.join(tests_dir, "*.cpp"))):
        with io.open(path, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
        for i, line in enumerate(lines):
            if _is_comment(line) or not SKIP_CALL.search(line):
                continue
            window = [l for l in lines[max(0, i - SKIP_WINDOW):i + 1]
                      if not _is_comment(l)]
            if any(CORPUS_REF.search(l) for l in window):
                found.append({
                    "file": os.path.basename(path),
                    "test": enclosing_test(lines, i),
                    "line": i + 1,
                    "snippet": line.strip()[:120],
                })
    return found


def load_baseline(path):
    if not os.path.exists(path):
        return []
    with io.open(path, encoding="utf-8") as fh:
        return json.load(fh).get("accepted", [])


def _key(entry):
    # (file, test). NOT the file alone -- see enclosing_test().
    return (entry.get("file", ""), entry.get("test", ""))


def compare(found, accepted):
    """Returns (new_sites, stale_entries)."""
    have = {}
    for f in found:
        have.setdefault(_key(f), []).append(f)
    want = {_key(a): a for a in accepted}
    new = [f for f in found if _key(f) not in want]
    stale = [a for k, a in sorted(want.items()) if k not in have]
    return new, stale


# --------------------------------------------------------------------------
# runtime half -- read a JUnit artifact and say what did not execute
# --------------------------------------------------------------------------

def corpus_skips_in_xml(path):
    """Skipped testcases whose skip message names the external corpus."""
    tree = ET.parse(path)
    out = []
    for tc in tree.getroot().findall(".//testcase"):
        sk = tc.find("skipped")
        if sk is None and tc.get("result") != "skipped" and tc.get("status") != "skipped":
            continue
        msg = (sk.get("message") if sk is not None else "") or ""
        if sk is not None and not msg:
            msg = sk.text or ""
        if CORPUS_REF.search(msg):
            out.append({
                "test": "%s.%s" % (tc.get("classname", "?"), tc.get("name", "?")),
                "message": msg.strip()[:200],
            })
    return out


def report_xml(path, accepted, out=sys.stdout):
    """Loud banner + ratchet. Returns True when the run is within the baseline."""
    skips = corpus_skips_in_xml(path)
    if not skips:
        print("[external-corpus] 0 oracles skipped for want of test_PDFs/ "
              "(the corpus was present, or nothing depends on it).", file=out)
        return True
    print("", file=out)
    print("=" * 74, file=out)
    print("[external-corpus] %d ORACLE(S) DID NOT EXECUTE (GitHub #189)." % len(skips),
          file=out)
    print("  test_PDFs/ is gitignored -- 332 MB of third-party documents, 1 of 64",
          file=out)
    print("  files tracked. These tests SKIPPED, which counts as green in the",
          file=out)
    print("  summary line and in the JUnit XML. They asserted nothing:", file=out)
    for s in skips:
        print("    %s" % s["test"], file=out)
        print("        %s" % s["message"], file=out)
    print("=" * 74, file=out)
    print("", file=out)
    if len(skips) > len(accepted):
        print("[external-corpus] FAIL: %d corpus-gated skips at run time, but the "
              "baseline accepts %d. The silent set has GROWN -- a new oracle has "
              "joined the group that reads green without running."
              % (len(skips), len(accepted)), file=out)
        return False
    return True


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------

_SRC_WITH_SITE = """
TEST(Foo, Bar) {
    const fs::path pdf = testPdfsDir() / "multicolumn.pdf";
    if (!fs::exists(pdf)) GTEST_SKIP() << "fixture missing (not a pass): " << pdf;
    ASSERT_TRUE(true);
}
"""

_SRC_PROSE_ONLY = """
// This oracle used to resolve test_PDFs/multicolumn.pdf and GTEST_SKIP to
// green. It no longer does.
TEST(Foo, Baz) {
    const fs::path pdf = fixturesDir() / "realworld" / "arxiv_bert.pdf";
    ASSERT_TRUE(fs::exists(pdf));
}
"""

_SRC_UNRELATED_SKIP = """
TEST(Foo, Qux) {
    if (!haveFont()) GTEST_SKIP() << "host font arial.ttf absent";
    ASSERT_TRUE(true);
}
"""

_XML_WITH_SKIP = """<?xml version="1.0"?>
<testsuites><testsuite name="s">
<testcase classname="LayoutExtractionCorpus" name="NativeMultiColumn" status="skipped">
<skipped message="fixture missing (not a pass): C:/r/test_PDFs/multicolumn.pdf"/>
</testcase>
<testcase classname="Other" name="FontThing" status="skipped">
<skipped message="host font arial.ttf absent"/>
</testcase>
<testcase classname="Other" name="Passing"/>
</testsuite></testsuites>
"""


def self_test():
    failures = 0
    devnull = io.StringIO()

    def check(label, got, want):
        nonlocal failures
        if got != want:
            failures += 1
        print("  [%s] %-56s got=%r" % ("ok  " if got == want else "FAIL", label, got))

    tmp = tempfile.mkdtemp(prefix="ext_corpus_selftest_")
    tdir = os.path.join(tmp, "tests")
    os.makedirs(tdir)

    def write(name, body):
        with io.open(os.path.join(tdir, name), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(body)

    write("a.cpp", _SRC_WITH_SITE)
    found = scan_sources(tdir)
    check("a corpus-gated GTEST_SKIP is found", len(found), 1)
    check("and it names the file", found[0]["file"] if found else None, "a.cpp")

    write("a.cpp", _SRC_PROSE_ONLY)
    check("PROSE about the corpus is not a site", len(scan_sources(tdir)), 0)

    write("a.cpp", _SRC_UNRELATED_SKIP)
    check("an unrelated GTEST_SKIP is not a site", len(scan_sources(tdir)), 0)

    # The ratchet, both directions.
    write("a.cpp", _SRC_WITH_SITE)
    found = scan_sources(tdir)
    new, stale = compare(found, [{"file": "a.cpp", "test": "Foo.Bar",
                                  "why": "recorded"}])
    check("a baselined site is not reported NEW", (len(new), len(stale)), (0, 0))
    # THE MUTATION THAT KILLED THE FIRST IMPLEMENTATION. Keyed on the file
    # alone, a baselined FILE absorbed any number of new silent oracles.
    new2, _ = compare(found, [{"file": "a.cpp", "test": "Some.OtherTest",
                               "why": "recorded"}])
    check("a DIFFERENT test in a baselined FILE is still NEW", len(new2), 1)
    check("the site carries its enclosing test name",
          found[0]["test"] if found else None, "Foo.Bar")
    new, stale = compare(found, [])
    check("an UNBASELINED site is reported NEW", len(new), 1)
    new, stale = compare([], [{"file": "gone.cpp", "test": "Gone.Test",
                               "why": "recorded"}])
    check("a baseline entry with no site is reported STALE", len(stale), 1)

    # Runtime half.
    xml = os.path.join(tmp, "r.xml")
    with io.open(xml, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_XML_WITH_SKIP)
    skips = corpus_skips_in_xml(xml)
    check("only the CORPUS skip is counted, not the font skip", len(skips), 1)
    check("and it names the test", skips[0]["test"] if skips else None,
          "LayoutExtractionCorpus.NativeMultiColumn")
    check("within the baseline the run is accepted",
          report_xml(xml, [{"file": "x"}], out=devnull), True)
    check("OVER the baseline the run FAILS",
          report_xml(xml, [], out=devnull), False)

    print("self-test: %s" % ("PASS" if failures == 0 else "%d FAILURE(S)" % failures))
    return 1 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--xml", default="",
                    help="a JUnit artifact to report corpus-gated skips from")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    accepted = load_baseline(args.baseline)

    if args.xml:
        return 0 if report_xml(args.xml, accepted) else 1

    tests_dir = os.path.join(os.path.abspath(args.repo_root), "tests")
    found = scan_sources(tests_dir)
    print("[external-corpus] %d corpus-gated GTEST_SKIP site(s) in %s"
          % (len(found), tests_dir))
    for f in found:
        print("    %s:%d  [%s]  %s"
              % (f["file"], f["line"], f["test"] or "?", f["snippet"]))

    new, stale = compare(found, accepted)
    rc = 0
    if new:
        rc = 1
        print("", file=sys.stderr)
        print("[external-corpus] FAIL: %d NEW corpus-gated skip site(s). An "
              "oracle whose input is gitignored SKIPS on every clean checkout, "
              "and a skip reads as green to every gate. Either use a committed "
              "fixture (see tests/fixtures/realworld/arxiv_bert.pdf, whose "
              "absence is a configure-time failure), or add an entry to %s "
              "saying why this one cannot:" % (len(new), args.baseline),
              file=sys.stderr)
        for f in new:
            print("    %s:%d  [%s]  %s"
                  % (f["file"], f["line"], f["test"] or "?", f["snippet"]),
                  file=sys.stderr)
    if stale:
        rc = 1
        print("", file=sys.stderr)
        print("[external-corpus] FAIL: %d baseline entry/entries name a file "
              "with no corpus-gated skip left. The baseline must not outlive "
              "what it excuses -- delete the entry:" % len(stale), file=sys.stderr)
        for a in stale:
            print("    %s :: %s" % (a.get("file"), a.get("test")),
                  file=sys.stderr)

    if rc == 0:
        print("[external-corpus] PASS -- the silent set is exactly the %d "
              "baselined site(s); it has neither grown nor gone stale."
              % len(accepted))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
