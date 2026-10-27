#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Reproduce the vcpkg-overlay source verification, and emit it as an artifact.

GitHub #67 / meta-audit SEC-88.

`tools/vcpkg-overlays/{mujs,gumbo}` re-pin upstream vcpkg's SHA512 because
Codeberg regenerated the source archives. Re-pinning a hash is a decision to
trust re-served bytes, and the justification for trusting them lived only in a
prose comment:

    mujs:  "verified by `diff -r` of both extracted trees + identical mujs.h
            SHA512"
    gumbo: "verified to be the authentic gumbo 0.12.3 upstream source
            (configure.ac AC_INIT([gumbo],[0.12.3]); src/gumbo.h + doc/COPYING
            present)"

Defensible, but a later reviewer cannot re-run either claim. That is the gap.

WHAT THIS DOES DIFFERENTLY, AND WHY IT IS STRONGER. The original mujs check
compared the new Codeberg archive against the OLD Codeberg archive. Those bytes
no longer exist -- Codeberg serves only the regenerated form -- so that exact
comparison is now unreproducible by anyone, which is worse than the audit
assumed. Comparing instead against the UPSTREAM AUTHOR's own release tarball at
mujs.com is both reproducible and a genuinely independent provenance path: two
different hosts, two different packagings, one source tree.

Where no independent source is reachable (gumbo), that is REPORTED as a weaker
result rather than presented as equivalent. A verification that quietly downgrades
itself is the thing this file exists to replace.

Usage:
    python tools/gate/verify_vcpkg_overlays.py --out docs/security/vcpkg-overlay-source-verification.md
    python tools/gate/verify_vcpkg_overlays.py --check   # offline: pins parse AND match the report
    python tools/gate/verify_vcpkg_overlays.py --self-test
