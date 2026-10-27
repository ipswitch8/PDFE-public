// Regression test for the INBOUND text-state leak in ContentStreamEditor
// (Register H1 / docs/content-stream-state-continuity-audit.md, pipeline
// e5b9c231 Phase 2).
//
// The editor injects its replacement text through the pdf_filter_page_contents
// `after_text_object` callback, emitting Tz/Tr/rg/Tf/Tm but never resetting the
// text-state parameters Tc (char spacing), Tw (word spacing) or Ts (text rise).
// On a page whose ambient state is set inside the BT and left active when the
// injection is emitted inline (the same structural pattern that triggered the
// original -100 Tz mirror bug), the injected replacement inherits that ambient
// state and is drawn distorted -- here, shifted vertically by an ambient "50 Ts".
//
// Fix: the injection preamble must establish a known text-state basis
// (0 Tc / 0 Tw / 0 Ts) alongside the existing 100 Tz, so the replacement lands
// exactly where the editor positioned it via Tm.
//
// Oracle (M-b: assert edited element AND untouched neighbor): the edited span's
// baseline must match the position of the ORIGINAL span it replaced (captured
// before the edit), and the untouched "KEEP ME" neighbor's color and baseline
// must be unchanged. Both are read back via fitz stext after a save/reopen
// round-trip -- exactly what a viewer renders.
#include <gtest/gtest.h>
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace pdfe::textedit;
using namespace pdfe::core;

namespace {

struct LineInfo {
    bool found = false;
    uint32_t firstArgb = 0;
    float firstBaselineY = 0.0f;  // quad.ll.y of the first glyph (device space)
    float ur_x = 0.0f, ul_x = 0.0f;
};

std::filesystem::path fixturePath() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "state_leak_ts.pdf";
}

// Read the first-glyph info of whichever rendered line contains `needle`
// from a PDF on disk, using an isolated fitz context.
LineInfo readLine(const std::filesystem::path& pdf, const std::u32string& needle) {
    LineInfo info;
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
                fz_stext_char* f = ln->first_char;
                info.found = true;
                info.firstArgb = f->argb & 0xFFFFFFu;
                info.firstBaselineY = f->quad.ll.y;
                info.ur_x = f->quad.ur.x;
                info.ul_x = f->quad.ul.x;
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) { /* leave info.found=false -> assertions fail loudly */ }
    fz_drop_context(ctx);
    return info;
}

// Edit "SHIP TO" -> "SHOP TO" on the fixture, save, and return the saved path.
std::filesystem::path editAndSave() {
    auto context = std::make_shared<MuPdfContext>();
    auto document = Document::open(context, fixturePath());
    EXPECT_NE(document, nullptr);
    auto page = document->page(0);
    EXPECT_NE(page, nullptr);

    auto blocks = page->extractText();
    TextEditModel model(blocks);
    std::vector<TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));

    int target = -1;
    for (int i = 0; i < static_cast<int>(spans.size()); ++i)
        if (spans[i].text.find(U"SHIP TO") != std::u32string::npos) { target = i; break; }
    EXPECT_GE(target, 0) << "fixture must contain a 'SHIP TO' span";

    ContentStreamEditor editor(context.get());
    std::vector<std::pair<int, std::u32string>> mods{{target, U"SHOP TO"}};
    editor.applyEdits(page.get(), document.get(), spans, mods);

    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_state_leak_ts_edited.pdf";
    document->saveAs(out);
    return out;
}

} // namespace

