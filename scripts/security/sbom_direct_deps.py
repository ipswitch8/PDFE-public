#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SBOM <-> vcpkg.json direct-dependency reconciliation (meta-audit F13).

Gap ids: SEC-76 (SBOM omits direct deps), SEC-77 (no completeness check),
SEC-78 (SBOM was gitignored), SEC-79 (no pinned dependency inventory).

THE FINDING
-----------
Phase-4 of the meta audit found that ``tests/security/reports/deps/sbom.spdx.json``
listed 191 packages yet omitted FOUR of the eleven direct dependencies declared
in ``vcpkg.json`` -- qtsvg, tesseract, pugixml and curl -- and that the file was
``.gitignore``d, so nobody could review it and no test could read it. An SBOM
that silently omits direct dependencies is worse than no SBOM: a CVE triage run
against it reports "not affected" for a component the product actually ships.

WHAT THIS SCRIPT DOES
---------------------
``--check``  Compare the direct dependencies declared in ``vcpkg.json`` against
             the packages enumerated in the SBOM. Writes a machine-readable
             difference report and exits non-zero when they disagree or when a
             direct dependency carries no readable version.

``--fill``   Add any missing direct dependency to the SBOM, taking its version
             from a vcpkg ``installed/vcpkg/status`` database (the versions the
             build actually resolved). Used to repair the committed SBOM; the
             completeness gate added to ``run_deps.ps1`` prevents a future
             regeneration from dropping them again.

Both modes are pure file operations. Nothing here reaches the network.
"""

from __future__ import annotations

import argparse
import io
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_VCPKG_JSON = REPO_ROOT / "vcpkg.json"
DEFAULT_SBOM = REPO_ROOT / "tests" / "security" / "reports" / "deps" / "sbom.spdx.json"


def read_json(path: Path):
    with io.open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path: Path, payload) -> None:
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False, sort_keys=False)
        handle.write("\n")


def direct_dependencies(vcpkg_json: Path) -> list[str]:
    """Every DIRECT dependency name declared in vcpkg.json, in declared order.

    Handles both the plain-string form ("qtsvg") and the object form
    ({"name": "libmupdf", "version>=": "..."}). Platform-conditional entries
    (curl is !windows) are INCLUDED: the SBOM describes the product, and a
    dependency that ships on Linux is still a dependency.
    """
    manifest = read_json(vcpkg_json)
    names: list[str] = []
    for dep in manifest.get("dependencies", []):
        if isinstance(dep, str):
            names.append(dep)
        elif isinstance(dep, dict) and dep.get("name"):
            names.append(str(dep["name"]))
    return names


def sbom_packages(sbom_path: Path) -> dict[str, str]:
    """name -> versionInfo for every package in the SBOM."""
    doc = read_json(sbom_path)
    out: dict[str, str] = {}
    for pkg in doc.get("packages", []):
        name = pkg.get("name")
        if not name:
            continue
        out[str(name)] = str(pkg.get("versionInfo") or "")
    return out


def parse_vcpkg_status(status_path: Path) -> dict[str, str]:
    """port name -> resolved version from a vcpkg installed status database."""
    versions: dict[str, str] = {}
    current: dict[str, str] = {}

    def flush() -> None:
        name = current.get("Package")
        version = current.get("Version")
        # Feature stanzas repeat the package with no Version; ignore them.
        if name and version and "Feature" not in current:
            port = current.get("Port-Version")
            versions.setdefault(name, version + ("#" + port if port else ""))

    with io.open(status_path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if not line.strip():
                flush()
                current = {}
                continue
            if ": " in line:
                key, value = line.split(": ", 1)
                current[key] = value
    flush()
    return versions


def do_check(vcpkg_json: Path, sbom_path: Path, out_paths: list[Path]) -> int:
    declared = direct_dependencies(vcpkg_json)
    present = sbom_packages(sbom_path)

    missing = [name for name in declared if name not in present]
    versionless = [
        name
        for name in declared
        if name in present and (not present[name] or present[name].startswith("unknown"))
    ]

    report = {
        "generated_by": "scripts/security/sbom_direct_deps.py --check",
        "gaps": ["SEC-76", "SEC-77", "SEC-78", "SEC-79"],
        "vcpkg_json": str(vcpkg_json.relative_to(REPO_ROOT).as_posix()),
        "sbom": str(sbom_path.relative_to(REPO_ROOT).as_posix()),
        "direct_dependency_count": len(declared),
        "sbom_package_count": len(present),
        "direct_dependencies": {name: present.get(name, "") for name in declared},
        "missing_from_sbom": missing,
        "present_but_versionless": versionless,
        "difference_count": len(missing) + len(versionless),
        "status": "OK" if not missing and not versionless else "MISMATCH",
    }

    for out in out_paths:
        out.parent.mkdir(parents=True, exist_ok=True)
        write_json(out, report)

    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0 if report["status"] == "OK" else 2


def do_fill(vcpkg_json: Path, sbom_path: Path, status_path: Path) -> int:
    declared = direct_dependencies(vcpkg_json)
    resolved = parse_vcpkg_status(status_path)
    doc = read_json(sbom_path)
    present = {str(p.get("name")) for p in doc.get("packages", []) if p.get("name")}

    added = []
    for name in declared:
        if name in present:
            continue
        version = resolved.get(name)
        if not version:
            print(
                "ERROR: {0} is a direct dependency but no version could be read "
                "from {1}".format(name, status_path),
                file=sys.stderr,
            )
            return 3
        doc.setdefault("packages", []).append(
            {
                "externalRefs": [
                    {
                        "referenceType": "purl",
                        "referenceLocator": "pkg:vcpkg/{0}@{1}".format(name, version),
                        "referenceCategory": "PACKAGE-MANAGER",
                    }
                ],
                "downloadLocation": "NOASSERTION",
                "versionInfo": version,
                "name": name,
                "filesAnalyzed": False,
                "SPDXID": "SPDXRef-{0}".format(name),
            }
        )
        added.append("{0}@{1}".format(name, version))

    doc["packages"] = sorted(doc.get("packages", []), key=lambda p: str(p.get("name", "")))
    write_json(sbom_path, doc)
    print("added: " + (", ".join(added) if added else "(nothing missing)"))
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcpkg-json", type=Path, default=DEFAULT_VCPKG_JSON)
    parser.add_argument("--sbom", type=Path, default=DEFAULT_SBOM)
    parser.add_argument(
        "--out",
        type=Path,
        action="append",
        default=None,
        help="where to write the --check difference report (repeatable)",
    )
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--fill",
        type=Path,
        metavar="VCPKG_STATUS",
        help="path to a vcpkg installed/vcpkg/status database to read versions from",
    )
    args = parser.parse_args(argv)

    if args.fill:
        rc = do_fill(args.vcpkg_json, args.sbom, args.fill)
        if rc or not args.check:
            return rc

    outs = args.out or [
        REPO_ROOT
        / "docs"
        / "audit"
        / "meta-audit-2026-07"
        / "followup"
        / "phase-13"
        / "sbom-vcpkg-diff.json"
    ]
    return do_check(args.vcpkg_json, args.sbom, outs)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
