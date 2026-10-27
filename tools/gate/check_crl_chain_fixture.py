#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Standing control: the two-level CA corpus stays the bytes it claims to be.

WHAT THIS CLOSES (SEC-7, phase-48 security review)
==================================================
Phase-48 committed `tests/fixtures/crl_chain/` — a root CA, two intermediates
differing ONLY in the `cRLSign` keyUsage bit, their leaves, their private keys,
and `MANIFEST.sha256` pinning the SHA-256 of every byte-deterministic member.
`python tests/fixtures/gen_two_level_ca_signer.py --check` verifies the
committed artifacts against a fresh generation.

**Nothing ran it.** No workflow step, no gate, no test referenced the script, so
the byte pin was enforced by nobody. A substituted, corrupted or silently
regenerated fixture would have been caught only by whatever the runtime gtest
assertions happened to notice — and the thing that most needs catching is
exactly the thing they would NOT notice.

WHY THAT MATTERS MORE HERE THAN FOR AN ORDINARY FIXTURE
======================================================
This corpus is a CONTROLLED COMPARISON. `CrlIssuerKeyUsageTest` asserts that a
CRL from `inter_crlsign` is honoured and a CRL from `inter_nocrlsign` is not,
and the conclusion "the cRLSign rule is what separates them" is sound only while
the two intermediates are identical in every other respect. Regenerate
`inter_nocrlsign` with `cRLSign` set and the negative case stops testing
anything while still passing something. `IssuerKeyUsageBitsAreAsIntended` pins
the keyUsage bits at runtime and is the first line of defence, but it reads only
the bits it thinks to read: validity window, key, serial, subject, the private
keys the tests mint CRLs with — none of those are pinned by any assertion. The
manifest pins all of them, and this checker is what makes the manifest load
bearing.

TWO LAYERS, DELIBERATELY
========================
1. **The byte pin** (always enforced, zero third-party dependencies): every
   name in `MANIFEST.sha256` exists, is tracked, and hashes to the recorded
   digest; and — the direction that is easy to forget — every committed member
   of the directory is accounted for, so a NEW unpinned file cannot be added
   into the corpus silently. Since F56 (phase-51) it additionally requires the
   in-file TEST-ONLY marker on every `*_key.pem` -- see `KEY_MARKER_LINES`.
2. **Full regeneration** (`gen_two_level_ca_signer.py --check`): proves the
   committed bytes are reproducible from the generator, which layer 1 cannot,
   and additionally covers the two `.pfx` files whose container bytes are
   non-deterministic by RFC 7292 (random salt/IV) and so are absent from the
   manifest by design. It needs the `cryptography` package. When that import is
   unavailable the layer reports UNAVAILABLE rather than passing quietly; pass
   `--require-regen` to make that a failure.

Layer 1 alone satisfies SEC-7. Layer 2 is what catches "someone edited the
generator and the fixture together, consistently, into a corpus that is no
longer a controlled comparison".

CRLF NOTE
=========
`core.autocrlf=true`. `.gitattributes` marks this directory's `.pem` and
`MANIFEST.sha256` members `-text` so they travel unnormalised, but a
`.gitattributes` rule can be lost, and a checker that fails on a lost rule
reports "the fixture is corrupt" for what is actually a checkout artefact. So
`.pem` and the manifest are compared with CRLF folded to LF — matching
`do_check()` in the generator. `.der` and `.pfx` are binary and are NEVER
normalised: a CRLF inside them is real damage and must red.

Exit: 0 all checks pass; 1 a check failed; 2 usage/IO error.
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

FIXTURE_DIR = "tests/fixtures/crl_chain"
MANIFEST = f"{FIXTURE_DIR}/MANIFEST.sha256"
GENERATOR = "tests/fixtures/gen_two_level_ca_signer.py"

# Members the manifest deliberately does NOT pin: PKCS#12 mandates a random
# salt and IV (RFC 7292 §4), so the container bytes differ on every generation.
# They are still required to be present and tracked, and layer 2 compares their
# UNWRAPPED contents.
UNPINNABLE = ("leaf_crlsign.pfx", "leaf_nocrlsign.pfx", "leaf_crldp.pfx",
              "leaf_noku.pfx")

