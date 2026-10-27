// -*- coding: utf-8 -*-
// Regression test for the INBOUND text-state leak in TRE PdfContentWriter::
// writeTextBlock (Register H1 / docs/content-stream-state-continuity-audit.md,
// pipeline e5b9c231 Phase 2).
//
// writeTextBlock emits "q BT /Fn sz Tf  r g b rg  Tm  ...Tj  ET Q". It sets its
// own font and colour but never resets Tz/Tc/Tw/Ts, so when its fragment is
// concatenated after page content that left an ambient "-100 Tz" active, the
// reconstructed text inherits the negative horizontal scale and renders
// horizontally mirrored.
//
// Fix: emit the shared knownTextStateBasis() (100 Tz + 0 Tc/Tw/Ts/Tr; its black
// rg is immediately overridden by writeTextBlock's own colour) right after the
// fragment's `q`.
//
// Oracle: append an ambient "-100 Tz" fragment, then the writeTextBlock
// fragment, to a real page; save; reopen; read the reconstructed text back via
// fitz stext and assert it is NOT mirrored (upper-right x > upper-left x).
#include <gtest/gtest.h>
#include "tre/PdfContentWriter.h"
#include "fonts/PdfFontEmbedder.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QImage>
#include <filesystem>
#include <memory>
#include <string>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace pdfe::tre;
using namespace pdfe::fonts;
using namespace pdfe::core;

namespace {

// Return {found, mirrored} for the first line containing `needle`.
struct Dir { bool found = false; bool mirrored = false; };
Dir readDir(const std::filesystem::path& pdf, const std::u32string& needle) {
    Dir d;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, pdf.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, 0, nullptr);
        for (fz_stext_block* b = stext->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
                std::u32string t;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next)
                    t.push_back(static_cast<char32_t>(ch->c));
                if (t.find(needle) == std::u32string::npos) continue;
                fz_quad q = ln->first_char->quad;
                d.found = true;
                d.mirrored = !(q.ur.x > q.ul.x);
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return d;
}

} // namespace

TEST(TreStateLeak, ReconstructedTextNotMirroredByAmbientNegTz) {
    auto context = std::make_shared<MuPdfContext>();
    PdfFontEmbedder embedder(context.get());
    PdfContentWriter writer(context.get(), &embedder);

    std::filesystem::path scanned =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_scanned.pdf";
    ASSERT_TRUE(std::filesystem::exists(scanned)) << "test_scanned.pdf required";
    std::filesystem::path arial = "C:\\Windows\\Fonts\\arial.ttf";
    ASSERT_TRUE(std::filesystem::exists(arial)) << "Arial required on this host";

    auto doc = Document::open(context, scanned);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    FontMatch fm;
    fm.fontFilePath = arial.string();
    fm.sizePt = 14.0f;
    fm.color = RgbColor{0, 0, 0};
    PdfRect rect{72.0, 120.0, 320.0, 145.0};

    std::string ops = writer.writeTextBlock(page.get(), doc.get(),
                                            "TREMARK", fm, rect);
    ASSERT_FALSE(ops.empty()) << "writeTextBlock returned no operators";

    // Ambient -100 Tz active when the TRE fragment runs (concatenated after).
    ASSERT_TRUE(writer.appendContentStream(page.get(), doc.get(), "-100 Tz\n"));
    ASSERT_TRUE(writer.appendContentStream(page.get(), doc.get(), ops));

    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_tre_state_leak.pdf";
    doc->saveAs(out);

    Dir d = readDir(out, U"TREMARK");
    ASSERT_TRUE(d.found) << "reconstructed 'TREMARK' text not found after round-trip";
    EXPECT_FALSE(d.mirrored)
        << "reconstructed text is horizontally mirrored -- ambient -100 Tz "
           "leaked into the writeTextBlock fragment";
}

// M-b untouched-neighbor: adding the TRE text block must not disturb the rest of
// the page. The neighbor here is the pre-existing scanned image; assert the
// render change is confined to the small TREMARK band and the rest of the page
// is pixel-identical.
TEST(TreStateLeak, UntouchedNeighborImageUnchanged) {
    auto context = std::make_shared<MuPdfContext>();
    PdfFontEmbedder embedder(context.get());
    PdfContentWriter writer(context.get(), &embedder);

    std::filesystem::path scanned =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_scanned.pdf";
    std::filesystem::path arial = "C:\\Windows\\Fonts\\arial.ttf";
    ASSERT_TRUE(std::filesystem::exists(scanned));
    ASSERT_TRUE(std::filesystem::exists(arial));

    auto doc = Document::open(context, scanned);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    QRectF bbox = page->boundingBox();
    QImage before = page->render(bbox, 1.0f).convertToFormat(QImage::Format_RGB888);
    ASSERT_FALSE(before.isNull());

    FontMatch fm;
    fm.fontFilePath = arial.string();
    fm.sizePt = 14.0f; fm.color = RgbColor{0, 0, 0};
    PdfRect rect{72.0, 120.0, 320.0, 145.0};
    std::string ops = writer.writeTextBlock(page.get(), doc.get(), "TREMARK", fm, rect);
    ASSERT_FALSE(ops.empty());
    ASSERT_TRUE(writer.appendContentStream(page.get(), doc.get(), ops));
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_tre_nbr.pdf";
    doc->saveAs(out);
    page.reset(); doc.reset();

    auto doc2 = Document::open(context, out);
    auto page2 = doc2->page(0);
    QImage after = page2->render(page2->boundingBox(), 1.0f).convertToFormat(QImage::Format_RGB888);
    ASSERT_FALSE(after.isNull());
    ASSERT_EQ(after.size(), before.size());

    // Bounding box of ALL changed pixels. The added TRE text should confine the
    // change to a small box; if a leaked state (e.g. the ambient scale/color)
    // had corrupted the underlying scanned image, the change box would sprawl
    // across most of the page.
    int minX = after.width(), minY = after.height(), maxX = -1, maxY = -1;
    long changed = 0;
    for (int y = 0; y < after.height(); ++y)
        for (int x = 0; x < after.width(); ++x)
            if (before.pixel(x, y) != after.pixel(x, y)) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y); ++changed;
            }
    ASSERT_GT(changed, 50) << "TREMARK text was not drawn (no render change)";
    const long pageArea = static_cast<long>(after.width()) * after.height();
    const long boxArea =
        static_cast<long>(maxX - minX + 1) * static_cast<long>(maxY - minY + 1);
    // The change (added 14pt text on ~a quarter page width) must be localized:
    // its bounding box covers well under 15% of the page. Everything outside the
    // box is pixel-identical to the original scanned image (the untouched
    // neighbor), which cannot be true if the box sprawls page-wide.
    EXPECT_LT(boxArea, pageArea * 15 / 100)
        << "changed-pixel box (" << boxArea << " px) is not localized vs page ("
        << pageArea << " px) -- the added text disturbed the neighbor scanned image";
}
