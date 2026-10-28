# -*- coding: utf-8 -*-
"""Pins for publish_snapshot.py -- the scan gate that makes publication safety
structural. The one promise: a scrubbed identifier in ANY surviving file
aborts the publish, before anything is pushed.
"""
from __future__ import annotations

import hashlib
import importlib.util
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


pub = _load(os.path.join(_ROOT, "tools", "ops", "publish_snapshot.py"), "pub")
csr = _load(os.path.join(_ROOT, "tools", "gate", "check_scrub_regressions.py"),
            "csr")

SYN = [{"label": "syn", "sha256": hashlib.sha256(b"zzsynth host").hexdigest(),
        "n": len("zzsynth host")}]


def test_denylist_excludes_the_migration_audit_trail():
    assert pub.is_denied("docs/audit/p101-route-b-migration/phase-4.md",
                         pub.DENYLIST)
    assert pub.is_denied("scripts/migration/migrate_tracker.py", pub.DENYLIST)
    assert pub.is_denied(".claude/pipelines/somepipe/gate-artifacts/x.json",
                         pub.DENYLIST)


def test_denylist_excludes_the_internal_operating_model_doc():
    # It names internal repo IDs and the dev repo's latent orphans, carries no
    # scrubbed needle (the scan cannot catch it), and must stay internal.
    assert pub.is_denied("docs/legal/publication-model-2026-10.md",
                         pub.DENYLIST)


def test_denylist_excludes_internal_ci():
    # The public repo is a snapshot mirror; internal CI (self-hosted runner,
    # gate scripts) and dependabot (PR noise on the end-user tracker) must not
    # publish.
    assert pub.is_denied(".github/workflows/fast-checks.yml", pub.DENYLIST)
    assert pub.is_denied(".github/actions/ensure-build-tree/action.yml",
                         pub.DENYLIST)
    assert pub.is_denied(".github/dependabot.yml", pub.DENYLIST)


def test_denylist_keeps_public_facing_github_and_security():
    # Issue templates and the security policy are the public-facing bits; they
    # MUST survive into the snapshot.
    assert not pub.is_denied(".github/ISSUE_TEMPLATE/bug_report.yml",
                             pub.DENYLIST)
    assert not pub.is_denied(".github/ISSUE_TEMPLATE/config.yml", pub.DENYLIST)
    assert not pub.is_denied("SECURITY.md", pub.DENYLIST)


def test_denylist_keeps_product_source_and_license():
    assert not pub.is_denied("src/core/Document.cpp", pub.DENYLIST)
    assert not pub.is_denied("LICENSE", pub.DENYLIST)
    assert not pub.is_denied("CORRESPONDING-SOURCE.txt", pub.DENYLIST)
    assert not pub.is_denied("CMakeLists.txt", pub.DENYLIST)


def test_a_needle_in_any_decoding_is_detected():
    assert pub.scan_blob("f", b"path zzsynth-host end", SYN, csr.scan_text) \
        == {"syn"}
    assert "syn" in pub.scan_blob("f", "zzsynth-host".encode("utf-16-le"),
                                  SYN, csr.scan_text)


def test_clean_bytes_do_not_hit():
    assert not pub.scan_blob("f", b"\x00\x01 nothing of note", SYN,
                             csr.scan_text)


def test_any_hit_aborts_the_publish():
    assert pub.publish_decision(50, [("f", ["syn"])], 10)[0] == "abort"


def test_an_empty_scan_aborts_never_publishes_nothing():
    assert pub.publish_decision(0, [], 10)[0] == "abort"


def test_clean_nonempty_publishes():
    assert pub.publish_decision(50, [], 10)[0] == "publish"


def test_public_variant_sources_are_denied():
    # A *.public.md must never appear under its own name in the public repo.
    assert pub.is_denied("README.public.md", pub.DENYLIST)


def test_overlay_swaps_public_readme_in_and_removes_the_variant(tmp_path):
    staging = str(tmp_path)
    with open(os.path.join(staging, "README.md"), "w", encoding="utf-8") as fh:
        fh.write("DEV readme: no installer here, build from source.\n")
    with open(os.path.join(staging, "README.public.md"), "w",
              encoding="utf-8") as fh:
        fh.write("PUBLIC readme: download the signed installer.\n")

    applied = pub.apply_overlay(staging, {"README.public.md": "README.md"})

    assert applied == [("README.public.md", "README.md")]
    # the variant source is gone...
    assert not os.path.exists(os.path.join(staging, "README.public.md"))
    # ...and README.md now carries the PUBLIC content.
    with open(os.path.join(staging, "README.md"), encoding="utf-8") as fh:
        assert "PUBLIC readme" in fh.read()


def test_overlay_is_a_noop_when_variant_absent(tmp_path):
    staging = str(tmp_path)
    with open(os.path.join(staging, "README.md"), "w", encoding="utf-8") as fh:
        fh.write("DEV readme.\n")
    applied = pub.apply_overlay(staging, {"README.public.md": "README.md"})
    assert applied == []
    with open(os.path.join(staging, "README.md"), encoding="utf-8") as fh:
        assert "DEV readme" in fh.read()


def test_the_real_needle_inventory_is_nonempty():
    # An empty inventory would make the gate a silent no-op.
    needles, err = csr.load_all_needles()
    assert err is None and len(needles) > 0


# --- the binary half: --attach-release only uploads a scanned-clean asset ---

def test_attach_gate_aborts_on_a_hit():
    assert pub.release_decision(3, [("installer", "syn")], [])[0] == "abort"


def test_attach_gate_aborts_on_an_unscanned_payload():
    # An archive whose payload could not be extracted is UNSCANNED, not clean.
    assert pub.release_decision(3, [], [("installer", "7z exit 2")])[0] \
        == "abort"


def test_attach_gate_aborts_on_an_empty_scan():
    assert pub.release_decision(0, [], [])[0] == "abort"


def test_attach_gate_attaches_only_when_clean():
    assert pub.release_decision(3, [], [])[0] == "attach"


def test_generated_release_body_names_the_corresponding_source():
    # A binary-only recipient must be able to act on the AGPL offer: the body
    # must carry the phrase AND a URL to the public repo.
    relhyg = _load(os.path.join(_ROOT, "tools", "gate",
                                "check_release_asset_hygiene.py"), "relhyg")
    body = pub._default_release_body("ipswitch8/PDFE-public", "v0.3.0")
    assert relhyg.source_statement_ok(body, "ipswitch8/PDFE-public")
