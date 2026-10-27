#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Post-build scrub: overwrite the build account name in packaged binaries.

GitHub #199 / #272. This is the "post-build scrub step" that #199's acceptance
criterion names alongside build-path stripping and deterministic build flags.

WHEN TO USE IT, AND WHEN NOT TO
===============================
Rebuilding through a neutral build root (scripts/Set-NeutralBuildRoot.ps1) is
the better fix and closes four of the five affected third-party DLLs outright.
Prefer it. This exists for the case that fix cannot reach:
`Qt6Widgets.dll` carries 2 `__FILE__` paths from `qdatetimeedit.cpp`, and
rebuilding qtbase needs tens of GB of buildtree plus a toolchain on the same
drive letter as the build root -- see #272 for the two measured failures.

So the ordering is: rebuild if you can, scrub what is left.

WHY THIS IS SAFE HERE, STATED PRECISELY
=======================================
1. **Length is preserved exactly.** The account name is replaced by the same
   number of bytes of a filler character, in place. No offset moves, no section
   grows, no relocation changes. A PE whose bytes change without changing length
   or structure loads identically.

2. **Nothing reads these strings as data.** They are `__FILE__` expansions used
   in assertion and warning text. Qt does not hash, length-check or compare
   them. (A string that WERE load-bearing -- a resource name, an import, a
   manifest -- must not be scrubbed, which is why this tool refuses anything
   that is not in its explicit target list; see below.)

3. **It cannot break a signature, because it never touches a signed file.**
   PDFE's own `pdfe.exe` IS code-signed for release, and byte-patching a signed
   image invalidates the signature. This tool therefore REFUSES any file that
   carries an Authenticode signature, rather than relying on the caller to pass
   the right list. vcpkg-built dependency DLLs are unsigned.

