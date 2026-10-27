# SAST high-severity finding triage

This document tracks every **high-severity** SAST finding that
appears in `tests/security/sast/baseline.json`. Each entry must
be one of:

| Disposition | Meaning |
|---|---|
| `fix-now`  | Must be fixed *before* phase 5 commits. After fix, the finding's hash drops out of `findings.json` and is removed from baseline. |
| `accept`   | A real finding that we have decided not to fix, with a written rationale. The hash stays in `baseline.json`. |
| `wontfix`  | Same as `accept` but with an explicit ticket / external dependency tracked. |

## Process

1. Run `pwsh scripts/security/run_sast.ps1` (no flags).
2. Inspect `tests/security/reports/sast/findings.json`.
3. For every finding with `severity == "high"` not currently in the
   baseline, decide its disposition and add an entry below.
4. If `fix-now`, fix in `src/` (limited to the finding -- no
   functional changes per phase 5 AC8).
5. Re-run `run_sast.ps1`. Once the high-severity set is the
   baseline + accept/wontfix entries, run `run_sast.ps1 -UpdateBaseline`
   to record the canonical hashes.

## Format for each entry

```
### <rule_id> -- <file>:<line>

**Disposition:** fix-now | accept | wontfix

**Hash:** <sha1 from findings.json>

**Rationale:** (only required for accept / wontfix)

**Tracking:** (link, only required for wontfix)
```

## Triaged findings

### unknownMacro (Qt slots) -- 29 entries across src/ui/*.h

**Disposition:** accept

**Hashes:** Multiple, recorded in `baseline.json`.

**Rationale:** cppcheck's preprocessor doesn't know about Qt's
moc-generated `slots:` / `signals:` keyword (Qt re-defines them
to empty in `qobjectdefs.h`). Without that knowledge cppcheck
flags every Qt class header with `slots:` as containing an
unknown macro. This is a cppcheck environmental issue, not a
real bug. Could be silenced via a Qt-specific cppcheck config
in a future hardening pass; for now, accepted.

### deallocuse + doubleFree in Pkcs7OpenSslBridge.cpp:462-466

**Disposition:** accept

**Hashes:** Multiple, recorded in `baseline.json`.

**Rationale:** cppcheck flags `fclose(fp)` at line 462 + line
466 as a double-free of the file pointer, plus `fread(... fp)`
at line 465 as use-after-close. Looking at the code, line 462
is inside an `if (!pfxData) { fclose(fp); fz_throw(...); }`
block. `fz_throw` is MuPDF's error mechanism: it longjmps out
of the function and never returns. cppcheck doesn't know
`fz_throw` is `[[noreturn]]` and so models a control flow that
falls through to lines 463+, treating the close as already-
done. This is a cppcheck modeling limitation, not a real bug.

A future hardening pass could annotate `fz_throw` as
`[[noreturn]]` in a wrapper header, or restructure the function
to put the throw in a way cppcheck can model. Neither is worth
the disruption right now.

### Lower-severity findings

249 low-severity + 7 info findings remain UN-triaged in
`tests/security/reports/sast/findings.json`. They include
`useStlAlgorithm` (51), `shadowFunction` (42),
`constParameterPointer` (35), and similar style-level rules.
These don't fail the gate but are real opportunities for
incremental cleanup; pick them up case-by-case in normal
development.
