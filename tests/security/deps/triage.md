# Dependency CVE triage

This document records the disposition for every CVE in
`tests/security/deps/baseline.json`. Each entry must be one of:

| Disposition | Meaning |
|---|---|
| `upgrade-now` | Bump the dep version this phase. Hash drops out of `cves.json` and is removed from baseline once the upgrade lands. |
| `accept`      | Real CVE we've decided not to patch (e.g., the vulnerable code path is unreachable from PDFE). Hash stays in baseline; rationale required. |
| `monitor`     | We're tracking but no action yet (e.g., upstream patch not released). Re-evaluation date required. |

## Process

1. Run `pwsh scripts/security/run_deps.ps1` (no flags).
2. Inspect `tests/security/reports/deps/cves.json` and
   `summary.md`.
3. For every CVE with `cvss >= 7.0` not in the current baseline,
   add a triage entry below.
4. For `upgrade-now`, edit `vcpkg.json` (or the relevant
   `Cargo.toml`), reconfigure CMake, rebuild, run the **full
   `pdfe_tests` suite** to confirm the bump didn't break anything
   (AC6). Once green, run `run_deps.ps1` again -- the CVE should
   no longer appear in `cves.json`.
   For MuPDF/QPDF specifically, follow the **Dependency bump
   runbook** in `docs/security-policy.md` -- the overlay ports carry
   security patches that a naive version bump silently drops.
   (This step used to quote a fixed suite size of 2068 tests. The figure
   was thousands of tests out of date and nothing checked it, so it
   is gone rather than re-guessed -- meta-audit gap `SEC-82`.)
5. Run `pwsh scripts/security/run_deps.ps1 -UpdateBaseline` to
   record the canonical baseline.

## Per-entry format

```
### <CVE-ID> -- <package>@<version>

**Disposition:** upgrade-now | accept | monitor

**Severity:** <cvss>

**Advisory:** <url>

**Rationale:** (required for accept; for upgrade-now the rationale
                is "patch available")

**Target version:** (required for upgrade-now)

**Re-evaluation date:** (required for monitor; ISO 8601 date)
```

## Triaged CVEs

### RUSTSEC-2024-0384 -- instant@0.1.13

**Disposition:** RESOLVED (2026-04-25)

**Resolution:** Bumped `tantivy` from 0.22 → 0.26 in
`rust/tantivy_bridge/Cargo.toml`. The newer tantivy no longer
pulls in `measure_time` → `instant`. Verified via `cargo tree`
(no `instant` in 0.26 dep graph) and `cargo audit` (no
warning).

### RUSTSEC-2026-0002 / GHSA-rhfx-m35p-ff5j -- lru@0.12.5

**Disposition:** RESOLVED (2026-04-25)

**Resolution:** Same `tantivy` 0.22 → 0.26 bump pulled in a
newer `lru` release that's past the advisory window. Verified
via `cargo audit` (no warnings, exit 0).

## MuPDF + QPDF advisory check (AC7)

Phase 6 AC7 explicitly says: "MuPDF 1.26.10 and QPDF 12.3.2 are
checked against advisories; if either has an unpatched high-CVSS
CVE the phase fails." The dispatcher special-cases these two
libraries in summary.md and refuses to PASS if either has an
unaccepted CVSS>=7 entry. As of phase 6 commit time, no such
advisories are known to apply; **verified by the live osv-scanner
run on 2026-04-25** -- 35 vcpkg packages including libmupdf and
qpdf, zero CVE findings on the C++ side.

> **De-duplicated 2026-08-02 (meta-audit follow-up F9, gap `SEC-80`).**
> This heading appeared TWICE in this file with contradictory bodies:
> the copy above says the advisory check was **verified by a live
> osv-scanner run**, while the deleted copy below it said the same
> check was **"unverified ... treat as the next required hands-on
> check"**. A reader who scrolled to the second one concluded the
> dependency audit had never been run. The stale copy has been
> removed; the verified one is authoritative. Heading uniqueness in
> this file is now asserted by
> `CiSupplyChain.DepsTriageHeadingsAreUnique` in
> `tests/test_ci_supplychain.cpp`, so the pair cannot reappear.
