#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Enumerate account-name-bearing strings in BUILT BINARIES (GitHub #191).

This is an ENUMERATION tool, not a guard. It answers "what leaks, from where,
by what mechanism" for a build-output tree, and it REDACTS the account name in
everything it prints so its output can be pasted into a tracked document.

The existing tracked-file guard (UiSuitePathHygiene.NoTrackedFileCarriesTheBuild
AccountName) inspects only files under git control. Build output is untracked,
so it was never in that guard's scope -- which is why none of this was caught.

Usage:
    python tools/audit/scan_build_output_account_name.py [ROOT ...]
                                                         [--account NAME]
                                                         [--samples N]

Defaults to build/vs2022/bin/Debug. Walks subdirectories (Qt plugin dirs
platforms/ imageformats/ iconengines/ tls/ live there).

Output sections:
  1. per-binary occurrence counts
  2. per-MECHANISM counts with redacted representative samples
  3. the PE debug-directory PDB path, read structurally rather than by
     string-scraping, because that is a different leak surface from __FILE__.
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import struct
import sys

BINARY_EXTS = (".dll", ".exe", ".pyd", ".node")

# Printable-ASCII runs long enough to be a path.
_ASCII_RUN = re.compile(rb"[ -~]{16,}")
_UTF16_RUN = re.compile(r"[ -~]{16,}")


def redact(s: str, account: str) -> str:
    """Replace every occurrence of the account name with a placeholder."""
    out = re.sub(re.escape(account), "<account>", s, flags=re.IGNORECASE)
    return out


def extract(data: bytes, account: str):
    """Yield (string, encoding) for every run containing the account name."""
    needle = account.lower()
    for m in _ASCII_RUN.finditer(data):
        s = m.group().decode("ascii", "replace")
        if needle in s.lower():
            yield s, "ascii"
    try:
        u = data.decode("utf-16-le", "ignore")
    except Exception:
        return
    for m in _UTF16_RUN.finditer(u):
        s = m.group()
        if needle in s.lower():
            yield s, "utf-16le"


# Mechanism classification. Ordered: first match wins, most specific first.
def classify(s: str) -> str:
    low = s.lower().replace("/", "\\")
    if ".cargo\\registry" in low or ".cargo\\git" in low:
        return "M4 Rust .cargo registry source path (panic/assert message)"
    if re.match(r"^\s*(openssldir|enginesdir|modulesdir)\s*:", low):
        return "M3 OpenSSL configuration directory (FUNCTIONAL, resolved at runtime)"
    if low.endswith(".pdb") or ".pdb" in low:
        return "M2 PDB path (linker debug directory / embedded debug record)"
    if "buildtrees" in low:
        return "M1b __FILE__ source path, vcpkg buildtrees (dependency sources)"
    if re.search(r"\.(c|cc|cpp|cxx|h|hpp|hxx|inl|ipp|rs|asm|y|l)\b", low):
        return "M1a __FILE__ source path (project / vcpkg_installed headers)"
    # Post-source-extension so an OpenSSL *source* path under ssl/ is not
    # mistaken for a configured directory. These are the runtime-resolved dirs.
    if re.search(r"packages\\openssl[^\\]*(\\debug)?(\\(lib\\(engines-3|ossl-modules)|ssl))?(\\|$)", low) \
            or low.endswith(".cnf") or "ct_log_list" in low:
        return "M3 OpenSSL configuration directory (FUNCTIONAL, resolved at runtime)"
    if re.search(r"\.(rc|manifest|qrc|ui|qm|onnx|traineddata|pdf|json|xml)\b", low):
        return "M5 resource / data / manifest path"
    if "\\pdfe\\tests" in low or "/pdfe/tests" in low or "test_fixtures" in low:
        return "M5 CMake-injected absolute-path compile definition"
    if "vcpkg" in low:
        return "M1c vcpkg tree path, no source extension"
    return "M6? unclassified"