# ---------------------------------------------------------------------------
# F56 (phase-51): every generated private key says in-file that it is test-only.
#
# The corpus ships unencrypted PKCS#8 private keys. That is deliberate and
# harmless -- they are throwaway material, already public in an AGPL repository,
# and embedding them is what makes the certificate bytes reproducible. But a
# `.pem` whose first line is `-----BEGIN PRIVATE KEY-----` and which says
# nothing else is INDISTINGUISHABLE from a real leaked key to anyone who meets
# it away from this directory: a secret scanner, a support bundle, a triage
# engineer. The cost of being wrong is asymmetric -- treating a real key as test
# material is a breach; treating test material as real is an afternoon.
#
# The generator therefore prefixes each key with the two lines below, ahead of
# the `-----BEGIN` boundary where RFC 7468 §5.2 requires readers to skip them.
# This gate is what makes the marker survive: `--check` inside the generator
# also asserts it, but the generator is the thing being changed when a marker
# gets dropped, so a control that lives only there is a control the same edit
# can remove. Both must agree.
#
# CRLF. `core.autocrlf=true` and `.gitattributes` marks these `-text`, but a
# rule can be lost, so the comparison folds CRLF -> LF exactly as `digest_of()`
# does. Without that this check passes in the session that wrote the files and
# fails on the next clean checkout -- for a checkout reason, reported as
# corruption.
KEY_MARKER_LINES = (
    b"# PDFE TEST-ONLY KEY -- generated by "
    b"tests/fixtures/gen_two_level_ca_signer.py\n"
    b"# Not a secret. Never used outside the test corpus. Safe to publish.\n"
)

# The PEM boundary the marker must precede.
_PEM_BEGIN = b"-----BEGIN"

# Files in the directory that are not corpus artifacts.
NON_ARTIFACTS = ("MANIFEST.sha256",)

# Text-ish members whose recorded digest is over canonical LF bytes.
_LF_NORMALISED_SUFFIXES = (".pem",)


class Failure(Exception):
    """Usage / IO error -- exit 2, distinct from a gate failure."""


def _tracked_files(prefix: str) -> list[str]:
    out = subprocess.run(["git", "ls-files", "--", prefix],
                         cwd=str(REPO_ROOT), capture_output=True, text=True,
                         encoding="utf-8")
    if out.returncode != 0:
        raise Failure(f"git ls-files failed for {prefix}: {out.stderr.strip()}")
    return [ln.strip() for ln in out.stdout.splitlines() if ln.strip()]


def parse_manifest(text: str) -> dict[str, str]:
    """`<sha256>  <name>` lines; `#` comments and blanks ignored.

    A malformed line is an error rather than a skip: silently ignoring an
    unparseable line is how a pin stops covering a file without anyone noticing.
    """
    entries: dict[str, str] = {}
    for lineno, raw in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2 or len(parts[0]) != 64:
            raise Failure(
                f"{MANIFEST}:{lineno}: cannot parse {raw!r} as "
                "'<sha256>  <filename>'")
        digest, name = parts[0].lower(), parts[1]
        if name in entries:
            raise Failure(f"{MANIFEST}:{lineno}: {name} listed twice")
        try:
            int(digest, 16)
        except ValueError:
            raise Failure(f"{MANIFEST}:{lineno}: {digest!r} is not hex")
        entries[name] = digest
    if not entries:
        raise Failure(f"{MANIFEST} pins no files at all")
    return entries


def digest_of(path: Path) -> str:
    raw = path.read_bytes()
    if path.suffix in _LF_NORMALISED_SUFFIXES:
        raw = raw.replace(b"\r\n", b"\n")
    return hashlib.sha256(raw).hexdigest()


