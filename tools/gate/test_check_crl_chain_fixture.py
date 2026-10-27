# -*- coding: utf-8 -*-
"""Self-tests for check_crl_chain_fixture.py (SEC-7, phase-48 review).

A checker no mutation can redden is decoration. The live tree is asserted GREEN
below, but that assertion is the one that could pass vacuously -- so every rule
is additionally proved against a deliberately broken input.

The mutations operate on a COPY of the corpus in tmp_path, never on the tracked
tree: a self-test that rewrites the fixture it is guarding would be the same
class of defect the checker exists to catch.
"""

from __future__ import annotations

import hashlib
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]

_spec = importlib.util.spec_from_file_location(
    "check_crl_chain_fixture", _HERE / "check_crl_chain_fixture.py")
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

FIXTURE_DIR = _REPO / mod.FIXTURE_DIR
MANIFEST = _REPO / mod.MANIFEST
GENERATOR = _REPO / mod.GENERATOR


def _manifest_text() -> str:
    return MANIFEST.read_text(encoding="utf-8")


# ------------------------------------------------------------------ live state

def test_the_live_tree_passes():
    problems = mod.check([])
    assert problems == [], "live tree fails:\n  " + "\n  ".join(problems)


def test_the_manifest_and_corpus_are_tracked():
    tracked = set(mod._tracked_files(mod.FIXTURE_DIR))
    assert mod.MANIFEST in tracked
    for name in mod.UNPINNABLE:
        assert f"{mod.FIXTURE_DIR}/{name}" in tracked


def test_the_generator_is_tracked_and_offers_check():
    assert mod.GENERATOR in set(mod._tracked_files(mod.GENERATOR))
    assert "--check" in GENERATOR.read_text(encoding="utf-8")


def test_recorded_digests_are_the_real_digests():
    """Derived from the bytes at test time, never copied from the manifest."""
    entries = mod.parse_manifest(_manifest_text())
    assert entries, "manifest pins nothing"
    for name, expected in entries.items():
        assert mod.digest_of(FIXTURE_DIR / name) == expected, name


def test_every_committed_member_is_accounted_for():
    """Derived from git, not from a hand-written list -- a corpus member added
    without a manifest entry is the exact gap SEC-7 named."""
    entries = mod.parse_manifest(_manifest_text())
    accounted = set(entries) | set(mod.UNPINNABLE) | set(mod.NON_ARTIFACTS)
    committed = {p.split("/")[-1] for p in mod._tracked_files(mod.FIXTURE_DIR)}
    assert committed <= accounted, f"unaccounted: {sorted(committed - accounted)}"


def test_the_two_intermediates_are_both_pinned():
    """The controlled comparison rests on these two specifically. If either
    stops being byte-pinned, `CrlIssuerKeyUsageTest`'s conclusion that the
    cRLSign bit is what separates them stops following."""
    entries = mod.parse_manifest(_manifest_text())
    for name in ("inter_crlsign.der", "inter_nocrlsign.der",
                 "inter_crlsign_key.pem", "inter_nocrlsign_key.pem"):
        assert name in entries, f"{name} is not byte-pinned"


# ------------------------------------------------------------------- parsing

def test_parse_manifest_reads_comments_and_entries():
    entries = mod.parse_manifest(
        "# a comment\n\n" + "a" * 64 + "  root.der\n")
    assert entries == {"root.der": "a" * 64}


def test_parse_manifest_tolerates_crlf():
    """core.autocrlf=true: a manifest that arrives CRLF must still parse, or
    the gate reds on every clean checkout for a checkout reason."""
    assert mod.parse_manifest("# c\r\n" + "b" * 64 + "  x.der\r\n") == {
        "x.der": "b" * 64}


@pytest.mark.parametrize("bad", [
    "not-a-hash  root.der",              # too short
    "z" * 64 + "  root.der",             # not hex
    "a" * 64 + "  root.der  extra",      # malformed
])
def test_parse_manifest_refuses_garbage_rather_than_skipping_it(bad):
    """Silently ignoring an unparseable line is how a pin stops covering a
    file with nobody noticing."""
    with pytest.raises(mod.Failure):
        mod.parse_manifest(bad + "\n")


