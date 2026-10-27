# /ToUnicode recovery — write-side scope decision

<!-- tracking-banner: exempt -- a decision record; GitHub Issues remains canonical for anything still open -->

**Status:** DECIDED — **(a) keep the mutation, and test it.**
**Finding:** GitHub #188, recorded in pipeline `p85-remaining-open`, phase 4.
**Owner:** PDFE maintainers.
**Decided:** 2026-09-15.
**Re-evaluate if:** any of the triggers in §5 fires.
**Machine guard:** `tests/test_tounicode_recovery_write_side.cpp`
(`ToUnicodeRecoveryWriteSide.*`).
**Code:** `src/core/ToUnicodeRecovery.cpp` (`recoverMissingToUnicode`), called
from `src/core/Page.cpp`'s constructor.

---

## 1. What #188 asked

`recoverMissingToUnicode()` installs a synthesized `/ToUnicode` stream into the
**in-memory PDF object graph**:

```cpp
pdf_dict_put_drop(ctx, fontDict, PDF_NAME(ToUnicode), streamRef);
```

It runs from the `core::Page` constructor, so merely **opening** a qualifying
document and later saving it writes font entries the input file did not have.
#188 asked for a deliberate choice between:

* **(a)** keep the mutation and assert the round-trip explicitly;
* **(b)** make it extraction-only — synthesize into a side table the extractor
  consults, leaving the document graph untouched;
* **(c)** keep it, but revert the object before save.

## 2. The trade-off being decided

This is **byte-fidelity versus a security-relevant false negative**, not
"correctness versus convenience".

**The case for (b).** PDFE is an editor. A user who edits page 7 and saves
expects the diff to be page 7. Silently enriching an unrelated font dictionary
breaks that expectation, it is invisible, and byte-fidelity is exactly the
property that matters most on the documents where it matters at all (signed,
archival, regulated).

**The case against (b), which is what decided it.** The recovery exists to close
the A3-48 class: Adobe Expert glyph names resolve through the AGL into the
Corporate-Use PUA, so the text "extracts" as `U+F773 U+F765 …` and **matches
nothing** — including nothing a redaction search looks for. An extraction-only
side table has to be consulted at every point PDFE turns glyphs into Unicode.
Counted in the tree at the time of the decision, `fz_new_stext_page_from_page`
has **11 call sites across 6 subsystems**:

| Subsystem | Sites |
|---|---|
| `src/core/Page.cpp` | 4 |
| `src/core/TextExtractor.cpp` | 2 |
| `src/compliance/PageFurnitureDetector.cpp` | 2 |
| `src/compliance/AutoTagger.cpp` | 1 |
| `src/redaction/RedactionScrubber.cpp` | 1 |
| `src/redaction/RedactionVerifier.cpp` | 1 |

Two of those are the redaction **scrubber** and the redaction **verifier**. A
site that forgets to consult the side table does not fail loudly; it silently
reproduces the exact false negative #23 was filed to remove, in the one place
where a false negative means undeleted content shipped as deleted. "Eleven
places that must each remember" is a defect generator, and the generated defect
is invisible.

There is a second, harder problem with (b). `fz_stext_char` carries the resolved
codepoint and the font — **not the source byte code**. A side table can
therefore only be keyed on the codepoint MuPDF produced unaided, which is a
lossy reverse map: two codes in one font that collide on the same unaided answer
(trivially available, since the unaided answer falls back to the raw code)
cannot be distinguished. So (b) is not merely more plumbing, it is *less
capable* than the current mechanism at the API boundary MuPDF offers.

**(c)** was rejected on the grounds #188 itself gives: an undo that has to run
correctly on every error path, including the ones that longjmp, is fragile in
the precise way that produces a corrupt save rather than a missing improvement.

## 3. What was decided, and the bounds that make it defensible

PDFE **enriches** a font it can demonstrably improve, and this is a stated
product behaviour rather than a side effect. The enrichment is bounded, and each
bound is now asserted by a test rather than reasoned about:

1. **It never contradicts the file.** A font that carries an authored
   `/ToUnicode` is left alone — byte-identical through open-and-save
   (`AuthoredToUnicodeIsNeverReplacedByTheRecovery`).
2. **It never invents.** A font whose glyph names carry no Unicode information
   (`/gx1`…`/gx6`) gains nothing; only the font PDFE can actually improve is
   touched (`OnlyTheImprovableFontGainsAToUnicodeOnSave`).
3. **It never causes a save the user did not ask for.** Opening does not set the
   modified flag (`OpeningAnExpertEncodedDocumentDoesNotMarkItModified`), so the
   enrichment reaches disk only inside a save the user initiated for their own
   reasons.
4. **The enrichment is real, not cosmetic.** The saved file extracts correctly
   in a reader that has never heard of PDFE — asserted by re-extracting the
   saved bytes through **raw MuPDF**, bypassing `core::Page` entirely
   (`SavedFileCarriesTheRecoveryForAReaderThatIsNotPDFE`), with the unsaved
   original as the control that still yields PUA.

## 4. Incremental save and signatures

Bound 3 is what answers #188's incremental-save / signed-rewrite question: since
opening does not mark the document modified, PDFE initiates no save of its own,
and the enrichment can only ride along with a save the user asked for. A save the
user asked for is already a document change, and the signed-save guard added for
GitHub #19 already governs it — the added font entry does not create a new
category of change for that guard to miss. What is NOT claimed here: that the
added object is invisible to an incremental-update diff. It is not, by design.

## 5. Re-evaluate if

* A byte-fidelity or "preserve input exactly" mode is added to PDFE. That mode
  would need the recovery suppressed, and the suppression point is the
  `core::Page` constructor call, not this file.
* MuPDF exposes the source character code on `fz_stext_char`, which removes the
  lossy-reverse-map objection to (b) and makes an extraction-only table
  genuinely equivalent.
* The extraction sites in §2 are consolidated behind a single PDFE chokepoint,
  which removes the "eleven places that must remember" objection.

## 6. Correction to #188's premise

#188 states that the round-trip question is untestable because "no such document
exists in the corpus". That is **not true** as of this decision:
`tests/fixtures/f18_no_tounicode.pdf` (from `gen_f18_backlog_corpus.py`) is an
Adobe Expert-set document with no `/ToUnicode` on either Type 3 font, and it is
already pinned in the CMake fixture floor. No new fixture was required; the
tests in §3 are built on it and on `tests/fixtures/text_soft_hyphen.pdf`, whose
font carries an authored `/ToUnicode` and therefore supplies bound 1's control.
