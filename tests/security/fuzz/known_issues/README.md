# Crash-regression corpus

**Status: intentionally empty. No fuzz campaign has produced a REAL-verdict
artifact.** Nothing has been fabricated into this directory.

This is the committed crash-regression corpus for the libFuzzer harnesses in
`tests/security/fuzz/`. It closes the "a reproducer only lives for 30 days"
hole recorded as gaps **SEC-12** and **SEC-33** in
`docs/audit/meta-audit-2026-07/gap-register.md`.

## Layout

```
known_issues/
  <harness-name>/
    crash-<sha1>          # the exact libFuzzer input that reproduced
    leak-<sha1>
    timeout-<sha1>
```

Subdirectories are created by CI on demand; only this README exists until a
finding lands.

## How entries get here

`.github/workflows/windows-fuzz.yml` triages every artifact a campaign
produces (see `Get-ArtifactVerdict` in the "Run fuzz campaigns" step). The
triage exists because on the SentinelOne-protected self-hosted runner a plain,
by-design C++ `MuPdfException` throw is mis-reported by the exception-dispatch
layer as a libFuzzer "deadly signal" — that signature is classified BENIGN.
Anything else (ASan/UBSan report, SEGV, stack overflow, timeout, or an
unrecognised crash) is classified **REAL**, and a REAL artifact is copied into
`known_issues/<harness>/` in the same loop, then uploaded with the run
artifacts.

The copy is a *staging* action inside the CI workspace. Committing the staged
file is a deliberate human step — a reproducer is only worth committing once
someone has confirmed it is not a runner artefact.

## Replaying entries

Replay drives the committed inputs through the same per-input cores the
harnesses use (`tests/security/fuzz/fuzz_targets.h`), from the ordinary MSVC
gtest build in `tests/test_fuzz_harness_smoke.cpp` — no libFuzzer and no
sanitizer runtime required. Add the replay loop when the first entry lands;
writing a replay test over an empty directory would be a green test that
asserts nothing.

## Why the directory is committed empty

Criterion 5 of meta-audit follow-up phase F4 permits either a non-empty corpus
or a written record that none exists. The written record is
`docs/audit/meta-audit-2026-07/followup/phase-4-dispositions.md`. Phase F4 was
scoped to file content only and deliberately ran no fuzz campaign, so there is
no honest way to populate this directory yet.