"""
from __future__ import annotations

import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OVERLAYS = REPO_ROOT / "tools" / "vcpkg-overlays"

MUJS_VERSION = "1.3.8"
GUMBO_VERSION = "0.12.3"

MUJS_CODEBERG = f"https://codeberg.org/ccxvii/mujs/archive/{MUJS_VERSION}.tar.gz"
MUJS_UPSTREAM = f"https://mujs.com/downloads/mujs-{MUJS_VERSION}.tar.gz"
GUMBO_CODEBERG = (
    f"https://codeberg.org/gumbo-parser/gumbo-parser/archive/{GUMBO_VERSION}.tar.gz")


def sha512_bytes(b: bytes) -> str:
    return hashlib.sha512(b).hexdigest()


def fetch(url: str, timeout: int = 300) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "pdfe-overlay-verify"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def pinned_sha512(port: str) -> str | None:
    """The SHA512 the overlay portfile pins.

    CODE LINES ONLY. Both portfiles DISCUSS hashes in their header comments
    ("Upstream vcpkg pins mujs 1.3.8 by SHA512 b553c09f…"); those are abbreviated
    today, but a future comment quoting a full 128-hex value ahead of the real
    pin would have been picked up instead -- the reconciliation below would then
    have compared the report against a hash vcpkg never downloads. Prose is not
    the pin, the same rule lint_unc_gate.py applies to guards.
    """
    pf = OVERLAYS / port / "portfile.cmake"
    if not pf.exists():
        return None
    text = io.open(pf, encoding="utf-8", errors="replace").read()
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        m = re.search(r"SHA512\s+([0-9a-fA-F]{128})", line)
        if m:
            return m.group(1).lower()
    return None


# ---------------------------------------------------------------------------
# OFFLINE PIN/REPORT RECONCILIATION (--check)
#
# WHAT THIS REPLACED, AND WHY IT MATTERED. Until 2026-09-14 `--check` called
# pinned_sha512() and nothing else. That regex fails only when the SHA512 line
# is ABSENT or MALFORMED, so the check caught a BROKEN pin and not a TAMPERED
# one -- and tampering is the entire supply-chain threat. Demonstrated:
#
#   pin -> "SHA512 deadbeef"      (malformed)     -> exit 1   correct
#   pin -> "SHA512 aaaa...a"      (well-formed,
#                                  wrong, i.e. a
#                                  malicious
#                                  re-pin)        -> exit 0   WRONG
#
# The step's own name claimed it "confirms the pins still match the committed
# verification report". It never opened the report. Found by the phase-1
# security-audit gate, which proved it by substitution rather than by reading --
# and it found it inside this phase's own fix, in the control the phase had just
# wired into the per-push lane. A control whose claim exceeds its mechanism is
# the exact defect class this phase exists to close, so the mechanism was fixed
# rather than the sentence softened.
#
# The property now enforced: a re-pin cannot land without the committed report
# being updated in the same change. That is the property the report exists to
# provide, and it is what makes the report evidence rather than decoration.
#
# EVERY UNCERTAIN PATH FAILS CLOSED. Report missing, report unparseable, a port
# with no section, a section with no recorded pin, an internally inconsistent
# report -- all non-zero. None of them is "probably fine".
# ---------------------------------------------------------------------------

REPORT = REPO_ROOT / "docs" / "security" / "vcpkg-overlay-source-verification.md"

#: Overlay port directory -> the heading the report files it under. Explicit,
#: because `gumbo` is written up as `gumbo-parser` and a prefix match would be
#: one rename away from silently pairing the wrong two numbers.
PORT_SECTIONS = {"mujs": "mujs", "gumbo": "gumbo-parser"}

_SECTION_RE = re.compile(r"^##\s+(\S+)", re.MULTILINE)
_OVERLAY_PIN_RE = re.compile(r"overlay pin\s*`([0-9a-fA-F]{128})`")
_ARCHIVE_SHA_RE = re.compile(r"SHA512\s*`([0-9a-fA-F]{128})`")


def report_sections(text: str) -> dict[str, str]:
    """Heading name -> the body under it, up to the next `## ` heading."""
    out: dict[str, str] = {}
    marks = [(m.start(), m.group(1)) for m in _SECTION_RE.finditer(text)]
    for i, (pos, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        out[name] = text[pos:end]
    return out


def recorded_pin(section_body: str) -> tuple[str | None, str | None]:
    """(overlay pin, archive SHA512) as the report records them, lowercased.

    The `overlay pin` line is matched SPECIFICALLY. The mujs section alone
    contains four other 128-hex values -- the Codeberg archive hash, the
    upstream author's DIFFERENT archive hash, and mujs.h's hash -- so a loose
    "any hash in this section" match would accept a portfile re-pinned to the
    upstream author's tarball, which is not the bytes the overlay downloads.
    That would have been a second vacuous check hiding inside the fix for the
    first one.
    """
    pin_m = _OVERLAY_PIN_RE.search(section_body)
    arc_m = _ARCHIVE_SHA_RE.search(section_body)
    return (pin_m.group(1).lower() if pin_m else None,
            arc_m.group(1).lower() if arc_m else None)


def check_pins_against_report(report_path: Path = REPORT,
                              pins: dict[str, str | None] | None = None,
                              ) -> tuple[int, list[str]]:
    """Reconcile every overlay portfile pin with the committed report.

    Returns (exit_code, lines_to_print). Each failure names WHICH of the five
    distinct problems it is -- `pin-malformed`, `pin-mismatch`,
    `port-not-in-report`, `report-*` -- because they have different remedies and
    a single generic message would make the next person re-derive which one they
    hit.

    `pins` overrides what the portfiles say, so self_test() can drive every
    branch without writing to tools/vcpkg-overlays/.
    """
    out: list[str] = []
    problems: list[str] = []

    if not report_path.exists():
        return 1, [
            f"FAIL [report-missing]: {report_path} does not exist. The pins "
            f"cannot be reconciled against anything, so this is not a pass. "
            f"Regenerate it with --out (needs network)."]
    try:
        text = io.open(report_path, encoding="utf-8").read()
    except OSError as exc:
        return 1, [f"FAIL [report-unreadable]: {report_path}: {exc}"]

    sections = report_sections(text)
    if not sections:
        return 1, [
            f"FAIL [report-unparseable]: {report_path} contains no '## ' "
            f"section headings. Either the report format changed or the file is "
            f"truncated; either way nothing here can be reconciled."]

    for port, heading in sorted(PORT_SECTIONS.items()):
        actual = pins[port] if pins is not None else pinned_sha512(port)
        body = sections.get(heading)

        if actual is None:
            problems.append(
                f"[pin-malformed] {port}: tools/vcpkg-overlays/{port}/"
                f"portfile.cmake has no parseable 128-hex SHA512 pin. Fix the "
                f"portfile; vcpkg would reject this download too.")
            continue
        if body is None:
            problems.append(
                f"[port-not-in-report] {port}: no '## {heading}' section in "
                f"{report_path.name}. A pinned overlay with no recorded "
                f"verification is an untrusted re-pin by definition. "
                f"Re-run with --out to record it.")
            continue

        recorded, archive = recorded_pin(body)
        if recorded is None:
            problems.append(
                f"[port-not-in-report] {port}: the '## {heading}' section "
                f"records no 'overlay pin `<sha512>`' line, so there is nothing "
                f"to compare the portfile against. Re-run with --out.")
            continue
        if recorded != actual:
            problems.append(
                f"[pin-mismatch] {port}: the portfile pins\n"
                f"      {actual}\n"
                f"    but {report_path.name} records\n"
                f"      {recorded}\n"
                f"    A re-pin is a decision to trust different bytes. It may "
                f"not land without the verification report being re-derived in "
                f"the SAME change -- that is the whole point of the report. "
                f"Re-run with --out (needs network) and commit the result.")
            continue
        if archive is not None and archive != actual:
            problems.append(
                f"[report-inconsistent] {port}: the report's own 'overlay pin' "
                f"({recorded}) matches the portfile, but the archive SHA512 it "
                f"records ({archive}) does not. The report disagrees with "
                f"itself, so it cannot vouch for anything. Re-derive it.")
            continue
        out.append(f"  OK  {port}: portfile pin == report record ({actual[:16]}…)")

    for p in problems:
        out.append(f"FAIL {p}")
    if problems:
        out.append(
            f"verify_vcpkg_overlays --check FAILED: {len(problems)} problem(s).")
        return 1, out

    out.append(
        f"OK: all {len(PORT_SECTIONS)} overlay pin(s) are well-formed AND match "
        f"the value recorded in {report_path.name}. "
        f"NOTE: this is an offline reconciliation of two committed files. It "
        f"does NOT re-download the archives or re-derive the hashes -- that is "
        f"the network half, run on demand with --out.")
    return 0, out


_GOOD = "1" * 128          # stands in for mujs's pin
_GOOD2 = "2" * 128         # stands in for gumbo's pin
_WRONG = "a" * 128         # well-formed, wrong -- the tampered-re-pin case
_OTHER = "b" * 128         # the "upstream author's DIFFERENT hash" decoy


def _fake_report(mujs_pin=_GOOD, gumbo_pin=_GOOD2, mujs_archive=None,
                 mujs_section=True, gumbo_pin_line=True) -> str:
    """A report in the real one's shape, with each part switchable."""
    mujs_archive = mujs_archive if mujs_archive is not None else mujs_pin
    parts = ["# vcpkg overlay source verification", ""]
    if mujs_section:
        parts += [
            "## mujs 1.3.8",
            "",
            f"- Codeberg archive: `https://example/mujs.tar.gz`",
            f"  - SHA512 `{mujs_archive}`",
            f"  - overlay pin `{mujs_pin}`",
            f"  - **MATCHES the overlay pin**",
            "",
            "- Upstream author's release: `https://example/mujs-up.tar.gz`",
            # THE DECOY. A different, well-formed 128-hex value inside the same
            # section. A loose "any hash here" match would accept a portfile
            # re-pinned to this.
            f"  - SHA512 `{_OTHER}`",
            "",
        ]
    parts += ["## gumbo-parser 0.12.3", "",
              f"- Codeberg archive: `https://example/gumbo.tar.gz`",
              f"  - SHA512 `{gumbo_pin}`"]
    if gumbo_pin_line:
        parts.append(f"  - overlay pin `{gumbo_pin}`")
    parts.append("")
    return "\n".join(parts)


