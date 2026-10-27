# Testing methodology: cumulative single-session state defects

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


Pipeline `f4a82c1e` — Phase 1 (methodology, doc-only). This document names a
bug-class the PDFE test suite has never systematically checked, shows with
reproducible evidence why the current suite is *structurally* unable to catch
it, and specifies a long-lived-session "journey" soak methodology (with named
checkpoints and cumulative invariants) that later phases will implement, use to
discover defects, and lock into CI.

---

## 1. The bug-class: cumulative single-session state defects

**Definition.** A *cumulative single-session state defect* is a failure that
only manifests after a **sequence or combination of operations performed in one
long-lived application session on one document** — not reproducible by any
single operation from a fresh launch. The fault lives in state that **accrues**
across operations: dirty-page sets, the undo/redo stack, the tile cache, the
MuPDF store (256 MB budget), OS handles/threads, and the on-disk file after
repeated in-place saves.

This is distinct from the single-operation defect classes the suite (and the
just-closed `3e91a7c4` pipeline) already address:

| Already-covered class | Nature | Example |
|---|---|---|
| Committed-output correctness | one edit → wrong/absent output | `#4/#5` small-text edit no-op |
| UI structural / affordance | one launch → missing icon/tab | `#1/#2/#3` |
| Single-op crash / extraction | one op → crash | `#6` `/ActualText` extraction abort |
| **Cumulative single-session (THIS doc)** | **op A *arms* a fault op B later triggers; state accrues to crash / lost change / corruption** | **`#6` in its real form: edit → scroll → crash** |

