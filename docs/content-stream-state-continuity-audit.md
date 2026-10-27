# Content-Stream State-Continuity Audit

**Date:** 2026-07-19
**Origin:** Pipeline a3f8e21d Phase 1 fixed a "backwards text" (mirror) bug in the
text editor — editing text on a page with an ambient `-100 Tz` rendered the
replacement mirrored (commit d8805a1). That was a *state-continuity gap*. This
audit asked "where else?" and swept every page-content injector in the codebase.

**Scope:** audit only — no code changed by this document.

---

## The bug class

PDF text/graphics-state operators — `Tz` (horizontal scale), `Tc` (char spacing),
`Tw` (word spacing), `Ts` (text rise), `Tr` (render mode), `Tf` (font),
`cm`/`Tm` (matrices), `rg`/`g`/`k` (fill color + colorspace), clip, line width,
dash — are **graphics state**. They persist across `BT`/`ET` and across every
operator until explicitly changed or restored by `q`/`Q`. (`BT` resets only the
text matrix, *not* `Tz`/`Tc`/`Tw`/`Ts`/`Tr`/`Tf`/color.)

MuPDF's `pdf_filter_page_contents` passes untouched operators through verbatim,
so **ambient state set earlier in the page always reaches an injection point.**
A subsystem that appends/injects operators into an existing page content stream
can therefore fail two ways:

- **Inbound leak** — the injected content inherits ambient state a prior operator
  left set (`-100 Tz` mirrors text; `Tr 3` makes it invisible; a non-black fill
  paints it white-on-white; a flipped/scaled `cm` mis-places it). This is the
  mirror bug.
- **Outbound corruption** — the injection changes state **without** a `q…Q`
  wrapper, so operators it emits persist forward and corrupt content that follows
  it at the same q-depth.

**Self-contained appearance-stream XObjects** (own `/BBox` + `/Matrix`, isolated
graphics state) are immune to *inbound* leak — that's why the annotation/redaction
paths are safe. Direct `/Contents` injection is not.

---

## Inventory (ranked)

| # | Site | Class | Inbound | Outbound | Trigger |
|---|---|---|---|---|---|
| 1 | `textedit/ContentStreamEditor` (shipped) | **BUG (inbound) — FIXED** | `Tc`/`Tw`/`Ts` leaked into the injected text — **FIXED** (injection now emits `100 Tz 0 Tc 0 Tw 0 Ts 0 Tr`) | **outbound = REFUTED false-positive** (see note below) | inbound: ambient `Tc`/`Tw`/`Ts` distorted the edited replacement |
| 2 | `reflow/AlignmentWriter` | **BUG** | `Tz`, `Tr`, **no `rg` at all**, colorspace; AT-RISK `Ts`/`Tc`/`cm` | safe (`q…Q` + appended last) | ambient non-black fill (very common at end-of-page) → invisible/tinted |
| 3 | `headerfooter/HeaderFooterEngine` | **BUG** | `Tz`/`Tc`/`Tw`/`Ts`/`Tr`/color/CTM — sets only `Tf` | safe | ambient `Tr 3` (invisible OCR layer) → header/footer invisible |
| 4 | `batch/BatchEngine` watermark | AT-RISK | CTM/`Tz`/`Ts`/`Tr` (does set color/font/`Tm`) | safe | flipped/scaled ambient CTM → watermark mirrored/off-page |
| 5 | `image` insert/resize/rotate/crop | AT-RISK ×4 | ambient **CTM/clip** (absolute `cm` assumes identity) | safe | top-level `cm` (N-up/imposed) → image half-size/shifted/clipped; **insert worst** (no sanitize) |
| 6 | `tre/PdfContentWriter::writeTextBlock` | AT-RISK | `Tz`/`Tc`/`Tw`/`Ts`/`Tr`/CTM (sets `Tf`/`rg`/`Tm`) | safe | mixed vector+scan page (image-only assumption usually holds) |
| — | `redaction/*` | **SAFE** | isolated AP XObject + `pdf_redact_page` (removes content) | safe | — |
| — | `annotations/*`, `forms/*` | **SAFE** | isolated AP XObjects / `pdf_update_annot` / `pdf_bake_document` | safe | — |

### Per-site evidence

