# Phase 6 — Dependency CVE audit

Enumerates every direct + transitive dependency on both the C++
side (vcpkg manifest) and the Rust side (`rust/tantivy_bridge/Cargo.lock`),
emits an SPDX 2.3 SBOM, runs OSV-Scanner (or grype as fallback)
against it, runs `cargo audit` inside the Rust crate, normalizes
all findings into a single `cves.json` and compares to a canonical
baseline.

## OPT-IN posture

The dependency audit is OPT-IN: invoked manually via
`scripts/security/run_deps.ps1` or by phase 9's nightly job. Not
part of regular ctest / `pdfe_tests.exe`.

## Tooling prerequisites

| Tool | Required for | Where to get it |
|---|---|---|
| `vcpkg` | SBOM (vcpkg list --x-json for transitives) | `C:\Users\dev\source\vcpkg\vcpkg.exe` (this machine; configurable via `-VcpkgRoot`) |
| `cargo` | Cargo.lock parse + `cargo audit` invocation | `cargo` on PATH |
| `cargo-audit` | RustSec advisory lookup | `cargo install cargo-audit` |
| `osv-scanner` | OSV/NVD advisory lookup against SBOM | https://github.com/google/osv-scanner/releases or `scoop install osv-scanner` |
| `grype` (alternative) | Fallback CVE scanner | https://github.com/anchore/grype/releases |

`run_deps.ps1` tolerates missing scanners — it always emits the
SBOM, and if neither osv-scanner nor grype is installed, just
warns and skips the SBOM CVE pass while still running `cargo
audit` for the Rust side. Without any scanner, `cves.json` will
be empty and the run is informational only.

## Running

```powershell
# Default: uses vcpkg at C:\Users\dev\source\vcpkg, expects
# osv-scanner and cargo-audit on PATH.
pwsh scripts/security/run_deps.ps1

# Different vcpkg path
pwsh scripts/security/run_deps.ps1 -VcpkgRoot D:\src\vcpkg

# Skip osv if not installed (just SBOM + cargo audit)
pwsh scripts/security/run_deps.ps1 -SkipOsv

# Update baseline after triaging cves.json in triage.md
pwsh scripts/security/run_deps.ps1 -UpdateBaseline
```

Output:

| File | Contents |
|---|---|
| `tests/security/reports/deps/sbom.spdx.json`    | SPDX 2.3 SBOM (one document, package-level) |
| `tests/security/reports/deps/cves.json`         | Normalized findings |
| `tests/security/reports/deps/summary.md`        | Counts + MuPDF/QPDF check |
| `tests/security/reports/deps/osv.json`          | Raw OSV/grype output |
| `tests/security/reports/deps/cargo_audit.json`  | Raw cargo-audit JSON |

## Schema

`cves.json` records:

| Field | Type | Notes |
|---|---|---|
| `package`       | string | as known to its ecosystem |
| `version`       | string | as installed (vcpkg list / Cargo.lock) |
| `ecosystem`     | string | `vcpkg` or `cargo` |
| `cve_id`        | string | OSV id, RUSTSEC id, or comma-joined aliases |
| `cvss`          | float  | numeric CVSS base score (0 if unscored) |
| `fixed_version` | string | comma-joined patched versions when known |
| `advisory_url`  | string | upstream advisory URL |
| `source`        | string | `osv-scanner`, `grype`, or `rustsec` |

## Severity gate

The script exits non-zero if a NEW CVE with `cvss >= 7.0` appears
that is not already accepted in `tests/security/deps/baseline.json`
under `cve_ids_ge_7`. To accept a new high-severity CVE:

1. Document it in `triage.md` with disposition
   `upgrade-now` / `accept` / `monitor` and the required fields.
2. If `upgrade-now`, bump the dependency in `vcpkg.json` /
   `Cargo.toml`, reconfigure, rebuild, run the standard 2068-test
   suite, re-run `run_deps.ps1`. The CVE should drop out of
   `cves.json`.
3. Once the live `cves.json` is the set of accepted CVEs, run
   `run_deps.ps1 -UpdateBaseline` to write the canonical
   baseline.

## MuPDF / QPDF special case (AC7)

The dispatcher prints a CRITICAL section in `summary.md` if either
`libmupdf` or `qpdf` has an unaccepted CVSS>=7 finding, and exits
1 even when the rest of the run would have passed. These two
libraries are PDFE's primary attack surface; an unpatched
high-CVSS finding here is an immediate phase failure regardless
of baseline.

## Why no live baseline is committed

Same Option A pattern as phases 3-5: the live SBOM CVE scan
requires `osv-scanner` (or `grype`) and `cargo-audit` installed,
plus a network connection to fetch fresh advisory data. Producing
the canonical `baseline.json` is a hands-on step:

1. Install `cargo-audit`, install `osv-scanner`.
2. Run `pwsh scripts/security/run_deps.ps1`.
3. Open `tests/security/reports/deps/cves.json`. For every CVE
   with cvss >= 7.0, add a triage entry in `triage.md`
   (`upgrade-now` / `accept` / `monitor`).
4. For every `upgrade-now`, bump the dependency, rebuild, re-run
   the 2068-test suite to confirm the bump didn't regress.
5. Run `run_deps.ps1 -UpdateBaseline` to write the canonical
   baseline. Commit it alongside the triage.md updates.

Phase 9's regression check then compares all future runs to that
baseline.

## Limitations

- **vcpkg list output schema can vary.** The dispatcher tolerates
  missing fields (`package_name`, `version`); the SBOM falls back
  to direct deps from `vcpkg.json` if `vcpkg list` is unavailable.
- **CVSS extraction is heuristic.** OSV-Scanner reports the
  numeric base score under `severity[].score` for some
  ecosystems but not all; cargo-audit doesn't always populate
  `cvss`. Findings without a numeric score get `cvss=0.0` and
  do NOT trigger the high-severity gate -- a future improvement
  is to look the score up via NVD when missing.
- **Fixed-version field is best-effort.** When osv-scanner/grype
  don't expose `fixed`, the field is empty.
