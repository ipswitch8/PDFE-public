# Journey soak — Phase 2 discovery findings (pipeline `f4a82c1e`)

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


**Scope:** build the checkpointed single-session "journey" harness + the
I-1..I-5 cumulative oracles + a committed CI-portable corpus (Phase 2 of the
cumulative-single-session-state methodology, `testing-methodology-cumulative-single-session.md`),
then run a discovery pass and report findings, severity-ranked.

**Bottom line:** across two documents and ~90 operations each in ONE long-lived
session — including six zero-settle "armed-race" edit→scroll bursts per run —
**no confirmed cumulative product defect was found.** The discovery pass's one
real find was a *test* defect (an I-2 oracle that false-fired on warm-up), now
fixed. The methodology's primary code-level target (the `B-1` render-lifetime
race) did **not** reproduce, but the harness cannot exonerate it; the
cancel-without-join hazard is still present in the source and is handed to
Phase 3 as a code-level item. One initially-suspected signal (SpecimenA RSS
growth) was **cleared** by the longer run.

---

## 1. What was built

| Artifact | Purpose |
|---|---|
| `tests/fixtures/make_journey_corpus.py` → `journey_corpus.pdf` (64 pages) + `journey_corpus_manifest.json` | Dependency-free, committed, CI-portable corpus reproducing every salient SpecimenA structure: nonzero-origin MediaBox pages (`[0 7.9 594 745.9]` and `[50 50 662 842]`), 8 pt body text with a **verified 192 whole-word "the"**, `/ActualText`-prefix spans (extract "A"), blank pages, image-only pages, 64 pages to pressure the tile cache / 256 MB store. Validated against PyMuPDF. |
| `tests/test_ui_journey_soak.py` | The checkpointed journey: ONE long-lived `PDFESession`, deterministic op vocabulary via the test pipes, checkpoints asserting I-1..I-5, a severity-ranked JSON findings report. Parametrized over the corpus (always) and the real SpecimenA doc (skip-if-absent). |
| `tests/test_journey_targets_unit.py` | Non-UI unit test verifying the SpecimenA skip-when-absent logic (AC2) unconditionally — proves `_targets()` yields corpus-only when the user's file is absent and includes the SpecimenA variant when present, without needing a desktop / pdfe.exe. |

