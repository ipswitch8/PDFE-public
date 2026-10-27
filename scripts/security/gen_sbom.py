#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Deterministically regenerate tests/security/reports/deps/sbom.spdx.json.

GitHub #71 / meta-audit-2026-07 phase-6 follow-up (pipeline p83-outstanding).

THE GAP THIS CLOSES
--------------------
``scripts/security/run_deps.ps1`` already emits the committed SBOM, but only
as a side effect of a full dependency-CVE-audit run that also shells out to
``vcpkg.exe``, OSV-Scanner / grype, and ``cargo audit`` -- several of which
reach the network or require tools that are not installed everywhere. That
makes the SBOM-generation step itself unreachable in isolation and untestable
from the fast-checks lane (which is offline by design).

This script pulls the SBOM-generation logic out into a small, independently
invocable, deterministic, pure-file-I/O tool:

  * vcpkg side: read ``vcpkg.json`` for the DECLARED direct dependencies (via
    ``sbom_direct_deps.direct_dependencies``, the same reconciliation logic
    ``--check``/``--fill`` already use, so there is exactly one definition of
    "declared dependency" in this repo) and a vcpkg ``installed/vcpkg/status``
    database for the resolved transitive closure and versions. No network,
    no ``vcpkg.exe`` invocation.
  * cargo side: parse ``Cargo.lock`` directly (already fully pinned and
    committed, so this half was already deterministic).

WHY "STRUCTURALLY EQUIVALENT", NOT BYTE-IDENTICAL
--------------------------------------------------
``creationInfo.created`` and the timestamp embedded in ``documentNamespace``
are, by construction, wall-clock values -- two runs a second apart cannot
produce the same bytes there and still be honest about when they ran. This
script makes BOTH deterministic via one shared input, ``--created``: pass the
same value twice and every byte matches (verified by
``tools/gate/test_gen_sbom_reproducibility.py``, which does exactly that).
Comparisons against a REAL previously-committed SBOM (produced by an earlier
run, necessarily at a different wall-clock time) instead use
``--compare-to``, which ignores ``creationInfo``/``documentNamespace``
entirely and compares only what SPDX packages actually assert: the
(name, versionInfo, purl, SPDXID) multiset. That is the documented, explicit
definition of "structurally equivalent" for this tool -- not a hand-wave.