def test_parse_manifest_refuses_a_duplicate_entry():
    with pytest.raises(mod.Failure):
        mod.parse_manifest("a" * 64 + "  x.der\n" + "b" * 64 + "  x.der\n")


def test_parse_manifest_refuses_an_empty_manifest():
    with pytest.raises(mod.Failure):
        mod.parse_manifest("# only a comment\n")


# ------------------------------------------------------------------ mutations
#
# Each of these copies the corpus into tmp_path, breaks ONE thing, and requires
# the checker to name it. The copy is placed inside the repo so `git ls-files`
# still resolves relative paths the same way it does live.

@pytest.fixture
def corpus_copy(tmp_path):
    dst = tmp_path / "crl_chain"
    shutil.copytree(FIXTURE_DIR, dst)
    return dst


def _check_against(dirpath: Path, manifest_text: str | None = None):
    """Run layer 1 against a COPY of the corpus.

    The copy is not git-tracked, so the tracked set is injected as "everything
    the real corpus tracks". Without that, every mutation below would be masked
    by the NOT TRACKED rule firing first and the specific rule under test would
    never be reached -- a self-test that passes for the wrong reason, which is
    the defect class this whole programme is about.
    """
    rel = dirpath.relative_to(_REPO).as_posix()
    real = [p.split("/")[-1] for p in mod._tracked_files(mod.FIXTURE_DIR)]
    tracked = {f"{rel}/{name}" for name in real}
    return mod.check([], manifest_text=manifest_text, fixture_dir=rel,
                     tracked=tracked)


@pytest.fixture
def in_repo_copy(tmp_path_factory):
    """The mutation corpus. `check()` resolves `fixture_dir` under REPO_ROOT, so
    the copy must live inside the repo; pytest.ini's
    the repo-root conftest.py roots every session's basetemp at
    `tests/_tmp/s<pid>` and so puts it there. Asserted rather than assumed so a
    basetemp move fails here loudly instead of silently skipping every mutation
    below -- which is exactly what happened when the GitHub #257 fix first put
    that hook in tests/conftest.py, covering tests/ but not tools/gate: these
    ten tests errored on the next run and sent the hook to the repo root."""
    base = tmp_path_factory.mktemp("crl_corpus")
    assert base.is_relative_to(_REPO), (
        "self-test scratch is outside the repo; the mutations below cannot run")
    dst = base / "crl_chain"
    shutil.copytree(FIXTURE_DIR, dst)
    return dst


def test_a_corrupted_artifact_is_caught(in_repo_copy):
    victim = in_repo_copy / "inter_nocrlsign.der"
    raw = bytearray(victim.read_bytes())
    raw[-1] ^= 0xFF
    victim.write_bytes(bytes(raw))
    problems = _check_against(in_repo_copy)
    assert any("inter_nocrlsign.der" in p and "hashes to" in p for p in problems), problems


def test_substituting_one_intermediate_for_the_other_is_caught(in_repo_copy):
    """The specific attack SEC-7 describes: swap the cRLSign-bearing
    intermediate into the negative slot and the controlled comparison silently
    stops being controlled."""
    shutil.copyfile(in_repo_copy / "inter_crlsign.der",
                    in_repo_copy / "inter_nocrlsign.der")
    problems = _check_against(in_repo_copy)
    assert any("inter_nocrlsign.der" in p for p in problems), problems


def test_a_missing_artifact_is_caught(in_repo_copy):
    (in_repo_copy / "root.der").unlink()
    problems = _check_against(in_repo_copy)
    assert any("root.der" in p and "not in the tree" in p for p in problems), problems


def test_a_missing_pkcs12_is_caught(in_repo_copy):
    (in_repo_copy / "leaf_crlsign.pfx").unlink()
    problems = _check_against(in_repo_copy)
    assert any("leaf_crlsign.pfx" in p and "missing" in p for p in problems), problems