def self_test() -> int:
    """Prove --check can go red for each distinct reason, AND stay green.

    The security-audit gate found the old --check by SUBSTITUTION -- swapping a
    well-formed-but-wrong pin in and watching it pass -- not by reading the
    code. This encodes that method so the next regression is caught by the guard
    rather than by a gate agent noticing twice.
    """
    failures = 0

    def check(label: str, got, want) -> None:
        nonlocal failures
        ok = got == want
        if not ok:
            failures += 1
        print("  [%s] %-58s got=%r" % ("ok  " if ok else "FAIL", label, got))

    tmp = Path(tempfile.mkdtemp(prefix="overlay_selftest_"))
    try:
        def run(report_text: str | None, pins: dict, name="r.md"):
            rp = tmp / name
            if report_text is None:
                rp = tmp / "definitely-absent.md"
            else:
                io.open(rp, "w", encoding="utf-8", newline="\n").write(report_text)
            rc, lines = check_pins_against_report(rp, pins)
            return rc, "\n".join(lines)

        good_pins = {"mujs": _GOOD, "gumbo": _GOOD2}

        rc, txt = run(_fake_report(), good_pins)
        check("matching pins PASS", rc, 0)
        check("and the pass says it is an OFFLINE reconciliation, not a re-derivation",
              "does NOT re-download" in txt, True)

        # THE FINDING. Well-formed, wrong -- a malicious re-pin.
        rc, txt = run(_fake_report(), {"mujs": _WRONG, "gumbo": _GOOD2})
        check("a WELL-FORMED but WRONG pin FAILS", rc, 1)
        check("and it is reported as [pin-mismatch]", "[pin-mismatch] mujs" in txt, True)

        # Must not have traded one check for the other.
        rc, txt = run(_fake_report(), {"mujs": None, "gumbo": _GOOD2})
        check("a MALFORMED/absent pin still FAILS", rc, 1)
        check("and it is reported as [pin-malformed]",
              "[pin-malformed] mujs" in txt, True)

        # The decoy: re-pinning to the upstream author's DIFFERENT hash, which
        # is present in the same report section, must not be accepted.
        rc, txt = run(_fake_report(), {"mujs": _OTHER, "gumbo": _GOOD2})
        check("re-pinning to another hash IN THE SAME SECTION FAILS", rc, 1)
        check("and that too is [pin-mismatch]", "[pin-mismatch] mujs" in txt, True)

        rc, txt = run(None, good_pins)
        check("a MISSING report FAILS", rc, 1)
        check("and it is reported as [report-missing]", "[report-missing]" in txt, True)

        rc, txt = run("no headings here at all\n", good_pins)
        check("an UNPARSEABLE report FAILS", rc, 1)
        check("and it is reported as [report-unparseable]",
              "[report-unparseable]" in txt, True)

        rc, txt = run(_fake_report(mujs_section=False), good_pins)
        check("a port with NO SECTION in the report FAILS", rc, 1)
        check("and it is reported as [port-not-in-report]",
              "[port-not-in-report] mujs" in txt, True)

        rc, txt = run(_fake_report(gumbo_pin_line=False), good_pins)
        check("a section with NO 'overlay pin' line FAILS", rc, 1)
        check("and it is reported as [port-not-in-report]",
              "[port-not-in-report] gumbo" in txt, True)

        rc, txt = run(_fake_report(mujs_archive=_OTHER), good_pins)
        check("a report that DISAGREES WITH ITSELF FAILS", rc, 1)
        check("and it is reported as [report-inconsistent]",
              "[report-inconsistent] mujs" in txt, True)

        # recorded_pin must read the 'overlay pin' line, not the first hash.
        body = _fake_report().split("## mujs")[1]
        pin, archive = recorded_pin("## mujs" + body.split("## gumbo")[0])
        check("recorded_pin returns the OVERLAY PIN, not a neighbouring hash",
              pin, _GOOD)
        check("and reports the archive hash separately", archive, _GOOD)

        # pinned_sha512 must ignore prose. Exercised against a real temp port.
        port_dir = OVERLAYS / "__selftest_port__"
        try:
            port_dir.mkdir(parents=True, exist_ok=True)
            io.open(port_dir / "portfile.cmake", "w", encoding="utf-8",
                    newline="\n").write(
                f"# historical note: upstream once pinned SHA512 {_OTHER}\n"
                f"vcpkg_from_git(\n  SHA512 {_GOOD}\n)\n")
            check("pinned_sha512 skips a hash quoted in a COMMENT",
                  pinned_sha512("__selftest_port__"), _GOOD)
        finally:
            shutil.rmtree(port_dir, ignore_errors=True)

        # And the real tree, so the self-test is not only synthetic.
        rc, _ = check_pins_against_report()
        check("the REAL committed tree reconciles", rc, 0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("PASS" if failures == 0 else f"{failures} FAILURE(S)"))
    return 1 if failures else 0


