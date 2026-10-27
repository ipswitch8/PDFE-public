# Test Categorization

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Purpose:** Document how PDFE tests are organized so that selective runs
(CI smoke, nightly full, ad-hoc) can be performed by filtering on
prefixes or labels.

## C++ Test Suite Categorization (by prefix)

GTest doesn't have first-class labels, but we use test-suite name
prefixes as the de facto categorization.

| Prefix family | Examples | Run when |
|---|---|---|
| **Unit / model** | `DocumentTest`, `PageTest`, `UndoManagerTest`, `AnnotationManagerTest`, `FormManagerTest` | PR (fast), nightly |
| **Integration** | `SignatureRoundtripTest`, `EncryptionRoundTripTest`, `LtvValidatorTest` | PR, nightly |
| **Security** | `MaliciousPdfTest`, `MaliciousPdfAdvancedTest`, `JsApiSurfaceTest`, `CveReplayTest`, `SecurityAudit`, `RedactionCompletenessTest`, `DocMdpByteRangeTest`, `OcspValidityTest`, `Pkcs7FuzzSmokeTest`, `FontFuzzSmokeTest`, `SavePathSecurityTest`, `PageOverflowTest` | PR (must pass), nightly |
| **Stress** | `PerfStressTest`, `UndoStackStressTest`, `FormFieldSaveStressTest` | Nightly (slow) |
| **Real-world** | `ExternalRealWorldTest`, `RealWorldCorpusTest` | Nightly (requires corpus assembled) |
| **UI access** | `DialogWidgetAccessTest`, `MenuActionAccessibility`, `UndoRedoFlowTest` | PR, nightly |
| **Q_INVOKABLE seams** | `QInvokableCoverageTest`, `MainWindowQInvokable*`, `PageOpsQInvokable*` | PR, nightly |
| **Compliance** | `PdfAValidatorTest`, `PdfUAConverterTest`, `ComplianceManagerTest` | PR, nightly |
| **TRE / OCR** | `OcrPipelineTest`, `GlyphAtlasTest`, `ScanStyleTransferTest` | Nightly (slow) |
| **Phase markers (legacy)** | `Phase4*`, `PhaseG*`, `PipelineG*` | Nightly |

### Running by category

```bash
# Fast smoke (security must always pass)
pdfe_tests --gtest_filter="MaliciousPdfTest.*:MaliciousPdfAdvancedTest.*:CveReplayTest.*:SecurityAudit.*:DocumentTest.*:PageTest.*"

# Nightly full
pdfe_tests

# Stress only
pdfe_tests --gtest_filter="PerfStressTest.*:UndoStackStressTest.*:FormFieldSaveStressTest.*"

# Real-world (requires corpus)
pdfe_tests --gtest_filter="ExternalRealWorldTest.*:RealWorldCorpusTest.*"
```

---

## Python UI Test Categorization (by file)

Pytest supports `-k` filtering and `-m` markers. PDFE uses file names
plus pytest markers.

### Smoke (5-10 min) — runs on every PR

- `tests/test_ui_canvas_sendinput.py`
- `tests/test_ui_canvas_coords_sendinput.py`
- `tests/test_ui_canvas_functional_sendinput.py`
- `tests/test_ui_sendinput.py` (multi-page select)
- `tests/test_ui_hidden_widgets_e2e.py`

### Standard (15-30 min) — runs on PR + nightly

- `tests/test_ui_menu_full_walk.py` (36 actions, ~5 min)
- `tests/test_ui_redaction_roundtrip.py`
- `tests/test_ui_js_sandbox.py`
- `tests/test_ui_e2e_workflows.py` (keyboard nav, multi-doc, DPI)

### Slow / nightly only

- `tests/test_ui_leak_detection.py` (100-iter is ~5 min; longer
  in PRE_RELEASE mode via `PDFE_LEAK_ITER=500`)
- `tests/test_compat_workflows.py` (multiple locale launches)

### Manual only (not in automation)

- `tests/test_winappdriver_smoke.py` — requires WinAppDriver running
- `tests/test_ui_encryption_flows_wad.py` — requires WinAppDriver

### Run examples

```bash
# Smoke
python -m pytest tests/test_ui_*sendinput*.py tests/test_ui_hidden_widgets_e2e.py

# Standard
python -m pytest tests/test_ui_menu_full_walk.py tests/test_ui_e2e_workflows.py

# Long leak detection
PDFE_LEAK_ITER=500 python -m pytest tests/test_ui_leak_detection.py
```

---

## CI Workflow Mapping

| Workflow | Tests run | When |
|---|---|---|
| `.github/workflows/sanitizer.yml` | All C++ under ASan+UBSan | PR + nightly |
| `.github/workflows/fuzz.yml` | 7 fuzz harnesses (matrix) | Nightly 30 min each |
| (future) `.github/workflows/ui-smoke.yml` | Python smoke set | PR |
| (future) `.github/workflows/ui-nightly.yml` | Full Python suite | Nightly |

---

## Adding a New Test — Classification Checklist

When adding a test, ask:

1. **Is it security-relevant?** → Add to `tests/test_security_*` or
   `tests/test_malicious_pdf_*` or `tests/test_cve_replay.cpp`. Must
   PASS on every PR.
2. **Is it slow (>5s)?** → Tag with `// SLOW` comment and prefer
   `PerfStressTest` or `StressTest` suite naming.
3. **Does it need external resources?** (network, real cert, etc.) →
   Use `GTEST_SKIP()` when resource absent. Document the dependency.
4. **Is it UI-driven?** → Put in `tests/test_ui_*.py`. Mark with
   `pytest.mark.skipif(not _is_interactive())`.
5. **Is it a regression for a specific bug?** → Suite name should
   include the bug ID (e.g., `Gap102ResidualsTest`).
