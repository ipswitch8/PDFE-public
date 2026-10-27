#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/AutoTagger.h"
#include "compliance/StructureTypes.h"
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pdfe::compliance;

class AutoTaggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_single.pdf not found";
        }
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override {
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
};

TEST_F(AutoTaggerTest, ConstructDestroy) {
    AutoTagger tagger(context_);
    SUCCEED();
}

TEST_F(AutoTaggerTest, TagDocument_ReturnsDocumentRoot) {
    AutoTagger tagger(context_);
    auto root = tagger.tagDocument(document_.get());
    EXPECT_EQ(root.tag, StructureTag::Document);
}

TEST_F(AutoTaggerTest, TagDocument_HasChildren) {
    AutoTagger tagger(context_);
    auto root = tagger.tagDocument(document_.get());
    EXPECT_FALSE(root.children.empty()) << "Document with text should produce tagged children";
}

TEST_F(AutoTaggerTest, TagPage_AssignsMcids) {
    AutoTagger tagger(context_);
    int nextMcid = 0;
    auto nodes = tagger.tagPage(document_.get(), 0, nextMcid);

    for (const auto& node : nodes) {
        EXPECT_GE(node.mcid, 0) << "Each tagged node should have a non-negative MCID";
        EXPECT_EQ(node.pageIndex, 0) << "All nodes should reference page 0";
    }

    if (!nodes.empty()) {
        EXPECT_GT(nextMcid, 0) << "nextMcid should advance past assigned MCIDs";
    }
}

TEST_F(AutoTaggerTest, TagPage_McidsSequential) {
    AutoTagger tagger(context_);
    int nextMcid = 0;
    auto nodes = tagger.tagPage(document_.get(), 0, nextMcid);

    for (size_t i = 0; i < nodes.size(); ++i) {
        EXPECT_EQ(nodes[i].mcid, static_cast<int>(i))
            << "MCIDs should be sequential starting from 0";
    }
}

TEST_F(AutoTaggerTest, TagPage_ValidTags) {
    AutoTagger tagger(context_);
    int nextMcid = 0;
    auto nodes = tagger.tagPage(document_.get(), 0, nextMcid);

    for (const auto& node : nodes) {
        // Should be one of our expected tags
        EXPECT_TRUE(
            node.tag == StructureTag::P ||
            node.tag == StructureTag::H1 ||
            node.tag == StructureTag::H2 ||
            node.tag == StructureTag::H3 ||
            node.tag == StructureTag::Figure
        ) << "Tag should be P, H1-3, or Figure, got: " << structureTagToName(node.tag);
    }
}

TEST_F(AutoTaggerTest, TagDocument_NullDoc) {
    AutoTagger tagger(context_);
    auto root = tagger.tagDocument(nullptr);
    EXPECT_EQ(root.tag, StructureTag::Document);
    EXPECT_TRUE(root.children.empty());
}

TEST_F(AutoTaggerTest, TagPage_NullDoc) {
    AutoTagger tagger(context_);
    int nextMcid = 0;
    auto nodes = tagger.tagPage(nullptr, 0, nextMcid);
    EXPECT_TRUE(nodes.empty());
}

TEST_F(AutoTaggerTest, TagDocument_MultiPage) {
    // Use multi-page PDF if available
    auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "test_3page.pdf not found";
    }

    auto doc = pdfe::core::Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    AutoTagger tagger(context_);
    auto root = tagger.tagDocument(doc.get());

    EXPECT_EQ(root.tag, StructureTag::Document);
    // Should have content from multiple pages
    bool hasPage0 = false, hasPage1 = false;
    for (const auto& child : root.children) {
        if (child->pageIndex == 0) hasPage0 = true;
        if (child->pageIndex == 1) hasPage1 = true;
    }
    EXPECT_TRUE(hasPage0) << "Should have tagged content from page 0";
}

// ===========================================================================
// GitHub #11 (gap-register PDF-14) — alternative text on auto-tagged figures.
//
// TWO product defects were found here, not a coverage gap:
//
//   1. AutoTagger::tagPage built its stext options with
//      FZ_STEXT_PRESERVE_WHITESPACE only. MuPDF emits FZ_STEXT_BLOCK_IMAGE
//      blocks ONLY when FZ_STEXT_PRESERVE_IMAGES is set (the same reason
//      Page.cpp passes it), so analyzeTextBlock never saw an image,
//      classifyBlock never returned StructureTag::Figure, and the entire
//      figure branch was unreachable. An image-only page auto-tagged to a
//      structure tree with no /Figure in it at all -- and the pre-existing
//      FiguresHaveAltText test, which looped over figures, therefore passed
//      by iterating an empty set.
//
//   2. That unreachable branch set `node.altText = "Image"` for every figure
//      (AutoTagger.cpp:161). A literal "Image" satisfies every automated
//      "figure has alternative text" check while telling a screen-reader user
//      nothing, so the document passes validation and stays inaccessible.
//      It now leaves /Alt absent and sets `needsAltText`, keeping the omission
//      visible to whoever can actually describe the image.
//
// AutoTaggerEmitsFigureForAnImagePage is the NON-VACUITY guard for the tests
// below: without it, "no figure carries a placeholder" is satisfied by
// producing no figures.
// ===========================================================================