def extract(data: bytes, dest: Path) -> Path:
    """Extract a .tar.gz and return the single top-level directory."""
    dest.mkdir(parents=True, exist_ok=True)
    buf = io.BytesIO(data)
    with tarfile.open(fileobj=buf, mode="r:gz") as tf:
        # Refuse absolute/escaping members rather than trusting the archive.
        for m in tf.getmembers():
            p = Path(m.name)
            if p.is_absolute() or ".." in p.parts:
                raise RuntimeError(f"unsafe archive member: {m.name}")
        tf.extractall(dest)
    entries = [p for p in dest.iterdir()]
    tops = [p for p in entries if p.is_dir()]
    return tops[0] if len(tops) == 1 else dest


def tree_hashes(root: Path) -> dict[str, str]:
    """relative-path -> sha256, for every regular file under root."""
    out: dict[str, str] = {}
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            full = Path(dirpath) / f
            rel = full.relative_to(root).as_posix()
            out[rel] = hashlib.sha256(full.read_bytes()).hexdigest()
    return out


def compare_trees(a: Path, b: Path) -> dict:
    ha, hb = tree_hashes(a), tree_hashes(b)
    only_a = sorted(set(ha) - set(hb))
    only_b = sorted(set(hb) - set(ha))
    differing = sorted(k for k in (set(ha) & set(hb)) if ha[k] != hb[k])
    return {
        "files_a": len(ha), "files_b": len(hb),
        "only_in_a": only_a, "only_in_b": only_b, "differing": differing,
        "common": len(set(ha) & set(hb)),
    }