def check_key_markers(problems, root: Path, key_names) -> list:
    """F56. Every `*_key.pem` in the corpus carries the test-only marker.

    `key_names` is derived from what is actually committed, never from a
    hand-written list: a key added to the corpus without a marker is precisely
    the case this rule exists for, and a hard-coded list would not see it.

    Two distinct failures are reported separately, because the fixes differ:
      * the marker is absent or altered -> the file no longer says what it is;
      * the marker sits AFTER the PEM boundary -> it is inside the base64 body
        rather than in the pre-encapsulation region, which breaks parsing.
    """
    if not key_names:
        problems.append(
            "the corpus contains no *_key.pem at all. The CRL tests mint their "
            "CRLs at run time from these keys, so this is not a corpus that can "
            "have passed -- and the F56 marker rule below would silently check "
            "nothing.")
        return problems

    for name in sorted(key_names):
        path = root / name
        if not path.is_file():
            continue  # the presence/tracking rules above already reported it
        raw = path.read_bytes().replace(b"\r\n", b"\n")
        if KEY_MARKER_LINES in raw:
            begin = raw.find(_PEM_BEGIN)
            if begin >= 0 and raw.find(KEY_MARKER_LINES) > begin:
                problems.append(
                    f"{name} carries the TEST-ONLY KEY marker but places it "
                    f"AFTER the {_PEM_BEGIN.decode()} boundary. Only text "
                    "BEFORE the boundary is skipped by PEM readers (RFC 7468 "
                    "§5.2); inside the body it corrupts the key. Move it to the "
                    "top of the file.")
            continue
        problems.append(
            f"{name} does not carry the in-file TEST-ONLY KEY marker (F56). An "
            "unlabelled private key is indistinguishable from a real one to "
            "anybody who meets it outside this directory. Regenerate with "
            f"`python {GENERATOR}`, which emits the marker; do not hand-edit "
            "the file, because the byte pin above must be regenerated with it.")
    return problems


def check(problems, manifest_text: str | None = None,
          fixture_dir: str = FIXTURE_DIR, tracked=None):
    """Layer 1. Appends one message per problem; returns the list.

    `tracked` exists so the self-tests can mutate a COPY of the corpus (which
    git necessarily does not track) and still exercise the byte-pin rules. It
    is never passed in production, where the tracked set is derived from git.
    """
    root = REPO_ROOT / fixture_dir

    if manifest_text is None:
        mpath = REPO_ROOT / MANIFEST
        if not mpath.is_file():
            raise Failure(f"{MANIFEST} does not exist")
        manifest_text = mpath.read_text(encoding="utf-8")

    entries = parse_manifest(manifest_text)
    tracked = set(_tracked_files(fixture_dir)) if tracked is None else set(tracked)

    # --- every pinned name is present, tracked, and hashes correctly ---
    for name, expected in sorted(entries.items()):
        rel = f"{fixture_dir}/{name}"
        path = root / name
        if not path.is_file():
            problems.append(
                f"{rel} is pinned by {MANIFEST} but is not in the tree. A pin "
                "for a missing file is not a control.")
            continue
        if rel not in tracked:
            problems.append(
                f"{rel} exists but is NOT TRACKED by git, so a clean checkout "
                "will not have it and every test using it will fail for a "
                "reason that names the wrong cause.")
            continue
        actual = digest_of(path)
        if actual != expected:
            problems.append(
                f"{rel} hashes to {actual}; {MANIFEST} records {expected}. The "
                "committed fixture no longer is the fixture the manifest "
                "describes. Do NOT paste the new hash in: this corpus is a "
                "CONTROLLED COMPARISON (inter_crlsign vs inter_nocrlsign differ "
                "only in the cRLSign bit), so re-derive what changed and why, "
                f"then regenerate with `python {GENERATOR}`.")

    # --- the other direction: nothing unpinned may sneak into the corpus ---
    accounted = set(entries) | set(UNPINNABLE) | set(NON_ARTIFACTS)
    for rel in sorted(tracked):
        name = rel.split("/")[-1]
        if name not in accounted:
            problems.append(
                f"{rel} is committed in the corpus but is neither pinned by "
                f"{MANIFEST} nor a known non-deterministic member "
                f"({', '.join(UNPINNABLE)}). Add it to the generator and the "
                "manifest, or remove it -- an unpinned member of a byte-pinned "
                "corpus is exactly the gap this checker exists to close.")

    # --- the PKCS#12 members, which the manifest cannot pin ---
    for name in UNPINNABLE:
        rel = f"{fixture_dir}/{name}"
        if not (root / name).is_file():
            problems.append(
                f"{rel} is missing. Its container bytes are non-deterministic "
                "so the manifest cannot pin it, but it must still be present: "
                "no test can sign a PDF without it.")
        elif rel not in tracked:
            problems.append(f"{rel} is present but NOT TRACKED by git.")

    # --- F56: the in-file test-only marker on every private key ---
    check_key_markers(
        problems, root,
        [rel.split("/")[-1] for rel in sorted(tracked)
         if rel.endswith("_key.pem")])

    # --- the generator itself ---
    gen = REPO_ROOT / GENERATOR
    if not gen.is_file():
        problems.append(
            f"{GENERATOR} is missing. Without it the corpus cannot be "
            "regenerated or verified, and the manifest becomes an unfalsifiable "
            "claim about bytes nobody can reproduce.")
    elif GENERATOR not in set(_tracked_files(GENERATOR)):
        problems.append(f"{GENERATOR} is present but NOT TRACKED by git.")
    else:
        src = gen.read_text(encoding="utf-8")
        if "--check" not in src:
            problems.append(
                f"{GENERATOR} no longer offers `--check`; layer 2 of this gate "
                "cannot run and the reproducibility claim is unverifiable.")

    return problems