namespace {

// Fixtures whose page 0 is dominated by a raster image. Several are tried
// because the assertions are worthless if none of them yields a figure.
const char* const kImageFixtures[] = {
    "image_pdf/a3_72dpi_web_letter.pdf",
    "image_pdf/a3_150dpi_a4_photo.pdf",
    "cmyk_image_xobject.pdf",
    "test_scanned.pdf",
};

struct TaggedImageDoc {
    std::string fixture;
    std::unique_ptr<pdfe::core::Document> doc;
    StructureNode root;
    int figureCount = 0;
};

// Auto-tag the first candidate fixture that actually yields a /Figure node.
TaggedImageDoc tagFirstImageFixture(
    const std::shared_ptr<pdfe::core::MuPdfContext>& ctx) {
    TaggedImageDoc best;
    for (const char* name : kImageFixtures) {
        auto path = std::filesystem::path(TEST_FIXTURES_DIR) / name;
        if (!std::filesystem::exists(path)) continue;
        auto doc = pdfe::core::Document::open(ctx, path);
        if (!doc) continue;
        AutoTagger tagger(ctx);
        auto root = tagger.tagDocument(doc.get());
        int figures = 0;
        for (const auto& c : root.children)
            if (c && c->tag == StructureTag::Figure) ++figures;
        if (figures > 0) {
            best.fixture = name;
            best.doc = std::move(doc);
            best.root = std::move(root);
            best.figureCount = figures;
            return best;
        }
    }
    return best;
}

} // namespace

TEST_F(AutoTaggerTest, AutoTaggerEmitsFigureForAnImagePage) {
    auto tagged = tagFirstImageFixture(context_);
    ASSERT_GT(tagged.figureCount, 0)
        << "AutoTagger produced no /Figure node for ANY image fixture. MuPDF "
           "only emits image blocks when FZ_STEXT_PRESERVE_IMAGES is set; "
           "without it the figure/alt-text branch of classifyBlock is dead "
           "code and every figure assertion is vacuous.";
}

TEST_F(AutoTaggerTest, AutoTaggerDoesNotEmitPlaceholderAltText) {
    auto tagged = tagFirstImageFixture(context_);
    ASSERT_GT(tagged.figureCount, 0)
        << "no figure produced -- see AutoTaggerEmitsFigureForAnImagePage";

    for (const auto& child : tagged.root.children) {
        if (!child || child->tag != StructureTag::Figure) continue;
        EXPECT_NE(child->altText, "Image")
            << "fixture " << tagged.fixture
            << ": the generic placeholder is back. It passes an automated "
               "alt-text check and conveys nothing to a screen-reader user.";
        EXPECT_TRUE(child->altText.empty())
            << "fixture " << tagged.fixture
            << ": AutoTagger fabricated alternative text (\"" << child->altText
            << "\") it has no way to derive";
    }
}

// The replacement for the old FiguresHaveAltText, which asserted the defect:
// a figure it cannot describe must be FLAGGED, not quietly filled in.
TEST_F(AutoTaggerTest, FiguresAreFlaggedAsNeedingAltTextInsteadOfPlaceholder) {
    auto tagged = tagFirstImageFixture(context_);
    ASSERT_GT(tagged.figureCount, 0)
        << "no figure produced -- see AutoTaggerEmitsFigureForAnImagePage";

    for (const auto& child : tagged.root.children) {
        if (!child || child->tag != StructureTag::Figure) continue;
        EXPECT_TRUE(child->needsAltText)
            << "fixture " << tagged.fixture
            << ": a figure with no /Alt was not flagged, so nothing downstream "
               "can tell the author the description is missing";
    }
}

// CONTROL: non-figure nodes are not flagged, so `needsAltText` is a real
// signal rather than a constant true.
TEST_F(AutoTaggerTest, TextNodesAreNotFlaggedAsNeedingAltText) {
    AutoTagger tagger(context_);
    auto root = tagger.tagDocument(document_.get());
    ASSERT_FALSE(root.children.empty()) << "test_single.pdf produced no nodes";

    int textNodes = 0;
    for (const auto& child : root.children) {
        if (!child || child->tag == StructureTag::Figure) continue;
        ++textNodes;
        EXPECT_FALSE(child->needsAltText)
            << "a text node was flagged as needing alternative text";
        EXPECT_TRUE(child->altText.empty())
            << "a text node was given alternative text: " << child->altText;
    }
    EXPECT_GT(textNodes, 0) << "no non-figure nodes to check";
}
