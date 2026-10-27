# Coverage-Expansion Phase-5 Report — C-10 cross-platform parity + SYNTHESIS

Stance: ADD real wiring + honest docs. Nothing weakened or skipped. **No product
source changed** (`git status --porcelain -- src/` clean). C-10 is largely
runner-only from the Windows dev box: it is WIRED with a byte-identical selection
across all three lanes, PROVEN green locally on Windows, and the macOS/Linux green
is honestly disclosed as CI-validated — **not faked**.

## What was wired

**A single-source CORE PARITY SUITE** — the platform-independent behavioral core
(document open/parse, save-routing, redaction confidentiality, text extraction,
page ops, content-stream editing, coordinate transforms, encryption/permissions,
forms/metadata round-trips, font parsing/substitution, undo/redo, PDF/A +
optimize, generative/format-compat invariants):

| File | Role |
|---|---|
| `tools/gate/parity_suite.txt` | **Single source of truth** — 123 gtest suite patterns, one per line, with the platform-DIVERGENT / platform-SPECIFIC exclusions (Qt UI widgets, GDI/handle endurance, HiDPI pixel fidelity, `PreflightOpen`/`PreflightXPlatform` OS-branch suites, perf, live-network) documented inline. |
| `tools/gate/parity_filter.py` | **Single generator** — reads that list, normalizes each bare suite to `Name.*`, prints the `:`-joined `gtest_filter`. UTF-8. |
| `.github/workflows/windows-validation.yml` | New step "Run + verify CROSS-PLATFORM CORE PARITY suite (C-10)" — runs the filter, gates via `check_junit_xml.py --min-tests 200`. |
| `.github/workflows/linux-validation.yml` | New step "Run + verify CROSS-PLATFORM CORE PARITY suite (C-10)" — same, via `xvfb-run`. |
| `.github/workflows/macos-validation.yml` | Expanded from the PreflightOpen-only R-36 subset: new "Run CROSS-PLATFORM CORE PARITY suite" + "Verify parity JUnit XML" steps. |

Each lane FAILS on a parity-suite failure by parsing the JUnit XML
(`tools/gate/check_junit_xml.py <xml> --min-tests 200`) — GAP-A15 compliant, never
trusting the process exit code. The `--min-tests 200` floor guards a
silently-shrunk selection.

## Identical-filter proof

The selection is **byte-identical across all three lanes by construction**: every
lane invokes the SAME `tools/gate/parity_filter.py` over the SAME
`tools/gate/parity_suite.txt`. There is NO per-lane copy of the filter string to
drift — the strongest form of "identical selection."

- All three workflows reference `tools/gate/parity_filter.py`
  (macOS ×2 incl. comment, Linux ×1, Windows ×1) — verified by grep.
- The emitted filter is deterministic:
  - `patterns = 123`, `filter length = 2820 bytes`
  - `sha256(filter) = 073cc1dcc4fcca745e5ab25ce62dd2e2e03c53d3b3b88255d8db60db93df3d18`
- Windows uses `python`, macOS/Linux use `python3` — same script, same relative
  path, same args; the only difference is the interpreter alias.

## Local parity run (Windows) — GREEN

```
pdfe_tests.exe --gtest_filter="$(python tools/gate/parity_filter.py)" \
    --gtest_output=xml:docs/coverage-expansion/parity_local_win.xml --gtest_brief=1
[  PASSED  ] 949 tests.
[  SKIPPED ] 2 tests.
  (2 DISABLED = the B1/B2 regression locks — intentionally not run)

check_junit_xml.py parity_local_win.xml --min-tests 200
=> PASS tests=953 failures=0 errors=0 run=951   (exit 0)
```

- **123 suites / 953 tests selected** — a meaningful, non-empty, non-platform-
  specific core, far above the 200 floor and not a token handful.
- Every one of the 123 patterns matches ≥1 real suite (zero dud patterns —
  verified by `comm` of the pattern list vs `--gtest_list_tests` output).

## Honest runner-only disclosure

- **macOS (Apple-Silicon `macos-14`, weekly)** and **Linux (Hetzner, dispatch)**
  parity green is validated **by CI, not from this Windows dev box.** It CANNOT be
  run here and is NOT fabricated. The wiring guarantees the byte-identical filter
  runs on those runners; a platform divergence surfaces when they next execute.
- The full closure of covered / runner-only / deferred / open-defects is in
  `docs/coverage-expansion/closure.md`; the open-item ledger is
  `docs/coverage-expansion/residuals.md`.

## The 2 OPEN product defects (restated — NOT closed)

The C-05 generative harness found two real defects, committed as executable
`DISABLED_` locks (they do not run by default; not in the parity pass count):

- **B1** `GenerativeInvariants.DISABLED_B1_RotateThenEditTextUndoRestoresText` —
  rotate-then-edit-text loses text on undo (digest 470→403). OPEN.
- **B2** `GenerativeInvariants.DISABLED_B2_PageStructUndoRedoSaveAsNoCrash` —
  page-struct ops + `saveAs` → `0xC0000005` on teardown. OPEN.

Each flips green (and sheds its `DISABLED_` prefix) only when the underlying
**product** defect is fixed. They are tracked as OPEN in `residuals.md`.

## Verification checklist

- No build required (test binary already current); parity filter run on Windows
  → GREEN, `check_junit_xml.py --min-tests 200` → PASS (953 tests).
- All three edited `.github/workflows/*.yml` parse as YAML (`yaml.safe_load`).
- Parity filter is IDENTICAL across the 3 lanes (single source list + single
  generator; sha256 pinned above).
- `git status --porcelain -- src/` CLEAN — no product source touched.
- `closure.md` + `residuals.md` written; both honestly separate covered /
  runner-only / deferred / open-defects.
- No test simplified, skipped, or weakened.

## Note on committing the tools/gate artifacts

`tools/gate/parity_suite.txt` and `tools/gate/parity_filter.py` fall under the
broad `tools/` `.gitignore` (line 268), like the existing `tools/gate/*` gate
scripts. They must be force-added when committing this phase:
`git add -f tools/gate/parity_suite.txt tools/gate/parity_filter.py`.