### Sub-classes (each gets an explicit oracle in Phase 2)
- **A — armed crash / UB:** operation A leaves the app in a state where a later,
  unrelated operation B crashes or reads freed memory. (`#6`'s real shape.)
- **B — lost changes:** an edit is silently dropped when interleaved with
  save / undo / navigation / another edit — the user believes it was applied.
- **C — on-disk corruption:** repeated `save → edit → save` on the same handle
  produces a file that no longer reopens, or reopens with a broken xref /
  missing objects.
- **D — resource exhaustion / leak:** RSS, OS handles, threads, or tile/store
  memory grow **monotonically and unbounded** under sustained editing, ending in
  a slowdown, an allocation failure, or an OOM crash.
- **E — undo divergence:** after a long, mixed op sequence, full undo does **not**
  return the document to its verified-clean starting state.

---

## 2. Why the current suite cannot catch this class (fresh-session-per-test)

The PDFE UI suite is **fresh-session-per-test**: almost every test launches a new
`PDFE.exe`, performs **one** operation, asserts, and tears the process down. A
methodology that restarts between operations is, by construction, incapable of
observing anything that accrues *across* operations.

### Evidence (independently re-derivable — run these from the repo root)
```bash
# Total fresh-session launches (each `with PDFESession(` = one PDFE process):
grep -rE "with PDFESession\(" tests/ | grep -v __pycache__ | wc -l      # -> 290

# Number of test files that launch a session:
grep -rlE "with PDFESession\(" tests/ | grep -v __pycache__ | wc -l     # -> 42

# Launches per file (the worst offenders relaunch dozens of times):
grep -rcE "with PDFESession\(" tests/ | grep -v __pycache__ \
    | awk -F: '$2>1{print $2"  "$1}' | sort -rn | head
#   47  tests/test_ui_annotations.py
#   41  tests/test_ui_file_operations.py
#   34  tests/test_ui_view_navigation.py
#   34  tests/test_ui_text_editing.py
#   31  tests/test_ui_page_operations.py
```

**290 launches across 42 files, one operation each** (excluding compiled
`.pyc` duplicates). `test_ui_annotations.py` alone starts and kills PDFE **47
times**. The few module-scoped-session tests
(`tests/ui_automation/test_ui_batch1_actions.py` and siblings) reuse a process,
but only to fire **independent, order-agnostic** "fire-and-survive" action
smokes — they neither perform a *diverse* accumulating edit sequence nor assert
any cumulative invariant. The one resource test, `tests/test_ui_leak_detection.py`,
measures growth over **open/close churn**, not sustained *editing* of one open
document.

### The motivating proof: bug `#6`
Bug `#6` (the `Access Violation: read 0x0` crash) is the canonical instance of
this class, and it is exactly the kind of defect fresh-session testing cannot
see. Its real-world trigger was a **combination in one session**: a text edit /
Replace-All calls `markPageDirty()` + `refreshAllPages()` → `cancelAll()` flags
in-flight render workers **without joining** → the user then **scrolls** →
`createDisplayList(dirtyPage)` runs `pdf_drop_page_tree()` while old workers are
still inside `fz_run_display_list()` → use-after-free. "After a few pages" =
accumulating in-flight workers + the 256 MB MuPDF store crossing eviction + each
revealed dirty page re-dropping the tree. (The `/ActualText` extraction underflow
fixed in `3e91a7c4` P2 was the *first* thing Replace-All hit; the render-lifetime
race is a **separate, still-open** cumulative hazard — tracked as `B-1` in the
`3e91a7c4` closure doc, and the primary discovery target here.)

No fresh-session test reaches this: each of the 290 launches does one thing. Only
a **single session that edits, then scrolls, then edits again** arms and fires
it. The `3e91a7c4` E2E walkthrough (`tests/test_ui_specimen_a_e2e_walkthrough.py`)
was a first step in this direction — but it is one scripted sequence with a
crash/lost-change check at the end, not a systematic *checkpointed journey* over
the full operation vocabulary with cumulative invariants throughout.

---

## 3. The long-lived-session "journey" soak methodology

**One** PDFE process is launched on **one** document and driven through a **long,
diverse, realistic operation sequence** — the *journey* — with **named
checkpoints interspersed** (not only a final assertion). At each checkpoint the
harness asserts the cumulative invariants below; between checkpoints it performs
a mixed batch of real operations drawn from the full editing vocabulary.

### Target documents
- The real **"SpecimenA Green"** 51-page user PDF
  (`C:\Users\dev\Desktop\SpecimenA Green A Tale of Two Collards - Original
  Recipe - Tuvache Betton.pdf`) — **skip-if-absent** (the user's copyrighted
  file cannot be committed).
- A newly **committed, CI-portable multi-page corpus** (Phase 2) that reproduces
  the salient structure — nonzero-origin page, small body text, `/ActualText`
  spans, blank/image-only pages, enough pages to force tile/store eviction — so
  the journey (and the Phase 4 CI lane) always has a non-skippable target.

### The journey (illustrative op vocabulary, order matters)
open → navigate/scroll all pages → edit small text on several *different* pages →
add sticky-note + highlight + ink annotations on several pages → move/delete some
annotations → Replace All → insert / rotate / delete pages → redact a region →
deep undo (unwind much of the above) → partial redo → **save → reopen → re-extract**
→ resume editing on the reopened document → scroll all pages again → repeat the
batch until the journey's op budget is spent.

### Checkpoints
A checkpoint fires after each mixed batch (e.g. every ~15–25 ops) **and** at each
save/reopen boundary. At every checkpoint the harness records the resource
snapshot and asserts the invariants in §4. The journey is parameterized by an op
budget: a **short** budget for the ~9 h self-hosted CI lane, a **long** budget
for a nightly/extended run.

---

## 4. Cumulative invariants / oracles (asserted at every checkpoint)

| ID | Invariant | Oracle | Sub-class |
|----|-----------|--------|-----------|
| I-1 | **Process alive** — no crash, abort dialog, or hang | PID still in `list_pdfe_pids()`; no MSVC abort/AV dialog window present; UIA main window still responsive | A |
| I-2 | **Resource bounded** — RSS, OS handle count, thread count do not grow **monotonically without bound** across checkpoints | `psutil` snapshot per checkpoint (as in `test_ui_leak_detection.py`); assert the trend is bounded (e.g. late-checkpoint delta < a budget, and not strictly increasing every checkpoint) — not a single before/after | D |
| I-3 | **Undo → clean** — after unwinding the batch, the document returns to the checkpoint's verified-clean baseline | dirty indicator (`*` in title) clears at full-undo; extracted text/structure matches the pre-batch snapshot | E |
| I-4 | **Save→reopen integrity** — after N mixed edits, `save → reopen → re-extract` yields **exactly** the expected content: every applied edit present, nothing lost, and the file **reopens without error** (no xref/object corruption) | drive `testSaveAsPipe` (or Ctrl+S), reopen the saved file (fresh `Document::open` / PyMuPDF), re-extract text + object structure, diff against the expected post-edit model; assert reopen succeeds and diff == expected | B, C |
| I-5 | **Tile/store sanity under editing** — the tile cache and the 256 MB MuPDF store **evict** (do not grow unbounded), and **no stale tile is served for an edited page** (post-edit render reflects the edit) | render/extract an edited page after further ops and assert it shows the edit, not a cached pre-edit tile; observe store/tile memory bounded via I-2's snapshot | A, D |

Each invariant maps to a discovery target; a violation at a checkpoint is a
found defect, ranked **crash (I-1) > corruption/lost-change (I-4) > leak (I-2) >
undo divergence (I-3) > stale-tile (I-5)**.

---

## 5. Reusable infra vs. what is missing

**Reuse (build on, don't rebuild):**
- **Module-scoped `PDFESession` reuse** — `tests/ui_automation/test_ui_batch1_actions.py`
  and siblings already keep one process alive across many `invoke_action()`
  calls, with a `conftest` autouse-killer opt-out and minimal UIA churn to avoid
  the COM `0x80040155` accumulation. This is the launch/reuse spine for the
  journey driver.
- **`tests/test_ui_leak_detection.py`** — already snapshots RSS / handles /
  threads via `psutil` and asserts bounded growth. This is the seed for I-2.
- **`testSaveAsPipe`** (gated on `PDFE_TEST_PIPES=1`, set by the launch env) —
  saves via `Document::saveAs()` without the native dialog; the spine for I-4.
- **PyMuPDF (`fitz`) re-extraction** — already used in the `3e91a7c4` E2E to
  confirm content landed; the read-back half of I-4.

**Missing (Phase 2 must build):**
1. A **checkpointed journey driver** — a long-lived-session harness that runs a
   diverse op batch, fires a checkpoint, asserts I-1…I-5, and repeats to an op
   budget. Nothing like it exists.
2. A **save → reopen → re-extract integrity oracle** (I-4) — the suite has round-
   trip tests that check annot/field **counts**, never a full content/structure
   diff after N *mixed* edits, and never asserts the file simply *reopens*.
3. **Sustained-editing** resource coverage (I-2 under editing, not open/close
   churn) and **tile/store-under-edit** coverage (I-5) — both absent.
4. The **committed CI-portable multi-page corpus** so the journey runs in CI.

---

## 6. Execution constraints for later phases (house rules)

- **Native stack only:** pywinauto (UIA backend) + pyautogui; **never Selenium**
  (Selenium is web-only and does not apply to this native Qt app). PyMuPDF/`fitz`
  and direct `Document`/gtest for read-back oracles.
- **Run Python UI tests via the Bash tool from the repo root** — the PowerShell
  tool runs in a desktop-less RDP session where UIA cannot see the window ("main
  window did not appear within 30s"). The app is `build/vs2022/bin/Debug/pdfe.exe`;
  build with the VS-bundled cmake.
- **Strict test-first, honest RED in commit history** — a single-session RED
  reproduction is committed and shown failing **before** the fix commit;
  co-committing the test with the fix is a karen-gate failure (enforced on this
  project).
- **Fixture safety** — any journey that saves must open a **throwaway copy**
  (under `tests/_runtime`), never a committed fixture or the user's original.
- **CI cost** — the self-hosted windows-validation lane is ~9 h; the CI journey
  uses a **bounded** op budget, with a longer nightly/extended variant.

---

## 7. Scope of this phase
This phase adds **only this document**. No production code, test code, or CI
configuration is created or modified. Phase 2 implements the journey harness +
the I-1…I-5 oracles + the committed corpus and runs the first discovery pass;
Phase 3 fixes what discovery finds (test-first); Phase 4 locks the harness into
CI and finalizes this methodology doc.

---

## 8. Outcomes (Phases 2–4, finalized)

*Added in Phase 4 — what actually happened when the plan above was executed.*

### Phase 2 — discovery: ZERO cumulative product defects
`tests/test_ui_journey_soak.py` (one long-lived session, full AC1 op vocab via
the deterministic test pipes) + the committed `tests/fixtures/journey_corpus.pdf`
(64 pages, 192 whole-word "the", nonzero-origin MediaBox, /ActualText spans,
blank+image pages) ran one discovery pass over the corpus **and** the real
51-page SpecimenA Green PDF (~90 ops each, ~48 armed edit→scroll rounds). Result:
**zero confirmed cumulative product defects** — no crash, corruption, lost
change, undo divergence, or unbounded resource growth. The render-lifetime race
carried from the `3e91a7c4` closure (**B-1**) did **not** reproduce. Two *test*
bugs were found and fixed (an I-2 leak oracle that false-fired on the one-time
warm-up ramp → reworked to a warm-up-invariant operating-window trend; and
Replace-All silently dropping keystrokes on RDP via `SendInput` → switched to the
UIA `ValuePattern`). See `tests/docs/journey-soak-phase2-findings.md`.

### Phase 3 — B-1 refuted; the real (latent) hazard fixed
B-1's render-lifetime framing was **refuted** at the code level: tile-render
workers own a self-contained `fz_display_list` and are save-safe (deterministic
`tests/test_document_save_file_lock.cpp`, 4 arms). The genuine residual was
different: a live `Page` held across a full-rewrite `Document::save()` locks the
source file on Windows → the save throws and silently loses the edit; and the
undo stack could dangle across such a save. Fixed under **Option B**: a
`Command` page-rebind contract (`pageIndex()`/`rebindPage()`), a
drain→write→rebind save orchestration (`src/core/SaveWithRebind.h`, for both
`save` and `saveAs`), all **18** Page*-holding undo command classes overriding
the contract (Annotation 3, Image 5, Form 6, Signature 1, Redaction 3), and
`MainWindow::onSaveFile`/`onSaveAsFile` wired through it. Covered by
`tests/test_command_page_rebind.cpp` (Document/command layer) and the native
`tests/test_ui_save_lock_rebind.py` (real onSaveFile in-place save). See
`tests/docs/journey-soak-phase3-findings.md` §6.

### Phase 4 — the CI lane
The journey soak is not a separate job: `tests/scripts/run_ui_suite.py` already
collects every `tests/test_ui_*.py`, so both `test_ui_journey_soak.py` and the
Phase-3 `test_ui_save_lock_rebind.py` run inside the existing **windows-validation**
UI E2E sweep against the committed corpus (the SpecimenA variant self-skips when
absent, so it is never required for CI green). The journey op budget is bounded
**explicitly** in `windows-validation.yml`: `JOURNEY_OP_BUDGET=120` (~30 min
wall-clock; P2 measured ~9.5 min / 40 ops), comfortably within the job's 720-min
timeout (> the ~9h self-hosted budget), with a matching 45-min per-file cap in
`run_ui_suite.py`'s `FILE_TIMEOUT_OVERRIDES_S`.

**Extended / nightly variant.** Rather than a separate scheduled workflow — a
standalone nightly full run would consume the single self-hosted Windows runner
for ~9–12 h every night, which is not justified for this lane — the budget is
made **env-tunable** via the repo/org variable `JOURNEY_OP_BUDGET` (read as
`${{ vars.JOURNEY_OP_BUDGET || '120' }}`). Raising that variable (or dispatching
the workflow with it set) runs a longer, deeper journey on demand without
editing the workflow. This is the deliberate, documented alternative to a
standing nightly schedule.
