// Descriptor-based font style detection (pipeline d4e7f921 phase 1).
//
// Locks the name-INDEPENDENT style path: italic/bold are read from the PDF
// /FontDescriptor (/Flags Italic bit 7 + ForceBold bit 19, /ItalicAngle) during
// extraction and carried onto TextChar -> TextSpan, so the edit overlay and the
// new-character fallback no longer depend on style keywords being present in the
// font NAME. The fixture embedded_descriptor_style.pdf has a style-NEUTRAL
// BaseFont ("...+Sutpenia Regular") whose descriptor carries Italic+ForceBold —
// so any italic/bold detected here can only have come from the descriptor.
//
// The font-name keyword scan is retained as the fallback for descriptor-less
// fonts; that path is covered too (NoDescriptor_*), as is the parseStyle
// descriptor-vs-name precedence the new-char fallback uses.

#include <gtest/gtest.h>

#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "tre/AutoMissingGlyphResolver.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <QFont>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::textedit;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::tre::AutoMissingGlyphResolver;

namespace {

std::string fixturesDir() { return TEST_FIXTURES_DIR; }
const char* kFixture = "embedded_descriptor_style.pdf";
// A style-neutral name (no bold/italic/oblique substring), matching the
// fixture's BaseFont sans the subset prefix.
const char* kNeutralName = "ADRXQZ+Sutpenia Regular";

TextSpan makeSpan(const std::string& fontName, float fontSize) {
    TextSpan s;
    s.fontName = fontName;
    s.fontSize = fontSize;
    s.text = U"Az";
    s.originalText = s.text;
    return s;  // isItalic/isBold default false (simulates no descriptor)
}

class FontDescriptorStyleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        auto p = std::filesystem::path(fixturesDir()) / kFixture;
        if (std::filesystem::exists(p))
            doc_ = Document::open(ctx_, p);
    }

    // First extracted char with a non-empty font, plus the first span built from
    // the page (one font run -> one span).
    bool firstCharStyle(bool& italic, bool& bold) {
        if (!doc_) return false;
        auto page = doc_->page(0);
        if (!page) return false;
        for (const auto& block : page->extractText())
            for (const auto& line : block.lines)
                for (const auto& ch : line.chars)
                    if (!ch.fontName.empty()) {
                        italic = ch.isItalic;
                        bold = ch.isBold;
                        return true;
                    }
        return false;
    }

    bool firstSpan(TextSpan& out) {
        if (!doc_) return false;
        auto page = doc_->page(0);
        if (!page) return false;
        TextEditModel model(page->extractText());
        if (model.spanCount() <= 0) return false;
        out = model.span(0);
        return true;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
};

}  // namespace

// (a) Descriptor-italic fixture -> isItalic=true on TextChar AND TextSpan.
TEST_F(FontDescriptorStyleTest, DescriptorItalic_CharAndSpan_IsItalic) {
    if (!doc_) GTEST_SKIP() << kFixture << " not available";
    bool chItalic = false, chBold = false;
    ASSERT_TRUE(firstCharStyle(chItalic, chBold)) << "no extractable char";
    EXPECT_TRUE(chItalic)
        << "TextChar.isItalic must come from /FontDescriptor (name is neutral)";

    TextSpan span;
    ASSERT_TRUE(firstSpan(span)) << "no span built";
    EXPECT_TRUE(span.isItalic) << "TextSpan.isItalic must aggregate from chars";
}

// (b) Descriptor-bold fixture (ForceBold) -> isBold=true on TextChar AND span.
TEST_F(FontDescriptorStyleTest, DescriptorBold_CharAndSpan_IsBold) {
    if (!doc_) GTEST_SKIP() << kFixture << " not available";
    bool chItalic = false, chBold = false;
    ASSERT_TRUE(firstCharStyle(chItalic, chBold));
    EXPECT_TRUE(chBold)
        << "TextChar.isBold must come from /FontDescriptor ForceBold";

    TextSpan span;
    ASSERT_TRUE(firstSpan(span));
    EXPECT_TRUE(span.isBold) << "TextSpan.isBold must aggregate from chars";
}

// (d) buildOverlayFont produces italic() for a descriptor-italic span whose NAME
// has no style keyword — proving the descriptor drives the overlay, not the name.
TEST_F(FontDescriptorStyleTest, BuildOverlayFont_DescriptorItalicSpan_IsItalic) {
    if (!doc_) GTEST_SKIP() << kFixture << " not available";
    TextSpan span;
    ASSERT_TRUE(firstSpan(span));
    ASSERT_TRUE(span.isItalic) << "precondition: span carries descriptor italic";
    // Sanity: the name itself carries NO italic keyword.
    EXPECT_EQ(span.fontName.find("Italic"), std::string::npos);
    EXPECT_EQ(span.fontName.find("italic"), std::string::npos);

    QFont font = TextEditTool::buildOverlayFont(span, 1.0f);
    EXPECT_TRUE(font.italic())
        << "overlay must be italic from the descriptor flag; fontName="
        << span.fontName;
    EXPECT_TRUE(font.bold())
        << "overlay must be bold from the descriptor ForceBold flag";
}

// (c) No-descriptor fallback: a span with descriptor flags false but a NAME that
// carries the style keyword still resolves correctly via the name-parse fallback.
TEST(FontDescriptorStyleFallback, NoDescriptor_NameParseFallback_StillItalic) {
    // Name carries the keyword; isItalic/isBold default false (no descriptor).
    QFont font = TextEditTool::buildOverlayFont(
        makeSpan("ABCDEF+SomeFace-BoldItalic", 20.0f), 1.0f);
    EXPECT_TRUE(font.italic()) << "name-parse fallback must still set italic";
    EXPECT_TRUE(font.bold()) << "name-parse fallback must still set bold";
}

// And a fully neutral name with no descriptor stays regular (no false positives).
TEST(FontDescriptorStyleFallback, NeutralName_NoDescriptor_IsRegular) {
    QFont font = TextEditTool::buildOverlayFont(makeSpan(kNeutralName, 20.0f), 1.0f);
    EXPECT_FALSE(font.italic())
        << "neutral name + no descriptor must not be italic";
    EXPECT_FALSE(font.bold())
        << "neutral name + no descriptor must not be bold";
}

// (e) parseStyle prefers the descriptor flags over the name scan: a neutral name
// + descriptorItalic=true returns italic, even though the name has no keyword.
TEST(FontDescriptorStyleParse, ParseStyle_DescriptorOverridesNeutralName) {
    // Baseline: neutral name, no descriptor -> name scan yields regular.
    auto base = AutoMissingGlyphResolver::parseStyle(kNeutralName, false, false);
    EXPECT_FALSE(base.italic) << "neutral name alone must parse non-italic";
    EXPECT_FALSE(base.bold) << "neutral name alone must parse non-bold";

    // Descriptor italic flips it despite the keyword-free name.
    auto it = AutoMissingGlyphResolver::parseStyle(kNeutralName, true, false);
    EXPECT_TRUE(it.italic) << "descriptor italic must override the name scan";
    EXPECT_FALSE(it.bold);

    // Descriptor bold flips bold.
    auto bd = AutoMissingGlyphResolver::parseStyle(kNeutralName, false, true);
    EXPECT_TRUE(bd.bold) << "descriptor bold must override the name scan";
    EXPECT_FALSE(bd.italic);
}
