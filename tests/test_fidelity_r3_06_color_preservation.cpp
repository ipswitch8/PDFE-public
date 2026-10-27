// -*- coding: utf-8 -*-
//
// R3-06 — Colour / CMYK / spot-colour / overprint preservation on edit+save.
//
// The gap (docs/audit/round-3/register.md R3-06): CMYK and transparency appear
// only in image-data and malicious-input tests. No test opens a CMYK /
// spot-colour / overprint document, performs a text edit, saves, and asserts
// that the colour operators and colourspace RESOURCES are preserved — editors
// routinely flatten CMYK->RGB or drop spot colours on save, a silent,
// print-critical regression with zero coverage.
//
// Fixture: tests/fixtures/adv_spot_overprint.pdf (gen_spot_overprint_fixture.py)
//   - span "EDITME" filled DeviceCMYK cyan via `1 0 0 0 k`, drawn under an
//     overprint ExtGState `/GS1 gs` (/OP true /op true /OPM 1)
//   - span "SPOTME" filled via a Separation colourspace `/CS1 cs` + `1 scn`
//     (CS1 = [/Separation /PANTONE /DeviceCMYK <tint->cyan>])
//
// The oracle EDITS the EDITME span (which forces a content-stream rewrite via
// pdf_filter_page_contents) and then asserts, on the SAVED-AND-REOPENED file:
//   (A) the Separation /ColorSpace resource still exists  (spot-colour drop guard)
//   (B) the unedited spot fill operator `scn` survives     (spot content guard)
//   (C) the overprint ExtGState resource survives          (overprint guard)
//   (D) neither the edited nor the spot span rendered to black
//       (no silent colour LOSS through the edit)            (colour-loss guard)
//
// Scope note (honest, not a SKIP): exact per-span *colourspace identity*
// round-trip on the edited glyphs (source `k` re-emitted as `k` rather than the
// visually-equivalent `rg`) is a deeper enhancement in ContentStreamEditor's
// filter (the MuPDF sanitize text_filter callback exposes trm/ctm/bbox but NOT
// the graphics-state fill colourspace, so preserving it needs a colour-tracking
// chain processor in a subsystem with documented mirror/state fragility). The
// assertions below pin the print-critical document-level guarantees — resource
// + unedited-operator + spot survival and no colour loss — which are what a
// CMYK/spot document owner actually loses when an editor mishandles colour.

#include <gtest/gtest.h>

// GetCurrentProcessId, for the per-process temp directory in SetUp.
#include <windows.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"

#include "support/PdfContentOracle.h"
#include "support/VisualOracle.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;
using namespace pdfe::textedit;
using pdfe::testsupport::decompressedPageContent;
using pdfe::testsupport::lineContaining;
using pdfe::testsupport::Glyph;