def regen_check(timeout: int = 600):
    """Layer 2. Returns (state, detail) with state in pass/fail/unavailable."""
    try:
        import cryptography  # noqa: F401
    except Exception as exc:  # pragma: no cover - depends on the environment
        return "unavailable", f"the `cryptography` package is not importable: {exc}"

    gen = REPO_ROOT / GENERATOR
    if not gen.is_file():
        return "fail", f"{GENERATOR} is missing"

    proc = subprocess.run([sys.executable, str(gen), "--check"],
                          cwd=str(REPO_ROOT), capture_output=True, text=True,
                          encoding="utf-8", timeout=timeout)
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode == 0:
        return "pass", output.strip().splitlines()[-1] if output.strip() else "ok"
    return "fail", output.strip()


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):  # pragma: no cover
            pass

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--require-regen", action="store_true",
                    help="treat an unavailable regeneration check as a failure "
                         "instead of a reported note")
    ap.add_argument("--skip-regen", action="store_true",
                    help="run the byte pin only (layer 1)")
    args = ap.parse_args(argv)

    try:
        problems = check([])
    except Failure as exc:
        print(f"[crl-chain-fixture] ERROR: {exc}", file=sys.stderr)
        return 2

    pinned = len(parse_manifest((REPO_ROOT / MANIFEST).read_text(encoding="utf-8")))

    state, detail = ("skipped", "layer 2 disabled by --skip-regen")
    if not args.skip_regen:
        try:
            state, detail = regen_check()
        except subprocess.TimeoutExpired:
            state, detail = "fail", "gen_two_level_ca_signer.py --check timed out"

    if state == "fail":
        problems.append(
            f"`python {GENERATOR} --check` failed — the committed corpus is not "
            f"reproducible from its generator:\n      "
            + detail.replace("\n", "\n      "))
    elif state == "unavailable":
        if args.require_regen:
            problems.append(
                f"the regeneration check could not run and --require-regen was "
                f"given: {detail}")

    if problems:
        print(f"[crl-chain-fixture] FAIL — {len(problems)} problem(s):")
        for p in problems:
            print(f"  * {p}")
        return 1

    print(f"[crl-chain-fixture] OK — {pinned} byte-pinned artifact(s) in "
          f"{FIXTURE_DIR} match {MANIFEST}, {len(UNPINNABLE)} PKCS#12 member(s) "
          f"present, nothing unpinned in the corpus.")
    if state == "pass":
        print(f"[crl-chain-fixture] OK — regeneration check: {detail}")
    else:
        print(f"[crl-chain-fixture] NOTE — regeneration check {state}: {detail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