# ---------------------------------------------------------------------------
# PE debug directory: read the CodeView (RSDS) PDB path structurally.
# ---------------------------------------------------------------------------
def pe_pdb_path(path: str):
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except Exception:
        return None
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew + 24 > len(data) or data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return None
    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    if opt + 2 > len(data):
        return None
    magic = struct.unpack_from("<H", data, opt)[0]
    # data-directory base differs between PE32 (0x60) and PE32+ (0x70)
    dd = opt + (0x60 if magic == 0x10B else 0x70)
    # debug directory is data-directory index 6
    if dd + 7 * 8 + 8 > len(data):
        return None
    dbg_rva, dbg_size = struct.unpack_from("<II", data, dd + 6 * 8)
    if not dbg_rva or not dbg_size:
        return None

    # Build the section table so an RVA can be turned into a file offset.
    sect = opt + opt_size
    sections = []
    for i in range(num_sections):
        off = sect + i * 40
        if off + 40 > len(data):
            break
        va, rawsize, rawptr = struct.unpack_from("<III", data, off + 12)
        vsize = struct.unpack_from("<I", data, off + 8)[0]
        sections.append((va, max(vsize, rawsize), rawptr))

    def rva_to_off(rva):
        for va, size, rawptr in sections:
            if va <= rva < va + size:
                return rawptr + (rva - va)
        return None

    out = []
    base = rva_to_off(dbg_rva)
    if base is None:
        return None
    for i in range(dbg_size // 28):
        ent = base + i * 28
        if ent + 28 > len(data):
            break
        dtype = struct.unpack_from("<I", data, ent + 12)[0]
        size_of_data, _addr, ptr = struct.unpack_from("<III", data, ent + 16)
        if dtype != 2:  # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        cv = data[ptr:ptr + size_of_data]
        if cv[:4] != b"RSDS" or len(cv) < 25:
            continue
        name = cv[24:].split(b"\0", 1)[0].decode("utf-8", "replace")
        out.append(name)
    return out or None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", nargs="*", default=None)
    ap.add_argument("--account", default=os.environ.get("USERNAME") or "")
    ap.add_argument("--samples", type=int, default=4)
    args = ap.parse_args()

    account = args.account
    if not account:
        print("error: no account name; pass --account", file=sys.stderr)
        return 2
    roots = args.roots or ["build/vs2022/bin/Debug"]

    per_file = {}
    per_file_mech = collections.defaultdict(collections.Counter)
    by_mech = collections.Counter()
    samples = collections.defaultdict(list)
    pdbs = {}

    for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                if not fn.lower().endswith(BINARY_EXTS):
                    continue
                p = os.path.join(dirpath, fn)
                rel = os.path.relpath(p, root).replace("\\", "/")
                try:
                    with open(p, "rb") as fh:
                        data = fh.read()
                except Exception as exc:
                    print("  !! unreadable %s: %s" % (rel, exc), file=sys.stderr)
                    continue
                cv = pe_pdb_path(p)
                if cv:
                    for name in cv:
                        if account.lower() in name.lower():
                            pdbs.setdefault(root, {})[rel] = redact(name, account)
                n = 0
                for s, enc in extract(data, account):
                    n += 1
                    mech = classify(s)
                    by_mech[mech] += 1
                    per_file_mech[rel][mech] += 1
                    if len(samples[mech]) < args.samples:
                        tag = "" if enc == "ascii" else "   [%s]" % enc
                        samples[mech].append(
                            "%s%s   <- %s" % (redact(s.strip(), account), tag, rel))
                if n:
                    per_file[rel] = n

        print("=" * 72)
        print("ROOT: %s" % root)
        print("files carrying the account name: %d" % len(per_file))
        print("-" * 72)
        for f, n in sorted(per_file.items(), key=lambda kv: (-kv[1], kv[0])):
            mechs = ", ".join("%s:%d" % (m.split()[0], c)
                              for m, c in per_file_mech[f].most_common())
            print("  %-36s %8d   [%s]" % (f, n, mechs))
        print()
        print("--- mechanisms ---")
        for m, n in by_mech.most_common():
            print("\n  %s  -- %d occurrence(s)" % (m, n))
            for s in samples[m]:
                print("      %s" % s[:200])
        print()
        print("--- PE debug directory (CodeView RSDS) PDB paths carrying it ---")
        found = pdbs.get(root, {})
        if not found:
            print("  (none -- /PDBALTPATH or no CodeView record)")
        for rel, name in sorted(found.items()):
            print("  %-36s %s" % (rel, name))
        print()

        per_file = {}
        per_file_mech = collections.defaultdict(collections.Counter)
        by_mech = collections.Counter()
        samples = collections.defaultdict(list)
    return 0


if __name__ == "__main__":
    sys.exit(main())