namespace {

// True iff the page's /Resources/ColorSpace dict contains at least one entry
// whose value is an array beginning with the /Separation (or /DeviceN) name.
// Reads the SAVED file through an independent fitz/pdf context.
bool savedFileHasSeparationColorspace(const std::filesystem::path& pdf) {
    bool found = false;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return false;
    fz_register_document_handlers(ctx);
    fz_document* fdoc = nullptr;
    fz_try(ctx) {
        fdoc = fz_open_document(ctx, pdf.string().c_str());
        pdf_document* doc = pdf_document_from_fz_document(ctx, fdoc);
        pdf_page* page = pdf_load_page(ctx, doc, 0);
        pdf_obj* res = pdf_dict_get(ctx, page->obj, PDF_NAME(Resources));
        pdf_obj* csdict = pdf_dict_get(ctx, res, PDF_NAME(ColorSpace));
        int n = pdf_dict_len(ctx, csdict);
        for (int i = 0; i < n && !found; ++i) {
            pdf_obj* val = pdf_dict_get_val(ctx, csdict, i);
            if (pdf_is_array(ctx, val) && pdf_array_len(ctx, val) > 0) {
                pdf_obj* kind = pdf_array_get(ctx, val, 0);
                if (pdf_name_eq(ctx, kind, PDF_NAME(Separation)) ||
                    pdf_name_eq(ctx, kind, PDF_NAME(DeviceN))) {
                    found = true;
                }
            }
        }
    }
    fz_always(ctx) { if (fdoc) fz_drop_document(ctx, fdoc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return found;
}

// True iff the page's /Resources/ExtGState dict has any entry carrying /OP or
// /op (overprint) set to a true boolean.
bool savedFileHasOverprintExtGState(const std::filesystem::path& pdf) {
    bool found = false;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return false;
    fz_register_document_handlers(ctx);
    fz_document* fdoc = nullptr;
    fz_try(ctx) {
        fdoc = fz_open_document(ctx, pdf.string().c_str());
        pdf_document* doc = pdf_document_from_fz_document(ctx, fdoc);
        pdf_page* page = pdf_load_page(ctx, doc, 0);
        pdf_obj* res = pdf_dict_get(ctx, page->obj, PDF_NAME(Resources));
        pdf_obj* egs = pdf_dict_get(ctx, res, PDF_NAME(ExtGState));
        int n = pdf_dict_len(ctx, egs);
        for (int i = 0; i < n && !found; ++i) {
            pdf_obj* gs = pdf_dict_get_val(ctx, egs, i);
            pdf_obj* op = pdf_dict_get(ctx, gs, PDF_NAME(OP));
            pdf_obj* op2 = pdf_dict_get(ctx, gs, PDF_NAME(op));
            if (pdf_to_bool(ctx, op) || pdf_to_bool(ctx, op2)) found = true;
        }
    }
    fz_always(ctx) { if (fdoc) fz_drop_document(ctx, fdoc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return found;
}

// A glyph run is "not black" iff at least one glyph has a non-black sRGB argb.
bool runIsNotBlack(const std::vector<Glyph>& g) {
    for (const auto& gl : g) if ((gl.argb & 0xFFFFFFu) != 0x000000u) return true;
    return false;
}

class FidelityR3_06_ColorPreservation : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path dir = TEST_FIXTURES_DIR;
        fixture = dir / "adv_spot_overprint.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "adv_spot_overprint.pdf fixture not available";
        }
        // Unique per-test output path: a shared name lets a prior test's
        // memory-mapped reopen (lineContaining / decompressedPageContent) still
        // hold the file when the next test's atomic-replace save runs, yielding
        // a Windows "Access is denied" on replace.
        const std::string tn =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        // PER-PROCESS DIRECTORY, not just a per-test NAME (phase-59). The
        // per-test name above fixed the IN-PROCESS collision; it does not
        // isolate this run from everything else in the shared %TEMP% root,
        // which a sharded suite, a rerun pass and an on-access scanner all
        // touch at once. Measured 2026-08-13: a 16-shard run failed this case
        // with "Access is denied" on the atomic replace, with sibling temp
        // files from the same window left undeleted because their TearDown
        // remove() failed as well. The save was RIGHT to refuse; the test was
        // wrong to write somewhere it does not own.
        outDir_ = std::filesystem::temp_directory_path()
                / ("pdfe_r3_06_p"
                   + std::to_string(static_cast<unsigned long>(
                         ::GetCurrentProcessId())));
        std::filesystem::create_directories(outDir_);
        out = outDir_ / (tn + ".pdf");
        std::error_code ec;
        std::filesystem::remove(out, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(out, ec);
        // Best-effort: removes the per-process directory only once the LAST
        // case in this process has cleaned up, because remove() on a
        // non-empty directory simply fails. No case may assume it ran last.
        std::filesystem::remove(outDir_, ec);
    }

    // Edit the span containing `needle`, replacing it with `replacement`, then
    // save to `out`. Returns false if the span could not be located.
    bool editSpanAndSave(const std::u32string& needle,
                         const std::u32string& replacement) {
        auto document = Document::open(context, fixture);
        if (!document) return false;
        auto page = document->page(0);
        if (!page) return false;

        auto blocks = page->extractText();
        TextEditModel model(blocks);
        std::vector<TextSpan> spans;
        for (int i = 0; i < model.spanCount(); ++i)
            spans.push_back(model.span(i));

        int targetIdx = -1;
        for (int i = 0; i < static_cast<int>(spans.size()); ++i) {
            if (spans[i].text.find(needle) != std::u32string::npos) {
                targetIdx = i;
                break;
            }
        }
        if (targetIdx < 0) return false;

        ContentStreamEditor editor(context.get());
        std::vector<std::pair<int, std::u32string>> mods;
        mods.emplace_back(targetIdx, replacement);
        editor.applyEdits(page.get(), document.get(), spans, mods);

        page.reset();
        document->save(out);
        document.reset();
        return true;
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path fixture;
    std::filesystem::path outDir_;
    std::filesystem::path out;
};

// (A)+(B)+(C): editing one CMYK span and saving must not drop the Separation
// colourspace resource, the unedited spot `scn` fill, or the overprint ExtGState.
// Kills a mutation where the save/filter path flattens or discards colour
// resources (silent spot-colour drop).
TEST_F(FidelityR3_06_ColorPreservation, EditPreservesSpotAndOverprintResources) {
    ASSERT_TRUE(editSpanAndSave(U"EDITME", U"EDITED"))
        << "could not locate the EDITME span to edit";
    ASSERT_TRUE(std::filesystem::exists(out));

    EXPECT_TRUE(savedFileHasSeparationColorspace(out))
        << "the /Separation spot colourspace resource was dropped on edit+save "
           "(silent spot-colour drop)";

    const std::string content = decompressedPageContent(out, 0);
    EXPECT_NE(content.find("scn"), std::string::npos)
        << "the unedited SPOTME spot-colour fill operator (scn) did not survive "
           "the content-stream rewrite";

    EXPECT_TRUE(savedFileHasOverprintExtGState(out))
        << "the overprint ExtGState resource was dropped on edit+save";
}

// (D): neither the edited span nor the untouched spot span may render black —
// i.e. the edit must not silently lose colour. Kills a mutation that drops the
// colour operator entirely (text falls to the default black fill).
TEST_F(FidelityR3_06_ColorPreservation, EditDoesNotLoseColourToBlack) {
    ASSERT_TRUE(editSpanAndSave(U"EDITME", U"EDITED"))
        << "could not locate the EDITME span to edit";
    ASSERT_TRUE(std::filesystem::exists(out));

    // The untouched spot span must still render in its spot colour.
    std::vector<Glyph> spot = lineContaining(out, U"SPOTME", 0);
    ASSERT_FALSE(spot.empty())
        << "SPOTME text disappeared from the saved file";
    EXPECT_TRUE(runIsNotBlack(spot))
        << "the untouched SPOTME spot colour was flattened to black on save";

    // The edited span must still render in colour (source colour not lost).
    std::vector<Glyph> edited = lineContaining(out, U"EDITED", 0);
    ASSERT_FALSE(edited.empty())
        << "the edited replacement text is missing from the saved file";
    EXPECT_TRUE(runIsNotBlack(edited))
        << "the edited span lost its source colour and rendered black";
}

} // namespace