1. **ContentStreamEditor** — injection callbacks emit `op_Tz(100)`
   (`ContentStreamEditor.cpp:737`,`:957`), `op_Tr` (`:740`,`:961`), `op_rg`
   (`:747`,`:968`), `op_Tf`, `op_Tm`. `FilterState` (`:246`) tracks none of
   `Tc`/`Tw`/`Ts`. The Phase-1 fix closed inbound `Tz` only.

   **RESOLUTION (pipeline e5b9c231 Phase 2, verified empirically):**
   - **Inbound leak = REAL, now FIXED.** In the structural pattern that produced
     the original `-100 Tz` mirror bug (ambient state set inside the `BT`, edited
     span not last → the injection is emitted *inline* with the ambient state
     still live in its saved gstate), the injected replacement inherited ambient
     `Tc`/`Tw`/`Ts`. Reproduced: a `50 Ts` ambient shifted the edited text ~50
     device units (`tests/fixtures/state_leak_ts.pdf`). Fix: both injection
     callbacks now emit `op_Tc(0)/op_Tw(0)/op_Ts(0)` alongside the existing
     `op_Tz(100)`. Regression `tests/test_content_stream_state_leak.cpp`
     (`EditedSpanNotShiftedByAmbientTextRise`).
   - **Outbound leak = REFUTED false-positive.** The static reading ("no `op_q`/
     `op_Q` in the file → emitted state persists past `ET`") did not account for
     MuPDF's `pdf_filter_page_contents`, which **always wraps the
     `after_text_object` injection in its own `q…Q`** and **re-materialises the
     active text state at the start of every following text object** (it re-emits
     e.g. `-100 Tz` into the next `BT`). Verified across multiple probe fixtures:
     a later, independent text object that inherits an ambient `-100 Tz` is NOT
     mirrored after editing an earlier span. Committed proof:
     `tests/fixtures/outbound_neighbor.pdf` +
     `test_content_stream_state_leak.cpp`
     (`OutboundLeakNeutralizedByFilterForLaterTextObject`) — a guard that would
     fire if a future MuPDF change ever regressed the wrapping. No product `q…Q`
     wrapping change is therefore needed for this injector.
2. **AlignmentWriter** — `emitFragment` (`AlignmentWriter.cpp:71-130`) writes
   `q / BT / Tf / Tw / Tm / Tj / ET / Q`; injected via `AlignmentInjector`
   appending to `/Contents`. No `rg`, no `Tz/Tc/Ts/Tr` reset.
3. **HeaderFooterEngine** — `buildOverlayStream` (`:226-260`) writes
   `q /F1 <sz> Tf … BT <x> <y> Td (text) Tj ET … Q`; appended to `/Contents`
   (`:344-369`). Only `Tf` set. Secondary: `/F1` resource-name collision
   (`:277`).
4. **BatchEngine watermark** — `applyWatermarkImpl` (`:187-341`), content
   `q /GS0 gs … cm/Tm … Q` (`:259`,`:273-279`) appended to `/Contents`
   (`:317-331`). Text sets `0.5 g`/`Tf`/`Tm` but no `Tz/Tc/Tw/Ts/Tr` reset;
   also ignores MediaBox origin (`:243-246`).
5. **image** — insert `ImageManager.cpp:331-337` (no sanitize pass, most
   exposed); resize `ImageContentEditor.cpp:193-196`; rotate `:248-266`;
   crop `:312-326`. All emit absolute `w 0 0 h x y cm` inside `q…Q`, assuming
   identity ambient CTM.
6. **TRE** — `PdfContentWriter::writeTextBlock` (`:238-388`) emits
   `q BT /res sz Tf r g b rg 1 0 0 1 x y Tm <glyphs> Tj ET Q`; sets `Tf`/`rg`/`Tm`
   only. Mitigated because TRE targets image-only pages (`q cm /Im Do Q`, no
   ambient text state).

---

## Root cause (architectural)

There is **no shared "reset to a known basis" helper**. Each injector hand-rolls
its own partial state setup, and `core/Document.cpp` provides only save-routing
(`markRequiresFullRewrite`) and cache invalidation (`createDisplayList`) — no
state-normalization layer. `pdf_filter_page_contents` guarantees *fidelity for
untouched operators*, nothing about the state injected content sees. The `-100 Tz`
fix was a point-patch in one callback pair, not a framework guarantee, so every
injector is independently exposed.

**Systemic remediation:** one shared preamble emitted right after each injection's
`q`/`BT`:
- `1 0 0 1 0 0 cm` (neutralize ambient CTM) — for page-content fragments
- `0 0 0 rg` (or the captured color) + explicit colorspace
- `100 Tz  0 Tc  0 Tw  0 Ts  0 Tr` (full text-state basis)
- and **wrap injected page-content fragments in `q…Q`** (ContentStreamEditor
  currently omits this).

---

## Shipped-code exposure

Finding #1's **outbound** half is a latent regression in Phase 1 (commit d8805a1).
`op_Tz(100)`/`op_rg` with no `q…Q` wrapper can leak into later same-q-depth
content. The neg-Tz fixture (`tests/fixtures/neg_tz_grey_text.pdf`) has an
unedited second span, but `tests/test_content_stream_neg_tz.cpp` never asserts
that span survives an edit — so this is untested and directly verifiable.

---

## Out-of-class findings (surfaced during the sweep)

- **RedactionVerifier** (`RedactionVerifier.cpp:120`): leaked-text check uses
  full-containment instead of intersection → boundary-straddling residual text
  passes verification. **Security false-negative.**
- **HeaderFooter `/F1`** collision (`HeaderFooterEngine.cpp:277`): reuses `/F1`
  if present → garbage glyphs when the page's `/F1` is a CID/Type0 font.
- **TRE glyph fallback** (`PdfContentWriter.cpp:351-353`,`:367-369`): writes
  Unicode codepoints as hex *glyph indices* → wrong glyphs.
- **Watermark + HeaderFooter**: ignore non-zero MediaBox origin → mispositioned
  on offset-MediaBox pages.

---

## Why none of this was caught by testing

The gap is not "we forgot to test the injectors" — they are heavily tested. It is
that the tests could not *structurally* expose this class:

1. **Fixtures carry only benign ambient state.** Every injection fixture is a
   well-formed synthetic PDF whose content returns to identity/default state (no
   `-100 Tz`, no ambient `Tr 3`, no top-level `cm`, black fill, identity CTM). The
   bug only manifests when ambient state is *non-default at the injection point* —
   i.e. the tests exercise the injectors under exactly the conditions where the
   bug cannot fire. `neg_tz_grey_text.pdf` was the **first** adversarial-ambient
   fixture in the suite, and it was created reactively, only after a customer hit
   the mirror.

2. **Oracles assert the edited element, never its neighbors.** Tests check "did
   the replacement appear / right color / right position" by reading back the one
   glyph/span edited. The *outbound* failure mode corrupts *other* content, which
   no oracle inspects — including the neg-Tz fixture's own second span.

3. **Text-extraction round-trips are blind to the failure modes.** Many tests do
   save→reopen→`extractPlainText` and check the text is present. `stext`
   extraction recovers the logical text regardless of whether it rendered
   mirrored, invisible, white-on-white, or off-page. The mirror needed a glyph
   **quad** oracle; a text-extraction oracle passes on all of these defects.

4. **No cross-subsystem "hostile ambient" corpus.** Each injector was tested in
   isolation with its own benign fixtures. The bug class is the *product*
   (arbitrary ambient state) × (injector); nothing ran a shared corpus of hostile
   pages — mirrored coordinate systems, invisible-text OCR layers, imposed/N-up
   CTMs, non-RGB colorspaces, dangling `q` — through *every* injector.

5. **Emitter-level unit tests validate output in a vacuum.** A test verifying
   "AlignmentWriter emits `q … BT … Tj … ET Q`" confirms the output is correct in
   isolation. The defect is that the output is *insufficient when concatenated
   after hostile content* — a property only a compose-and-render test catches, not
   an emitter-output assertion.

6. **The immune subsystems set a false expectation.** Annotations/redaction use
   isolated appearance XObjects and are genuinely robust to ambient state. Nothing
   compared the two designs under identical hostile input, so the inconsistency
   (text/header/watermark/image inject directly into `/Contents` and get no
   isolation) was never surfaced.

7. **Triggering PDFs are underrepresented in the corpus.** The flaw-corpus and
   journey PDFs are mostly "normal" documents. The triggers come from specific
   production toolchains — label printers (`-100 Tz`), scanners/OCR (`Tr 3` left
   set), imposition/N-up software (global `cm`) — that weren't represented. The
   customer RTV PDF was the first such artifact to reach the editor.

8. **TDD-from-a-bug-report scopes the test to the report, not the class.**
   Phase 1 was correctly test-first, but the fixture was reverse-engineered to
   reproduce the two *reported* symptoms (color + mirror) on the *specific* page.
   Red-green-refactor fixes the symptom; it does not include the step "what other
   ambient operators are in the same category?" That generalization — the whole
   point of this audit — is exactly what the test-driven loop omitted.

### What would have caught it (and would catch the rest)

A single **adversarial-ambient-state test corpus** — a handful of fixtures that
each set one hostile operator at end-of-stream (`-100 Tz`, `2 Tc`, `5 Ts`,
`3 Tr`, `1 1 1 rg`, `0.5 0 0 0.5 0 0 cm`, a dangling `q`, a `k` colorspace) —
run through **every** injector, with **rendered-pixel or glyph-quad oracles**
(not text-extraction), asserting **both** the injected element *and* a
pre-existing neighbor. That is the test layer the suite never had; building it is
the recommended follow-up, ahead of or alongside the shared-preamble remediation.
