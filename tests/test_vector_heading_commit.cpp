// -*- coding: utf-8 -*-
// Regression lock for discovery finding D8-1 (pipeline c4a9e271 Phase 9):
//
//   Editing the bold "Invoice" heading on invoice_vector_heading.pdf with the
//   vector text-edit tool and committing collapses the heading on the rendered
//   page (heading-bbox ink ~0.13 -> ~0.038, ~29% kept; the render oracle in
//   tests/ui_automation/test_render_text_edit.py requires >=60%).
//
// ROOT CAUSE (found Phase 9, NOT what the discovery doc hypothesized):
//   The heading font HDJKHA+Arial,Bold is a SUBSET embedded font — it only
//   carries glyphs for the original word "Invoice" (I,n,v,o,i,c,e). Editing it
//   to "Quote" needs Q,u,t — codepoints the subset cannot encode. They render
//   as .notdef (blank), so the heading visually collapses to just the surviving
//   'o' and 'e'. (Confirmed: a headless re-extract returns "??o?e".)
//
//   bbox/font-size are PRESERVED across the commit (MuPDF lays the missing
//   glyphs out with advance widths), so a geometry-only oracle is FALSE-GREEN.
//   The only sound oracle is RENDERED INK — this test renders the committed
//   heading region and asserts it keeps >=60% of its original ink, mirroring
//   the UI render oracle but deterministically and headlessly.
//
// The pre-existing oracles missed it:
//   * test_text_edit_span_provenance.cpp only checks routing PREDICATES.
//   * test_content_stream_tm_sign.cpp::EditSaveCycleTest checks bbox geometry
//     (false-green here) on a non-subset fixture (test_3page.pdf).
#include <gtest/gtest.h>

#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "textedit/TextEditCommands.h"
#include "tre/AutoMissingGlyphResolver.h"
#include "fonts/VectorSubsetDetector.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/TextExtractor.h"
#include "core/UndoManager.h"

#include <QImage>
#include <QRectF>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string u32ToAscii(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        if (c >= 32 && c < 127) out += static_cast<char>(c);
        else out += '?';
    }
    return out;
}

// Largest-font span whose text contains "invoice" (case-insensitive) — the
// "Invoice" heading. Returns -1 if not found.
int findHeadingSpan(const pdfe::textedit::TextEditModel& model) {
    int best = -1;
    float bestSize = -1.0f;
    for (int i = 0; i < model.spanCount(); ++i) {
        const auto& sp = model.span(i);
        std::string lower = u32ToAscii(sp.text);
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(ch));
        if (lower.find("invoice") != std::string::npos &&
            sp.fontSize > bestSize) {
            bestSize = sp.fontSize;
            best = i;
        }
    }
    return best;
}

// Composite an RGBA render over white (Page::render returns an alpha channel;
// unpainted page background is transparent, NOT white) and return the result
// as RGB888 so "ink" measures real painted pixels, not transparent background.
QImage compositeOverWhite(const QImage& src) {
    QImage rgba = src.convertToFormat(QImage::Format_RGBA8888);
    QImage out(rgba.width(), rgba.height(), QImage::Format_RGB888);
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar* s = rgba.scanLine(y);
        uchar* d = out.scanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            int r = s[x * 4 + 0], g = s[x * 4 + 1], b = s[x * 4 + 2];
            int a = s[x * 4 + 3];
            d[x * 3 + 0] = static_cast<uchar>((r * a + 255 * (255 - a)) / 255);
            d[x * 3 + 1] = static_cast<uchar>((g * a + 255 * (255 - a)) / 255);
            d[x * 3 + 2] = static_cast<uchar>((b * a + 255 * (255 - a)) / 255);
        }
    }
    return out;
}

// Fraction of dark "ink" pixels (luma < 110) in an RGB888 image.
double inkRatio(const QImage& rgb) {
    if (rgb.isNull() || rgb.width() == 0 || rgb.height() == 0) return 0.0;
    long long dark = 0;
    const long long total =
        static_cast<long long>(rgb.width()) * rgb.height();
    for (int y = 0; y < rgb.height(); ++y) {
        const uchar* row = rgb.scanLine(y);
        for (int x = 0; x < rgb.width(); ++x) {
            int r = row[x * 3 + 0], g = row[x * 3 + 1], b = row[x * 3 + 2];
            int luma = (r * 299 + g * 587 + b * 114) / 1000;
            if (luma < 110) ++dark;
        }
    }
    return total > 0 ? static_cast<double>(dark) / static_cast<double>(total)
                     : 0.0;
}

// Crop the heading region from a full-page render. Page::render IGNORES its
// region arg and always renders the whole page at `scale`, so we crop in pixel
// space: fitz bbox (Y-down, origin top-left) * scale.
QImage cropHeading(const QImage& fullPage, const QRectF& fitzBBox, float scale) {
    QRect px(static_cast<int>(fitzBBox.x() * scale) - 4,
             static_cast<int>(fitzBBox.y() * scale) - 4,
             static_cast<int>(fitzBBox.width() * scale) + 8,
             static_cast<int>(fitzBBox.height() * scale) + 8);
    px = px.intersected(fullPage.rect());
    return fullPage.copy(px);
}

}  // namespace

class VectorHeadingCommitTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        path_ = fixtures / "invoice_vector_heading.pdf";
        if (!std::filesystem::exists(path_)) {
            GTEST_SKIP() << "invoice_vector_heading.pdf not available";
        }
        doc_ = pdfe::core::Document::open(ctx_, path_);
        ASSERT_NE(doc_, nullptr);
    }
    void TearDown() override { doc_.reset(); ctx_.reset(); }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::core::Document> doc_;
    std::filesystem::path path_;
};

