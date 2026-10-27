# Fix B1 — rotate × text-edit data-loss (clean-inverse under a changed /Rotate)

**Defect class:** DATA LOSS. Discovered by the coverage-expansion generative
harness (`tests/test_generative_invariants.cpp`, C-05). Product-code fix.

## Deterministic repro (the committed lock)

`tests/test_generative_invariants.cpp::B1_RotateThenEditTextUndoRestoresText`
(shrunk from seed 1001):

1. Open `test_3page.pdf`; take the original whitespace-normalized text digest.
2. `RotatePageCommand(doc, 0, 90)` — rotate page 0 by 90°.
3. `EditTextCommand` span index 2 on page 0, text → `EDIT2`.
4. `while (undo.canUndo()) undo.undo()` — undo the edit, then the rotate.
5. Re-digest → page text NOT restored.

Before the fix the digest for page 1 collapsed from
`Page1-TitlePageThisisatestPDFforPDFEunittests.Generatedby...` (470 chars total)
to **`PeEDIT2Ts.`** (403 chars total). Almost every glyph on the edited page was
permanently destroyed, and `EDIT2` survived through the undo — i.e. the undo was
not even a partial inverse.

## Precise root cause

The page content stream is authored in **unrotated user space**; a page
`/Rotate` only rotates the rendered output — it is not a content-stream `cm`.
`ContentStreamEditor`'s glyph→span match in `textFilterCallback` (and the
injection positioning in `repositionFromSpanBbox`) rely on **line-oriented
heuristics**:

- a **vertical-centre Y-band** test (a glyph belongs to the clicked span iff its
  Y centre lies in the span box's Y range), and
- a **right-edge extension** of the match region out to the page's right margin
  (to sweep up overflow glyphs from prior longer edits).

Both are sound only while text lines run **horizontally**. The setup code fed the
matcher `state.pageCtm = pdf_page_transform(...)` and
`pageHeight/pageWidth = fz_bound_page(...)`, **both of which fold in the page
/Rotate**, and the text extractor likewise reports span boxes in **rotated
device space**. Under a 90/270° rotation lines run vertically, so span 2's tall
Y-band × everything-to-the-right overlaps glyphs from many unrelated lines. The
sanitize filter (`text_filter` returns 1 = remove) deletes all of them; the
injection re-adds only span 2's `EDIT2`. The other glyphs are gone from the
content stream, so `undo`'s `applyText(oldText)` — which re-runs the same broken
match on the already-mangled stream — has nothing to restore. Net: page text is
permanently lost; do-all/undo-all is not a clean inverse.

This is purely a heuristic-vs-rotation mismatch. Coordinate spaces were
internally consistent (both glyph and span boxes in rotated device space), so the
match *ran* — it just matched the wrong glyphs once lines were vertical.

## The product fix (minimal, correct)

`src/textedit/ContentStreamEditor.cpp` — new helper `setupSpanMatchSpace()`
(added after `bboxOverlaps`), called from both edit paths in place of the old
`fz_bound_page` + `pdf_page_transform` setup:

- `applyEdits()` (was ~line 1565–1573)
- `applyEditsWithSubstitution()` (was ~line 1785–1793)

`setupSpanMatchSpace()`:

1. `pdf_page_transform()` → `T` (user→device, rotation folded in) and the page
   box `mb`.
2. Builds the **unrotated** device transform `U = [1, 0, 0, -1, -mb.x0, mb.y1]`
   (y-flip + origin translate, no rotation) — which maps the page box to
   `[0,0,w,h]` exactly like `fz_bound_page` on an unrotated page.
3. If `T == U` within 1e-3 (the page is unrotated) → **legacy path, byte-identical
   to the pre-fix code**: `pageCtm = T`, dims from `fz_bound_page`. This protects
   the entire existing unrotated regression surface from any numerical change.
4. Otherwise (rotated page): map every span match box from rotated-device to
   unrotated-device via `C = T⁻¹ · U`, set `pageCtm = U`, and set
   `pageHeight/pageWidth` from `mb`. Now the matcher and the injection both work
   in unrotated device space, where text lines are horizontal again.

### Why this is the clean-inverse fix

- The match and injection are performed in the coordinate space the content
  stream actually lives in (unrotated). The Y-band + right-extension heuristics
  are sound there, so **exactly** span 2's glyphs are removed and `EDIT2` is
  injected at span 2's baseline (the `/Rotate` still renders it rotated with the
  rest of the page — the edit lands correctly).
- Because only span 2 is touched, `undo`'s `applyText(oldText)` re-matches the
  same single anchor and re-emits the original glyphs — a true inverse. No
  sibling text is ever removed, so nothing is lost to restore.
- For unrotated pages the helper is a no-op (`T == U`), so the ~569-test
  text-edit / content-stream / CTM / page-op / undo regression surface is
  unchanged.

Diff: `git diff --stat -- src/` → `ContentStreamEditor.cpp | 73 insertions(+), 18 deletions(-)` (1 file).

## Lock flipped to enabled + passing

`DISABLED_B1_RotateThenEditTextUndoRestoresText` →
`B1_RotateThenEditTextUndoRestoresText` (now runs in the normal suite). The exact
repro and both digest assertions are preserved unweakened.

### Added positive "edit actually landed" check

Before the undo, the test re-extracts the (still-rotated) page 0 and asserts:

- `EDIT2` **is present** — the fix is not a no-op; the edit lands under rotation.
- `unittests` (sibling text outside the edited span) **survives** — the rotated
  match no longer sweeps up and drops unrelated glyphs (the exact B1 symptom that
  collapsed the page to `PeEDIT2Ts.`).

## Verification

- `GenerativeInvariants.*` — 4/4 PASS (B1 lock green, including the positive
  edit-landed check; content + pagestruct campaigns 50/50 each; determinism).
- Touched surface (ContentStream / ContentStreamEditor / ContentStreamSubstitution
  / CtmReset / TextEdit / PageCommand / Rotate / Undo / Redo / Ctm) — **569/569
  PASS**.
- Full suite (`--gtest_filter=-PerfStressTest.TenThousandAnnotationsOnePage`),
  run as 16 sequential gtest shards and aggregated from JUnit XML (GAP-A15):
  **4706 tests, 0 failures, 0 errors** (B1 test present and green in the
  aggregate).

### Note on running the full suite on this box

The suite must be run **strictly sequentially** — two `pdfe_tests.exe` processes
running at once cause CRT heap corruption that faults at a non-deterministic,
unrelated heap-intensive test (typically an `ExternalCorpus SaveRoundTrip`
canary or a GUI/Print/Canvas test). This is a pre-existing environmental
condition (see project memory: `crt_modal_dialogs_hang_ci`,
`ci_reactivation_and_teardown_av`), NOT caused by this change: every suite that
faulted mid-run (AutosaveCaptureAudit, PrintTest, ImageTextEditDisplay,
FuzzEncryption/FuzzWriteSurfaces, FidelityR3_24, FidelityR3_08, ExternalCorpus
SaveRoundTrip) passes cleanly in isolation, and all text-edit suites — the only
ones this change touches — pass together (569/569). `FidelityR3_24_EditSaveScale`
is a timing-sensitive perf test that can flake under concurrent load; it passes
3/3 in isolation and in the sequential shard aggregate. Also note the XML output
path must be a Windows path (`C:/…`), not an msys path (`/c/…`), or the native
exe silently writes no XML.
</content>
</invoke>
