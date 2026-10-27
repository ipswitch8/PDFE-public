#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sanitize_gtest_xml.py -- make a GoogleTest JUnit XML safe to commit.

WHY THIS EXISTS

GoogleTest writes the ABSOLUTE source path of every test into the `file`
attribute of every `<testcase>`:

    <testcase name="OptimizeEngineRefusesUnc"
              file="C:\\Users\\<account>\\source\\PDFE\\tests\\test_engine_unc_gate.cpp" ...>

So a raw --gtest_output=xml report carries the build account name once per test
-- 90 times in a 90-test report. Committing one as evidence writes that string
back into the repository.

That is not a theoretical concern here. This repository had its history
rewritten across 1,290 commits, over three separate leak surfaces, and was
finally recreated to drop PR refs, all to remove that string; it is intended to
become public. `UiSuitePathHygiene.NoTrackedFileCarriesTheBuildAccountName`
guards against it, and it caught exactly this: an evidence XML committed by
pipeline backlog-workable-p82 phase-1.

It was missed at the time because that phase's evidence run used a TARGETED
gtest filter which did not include the hygiene suite. A targeted filter is not a
sufficient oracle for "did I just commit something I should not have".

WHAT IT DOES

Rewrites absolute paths to repo-relative ones, so the evidence keeps its
meaning (you can still see which file each test came from) while carrying no
account name. Also scrubs the temp directory, which embeds the account name on
Windows via C:\\Users\\<account>\\AppData\\Local\\Temp.

Idempotent: running it twice produces the same bytes.

Exit 0 = wrote a clean file, 1 = something still matched after scrubbing
(which is a bug in this script, and is checked rather than assumed).
"""
import argparse
import io
import os
import re
import sys



# Redaction tokens, XML-escaped because this script edits raw XML text. A bare
# `<user>` would make the output ill-formed; `&lt;user&gt;` renders as `<user>`
# to any reader and keeps the document parseable.
_USER_TOKEN = "&lt;user&gt;"
_ACCOUNT_TOKEN = "&lt;account&gt;"

def sanitize(text, repo_root, account=""):
    """Replace absolute paths with repo-relative equivalents.

    `account`, when given, is additionally scrubbed as a BARE word. Paths are
    not the only carrier: `UiSuitePathHygiene`'s own failure message prints the
    account name in prose while explaining the violation, so an XML recording
    that failure leaks it even after every path is relative. Found by this
    script's own post-check refusing to write, which is why that check exists.
    """
    # Normalise both separators so one pattern covers Windows and POSIX forms.
    root_variants = {
        repo_root,
        repo_root.replace("/", "\\"),
        repo_root.replace("\\", "/"),
    }
    for root in sorted(root_variants, key=len, reverse=True):
        if not root:
            continue
        text = text.replace(root + "\\", "")
        text = text.replace(root + "/", "")
        text = text.replace(root, "")

    # Any remaining user-profile path (temp dirs, other checkouts) becomes a
    # placeholder. Deliberately broad: an unrecognised absolute path is more
    # likely to leak than to be informative.
    #
    # The tokens are XML-ESCAPED. This edits raw XML text, so writing a literal
    # `<user>` produces a file that is no longer well-formed -- and silently, on
    # exactly the inputs that needed redacting most. Found by the phase-5 karen
    # gate, which could not parse the evidence artifact this script had just
    # certified as clean: a pytest junit XML whose test id legitimately
    # contained `<`, giving `name="test_...[<user>\x-carol]"`. The account-name
    # post-check passed, because that check looks for the account name and not
    # for well-formedness, so the file read as verified while being unparseable.
    text = re.sub(r"[A-Za-z]:\\\\Users\\\\[^\\\\\"'<>\s]+", _USER_TOKEN, text)
    text = re.sub(r"[A-Za-z]:\\Users\\[^\\\"'<>\s]+", _USER_TOKEN, text)
    text = re.sub(r"/(?:c|C)/Users/[^/\"'<>\s]+", _USER_TOKEN, text)

    # Bare occurrences, case-insensitively -- prose, not paths.
    if account:
        text = re.sub(re.escape(account), _ACCOUNT_TOKEN, text,
                      flags=re.IGNORECASE)

    # GitHub #146 -- strip the JUnit `hostname` attribute.
    #
    # pytest emits `hostname="<machine>"` on every <testsuite> automatically, so
    # any committed pytest artifact carries the runner's host name whether or not
    # anyone intended it. That is machine-generated growth of an identifier the
    # repository otherwise ratchets (tools/gate/hostname_baseline.json), and it
    # is invisible in review because nobody writes it.
    #
    # Fixed AT EMISSION rather than only by the scan: the attribute carries no
    # information a reader of this artifact needs, so it is removed outright
    # instead of being placeholdered. GoogleTest does not emit it, so this is a
    # no-op for gtest reports.
    text = re.sub(r'\s+hostname="[^"]*"', "", text)

    return text


def main(argv):
    ap = argparse.ArgumentParser(
        description="Strip absolute paths (and thus the build account name) "
                    "from a GoogleTest JUnit XML so it can be committed.")
    ap.add_argument("src", help="input XML")
    ap.add_argument("dst", nargs="?", help="output XML (default: in place)")
    ap.add_argument("--repo-root", default=os.getcwd(),
                    help="path to strip; default is the cwd")
    ap.add_argument("--account", default="",
                    help="account name to verify absent afterwards; default is "
                         "taken from the repo root's own path")
    args = ap.parse_args(argv)

    dst = args.dst or args.src
    repo_root = os.path.abspath(args.repo_root)

    with io.open(args.src, encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    account = args.account
    if not account:
        parts = repo_root.replace("\\", "/").split("/")
        for i, p in enumerate(parts):
            if p.lower() == "users" and i + 1 < len(parts):
                account = parts[i + 1]
                break

    cleaned = sanitize(text, repo_root, account)

    # Verify rather than assume. If the account name is still present the file
    # must NOT be written, or this script becomes a false sense of security.

    if account and account.lower() in cleaned.lower():
        sys.stderr.write(
            "sanitize_gtest_xml: REFUSING TO WRITE -- %r still appears %d time(s) "
            "after scrubbing. Fix this script rather than committing the file.\n"
            % (account, cleaned.lower().count(account.lower())))
        return 1

    # ...and verify the result is still XML. The check above asks only "is the
    # account name gone", so for years this script could certify a file as clean
    # while having made it unparseable -- which is precisely what it did to a
    # pytest junit report whose test id contained `<`. "Verified" meant one
    # property, and the message did not say which. A consumer of the artifact
    # (CI, a gate, a person) then discovers the breakage far from here.
    try:
        import xml.etree.ElementTree as _ET
        _ET.fromstring(cleaned)
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write(
            "sanitize_gtest_xml: REFUSING TO WRITE -- the scrubbed output is no "
            "longer well-formed XML (%s). The redaction tokens must be "
            "XML-escaped; a bare '<user>' breaks any attribute it lands in.\n"
            % exc)
        return 1

    with io.open(dst, "w", encoding="utf-8", newline="") as fh:
        fh.write(cleaned)

    print("sanitize_gtest_xml: wrote %s (account name absent: verified)" % dst)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