// The headless analogue of test_render_text_edit::test_vector_text_edit_visible.
// Edit the "Invoice" heading to "Quote", commit, and assert the RENDERED
// heading keeps >=60% of its ink (catches the missing-glyph collapse).
TEST_F(VectorHeadingCommitTest, HeadingStaysVisibleAfterCommit) {
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::core::TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(page.get());
    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    int headIdx = findHeadingSpan(model);
    ASSERT_GE(headIdx, 0) << "could not locate an 'Invoice' heading span";

    const auto& headSpan = model.span(headIdx);
    const QRectF origBBox = headSpan.boundingBox;
    const std::u32string oldText = headSpan.text;
    ASSERT_GT(origBBox.height(), 0.0);
    ASSERT_FALSE(headSpan.isOcrInvisibleLayer())
        << "the Invoice heading is a real vector span, not an OCR layer";
    EXPECT_TRUE(pdfe::fonts::VectorSubsetDetector::hasSubsetPrefix(
        headSpan.fontName))
        << "fixture precondition: heading uses a subset font ("
        << headSpan.fontName << ")";

    // Render the WHOLE page (Page::render ignores its region arg), composite
    // over white, then crop the heading in pixel space.
    const float kScale = 4.0f;
    QImage beforeFull = compositeOverWhite(page->render(origBBox, kScale));
    ASSERT_FALSE(beforeFull.isNull());
    QImage before = cropHeading(beforeFull, origBBox, kScale);
    const double inkBefore = inkRatio(before);
    const char* dump = std::getenv("D8_DUMP_DIR");
    if (dump) before.save(QString::fromUtf8(dump) + "/heading_before.png");
    std::cout << "[D8-1] heading [" << headIdx << "] '" << u32ToAscii(oldText)
              << "' font='" << headSpan.fontName << "' size="
              << headSpan.fontSize << " inkBefore=" << inkBefore << "\n";
    ASSERT_GT(inkBefore, 0.02) << "heading should have visible ink before edit";

    // Commit "Quote" through the same command path the UI commit uses.
    std::u32string newText = U"Quote";
    pdfe::commands::UndoManager undoMgr;
    auto cmd = std::make_unique<pdfe::textedit::EditTextCommand>(
        doc_.get(), ctx_, /*pageIndex=*/0, headIdx,
        headSpan, oldText, newText);
    undoMgr.execute(std::move(cmd));

    // Reload + render the SAME region after commit.
    page = doc_->page(0);
    ASSERT_NE(page, nullptr);
    QImage afterFull = compositeOverWhite(page->render(origBBox, kScale));
    ASSERT_FALSE(afterFull.isNull());
    QImage after = cropHeading(afterFull, origBBox, kScale);
    const double inkAfter = inkRatio(after);
    if (dump) after.save(QString::fromUtf8(dump) + "/heading_after.png");

    // Re-extract to log what the committed text round-trips as.
    auto newBlocks = extractor.extract(page.get());
    pdfe::textedit::TextEditModel newModel(newBlocks);
    std::string committed;
    for (int i = 0; i < newModel.spanCount(); ++i) {
        if (newModel.span(i).boundingBox.intersects(origBBox))
            committed += u32ToAscii(newModel.span(i).text) + "|";
    }
    std::cout << "[D8-1] inkAfter=" << inkAfter << " (keep="
              << (inkBefore > 0 ? inkAfter / inkBefore : 0.0)
              << ") committed-text-near-heading='" << committed << "'\n";

    EXPECT_GE(inkAfter, 0.6 * inkBefore)
        << "heading collapsed on commit: rendered ink " << inkBefore
        << " -> " << inkAfter << " (kept "
        << (inkBefore > 0 ? 100.0 * inkAfter / inkBefore : 0.0)
        << "%, need >=60%). Missing-glyph .notdef regression (D8-1).";
}

// Diagnostic: does AutoMissingGlyphResolver produce substitution runs for the
// codepoints the subset heading font cannot encode? If this returns no runs,
// the missing glyphs fall through to the subset font and render .notdef.
TEST_F(VectorHeadingCommitTest, ResolverCoversMissingHeadingGlyphs) {
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);
    pdfe::core::TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(page.get());
    pdfe::textedit::TextEditModel model(blocks);
    int headIdx = findHeadingSpan(model);
    ASSERT_GE(headIdx, 0);
    const auto& headSpan = model.span(headIdx);

    std::u32string newText = U"Quote";
    auto runs = pdfe::tre::AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(),
        headSpan.fontName, headSpan.fontSize, newText);

    std::cout << "[D8-1] resolver produced " << runs.size()
              << " run(s) for 'Quote' on font '" << headSpan.fontName << "':\n";
    for (const auto& r : runs) {
        std::cout << "   chars[" << r.charStart << "," << r.charEnd << ")"
                  << " family='" << r.fontFamily << "'"
                  << " path='" << r.fontFilePath << "'"
                  << " preInstalled='" << r.preInstalledResName << "'\n";
    }

    // Q, u, t are not in the "Invoice" subset; the resolver must cover them
    // (system-font fallback at minimum). Empty runs == the bug.
    EXPECT_FALSE(runs.empty())
        << "AutoMissingGlyphResolver returned no substitution runs — the "
           "missing glyphs Q/u/t will render .notdef (D8-1 root cause).";
}