SPDXID SCHEME (GitHub #181)
----------------------------
Element ids are ``SPDXRef-<ecosystem>-<name>-<version>``, each segment reduced
to the SPDX-legal charset (letters, digits, ``.``, ``-``). The earlier scheme
derived the id from the NAME ALONE, which produced 203 distinct ids for 211
packages in the committed artifact -- SPDX 2.3 requires uniqueness within a
document. ``assert_unique_spdx_ids`` re-checks the finished package list and
raises rather than emitting a colliding document, so neither the scheme nor the
sanitiser is trusted by construction.

Consequence for ``--compare-to``: the structural signature includes SPDXID, so
comparing a freshly generated document against an SBOM produced BEFORE this
change reports every package as differing. That is intended -- the ids really
did change. Compare only against a post-#181 artifact.

WHY THE PORT-VERSION SUFFIX IS DROPPED
---------------------------------------
vcpkg's installed status database carries an optional ``Port-Version:``
field (e.g. ``qtbase 6.10.0`` at port-version 1). The currently-committed
SBOM was produced by ``run_deps.ps1``, which reads its versions from
``vcpkg list --x-json`` -- and that command's ``.version`` field does NOT
include the port-version suffix. Appending it here would make every
port-versioned package (14 of 47 in a real build tree, measured 2026-09-11)
disagree with the committed baseline on ``versionInfo`` alone. This script
therefore reads only ``Version:`` from the status stanza, matching the
existing artifact's schema, and documents the intentional information loss
here rather than silently reproducing it.

WHAT IT DOES NOT DO
--------------------
It does not run OSV-Scanner, grype, or cargo audit, and it does not touch
``cves.json``, ``summary.md``, or the CVE baseline -- that is still
``run_deps.ps1``'s job. It does not attach anything to a GitHub release;
that is explicitly out of scope for this phase (#71).

USAGE
-----
    python scripts/security/gen_sbom.py --created 2026-09-08T01:02:25Z
    python scripts/security/gen_sbom.py --installed-dir build/vs2022/vcpkg_installed \\
        --created 2026-09-08T01:02:25Z --output /tmp/sbom.json
    python scripts/security/gen_sbom.py --compare-to tests/security/reports/deps/sbom.spdx.json

Exit codes: 0 success (and, with --compare-to, structurally equivalent);
2 required input missing; 3 a declared/transitive dependency has no
resolvable version in the status database; 4 --compare-to found a structural
difference.
"""
from __future__ import annotations

import argparse
import io
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import sbom_direct_deps  # noqa: E402  (reuse the one definition of "declared dep")

DEFAULT_VCPKG_JSON = REPO_ROOT / "vcpkg.json"
DEFAULT_CARGO_LOCK = REPO_ROOT / "rust" / "tantivy_bridge" / "Cargo.lock"
DEFAULT_OUTPUT = REPO_ROOT / "tests" / "security" / "reports" / "deps" / "sbom.spdx.json"
DEFAULT_TOOL_NAME = "pdfe-gen_sbom"
DEFAULT_NAMESPACE_PREFIX = "https://github.com/itwerx/PDFE/sbom-"

_PACKAGE_HEADER_RE = re.compile(r"^Package:\s*(.+)$", re.M)
_VERSION_RE = re.compile(r"^Version:\s*(.+)$", re.M)
_DEPENDS_RE = re.compile(r"^Depends:\s*(.+)$", re.M)


class SbomGenerationError(RuntimeError):
    """Raised when the inputs cannot produce a complete, honest SBOM."""


def read_json(path: Path):
    with io.open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False, sort_keys=False)
        handle.write("\n")


def parse_vcpkg_status(status_path: Path):
    """Return (version_by_name, deps_by_name) parsed from a vcpkg status db.

    A port with feature stanzas repeats ``Package: <name>`` once per feature,
    each carrying its own (optional) ``Depends:`` line -- those dependencies
    are real build/runtime dependencies of the port and are folded into the
    same ``deps_by_name[name]`` list as the base stanza's, so the transitive
    closure below does not silently stop at a feature boundary.
    """
    text = status_path.read_text(encoding="utf-8", errors="replace")
    version_by_name: dict[str, str] = {}
    deps_by_name: dict[str, list[str]] = {}

    for block in text.split("\n\n"):
        name_match = _PACKAGE_HEADER_RE.search(block)
        if not name_match:
            continue
        name = name_match.group(1).strip()

        version_match = _VERSION_RE.search(block)
        if version_match and name not in version_by_name:
            version_by_name[name] = version_match.group(1).strip()

        depends_match = _DEPENDS_RE.search(block)
        deps: list[str] = []
        if depends_match:
            for raw in depends_match.group(1).split(","):
                dep_name = raw.strip().split(" ")[0].split("[")[0].split("(")[0].strip()
                if dep_name:
                    deps.append(dep_name)
        deps_by_name.setdefault(name, []).extend(deps)

    return version_by_name, deps_by_name


def resolve_closure(declared: list[str], deps_by_name: dict[str, list[str]]) -> list[str]:
    """Every declared dependency plus everything it transitively pulls in.

    Deliberately NOT "every package in the status database" -- a build tree
    can (and, measured 2026-09-11, does) carry packages installed for an
    unrelated purpose that vcpkg.json no longer declares. Walking the
    dependency graph from the manifest, rather than dumping the whole
    install tree, is what makes this reproducible independent of whatever
    else happens to be sitting under build/.
    """
    seen: set[str] = set()
    stack = list(declared)
    while stack:
        name = stack.pop()
        if name in seen:
            continue
        seen.add(name)
        for dep in deps_by_name.get(name, []):
            if dep not in seen:
                stack.append(dep)
    return sorted(seen)


def parse_cargo_lock(cargo_lock_path: Path) -> list[dict[str, str]]:
    """[{"name":..., "version":...}, ...] in file order, duplicates included.

    A crate name can legitimately appear more than once (different versions
    pulled in via different dependency edges) -- collapsing by name would
    silently drop a real, distinct dependency.
    """
    if not cargo_lock_path.exists():
        return []
    packages: list[dict[str, str]] = []
    current: dict[str, str] = {}
    with io.open(cargo_lock_path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if re.match(r"^\[\[package\]\]\s*$", line):
                if current.get("name"):
                    packages.append(current)
                current = {}
                continue
            name_match = re.match(r'^\s*name\s*=\s*"([^"]+)"\s*$', line)
            if name_match:
                current["name"] = name_match.group(1)
                continue
            version_match = re.match(r'^\s*version\s*=\s*"([^"]+)"\s*$', line)
            if version_match:
                current["version"] = version_match.group(1)
    if current.get("name"):
        packages.append(current)
    return packages


# SPDX 2.3 restricts an element id to ``SPDXRef-`` followed by letters, digits,
# ``.`` and ``-`` ONLY. The pre-#181 scheme substituted ``_`` for every other
# character, which is itself off-charset (``SPDXRef-wit_bindgen``), and keyed the
# id on the NAME ALONE, which collides whenever the dependency graph legitimately
# carries one component at several versions (measured on the committed artifact:
# 211 packages, 203 distinct ids) or one name in two ecosystems (``zstd`` is in
# both vcpkg 1.5.7 and cargo 0.13.3). A conforming consumer may reject such a
# document or keep only the last package per id -- and CVE triage would then
# answer "not affected" for a version that actually ships.
_SPDX_ID_DISALLOWED_RE = re.compile(r"[^A-Za-z0-9.\-]")


def sanitize_spdx_component(value: str) -> str:
    """One id segment, reduced to the SPDX-legal charset.

    Off-charset characters collapse to ``-``. That mapping is NOT injective
    (``a_b`` and ``a-b`` both become ``a-b``), so it can in principle create a
    collision it was meant to avoid -- which is exactly why
    ``assert_unique_spdx_ids`` below runs on every generated document and
    FAILS CLOSED rather than trusting the sanitiser to be safe by construction.
    """
    cleaned = _SPDX_ID_DISALLOWED_RE.sub("-", value.strip())
    return cleaned or "NONE"


def spdx_id_for(name: str, version: str, ecosystem: str) -> str:
    """``SPDXRef-<ecosystem>-<name>-<version>`` -- unique per shipped component.

    All three parts are load-bearing: version separates the same component at
    several versions, ecosystem separates the same name resolved by two package
    managers. Dropping either reintroduces GitHub #181.
    """
    return "SPDXRef-{0}-{1}-{2}".format(
        sanitize_spdx_component(ecosystem),
        sanitize_spdx_component(name),
        sanitize_spdx_component(version),
    )


def assert_unique_spdx_ids(packages: list[dict]) -> None:
    """Refuse to emit a document whose SPDXIDs are not unique (GitHub #181)."""
    seen: dict[str, list[str]] = {}
    for pkg in packages:
        seen.setdefault(pkg["SPDXID"], []).append(
            "{0}@{1}".format(pkg.get("name", ""), pkg.get("versionInfo", ""))
        )
    collisions = {key: value for key, value in seen.items() if len(value) > 1}
    if collisions:
        detail = "; ".join(
            "{0} <- {1}".format(key, ", ".join(sorted(value)))
            for key, value in sorted(collisions.items())
        )
        raise SbomGenerationError(
            "{0} SPDXID collision(s) in the generated document: {1}. SPDX 2.3 "
            "requires an element id to be unique within a document; a colliding "
            "id lets a conforming consumer keep only the last package per id, so "
            "CVE triage would report 'not affected' for a version that ships "
            "(GitHub #181).".format(len(collisions), detail)
        )


def _package_entry(name: str, version: str, ecosystem: str) -> dict:
    return {
        "filesAnalyzed": False,
        "name": name,
        "downloadLocation": "NOASSERTION",
        "versionInfo": version,
        "externalRefs": [
            {
                "referenceLocator": "pkg:{0}/{1}@{2}".format(ecosystem, name, version),
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
            }
        ],
        "SPDXID": spdx_id_for(name, version, ecosystem),
    }


def namespace_stamp(created: str) -> str:
    """``2026-09-08T01:02:25Z`` -> ``20260908T010225Z`` (matches run_deps.ps1)."""
    return re.sub(r"[-:]", "", created)


def build_sbom(
    vcpkg_json: Path,
    vcpkg_status: Path,
    cargo_lock: Path,
    created: str,
    tool_name: str = DEFAULT_TOOL_NAME,
    doc_name: str = "PDFE",
    namespace_prefix: str = DEFAULT_NAMESPACE_PREFIX,
) -> dict:
    declared = sbom_direct_deps.direct_dependencies(vcpkg_json)
    version_by_name, deps_by_name = parse_vcpkg_status(vcpkg_status)
    closure = resolve_closure(declared, deps_by_name)

    missing = [name for name in closure if name not in version_by_name]
    if missing:
        raise SbomGenerationError(
            "{0} declares (directly or transitively) {1} package(s) with no "
            "resolvable version in {2}: {3}. Refusing to emit an SBOM that "
            "silently omits or mis-versions a shipped component (meta-audit "
            "SEC-76/SEC-77).".format(
                vcpkg_json, len(missing), vcpkg_status, ", ".join(missing)
            )
        )

    packages = [
        _package_entry(name, version_by_name[name], "vcpkg") for name in closure
    ]
    for pkg in parse_cargo_lock(cargo_lock):
        packages.append(_package_entry(pkg["name"], pkg.get("version", ""), "cargo"))

    # GitHub #181: never emit a document with a duplicate element id. This runs
    # on the FINAL package list, so it also catches a sanitiser-induced collision
    # (see sanitize_spdx_component) rather than assuming the scheme is safe.
    assert_unique_spdx_ids(packages)

    return {
        "spdxVersion": "SPDX-2.3",
        "name": doc_name,
        "documentNamespace": namespace_prefix + namespace_stamp(created),
        "creationInfo": {
            "created": created,
            "creators": ["Tool: {0}".format(tool_name)],
        },
        "SPDXID": "SPDXRef-DOCUMENT",
        # GitHub #177: STATE THE SCOPE, so the document's silence is not read as
        # absence. This SBOM enumerates vcpkg and cargo dependencies. PDFE also
        # ships THIRD-PARTY BINARIES COMMITTED INTO THE REPOSITORY -- the three
        # tesseract .traineddata blobs under tests/fixtures/tessdata/ -- which
        # are structurally outside a dependency-manifest SBOM and are inventoried
        # in THIRD-PARTY-NOTICES.md instead (upstream repo, tag, licence and a
        # per-file checksum, enforced by tools/gate/check_third_party_notices.py).
        #
        # A reader who takes this file as the complete third-party inventory
        # would miss them. That is the whole reason this field exists; see
        # docs/security/supply-chain-scope-decision.md for why the alternative
        # (extending this document to cover them) is sequenced AFTER GitHub #181,
        # which is a defect in this document's own SPDXID scheme.
        "comment": (
            "SCOPE: vcpkg and cargo dependencies only. PDFE additionally ships "
            "third-party binaries committed into the repository "
            "(tests/fixtures/tessdata/*.traineddata) which are NOT enumerated "
            "here; see THIRD-PARTY-NOTICES.md for their provenance, licence and "
            "checksums. This document's silence about an artifact is not "
            "evidence of its absence. Rationale: "
            "docs/security/supply-chain-scope-decision.md (GitHub #177)."
        ),
        "packages": packages,
        "dataLicense": "CC0-1.0",
    }


def structural_signature(doc: dict) -> list[tuple]:
    """The (name, versionInfo, purl, SPDXID) multiset a structural diff cares about.

    Sorted so two documents that differ only in package ORDER (e.g. vcpkg
    list emitting a different but equally valid enumeration order) still
    compare equal -- order was never part of what the SBOM asserts.
    """
    sig = []
    for pkg in doc.get("packages", []):
        purl = ""
        for ref in pkg.get("externalRefs", []):
            if ref.get("referenceType") == "purl":
                purl = ref.get("referenceLocator", "")
                break
        sig.append((pkg.get("name", ""), pkg.get("versionInfo", ""), purl, pkg.get("SPDXID", "")))
    return sorted(sig)


def structural_diff(doc_a: dict, doc_b: dict) -> dict:
    sig_a = structural_signature(doc_a)
    sig_b = structural_signature(doc_b)
    set_a, set_b = set(sig_a), set(sig_b)
    return {
        "equivalent": sig_a == sig_b,
        "count_a": len(sig_a),
        "count_b": len(sig_b),
        "only_in_a": sorted(set_a - set_b),
        "only_in_b": sorted(set_b - set_a),
    }


def _auto_detect_status(repo_root: Path) -> Path:
    """First vcpkg_installed/vcpkg/status under build/*, sorted for determinism.

    Sorted alphabetically over the candidate build-directory names so the
    choice does not depend on filesystem enumeration order (which Windows
    does not guarantee). Prefer an explicit --vcpkg-status/--installed-dir
    over relying on this at all -- see the module docstring.
    """
    build_dir = repo_root / "build"
    candidates = sorted(build_dir.glob("*/vcpkg_installed/vcpkg/status")) if build_dir.is_dir() else []
    if candidates:
        return candidates[0]
    root_status = repo_root / "vcpkg_installed" / "vcpkg" / "status"
    if root_status.exists():
        return root_status
    raise SbomGenerationError(
        "no vcpkg_installed/vcpkg/status found under {0}/build/* or {0}/vcpkg_installed. "
        "Pass --vcpkg-status or --installed-dir explicitly.".format(repo_root)
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--vcpkg-json", type=Path, default=DEFAULT_VCPKG_JSON)
    parser.add_argument("--vcpkg-status", type=Path, default=None, help="path to a vcpkg installed/vcpkg/status file")
    parser.add_argument("--installed-dir", type=Path, default=None, help="a vcpkg_installed dir; status = <dir>/vcpkg/status")
    parser.add_argument("--cargo-lock", type=Path, default=DEFAULT_CARGO_LOCK)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--created", default=None, help="ISO8601 UTC timestamp, e.g. 2026-09-08T01:02:25Z; default: now")
    parser.add_argument("--tool-name", default=DEFAULT_TOOL_NAME)
    parser.add_argument("--namespace-prefix", default=DEFAULT_NAMESPACE_PREFIX)
    parser.add_argument("--no-write", action="store_true", help="do not write --output; just report/compare")
    parser.add_argument("--compare-to", type=Path, default=None, help="an existing SBOM to structurally diff against")
    args = parser.parse_args(argv)

    vcpkg_status = args.vcpkg_status
    if vcpkg_status is None and args.installed_dir is not None:
        vcpkg_status = args.installed_dir / "vcpkg" / "status"
    if vcpkg_status is None:
        try:
            vcpkg_status = _auto_detect_status(REPO_ROOT)
            print("[gen_sbom] auto-detected vcpkg status db: {0}".format(vcpkg_status))
        except SbomGenerationError as exc:
            print("ERROR: {0}".format(exc), file=sys.stderr)
            return 2

    if not args.vcpkg_json.exists():
        print("ERROR: {0} not found".format(args.vcpkg_json), file=sys.stderr)
        return 2
    if not vcpkg_status.exists():
        print("ERROR: {0} not found".format(vcpkg_status), file=sys.stderr)
        return 2

    created = args.created or datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    try:
        doc = build_sbom(
            args.vcpkg_json,
            vcpkg_status,
            args.cargo_lock,
            created,
            tool_name=args.tool_name,
            namespace_prefix=args.namespace_prefix,
        )
    except SbomGenerationError as exc:
        print("ERROR: {0}".format(exc), file=sys.stderr)
        return 3

    print("[gen_sbom] {0} packages ({1} vcpkg + {2} cargo)".format(
        len(doc["packages"]),
        sum(1 for p in doc["packages"] if p["externalRefs"][0]["referenceLocator"].startswith("pkg:vcpkg")),
        sum(1 for p in doc["packages"] if p["externalRefs"][0]["referenceLocator"].startswith("pkg:cargo")),
    ))

    if not args.no_write:
        write_json(args.output, doc)
        print("[gen_sbom] wrote {0}".format(args.output))

    if args.compare_to:
        if not args.compare_to.exists():
            print("ERROR: --compare-to {0} not found".format(args.compare_to), file=sys.stderr)
            return 2
        other = read_json(args.compare_to)
        diff = structural_diff(doc, other)
        print(json.dumps(diff, indent=2, ensure_ascii=False))
        if not diff["equivalent"]:
            return 4

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
