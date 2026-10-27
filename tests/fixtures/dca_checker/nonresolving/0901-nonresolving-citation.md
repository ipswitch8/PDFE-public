# DCA 0901 — fixture: the cited class-level Suite.Case does not exist

Meta-audit follow-up phase 41 fixture. This record is the FAILING half of the
discriminating pair for `tools/gate/check_dca.py`. It is byte-for-byte
identical to `../resolving/0900-resolving-citation.md` except for the case name
in the **Class-level test** field: there it is a case that really is defined in
the cited file, here it is one that is not.

- **Date:** 2026-08-08
- **Escaped to:** gate tooling
- **Severity:** High
- **Reproducing test:** `tests/fixtures/dca_checker/dca_checker_sample.cpp::DcaCheckerSample.PlainCase`
- **Class-level test:** `tests/fixtures/dca_checker/dca_checker_sample.cpp::DcaCheckerSampleFixture.TotallyMadeUpCase`
- **Harness property that missed it:** the citation resolver checked only that the file existed, never that the named case was defined in it.

## Escaped defect
A DCA record could name a class-level test that had never been written, and the
CI step whose name claims the records "name real tests" passed anyway.

## Reproducing test (instance)
`DcaCheckerSample.PlainCase` is a plain `TEST` in the cited fixture source.

## Causal analysis
`check_dca.py` split the citation on `::` and called `Path.exists()` on the left
half only. The right half — the part carrying the actual claim — was discarded.

## Class-level test
`DcaCheckerSampleFixture.TotallyMadeUpCase` is named nowhere in the cited file.
Every other field of this record is well-formed, so the ONLY reason it may fail
is the unresolved citation.

## Prevention / standing control
`check_dca.py` now parses GTest macro definitions out of comment- and
string-blanked source and fails closed on any citation it cannot resolve. This
fixture pair, exercised by `tools/gate/test_check_dca.py`, keeps that behaviour
pinned in CI.
