// -*- coding: utf-8 -*-
//
// Regression test for bugfix-3e91a7c4 #4/#5 — a committed text edit silently
// NO-OPS for small font sizes on a page with a nonzero MediaBox/CropBox origin.
//
// Root cause (see tests/docs/bugfix-3e91a7c4-phase1-findings.md + Phase 3 trace):
// ContentStreamEditor's glyph matcher (textFilterCallback) builds the glyph box
// via `pageHeight - y`, which discards the page's coordinate ORIGIN, while the
// span box (repl.bbox) is in MuPDF's CropBox-normalized space. On a page whose
// origin y0 != 0 the two diverge by a constant Δ = origin.y0. A glyph is matched
// only if its vertical center lies inside the span's y-range (zero tolerance);
// tall (large-font) spans absorb Δ, but short (small-font) spans — half-height
// smaller than Δ — reject every glyph, so nothing is removed/injected and
// /Contents is left unchanged while the edit overlay shows the change. The user's
// real doc (SpecimenA) has MediaBox [0 7.9 594 745.9], so ~9-11pt body edits fail
// while 24pt+ titles work. This fixture replicates that box structure.
//
// RED before the fix: the 11/9/7pt edits do not land (old tokens persist).
// GREEN after: all four edits (18/11/9/7pt) land. The 18pt line is the control
// that works even pre-fix.

#include <gtest/gtest.h>

#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "ui/MainWindow.h"

#include <QString>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::textedit;
using namespace pdfe::core;
using namespace pdfe::ui;

namespace {
std::u32string u32(const std::string& s) { return std::u32string(s.begin(), s.end()); }

// ASCII-only tokens -> narrow the span text for substring search.
std::string narrow(const std::u32string& t) { return std::string(t.begin(), t.end()); }

int findSpanContaining(const TextEditModel& model, const std::string& token) {
    for (int i = 0; i < model.spanCount(); ++i) {
        if (narrow(model.span(i).text).find(token) != std::string::npos) return i;
    }
    return -1;
}
}  // namespace

TEST(SmallTextEditRender, EditLandsAtAllFontSizesOnOffsetOriginPage) {
    auto context = std::make_shared<MuPdfContext>();
    const std::filesystem::path pdf =
        std::filesystem::path(TEST_FIXTURES_DIR) / "smalltext_edit.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf)) << "fixture missing: " << pdf.string();

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto blocks = page->extractText();
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    struct Edit { std::string oldTok, newTok; double sizePt; };
    const std::vector<Edit> edits = {
        {"HEADINGBIG", "HEADINGEDIT", 18.0},  // control — works even pre-fix
        {"BODYELEVEN", "BODYEDITXX",  11.0},
        {"SMALLNINE",  "NINEEDITXX",   9.0},
        {"TINYSEVEN",  "SEVENEDITX",   7.0},
    };

    std::vector<TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));

    std::vector<std::pair<int, std::u32string>> mods;
    for (const auto& e : edits) {
        int idx = findSpanContaining(model, e.oldTok);
        ASSERT_GE(idx, 0) << "span not found for token: " << e.oldTok;
        mods.push_back({idx, u32(e.newTok)});
    }

    ContentStreamEditor editor(context.get());
    editor.applyEdits(page.get(), document.get(), spans, mods);

    // applyEdits rewrites /Contents in the in-memory pdf_document; re-extracting
    // from the SAME open page reflects the change (no save/reopen needed).
    const std::string after = page->extractPlainText();

    for (const auto& e : edits) {
        EXPECT_NE(after.find(e.newTok), std::string::npos)
            << e.sizePt << "pt edit did NOT land: new text '" << e.newTok
            << "' absent after editing '" << e.oldTok << "'.\nExtracted: " << after;
        EXPECT_EQ(after.find(e.oldTok), std::string::npos)
            << e.sizePt << "pt original '" << e.oldTok
            << "' still present — the edit was a silent no-op.\nExtracted: " << after;
    }
}

// AC1(a): editing small text on PAGE 2+ (index 1) lands in the re-extracted content.
// The offset-origin no-op is size-driven, but the user reported it per-PAGE, so this
// exercises the page>0 dimension explicitly. RED before the fix, GREEN after.
TEST(SmallTextEditRender, EditOnPage2LandsInRenderedContent) {
    auto context = std::make_shared<MuPdfContext>();
    const std::filesystem::path pdf =
        std::filesystem::path(TEST_FIXTURES_DIR) / "multipage_smalltext_edit.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf)) << pdf.string();
    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);

    auto page = document->page(1);  // second page
    ASSERT_NE(page, nullptr);
    auto blocks = page->extractText();
    TextEditModel model(blocks);

    int idx = findSpanContaining(model, "REPLNINE");  // the 9pt body token on page 2
    ASSERT_GE(idx, 0);
    std::vector<TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));
    std::vector<std::pair<int, std::u32string>> mods = {{idx, u32("DONEPAGE2")}};

    ContentStreamEditor editor(context.get());
    editor.applyEdits(page.get(), document.get(), spans, mods);

    const std::string after = page->extractPlainText();
    EXPECT_NE(after.find("DONEPAGE2"), std::string::npos)
        << "page-2 small-text edit did not land. Extracted: " << after;
    EXPECT_EQ(after.find("REPLNINE"), std::string::npos)
        << "page-2 original still present (no-op). Extracted: " << after;
}

// AC1(b)+AC3: Replace All updates EVERY matched page (not just page 1), and the
// reported count equals the number of occurrences that actually landed. Uses the
// real MainWindow::onReplaceAll path (edits applied in memory). RED before the
// fix (9pt spans no-op: 0 land but count reports 3), GREEN after (3 land, count 3).
TEST(SmallTextEditRender, ReplaceAllUpdatesEveryMatchedPageAndCountMatchesActual) {
    MainWindow mw;
    const std::filesystem::path pdf =
        std::filesystem::path(TEST_FIXTURES_DIR) / "multipage_smalltext_edit.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf)) << pdf.string();
    mw.openFile(QString::fromStdString(pdf.string()));

    mw.replaceAllForTesting("REPLNINE", "DONENINE");

    auto* doc = mw.documentForTesting();
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3);

    int actualLanded = 0;
    for (int i = 0; i < doc->pageCount(); ++i) {
        auto page = doc->page(i);
        ASSERT_NE(page, nullptr);
        const std::string t = page->extractPlainText();
        EXPECT_NE(t.find("DONENINE"), std::string::npos)
            << "page " << i << " was NOT updated by Replace All. Extracted: " << t;
        EXPECT_EQ(t.find("REPLNINE"), std::string::npos)
            << "page " << i << " still has the original token. Extracted: " << t;
        for (size_t p = t.find("DONENINE"); p != std::string::npos; p = t.find("DONENINE", p + 1))
            ++actualLanded;
    }

    EXPECT_EQ(actualLanded, 3) << "expected one landed replacement per page";
    EXPECT_EQ(mw.lastReplaceAllCountForTesting(), actualLanded)
        << "reported Replace All count (" << mw.lastReplaceAllCountForTesting()
        << ") does not match the actual number of landed replacements (" << actualLanded << ")";
}
