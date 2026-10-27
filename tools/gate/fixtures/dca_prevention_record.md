# DCA 9903 — fixture record with a resolving Prevention section

FIXTURE, not a real DCA record. It lives under `tools/gate/fixtures/` (outside
`docs/audit/dca/`, so the live scan never sees it) and exists so
`test_check_dca.py` can pin BOTH directions of the Prevention-section
resolution added for GitHub #203: this document as committed must PASS, and the
two single-token mutations the self-test applies must FAIL.

Every citation below is deliberately pointed at something committed and stable:
the purpose-built GTest fixture source `tests/fixtures/dca_checker/dca_checker_sample.cpp`,
and mutation seed `M01-statecont-ctm-guard-drop-Q` in `tools/gate/mutants.json`.

- **Date:** 2026-09-18
- **Escaped to:** fixture
- **Severity:** Low
- **Reproducing test:** `tests/fixtures/dca_checker/dca_checker_sample.cpp::DcaCheckerSample.PlainCase`
- **Class-level test:** `tests/fixtures/dca_checker/dca_checker_sample.cpp::DcaCheckerSampleFixture.FixtureCase`
- **Harness property that missed it:** the Prevention section was required to be
  present and nothing inside it was resolved against the tree.

## Escaped defect

A standing control was described in prose and never existed.

## Reproducing test (instance)

The instance test replays the exact escaping sequence.

## Causal analysis

The checker resolved the two bold-label fields and stopped there, so the one
section that describes only standing controls was the one section nothing
checked.

## Class-level test

A fixture-parametrised corpus covers the family rather than the instance.

## Prevention / standing control

Three standing controls keep this class dead:

- the bare-name control `DcaCheckerSampleFixture.MultiLineCase` — a `Suite.Case`
  named without a file path, resolved against the GTest macro definitions
  present anywhere under `tests/`;
- the path-qualified control
  `tests/fixtures/dca_checker/dca_checker_sample.cpp::DcaCheckerSampleParam.ParameterisedCase`
  — resolved against that file specifically, including the runtime-instantiated
  spelling;
- a **mutation seed** `M01-statecont-ctm-guard-drop-Q` registered in
  `tools/gate/mutants.json`, so the claim that a seed exists is checked against
  the manifest rather than taken on trust.

Prose in this section deliberately contains backticked things that are NOT
citations and must not be resolved: `Document.cpp`, `MANIFEST.sha256`,
`CMakeLists.txt`, `Qt::CaseInsensitive`, `pdfe::core::detail::setExecutableDirForTesting`,
`std::vector` and the metasyntactic `Suite.Case`. If a future change to the
checker starts reddening on those, this fixture fails and says so.

**Correction, 2026-08-09 (fixture).** This dated block is here to pin the
historical-addendum exemption: it cites `OcspValidityTest.FreshCrlAccepted`, a
test name that no longer exists in the tree, exactly as DCA 0014 does when
describing a rename that has since happened. Narrative about the past must not
redden an honest record.