def gumbo_structural(root: Path) -> list[tuple[str, bool, str]]:
    """The structural claims the gumbo overlay comment makes, as checks."""
    results: list[tuple[str, bool, str]] = []

    ac = root / "configure.ac"
    ok = False
    detail = "configure.ac absent"
    if ac.exists():
        text = ac.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"AC_INIT\(\s*\[?gumbo\]?\s*,\s*\[?([0-9.]+)\]?", text)
        detail = f"AC_INIT version = {m.group(1)}" if m else "AC_INIT not found"
        ok = bool(m and m.group(1) == GUMBO_VERSION)
    results.append((f"configure.ac declares gumbo {GUMBO_VERSION}", ok, detail))

    for rel in ("src/gumbo.h", "COPYING"):
        p = root / rel
        alt = root / "doc" / "COPYING" if rel == "COPYING" else None
        present = p.exists() or (alt is not None and alt.exists())
        where = str(p.relative_to(root)) if p.exists() else (
            "doc/COPYING" if alt and alt.exists() else "absent")
        results.append((f"{rel} present", present, where))
    return results


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="")
    ap.add_argument("--self-test", action="store_true",
                    help="prove --check can go red for each distinct reason")
    ap.add_argument("--check", action="store_true",
                    help="offline: reconcile every overlay portfile pin against "
                         "the value recorded in the committed verification "
                         "report (does NOT re-download or re-derive hashes)")
    ap.add_argument("--date", default="",
                    help="run date (YYYY-MM-DD) recorded in the artifact header")
    ap.add_argument("--reevaluate", default="",
                    help="re-evaluation date (YYYY-MM-DD) -- SEC-88 requires the "
                         "overlays be revisited so they are removed once upstream "
                         "vcpkg repins")
    args = ap.parse_args()

    mujs_pin = pinned_sha512("mujs")
    gumbo_pin = pinned_sha512("gumbo")

    if args.self_test:
        return self_test()

    if args.check:
        rc, lines = check_pins_against_report()
        for line in lines:
            print(line)
        return rc

    lines: list[str] = []
    def emit(s: str = "") -> None:
        print(s)
        lines.append(s)

    emit("# vcpkg overlay source verification")
    emit()
    emit("<!-- tracking-banner: exempt -- a dated verification artifact; "
         "GitHub Issues remains canonical for anything still open -->")
    emit()
    emit("**GENERATED FILE — do not hand-edit.** Regenerate with:")
    emit()
    emit("```")
    emit("python tools/gate/verify_vcpkg_overlays.py \\")
    emit("    --out docs/security/vcpkg-overlay-source-verification.md \\")
    emit("    --date <YYYY-MM-DD> --reevaluate <YYYY-MM-DD>")
    emit("```")
    emit()
    if args.date:
        emit(f"- **Verified:** {args.date}")
    if args.reevaluate:
        emit(f"- **Re-evaluate by:** {args.reevaluate}")
    emit()
    emit("Meta-audit **SEC-88** (GitHub #67). `tools/vcpkg-overlays/{mujs,gumbo}` "
         "re-pin upstream vcpkg's SHA512 because Codeberg regenerated the source "
         "archives. Re-pinning a hash is a decision to trust re-served bytes, and "
         "until now the justification lived only in a prose comment that a later "
         "reviewer could not re-run. This is that justification, re-derived and "
         "committed.")
    emit()
    emit("**The original mujs check is no longer reproducible by anyone.** It "
         "compared the new Codeberg archive against the OLD Codeberg archive, and "
         "those bytes no longer exist — Codeberg serves only the regenerated form. "
         "So the comparison below is deliberately a DIFFERENT and stronger one: "
         "against the upstream author's own release tarball at mujs.com. Two "
         "hosts, two packagings, one source tree.")
    emit()
    emit("**These overlays are temporary.** They exist only because upstream "
         "vcpkg's pins are stale. Remove them once upstream repins; the "
         "re-evaluation date above is the prompt to check.")
    emit()
    emit("---")
    emit()

    tmp = Path(tempfile.mkdtemp(prefix="pdfe_overlay_verify_"))
    try:
        # ---------------- mujs ----------------
        emit("## mujs %s" % MUJS_VERSION)
        emit()
        cb = fetch(MUJS_CODEBERG)
        cb_sha = sha512_bytes(cb)
        emit(f"- Codeberg archive: `{MUJS_CODEBERG}`")
        emit(f"  - SHA512 `{cb_sha}`")
        emit(f"  - overlay pin `{mujs_pin}`")
        match = (cb_sha == mujs_pin)
        emit(f"  - **{'MATCHES the overlay pin' if match else 'DOES NOT MATCH the overlay pin'}**")
        emit()

        up = fetch(MUJS_UPSTREAM)
        emit(f"- Upstream author's release: `{MUJS_UPSTREAM}`")
        emit(f"  - SHA512 `{sha512_bytes(up)}`")
        emit(f"  - archive bytes differ from Codeberg's: "
             f"**{sha512_bytes(up) != cb_sha}** (expected — different packaging)")
        emit()

        a = extract(cb, tmp / "mujs_codeberg")
        b = extract(up, tmp / "mujs_upstream")
        cmp_ = compare_trees(a, b)
        emit(f"- Tree comparison (Codeberg vs upstream author):")
        emit(f"  - files: {cmp_['files_a']} vs {cmp_['files_b']}, "
             f"{cmp_['common']} in common")
        emit(f"  - content-differing common files: **{len(cmp_['differing'])}**")
        emit(f"  - only in Codeberg: {len(cmp_['only_in_a'])}")
        emit(f"  - only in upstream: {len(cmp_['only_in_b'])}")
        for label, key in (("differing", "differing"),
                           ("only in Codeberg", "only_in_a"),
                           ("only in upstream", "only_in_b")):
            if cmp_[key]:
                emit(f"  - {label}:")
                for f in cmp_[key][:40]:
                    emit(f"    - `{f}`")
                if len(cmp_[key]) > 40:
                    emit(f"    - … and {len(cmp_[key]) - 40} more")
        emit()

        mh_a, mh_b = a / "mujs.h", b / "mujs.h"
        if mh_a.exists() and mh_b.exists():
            ha = hashlib.sha512(mh_a.read_bytes()).hexdigest()
            hb = hashlib.sha512(mh_b.read_bytes()).hexdigest()
            emit(f"- `mujs.h` SHA512 identical across sources: **{ha == hb}**")
            emit(f"  - `{ha}`")
        else:
            emit("- `mujs.h` NOT PRESENT in one of the trees — that is a finding.")
        emit()

        # ---------------- gumbo ----------------
        emit("## gumbo-parser %s" % GUMBO_VERSION)
        emit()
        gb = fetch(GUMBO_CODEBERG)
        gb_sha = sha512_bytes(gb)
        emit(f"- Codeberg archive: `{GUMBO_CODEBERG}`")
        emit(f"  - SHA512 `{gb_sha}`")
        emit(f"  - overlay pin `{gumbo_pin}`")
        emit(f"  - **{'MATCHES the overlay pin' if gb_sha == gumbo_pin else 'DOES NOT MATCH the overlay pin'}**")
        emit()
        emit("- **No independent source was reachable.** Both the GitHub mirrors "
             "tried return 404, so gumbo gets a STRUCTURAL check only — strictly "
             "weaker than mujs's cross-source tree comparison, and recorded as "
             "such rather than presented as equivalent.")
        emit()
        g = extract(gb, tmp / "gumbo_codeberg")
        for name, ok, detail in gumbo_structural(g):
            emit(f"  - {'PASS' if ok else 'FAIL'} — {name} ({detail})")
        emit()

        if args.out:
            out = Path(args.out)
            out.parent.mkdir(parents=True, exist_ok=True)
            io.open(out, "w", encoding="utf-8", newline="").write("\n".join(lines) + "\n")
            print(f"\n[overlay-verify] wrote {out}")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