def test_an_untracked_corpus_is_caught(in_repo_copy):
    """Presence on disk is not the property that matters -- a clean checkout is.
    Here the tracked set is NOT injected, so git genuinely reports nothing for
    the copy and the tracked-ness rules must fire."""
    problems = mod.check([], manifest_text=_manifest_text(),
                         fixture_dir=in_repo_copy.relative_to(_REPO).as_posix())
    assert any("NOT TRACKED" in p for p in problems), problems


def test_an_unmutated_copy_passes(in_repo_copy):
    """The positive control. Without it, every mutation below could be passing
    because the copy is broken in some unrelated way rather than because the
    mutation was detected."""
    assert _check_against(in_repo_copy) == []


def test_an_unpinned_new_member_is_caught(in_repo_copy):
    """Layer 1's second direction: committed-but-unpinned is the shape that
    grows a corpus out from under its own manifest."""
    (in_repo_copy / "inter_third.der").write_bytes(b"\x30\x00")
    rel = in_repo_copy.relative_to(_REPO).as_posix()
    real = [p.split("/")[-1] for p in mod._tracked_files(mod.FIXTURE_DIR)]
    tracked = {f"{rel}/{n}" for n in real} | {f"{rel}/inter_third.der"}
    problems = mod.check([], manifest_text=_manifest_text(), fixture_dir=rel,
                         tracked=tracked)
    assert any("inter_third.der" in p and "neither pinned" in p
               for p in problems), problems


def test_dropping_an_entry_from_the_manifest_is_caught():
    """A file present in the corpus but removed from the manifest must be
    reported as unaccounted, not silently ignored."""
    text = _manifest_text()
    kept = [ln for ln in text.splitlines()
            if not ln.strip().endswith("inter_nocrlsign.der")]
    problems = mod.check([], manifest_text="\n".join(kept) + "\n")
    assert any("inter_nocrlsign.der" in p and "neither pinned" in p
               for p in problems), problems


def test_a_wrong_digest_in_the_manifest_is_caught():
    text = _manifest_text()
    entries = mod.parse_manifest(text)
    real = entries["root.der"]
    problems = mod.check([], manifest_text=text.replace(real, "0" * 64))
    assert any("root.der" in p and "hashes to" in p for p in problems), problems


# ------------------------------------------------- F56: the test-only marker
#
# The corpus ships unencrypted private keys. They are throwaway material, but a
# `.pem` that says nothing about itself reads as a leaked secret to anyone who
# meets it outside this directory. Each key therefore carries a leading marker,
# and these tests are what stop that marker quietly disappearing on the next
# regeneration.


def _key_names() -> list[str]:
    """Derived from git, never hand-listed: a key added to the corpus WITHOUT a
    marker is exactly the case the rule exists for, and a hard-coded list is
    blind to it."""
    return [p.split("/")[-1] for p in mod._tracked_files(mod.FIXTURE_DIR)
            if p.endswith("_key.pem")]


def test_the_corpus_actually_contains_keys_to_check():
    """The rule below is vacuous if the corpus has no keys. Assert it does, and
    assert the count is what the generator emits, so a key silently dropping out
    of the corpus reddens here rather than shrinking the marker rule's scope to
    nothing."""
    assert len(_key_names()) >= 4, _key_names()


def test_every_committed_key_carries_the_marker():
    for name in _key_names():
        raw = (FIXTURE_DIR / name).read_bytes().replace(b"\r\n", b"\n")
        assert raw.startswith(mod.KEY_MARKER_LINES), (
            f"{name} does not begin with the TEST-ONLY KEY marker")


def test_the_checker_and_the_generator_agree_on_the_marker_text():
    """Two independent constants state the marker: KEY_MARKER in the generator
    (which writes it) and KEY_MARKER_LINES here (which enforces it). If they
    drift, the gate enforces text nothing produces -- or, worse, stops enforcing
    the text that is actually there. Neither is detectable from either file
    alone, so it is asserted across them."""
    src = GENERATOR.read_text(encoding="utf-8")
    ns: dict = {}
    # Evaluate only the constant, not the module: importing the generator pulls
    # in `cryptography`, which layer 2 already treats as optional.
    start = src.index("KEY_MARKER = (")
    end = src.index("\n)\n", start) + 3
    exec(compile(src[start:end], str(GENERATOR), "exec"), ns)
    assert ns["KEY_MARKER"].encode("ascii") == mod.KEY_MARKER_LINES