// PRIMARY: the edited span must land at the SAME baseline as the original span
// it replaced. Before the fix the injection inherits the ambient "50 Ts" and the
// replacement is drawn ~50 device units off. Tolerance 5 << 50 leak.
TEST(ContentStreamStateLeak, EditedSpanNotShiftedByAmbientTextRise) {
    LineInfo orig = readLine(fixturePath(), U"SHIP TO");
    ASSERT_TRUE(orig.found) << "original 'SHIP TO' span not found in fixture";

    std::filesystem::path edited = editAndSave();
    LineInfo shop = readLine(edited, U"SHOP");
    ASSERT_TRUE(shop.found) << "edited 'SHOP TO' line not found after round-trip";

    EXPECT_NEAR(shop.firstBaselineY, orig.firstBaselineY, 5.0f)
        << "edited span shifted vertically -- ambient text rise (Ts) leaked into "
           "the injection (orig ll.y=" << orig.firstBaselineY
        << " edited ll.y=" << shop.firstBaselineY << ")";

    // The Phase-1 mirror fix must still hold (left-to-right, not mirrored).
    EXPECT_GT(shop.ur_x, shop.ul_x)
        << "edited span is horizontally mirrored (regression of the Tz fix)";
}

// REFUTATION of the audit's predicted OUTBOUND leak: editing a span in one text
// object must not corrupt a LATER independent text object that inherits the
// page's ambient -100 Tz. Empirically MuPDF's pdf_filter_page_contents wraps the
// injected BT...ET in q...Q and re-materialises the active text state at each
// following BT, so the injection's "100 Tz" cannot leak outbound to "NEIGHBOR".
// This makes the refutation a committed, reproducible fact (see the fixture
// header for the full rationale). If a future MuPDF change ever regressed this,
// the neighbor would render mirrored and this test would catch it.
TEST(ContentStreamStateLeak, OutboundLeakNeutralizedByFilterForLaterTextObject) {
    auto context = std::make_shared<MuPdfContext>();
    std::filesystem::path fx =
        std::filesystem::path(TEST_FIXTURES_DIR) / "outbound_neighbor.pdf";
    ASSERT_TRUE(std::filesystem::exists(fx)) << fx.string();

    auto document = Document::open(context, fx);
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);
    auto blocks = page->extractText();
    TextEditModel model(blocks);
    std::vector<TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));
    int target = -1;
    for (int i = 0; i < static_cast<int>(spans.size()); ++i)
        if (spans[i].text.find(U"EDITME") != std::u32string::npos) { target = i; break; }
    ASSERT_GE(target, 0) << "fixture must contain an 'EDITME' span";

    ContentStreamEditor editor(context.get());
    std::vector<std::pair<int, std::u32string>> mods{{target, U"EDITED"}};
    editor.applyEdits(page.get(), document.get(), spans, mods);
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_outbound_neighbor_edited.pdf";
    document->saveAs(out);
    document.reset(); context.reset();

    LineInfo nbr = readLine(out, U"NEIGHBOR");
    ASSERT_TRUE(nbr.found) << "unedited NEIGHBOR text object not found after round-trip";
    EXPECT_GT(nbr.ur_x, nbr.ul_x)
        << "NEIGHBOR is horizontally mirrored -- the injection's text state leaked "
           "outbound into a later text object (audit's outbound prediction would "
           "then be real; MuPDF's filter is expected to neutralize it)";
}

// M-b untouched-neighbor: editing "SHIP TO" must not disturb the "KEEP ME"
// neighbor's rendered color or baseline.
TEST(ContentStreamStateLeak, UntouchedNeighborUnchanged) {
    LineInfo before = readLine(fixturePath(), U"KEEP ME");
    ASSERT_TRUE(before.found) << "'KEEP ME' neighbor not found in fixture";

    std::filesystem::path edited = editAndSave();
    LineInfo after = readLine(edited, U"KEEP ME");
    ASSERT_TRUE(after.found) << "'KEEP ME' neighbor not found after round-trip";

    EXPECT_EQ(after.firstArgb, before.firstArgb)
        << "neighbor color changed by the edit (outbound color leak)";
    EXPECT_NEAR(after.firstBaselineY, before.firstBaselineY, 2.0f)
        << "neighbor baseline moved by the edit (outbound state leak)";
}