4. **It must run BEFORE signing**, for the same reason. In this repository that
   ordering is currently trivial -- releases are hand-signed on a workstation
   and no workflow signs anything (SEC-94's own open finding) -- but the
   constraint is real and is asserted here rather than left as a comment
   someone has to remember.

BOTH ENCODINGS, BECAUSE ONE IS NOT ENOUGH
=========================================
MSVC's `assert()` expands to `_wassert(L"" __FILE__, ...)`, so a large share of
embedded paths are UTF-16LE. An ASCII-only scrub leaves those untouched while
reporting success -- the exact false-green this project keeps paying for. Both
encodings are rewritten, and the verification pass re-reads the file with the
same matcher the gate uses.

Usage:
    python tools/gate/scrub_package_account_name.py <payload-root> [...]
        [--account NAME] [--filler x] [--dry-run] [--self-test]

Exit 0 = every occurrence removed (or none present), 1 = something remains or a
refusal applied, 2 = usage error.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import sys

# Share the gate's matcher so "did the scrub work" is answered by the same code
# that decides "is this a leak". Two matchers would let the scrub report success
# against a definition the gate does not use.
def _load(relpath, name):
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.normpath(os.path.join(here, relpath))
    if not os.path.exists(path):
        raise SystemExit("error: required module missing: %s" % path)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_audit = _load(os.path.join(os.pardir, "audit",
                            "scan_build_output_account_name.py"),
               "pdfe_acct_scan")

extract = _audit.extract
redact = _audit.redact

BINARY_EXTS = (".dll", ".exe", ".pyd", ".node", ".so")


def has_authenticode(data: bytes) -> bool:
    """True if the PE carries a signature (certificate table entry non-empty).

    Read structurally from the optional header's data directory rather than by
    searching for a marker, because a marker search both false-positives on
    embedded certificates and false-negatives on a stripped one -- and the
    direction that matters here is the false NEGATIVE, which would let the tool
    patch a signed image.

    LENGTHS ARE CHECKED, NOT JUST CONTENTS. Python slicing does not raise on an
    out-of-range slice, it returns a short result -- so a TRUNCATED file whose
    e_lfanew points past EOF yields b'' from data[pe:pe+4], the `!= b"PE\\0\\0"`
    comparison is true, and an earlier version of this returned False (NOT
    signed) without ever reaching the refuse-by-default branch. The phase-6
    security-audit gate found that. The property it claimed -- "refuses on any
    malformed header" -- was really "refuses once the header is readable far
    enough to see a bad magic".

    Note the deliberate asymmetry. A file that is not a PE at all returns False,
    because an unsigned non-PE binary should still be scrubbed normally. A file
    that CLAIMS to be a PE and then does not parse returns True (refuse),
    because that is precisely the case where we cannot tell whether a signature
    is present.
    """
    try:
        if len(data) < 0x40 or data[:2] != b"MZ":
            return False            # not a PE at all; scrub it normally
        pe = int.from_bytes(data[0x3C:0x40], "little")
        # Everything below reads fixed offsets from `pe`; bound them first.
        if pe < 0 or pe + 24 + 2 > len(data):
            return True             # truncated after claiming to be a PE: refuse
        if data[pe:pe + 4] != b"PE\0\0":
            return True             # MZ but no PE signature where promised: refuse
        opt = pe + 24
        magic = int.from_bytes(data[opt:opt + 2], "little")
        if magic == 0x20B:          # PE32+
            dd = opt + 112
        elif magic == 0x10B:        # PE32
            dd = opt + 96
        else:
            return True             # unknown: refuse rather than guess
        # Data directory entry 4 is IMAGE_DIRECTORY_ENTRY_SECURITY.
        ent = dd + 4 * 8
        if ent + 8 > len(data):
            return True             # directory truncated: refuse
        rva = int.from_bytes(data[ent:ent + 4], "little")
        size = int.from_bytes(data[ent + 4:ent + 8], "little")
        return rva != 0 and size != 0
    except Exception:
        return True                 # unreadable header: refuse


def scrub_bytes(data: bytes, account: str, filler: str):
    """Return (new_data, ascii_hits, utf16_hits). Length is preserved."""
    acct_a = account.encode("ascii", "ignore")
    repl_a = (filler * len(account)).encode("ascii")[:len(acct_a)]
    acct_w = account.encode("utf-16-le")
    repl_w = (filler * len(account)).encode("utf-16-le")[:len(acct_w)]

    out = bytearray(data)

    def replace_all(needle, repl):
        assert len(needle) == len(repl), "length must be preserved"
        n = 0
        start = 0
        low = bytes(out).lower()
        nl = needle.lower()
        while True:
            i = low.find(nl, start)
            if i < 0:
                break
            out[i:i + len(repl)] = repl
            low = low[:i] + repl.lower() + low[i + len(repl):]
            n += 1
            start = i + len(repl)
        return n

    a = replace_all(acct_a, repl_a)
    w = replace_all(acct_w, repl_w)
    return bytes(out), a, w


def self_test() -> int:
    ok = True
    acct = "someaccount"
    users = "C:\\" + "Users" + "\\"
    base = users + acct

    blob = (base + "\\src\\a.cpp").encode("ascii")
    new, a, w = scrub_bytes(blob, acct, "x")
    if len(new) != len(blob):
        print("self-test FAIL: length changed (%d -> %d)" % (len(blob), len(new)))
        ok = False
    if a != 1:
        print("self-test FAIL: expected 1 ASCII replacement, got %d" % a)
        ok = False
    if acct.encode() in new:
        print("self-test FAIL: the account name survived an ASCII scrub")
        ok = False

    wide = (base + "\\src\\b.cpp").encode("utf-16-le")
    new2, a2, w2 = scrub_bytes(wide, acct, "x")
    if w2 != 1:
        print("self-test FAIL: expected 1 UTF-16LE replacement, got %d -- "
              "MSVC's assert() puts __FILE__ in a WIDE string, so an "
              "ASCII-only scrub reports success and changes nothing" % w2)
        ok = False
    if acct.encode("utf-16-le") in new2:
        print("self-test FAIL: the account name survived a UTF-16LE scrub")
        ok = False
    if len(new2) != len(wide):
        print("self-test FAIL: UTF-16LE scrub changed length")
        ok = False

    # Verified with the SHARED matcher, not a local one.
    if list(extract(new, acct)) or list(extract(new2, acct)):
        print("self-test FAIL: the shared matcher still finds the name after "
              "a scrub")
        ok = False

    # --- the signature check's refusal behaviour -----------------------------
    # The phase-6 security-audit gate found that this was weaker than its own
    # docstring claimed: a truncated PE returned "not signed". These cases
    # assert the asymmetry directly, because "defaults to refuse" is exactly
    # the kind of property that reads as true and is not.
    if has_authenticode(b"not a pe at all"):
        print("self-test FAIL: a non-PE blob was treated as signed; it should "
              "be scrubbed normally")
        ok = False

    # Shorter than a DOS header -> not a PE, so False rather than an exception.
    if has_authenticode(b"MZ"):
        print("self-test FAIL: a 2-byte stub should read as not-a-PE, not as "
              "signed")
        ok = False

    # MZ present, e_lfanew points far past EOF -> REFUSE. This is the case that
    # previously returned False, because data[pe:pe+4] yields b'' rather than
    # raising, so the comparison succeeded and the refuse branch was skipped.
    truncated = bytearray(b"MZ" + b"\0" * 0x3E)
    truncated[0x3C:0x40] = (0x7FFF0000).to_bytes(4, "little")
    if not has_authenticode(bytes(truncated)):
        print("self-test FAIL: a PE whose e_lfanew points past EOF must be "
              "REFUSED. Python slicing returns b'' instead of raising, so this "
              "path silently read as 'not signed' before.")
        ok = False

    # MZ present, e_lfanew in range, but no PE signature there -> REFUSE.
    bad_sig = bytearray(b"MZ" + b"\0" * 0x7E)
    bad_sig[0x3C:0x40] = (0x40).to_bytes(4, "little")
    bad_sig[0x40:0x44] = b"XXXX"
    if not has_authenticode(bytes(bad_sig)):
        print("self-test FAIL: a file claiming to be a PE whose signature is "
              "not where it promised must be REFUSED, not scrubbed")
        ok = False

    print("self-test %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", nargs="*")
    ap.add_argument("--account", default=os.environ.get("USERNAME") or "")
    ap.add_argument("--filler", default="x")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.account:
        print("error: no account name; pass --account", file=sys.stderr)
        return 2
    if len(args.filler) != 1:
        print("error: --filler must be exactly one character", file=sys.stderr)
        return 2
    if not args.roots:
        print("error: no payload root given", file=sys.stderr)
        return 2

    print("=== post-build account-name scrub (GitHub #199 / #272) ===")
    listed = read = patched = refused = 0
    remaining = {}

    # Roots may be DIRECTORIES or individual FILES. Files matter because the
    # recorded decision on the test-only carriers (GitHub #191's third
    # criterion) is to leave them alone: `pdfe_tests.exe` holds 850 occurrences,
    # 531 of them this project's own test sources, and rewriting those would
    # make its assertion output less useful for no gain, since nothing ships it.
    # A directory walk over a build output folder cannot express "these five
    # DLLs but not that test binary"; a file list can.
    targets = []
    for root in args.roots:
        if os.path.isfile(root):
            targets.append((root, os.path.basename(root)))
        elif os.path.isdir(root):
            for dirpath, _d, files in os.walk(root):
                for fn in files:
                    if fn.lower().endswith(BINARY_EXTS):
                        p = os.path.join(dirpath, fn)
                        targets.append(
                            (p, os.path.relpath(p, root).replace("\\", "/")))
        else:
            print("error: not a file or directory: %s" % root, file=sys.stderr)
            return 2

    for p, rel in targets:
        if not p.lower().endswith(BINARY_EXTS):
            continue
        listed += 1
        try:
            with open(p, "rb") as fh:
                data = fh.read()
        except OSError as exc:
            print("  UNREADABLE %s (%s)" % (rel, exc))
            continue
        read += 1

        before = len(list(extract(data, args.account)))
        if before == 0:
            continue

        if has_authenticode(data):
            # REFUSE. Patching a signed image invalidates the
            # signature, and this must never be the caller's problem
            # to remember.
            print("  REFUSED (signed) %-28s %d occurrence(s)"
                  % (rel, before))
            refused += 1
            remaining[rel] = before
            continue

        new, a, w = scrub_bytes(data, args.account, args.filler)
        if len(new) != len(data):
            print("  REFUSED (length changed) %s" % rel)
            refused += 1
            remaining[rel] = before
            continue

        after = len(list(extract(new, args.account)))
        if after != 0:
            print("  INCOMPLETE %-28s %d -> %d" % (rel, before, after))
            remaining[rel] = after
            continue

        if args.dry_run:
            print("  would scrub %-28s %d occurrence(s) "
                  "(ascii %d, utf-16 %d)" % (rel, before, a, w))
        else:
            # The write is guarded because an unwritable file is a realistic
            # case -- a read-only attribute, a DLL held open by a running
            # process, an ACL -- and an unhandled OSError here would end the
            # run with a traceback. A traceback is a worse outcome than a clean
            # failure for the same reason a silent skip is: whoever is reading
            # CI output has to work out whether anything was scrubbed at all.
            try:
                with open(p, "wb") as fh:
                    fh.write(new)
            except OSError as exc:
                print("  UNWRITABLE %-26s %s" % (rel, exc))
                remaining[rel] = before
                continue
            # Re-read from DISK. An in-memory check does not prove the
            # write landed.
            try:
                with open(p, "rb") as fh:
                    back = fh.read()
            except OSError as exc:
                print("  UNVERIFIABLE AFTER WRITE %-14s %s" % (rel, exc))
                remaining[rel] = before
                continue
            if len(back) != len(data) or list(extract(back, args.account)):
                print("  FAILED AFTER WRITE %s" % rel)
                remaining[rel] = before
                continue
            print("  scrubbed %-28s %d occurrence(s) "
                  "(ascii %d, utf-16 %d)" % (rel, before, a, w))
        patched += 1

    print("\n  binaries listed : %d" % listed)
    print("  binaries read   : %d" % read)
    print("  scrubbed        : %d" % patched)
    print("  refused         : %d" % refused)

    if read != listed:
        print("\nFAIL: %d binar(ies) listed but not read. A scrub that skipped "
              "a file it could not open must not report success."
              % (listed - read))
        return 1

    if remaining:
        print("\nFAIL: the account name remains in:")
        for rel, n in sorted(remaining.items(), key=lambda kv: -kv[1]):
            print("    %-30s %d" % (rel, n))
        return 1

    print("\nscrub_package_account_name: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