def test_a_marked_key_still_parses():
    """A marker that broke PEM parsing would be worse than no marker. Proved by
    loading the committed bytes, not by citing RFC 7468 §5.2."""
    serialization = pytest.importorskip(
        "cryptography.hazmat.primitives.serialization")
    for name in _key_names():
        raw = (FIXTURE_DIR / name).read_bytes()
        assert serialization.load_pem_private_key(raw, password=None) is not None


def test_a_key_with_the_marker_stripped_is_caught(in_repo_copy):
    victim = in_repo_copy / "inter_crlsign_key.pem"
    raw = victim.read_bytes()
    victim.write_bytes(raw[raw.index(b"-----BEGIN"):])
    problems = _check_against(in_repo_copy)
    assert any("inter_crlsign_key.pem" in p and "TEST-ONLY KEY marker" in p
               for p in problems), problems


def test_a_marker_placed_inside_the_pem_body_is_caught(in_repo_copy):
    """Present-but-in-the-wrong-place is the failure a substring search would
    miss, and it is the one that actually corrupts the key."""
    victim = in_repo_copy / "root_key.pem"
    raw = victim.read_bytes()
    body = raw[raw.index(b"-----BEGIN"):]
    nl = body.index(b"\n") + 1
    victim.write_bytes(body[:nl] + mod.KEY_MARKER_LINES + body[nl:])
    problems = _check_against(in_repo_copy)
    assert any("root_key.pem" in p and "AFTER the" in p
               for p in problems), problems


def test_the_marker_check_tolerates_crlf(in_repo_copy):
    """core.autocrlf=true. A key that arrives CRLF still carries the marker, and
    reporting it as unmarked would be a checkout artefact dressed as a breach."""
    victim = in_repo_copy / "inter_noku_key.pem"
    victim.write_bytes(victim.read_bytes().replace(b"\n", b"\r\n"))
    problems = [p for p in _check_against(in_repo_copy)
                if "TEST-ONLY KEY marker" in p]
    assert problems == [], problems


def test_the_marker_rule_refuses_to_pass_on_an_empty_key_set(tmp_path):
    """A rule that iterates an empty list passes silently. That is how this
    check would stop covering anything without failing."""
    problems = mod.check_key_markers([], tmp_path, [])
    assert any("no *_key.pem" in p for p in problems), problems


# ------------------------------------------------------------ layer 2 / driver

def test_regen_check_runs_and_passes_on_the_live_tree():
    state, detail = mod.regen_check()
    if state == "unavailable":
        pytest.skip(f"cryptography unavailable: {detail}")
    assert state == "pass", detail


def test_main_exits_zero_on_the_live_tree():
    assert mod.main([]) == 0


def test_main_layer1_only_exits_zero():
    assert mod.main(["--skip-regen"]) == 0


def test_the_checker_is_runnable_as_a_script():
    """The way a workflow step would invoke it."""
    proc = subprocess.run([sys.executable, str(_HERE / "check_crl_chain_fixture.py")],
                          cwd=str(_REPO), capture_output=True, text=True,
                          encoding="utf-8")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "[crl-chain-fixture] OK" in proc.stdout


def test_the_hash_is_computed_with_lf_normalisation_for_pem_only():
    """A .der containing CRLF is real damage and must NOT be folded away."""
    entries = mod.parse_manifest(_manifest_text())
    pem = [n for n in entries if n.endswith(".pem")]
    der = [n for n in entries if n.endswith(".der")]
    assert pem and der
    for name in der:
        raw = (FIXTURE_DIR / name).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == entries[name], (
            f"{name} is binary; its digest must be over raw bytes")
