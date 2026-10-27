# -*- coding: utf-8 -*-
"""Self-tests for check_test_key_markers.py (F1, phase-51 adversarial review).

A checker no mutation can redden is decoration. The live tree is asserted GREEN
below, but that is the assertion that could pass vacuously -- so every rule is
additionally proved against a deliberately broken input, and the two
FALSE-POSITIVE directions are proved too, because a repo-wide content scan that
reds on prose gets switched off within a week.

Note this file itself contains complete PEM armour text (the synthetic keys
below), which puts it in the checker's own scope. It is deliberately labelled:
PDFE TEST-ONLY KEY -- the synthetic material here is generated at test time and
is not a secret. `test_the_live_tree_passes` is what proves that self-reference
resolves, rather than a comment claiming it does.

Mutations never touch the tracked tree: every one operates on a synthetic file
set under tmp_path. A self-test that rewrote the fixtures it guards would be the
same class of defect the checker exists to catch.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]

_spec = importlib.util.spec_from_file_location(
    "check_test_key_markers", _HERE / "check_test_key_markers.py")
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

# A real, tiny, throwaway key generated at import time. Using generated material
# rather than a pasted constant means this file adds no long-lived key bytes to
# the repository -- and it still exercises the armour the checker looks for.
_serialization = pytest.importorskip(
    "cryptography.hazmat.primitives.serialization")
_rsa = pytest.importorskip("cryptography.hazmat.primitives.asymmetric.rsa")

_KEY = _rsa.generate_private_key(public_exponent=65537, key_size=1024)
PKCS8_PEM = _KEY.private_bytes(
    encoding=_serialization.Encoding.PEM,
    format=_serialization.PrivateFormat.PKCS8,
    encryption_algorithm=_serialization.NoEncryption())
PKCS1_PEM = _KEY.private_bytes(
    encoding=_serialization.Encoding.PEM,
    format=_serialization.PrivateFormat.TraditionalOpenSSL,
    encryption_algorithm=_serialization.NoEncryption())
ENCRYPTED_PEM = _KEY.private_bytes(
    encoding=_serialization.Encoding.PEM,
    format=_serialization.PrivateFormat.PKCS8,
    encryption_algorithm=_serialization.BestAvailableEncryption(b"pw"))

MARKER = mod.MARKER_TEMPLATE.format(gen="tests/fixtures/x.py").encode("ascii")


def _tree(tmp_path: Path, files: dict[str, bytes]) -> tuple[Path, list[str]]:
    for rel, data in files.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return tmp_path, sorted(files)


# ------------------------------------------------------------------ live state

def test_the_live_tree_passes():
    problems = mod.check([])
    assert problems == [], "live tree fails:\n  " + "\n  ".join(problems)


def test_the_scan_actually_finds_keys():
    """The rule is vacuous over an empty scope. If the repo-wide scan ever
    returns nothing, this gate silently stops being a gate -- which is exactly
    the failure mode F1 was about, one level up."""
    found = mod.scan()
    assert len(found) >= 8, found


def test_the_three_keys_f1_named_are_in_scope_and_marked():
    """F1's whole point: these sit OUTSIDE tests/fixtures/crl_chain/, so the
    directory-keyed check_crl_chain_fixture.py never saw them."""
    found = set(mod.scan())
    for rel in ("tests/fixtures/test_tsa_key.pem",
                "tests/fixtures/test_signer_ca_key.pem",
                "tests/fixtures/test_ca_key.pem"):
        assert rel in found, f"{rel} is not in the checker's scope"
        raw = (_REPO / rel).read_bytes().replace(b"\r\n", b"\n")
        assert raw.startswith(b"# " + mod.MARKER_TOKEN), rel


def test_scope_is_not_the_crl_chain_directory():
    """A regression guard aimed at the defect itself, not at its symptom: if
    somebody re-scopes this checker to a directory, the sibling keys leave scope
    and this reds."""
    found = mod.scan()
    assert any(not f.startswith("tests/fixtures/crl_chain/") for f in found)


def test_the_marked_keys_still_parse():
    """A marker that broke PEM parsing would be worse than no marker. Proved by
    LOADING the committed bytes, not by citing RFC 7468 §5.2 -- and in the CRLF
    shape a clean checkout produces, since none of the three is `-text`."""
    for rel in ("tests/fixtures/test_tsa_key.pem",
                "tests/fixtures/test_signer_ca_key.pem",
                "tests/fixtures/test_ca_key.pem"):
        raw = (_REPO / rel).read_bytes()
        assert _serialization.load_pem_private_key(raw, password=None)
        crlf = raw.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        assert _serialization.load_pem_private_key(crlf, password=None)


def test_the_generators_emit_the_marker_they_are_credited_with():
    """Both halves must agree, for the same reason the F56 pair does: the
    generator is the thing being edited when a marker gets dropped, so a control
    living only in the gate reports a mystery and a control living only in the
    generator can be removed by the same edit."""
    for gen in ("tests/fixtures/generate_ca_signed_pdf.py",
                "tests/fixtures/generate_timestamped_pdf.py",
                "tests/fixtures/gen_two_level_ca_signer.py"):
        src = (_REPO / gen).read_bytes()
        assert mod.MARKER_TOKEN in src, f"{gen} does not emit the marker"


# ------------------------------------------------------- detection (true side)

@pytest.mark.parametrize("pem,label", [
    (PKCS8_PEM, "PKCS#8 PRIVATE KEY"),
    (PKCS1_PEM, "PKCS#1 RSA PRIVATE KEY"),
    (ENCRYPTED_PEM, "ENCRYPTED PRIVATE KEY"),
])
def test_every_armour_variant_is_recognised(pem, label):
    assert mod.contains_private_key(pem), label


def test_a_key_is_recognised_in_a_crlf_file():
    """core.autocrlf=true. None of the three keys F1 named is `-text`, so a
    clean checkout DOES deliver them CRLF. A detector that only understood LF
    would silently stop covering them one checkout later."""
    assert mod.contains_private_key(PKCS8_PEM.replace(b"\n", b"\r\n"))


def test_a_key_embedded_in_source_is_recognised():
    """Scope is decided by CONTENT, not suffix or directory: a key pasted into a
    .py or a .cpp string literal is the same disclosure risk as a .pem."""
    src = b'static const char* k =\n"' + PKCS8_PEM + b'";\n'
    assert mod.contains_private_key(src)


# ------------------------------------------------ non-detection (false side)

def test_prose_quoting_the_armour_line_is_not_a_key():
    """docs/.../deferred-backlog.md quotes `-----BEGIN PRIVATE KEY-----` while
    DESCRIBING this very finding. A checker that red on that would be turned off
    within a week, and then it protects nothing."""
    prose = (b"| **F56: the committed test private keys carry no marker** | "
             b"whose first line is a bare `-----BEGIN PRIVATE KEY-----`. "
             b"Nothing in the file says it is test-only. |\n")
    assert not mod.contains_private_key(prose)


def test_a_begin_armour_with_no_matching_end_is_not_a_key():
    truncated = PKCS8_PEM[:PKCS8_PEM.index(b"-----END")]
    assert not mod.contains_private_key(truncated)


def test_a_mismatched_end_armour_is_not_a_key():
    """The backreference is load-bearing: an ENCRYPTED armour closed by a bare
    `END PRIVATE KEY` is malformed, not a key. Written as a regression guard
    because the first cut used an OPTIONAL backreference, which accepted it."""
    mismatched = ENCRYPTED_PEM.replace(b"-----END ENCRYPTED PRIVATE KEY-----",
                                       b"-----END PRIVATE KEY-----")
    assert not mod.contains_private_key(mismatched)


def test_a_public_key_is_not_a_private_key():
    pub = _KEY.public_key().public_bytes(
        encoding=_serialization.Encoding.PEM,
        format=_serialization.PublicFormat.SubjectPublicKeyInfo)
    assert not mod.contains_private_key(pub)


def test_a_certificate_is_not_a_private_key():
    cert_like = (b"-----BEGIN CERTIFICATE-----\nMIIB\n"
                 b"-----END CERTIFICATE-----\n")
    assert not mod.contains_private_key(cert_like)


# ------------------------------------------------------------------ mutations

def test_an_unmarked_key_is_flagged(tmp_path):
    root, files = _tree(tmp_path, {"tests/fixtures/k.pem": PKCS8_PEM})
    problems = mod.check([], files=files, root=root)
    assert len(problems) == 1, problems
    assert "tests/fixtures/k.pem" in problems[0]
    assert "unlabelled PEM private key" in problems[0]


def test_a_marked_key_passes(tmp_path):
    """The positive control. Without it every mutation here could be passing
    because the synthetic tree is broken in some unrelated way."""
    root, files = _tree(tmp_path, {"tests/fixtures/k.pem": MARKER + PKCS8_PEM})
    assert mod.check([], files=files, root=root) == []


def test_a_marker_after_the_armour_is_flagged_separately(tmp_path):
    """Present-but-in-the-wrong-place is the failure a substring search misses,
    and it is the one that actually corrupts the key."""
    nl = PKCS8_PEM.index(b"\n") + 1
    broken = PKCS8_PEM[:nl] + MARKER + PKCS8_PEM[nl:]
    root, files = _tree(tmp_path, {"tests/fixtures/k.pem": broken})
    problems = mod.check([], files=files, root=root)
    assert len(problems) == 1, problems
    assert "AFTER the" in problems[0], problems


def test_a_marker_after_the_armour_really_does_break_parsing():
    """Proves the rule above guards something real rather than a style
    preference: the same bytes fail to load."""
    nl = PKCS8_PEM.index(b"\n") + 1
    broken = PKCS8_PEM[:nl] + MARKER + PKCS8_PEM[nl:]
    with pytest.raises(Exception):
        _serialization.load_pem_private_key(broken, password=None)


def test_the_marker_check_tolerates_crlf(tmp_path):
    """core.autocrlf=true. A marked key that arrives CRLF is still marked, and
    reporting it as unlabelled would be a checkout artefact dressed as a
    breach. Without this the gate passes in the session that wrote the marker
    and reds on the next clean checkout."""
    crlf = (MARKER + PKCS8_PEM).replace(b"\n", b"\r\n")
    root, files = _tree(tmp_path, {"tests/fixtures/k.pem": crlf})
    assert mod.check([], files=files, root=root) == []


def test_an_unmarked_key_in_any_directory_is_flagged(tmp_path):
    """The F1 fix itself: the rule follows the KEY, not a directory. Three
    unrelated locations, none of them tests/fixtures/crl_chain/."""
    files = {
        "src/deep/nested/leaked.pem": PKCS8_PEM,
        "tools/whatever.txt": PKCS1_PEM,
        "tests/other/embedded.py": b"K = '''" + PKCS8_PEM + b"'''\n",
    }
    root, names = _tree(tmp_path, files)
    problems = mod.check([], files=names, root=root)
    assert len(problems) == 3, problems
    for rel in files:
        assert any(rel in p for p in problems), rel


def test_an_untracked_key_on_disk_is_out_of_scope(tmp_path):
    """`git ls-files`, not a filesystem walk. A developer's scratch key is not
    what a clone or a support bundle carries, and reddening on it would make the
    gate a nuisance that gets disabled."""
    (tmp_path / "scratch.pem").write_bytes(PKCS8_PEM)
    assert mod.check([], files=[], root=tmp_path) == []


def test_the_file_set_comes_from_git():
    tracked = mod.tracked_files()
    assert "tools/gate/check_test_key_markers.py" in tracked
    out = subprocess.run(["git", "ls-files"], cwd=str(_REPO),
                         capture_output=True, text=True, encoding="utf-8")
    assert len(tracked) == len([l for l in out.stdout.splitlines() if l.strip()])


# ------------------------------------------------------------------- driver

def test_main_exits_zero_on_the_live_tree():
    assert mod.main([]) == 0


def test_main_list_exits_zero():
    assert mod.main(["--list"]) == 0


def test_the_checker_is_runnable_as_a_script():
    """The way the workflow step invokes it."""
    proc = subprocess.run([sys.executable,
                           str(_HERE / "check_test_key_markers.py")],
                          cwd=str(_REPO), capture_output=True, text=True,
                          encoding="utf-8")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "[test-key-markers] OK" in proc.stdout