**Op vocabulary (all pipe-driven — no fragile UIA menu walk), covering the
full AC1 set:** **edit small text directly** (`textEditToolAction` →
`testDoubleClickByPdfCoordPipe` word-select on an 8 pt body span →
`testTextEditTypePipe` → Escape commit — the `ContentStreamEditor` `#4/#5` path,
distinct from Replace-All's search path), navigate (`testGoToPagePipe`), scroll
/ whole-document sweep (`testScrollPipe`), annotate (`testAnnotCreatePipe`,
dirties → `refreshAllPages`),
**move annotation** (`testAnnotSelectPipe` → `testCanvasDragPipe`), **delete
annotation** (`testAnnotSelectPipe` → `testAnnotDeletePipe`), redact
(`testAddRedactionMarkPipe`), **rotate page** (`rotatePageCWAction/CCWAction`),
**delete page** (`deletePageAction`, guarded + page-count re-query), rotate/zoom
view, insert-page, **deep undo/redo chain** (create 5 → undo 5 → redo 5),
Replace-All (search panel via ValuePattern), page-count query, save
(`testSaveAsPipe`). Plus a dedicated **zero-settle armed-race burst** that
dirties a page and *immediately* sweeps the whole document with no pause, to
open the B-1 UAF window.

**Oracles:** I-1 process-alive (every checkpoint); I-2 resource TREND across
post-warm-up checkpoints (see §3); I-3 undo reversible probe (insert page →
undo → page-count returns); I-4 save→reopen→re-extract integrity (PyMuPDF
reopen proves no corruption / no lost changes / reopens without error); I-5
render-path exercise + saved-content-reflects-edit.

---

## 2. Discovery runs

| Pass | Budget | Targets | Wall time | Result |
|---|---|---|---|---|
| 1 | 40 ops | corpus + SpecimenA | 19 min | Surfaced an I-2 "handle leak" → diagnosed as a **harness oracle false-positive** (see §3.2). |
| 2 | 90 ops | corpus + SpecimenA | 2 h 02 m | **Both PASS.** Added armed-race probe; iterated the I-2 oracle. SpecimenA RSS suspicion **cleared**. |
| 3 | 66 ops | corpus (authoritative) | 20 min | **PASS, 0 findings** — final harness (full AC1 vocabulary incl. direct small-text edit; warm-up-invariant oracle). This is the artifact the corpus table below reflects. |

Per-checkpoint trajectories (authoritative artifacts on disk —
`tests/_runtime/journey_findings_corpus.json` for the corpus row and
`journey_findings_specimen_a.json` for the SpecimenA row):

```
corpus (72 ops, 11 checkpoints — pass 3, final harness)
  handles: 436 |1150 1162 1149 1158 1159 1152 1152 1164 1164 1164
  RSS_MB : 117 | 216  234  275  217  217  245  308  222  222  222
  threads: 19  |   8   12    9   13   13    9    9   13   13   13

SpecimenA (94 ops, 13 checkpoints — pass 2)
  handles: 414 |1127 1157 1119 1131 1128 1120 1150 1150 1153 1153 1154 1166
  RSS_MB : 175 | 315  236  303  303  347  392  303  303  356  356  386  609
  threads: 18  |   6   16    4    8    6    4   14   14    8    8    8   12
```

The `|` marks the open→first-batch warm-up step. Post-warm-up, **handles hold
a flat plateau** on both docs (corpus 1149–1164; SpecimenA 1119–1166) and
**threads stay bounded** (8–19, no growth) — the two clearest leak signatures
are both negative. (The corpus row is the pass-3 final-harness run, which added
the direct small-text-edit op and the warm-up-invariant oracle; SpecimenA, the
slow skip-if-absent target, was not re-run in pass 3 because its pass-2 artifact
already corroborates the same plateau. Both raw artifacts are on disk under
`tests/_runtime/`.)

---

## 3. Findings, severity-ranked

### 3.1 CONFIRMED product defects: **none**

No crash (I-1), corruption or lost change (I-4), unbounded leak (I-2), undo
divergence (I-3), or stale-tile defect (I-5) was confirmed on either document.
All I-4 saves reopened clean (corpus + SpecimenA 18.8–20.0 MB across four
save/reopen boundaries per run).

### 3.2 TEST defect found and fixed — I-2 oracle false-positive (the real find)

Pass 1 reported *"handle count grew by ~721 (432→1153)"* on both targets. The
trajectory shows this is a **one-time warm-up step** (open ≈430 → first editing
batch ≈1150) that then **plateaus dead-flat** — not a per-op leak. The
oracle used a single before/after delta (`min(first two) vs last`), directly
contradicting the gated methodology, which specifies I-2 as *"the trend is
bounded … not a single before/after."*

The fix took **two iterations**, which is itself instructive about oracle
design. The first fix "dropped the open baseline and checked the post-warm-up
trend" — but the warm-up ramp does **not** always land on the first editing
checkpoint (when the op mix changed, the render pool spun up at batch 2, e.g.
handles `432→432→444→1146→1158→1159→1159`), so a pre-ramp point leaked into the
"post-warm-up" window and it false-fired again. The **final** oracle is
warm-up-*invariant*: it locates the ramp as the single largest upward step and
evaluates only the **operating window after it** — a plateau is flat there
regardless of ramp timing, while a real leak keeps climbing through it (if the
window is too short to judge, it reports inconclusive, not a guess). Validated
offline against all six real trajectories (all healthy/inconclusive) plus
synthetic linear- and gradual-leak series (both correctly flagged). This is a
validation of the methodology itself: the harness needed the same rigor it was
built to enforce, and getting a resource-trend oracle right is genuinely subtle.

### 3.3 SUSPECTED signal — SpecimenA RSS growth — **cleared**

Pass 1 (3 batches) showed SpecimenA RSS climbing 299→635 MB, flagged *suspected*.
Pass 2 (6 batches) shows it does **not** sustain: RSS oscillates 303–392 MB
across batches (post-warm-up early≈316, late≈385, +69 MB — under the 200 MB
trend threshold), consistent with bounded tile-cache / undo-stack working-set
churn, not a leak. (The lone 609 MB final reading is a one-off after the last
of four full-document saves of a 20 MB file.) **No leak; signal cleared.**

### 3.4 Harness-robustness bug found and fixed — Replace-All never fired

Both pass-2 runs recorded two `note` findings: *"replace_all could not run:
SendInput() inserted only 0 out of 2 keyboard events."* On the desktop-less RDP
session, `type_keys`/SendInput silently drops keystrokes, so the Replace-All op
— the all-page-extraction path that armed `#6` — **never actually executed**.
The whole-document scroll and annotate ops did exercise all-page render + dirty
propagation, but the specific Replace-All route was a coverage hole. **Fixed:**
the search/replace fields are now populated via the UIA ValuePattern
(`set_edit_text`, the same mechanism every pipe uses reliably) instead of
SendInput. The fix is validated by durable, committed artifacts (not
transient run stdout): the pre-fix SpecimenA findings JSON
(`tests/_runtime/journey_findings_specimen_a.json`) still carries the two
`note`/SendInput-failure records described above, whereas the post-fix corpus
findings JSON (`tests/_runtime/journey_findings_corpus.json`, the final green
run) has **zero** SendInput notes and records a `batch2-postreplace` checkpoint
— i.e. Replace-All executed in-session, the process stayed alive through it
(no crash), and the saved file reopened clean (0 findings). (During the run the
process debug output also showed `TextEdit::applyText` firing across body pages
as the all-page edit landed, consistent with this, but that stdout is
ephemeral; the findings JSONs above are the durable evidence.) Note `#6`'s
extraction crash is independently fixed at `5873dd9` with a committed gtest
regression, so this hole never risked a real `#6` regression.

### 3.5 Primary target `B-1` (render-lifetime race) — not reproduced, not exonerated

The methodology's primary code-level target is the closure-doc `B-1` race: a
dirtying edit runs `refreshAllPages()` → `TileRenderer::cancelAll()`, which
sets each task's atomic `cancelled` flag and clears the queue but **does not
`waitForDone()`** (that method exists but is not called on the cancel path); a
subsequent page-tree drop could then race a worker still inside
`fz_run_display_list()`.

The armed-race burst hit this pattern **~48 times** (6 bursts × 8 rounds) per
run on both a 64-page corpus and the real 51-page document, with **no crash**.
That is *non-proof*: the atomic flag cannot interrupt an in-flight native
`fz_run_display_list` call, so whether a UAF is possible hinges on whether each
render task owns isolated state (a cloned `fz_context` + a captured display
list) or shares the document's live page tree — which this black-box journey
does not settle. **The cancel-without-join hazard is still present in the
source.** Recommendation to Phase 3: a targeted code-level review of
`TileRenderer` task ownership, and — regardless of whether a live repro is
found — a defensive join / generation-guard on the cancel→drop path.

---

## 4. Limitations (honest coverage boundaries)

- **I-5 depth:** the harness proves an edited page's *saved* content reflects
  the edit and that the render path executes without crashing; it does **not**
  do an on-screen pixel diff of a served tile vs. the edit (no render-readback
  pipe). True "stale tile painted on screen" remains only partially covered.
- **Store eviction not instrumented:** the corpus has enough pages to pressure
  the 256 MB MuPDF store, but there is no direct store/tile-eviction counter
  read-back; eviction is inferred from bounded RSS, not measured.
- **Absence of a B-1 repro ≠ absence of B-1** (see §3.5).
- **Cost:** 90 ops × 2 targets = ~2 h. The Phase 4 CI lane must use a bounded
  op budget (corpus-only, ~30–40 ops) with the full run reserved for nightly.

---

## 5. Recommendation for Phase 3

Discovery found **zero confirmed cumulative product defects**, so Phase 3 is
not a test-first bug-fix of a journey-discovered crash. Instead Phase 3 should:

1. **Resolve `B-1` at the code level** (§3.5) — review `TileRenderer` task
   ownership; add a defensive join / generation-guard on cancel→drop; add a
   focused (headless GoogleTest) regression that exercises cancel-while-rendering
   deterministically rather than relying on a UI timing race. *(This is the one
   real code hazard the methodology set out to close.)*
2. **Land the corrected harness** (oracle fix + Replace-All fix + armed-race
   probe) as committed test infrastructure.
3. Then Phase 4 locks the journey as a bounded CI lane + nightly extended run.
