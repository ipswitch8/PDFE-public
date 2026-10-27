#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/ContentStreamTagger.h"
#include "compliance/StructureTypes.h"
#include <filesystem>
#include <memory>

using namespace pdfe::compliance;

class ContentStreamTaggerTest : public ::testing::Test {
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

TEST_F(ContentStreamTaggerTest, ConstructDestroy) {
    ContentStreamTagger tagger(context_);
    SUCCEED();
}

TEST_F(ContentStreamTaggerTest, PageHasMarkedContent_FreshDoc_False) {
    ContentStreamTagger tagger(context_);
    EXPECT_FALSE(tagger.pageHasMarkedContent(document_.get(), 0));
}

TEST_F(ContentStreamTaggerTest, TagPageContent_InjectsBDC) {
    ContentStreamTagger tagger(context_);

    std::vector<MarkedContentRegion> regions;
    MarkedContentRegion r;
    r.tag = StructureTag::P;
    r.mcid = 0;
    regions.push_back(r);

    tagger.tagPageContent(document_.get(), 0, regions);

    // Save and reopen to verify
    auto tempPath = std::filesystem::temp_directory_path() / "test_tag_bdc.pdf";
    document_->saveAs(tempPath);
    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    EXPECT_TRUE(tagger.pageHasMarkedContent(doc2.get(), 0));

    doc2.reset();
    std::filesystem::remove(tempPath);
}

TEST_F(ContentStreamTaggerTest, TagPageContent_MultipleRegions) {
    ContentStreamTagger tagger(context_);

    std::vector<MarkedContentRegion> regions;
    regions.push_back({StructureTag::H1, 0, 0, 0});
    regions.push_back({StructureTag::P, 1, 0, 0});
    regions.push_back({StructureTag::Figure, 2, 0, 0});

    tagger.tagPageContent(document_.get(), 0, regions);

    auto tempPath = std::filesystem::temp_directory_path() / "test_tag_multi.pdf";
    document_->saveAs(tempPath);
    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    EXPECT_TRUE(tagger.pageHasMarkedContent(doc2.get(), 0));

    doc2.reset();
    std::filesystem::remove(tempPath);
}

TEST_F(ContentStreamTaggerTest, TagPageContent_PreservesRendering) {
    ContentStreamTagger tagger(context_);

    std::vector<MarkedContentRegion> regions;
    regions.push_back({StructureTag::P, 0, 0, 0});

    tagger.tagPageContent(document_.get(), 0, regions);

    auto tempPath = std::filesystem::temp_directory_path() / "test_tag_render.pdf";
    document_->saveAs(tempPath);
    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    // Should still be renderable (page count preserved)
    EXPECT_GT(doc2->pageCount(), 0);

    doc2.reset();
    std::filesystem::remove(tempPath);
}

TEST_F(ContentStreamTaggerTest, TagPageContent_EmptyRegions_NoOp) {
    ContentStreamTagger tagger(context_);

    std::vector<MarkedContentRegion> regions;
    tagger.tagPageContent(document_.get(), 0, regions);

    // Should not have added marked content
    EXPECT_FALSE(tagger.pageHasMarkedContent(document_.get(), 0));
}

TEST_F(ContentStreamTaggerTest, PageHasMarkedContent_NullDoc) {
    ContentStreamTagger tagger(context_);
    EXPECT_FALSE(tagger.pageHasMarkedContent(nullptr, 0));
}

TEST_F(ContentStreamTaggerTest, PageHasMarkedContent_InvalidPage) {
    ContentStreamTagger tagger(context_);
    EXPECT_FALSE(tagger.pageHasMarkedContent(document_.get(), 999));
}

TEST_F(ContentStreamTaggerTest, TagPageContent_RoundtripValid) {
    ContentStreamTagger tagger(context_);

    std::vector<MarkedContentRegion> regions;
    regions.push_back({StructureTag::P, 0, 0, 0});

    tagger.tagPageContent(document_.get(), 0, regions);

    // Save, reopen, save again -- should still be valid
    auto tempPath1 = std::filesystem::temp_directory_path() / "test_tag_rt1.pdf";
    document_->saveAs(tempPath1);
    auto doc2 = pdfe::core::Document::open(context_, tempPath1);
    ASSERT_NE(doc2, nullptr);

    auto tempPath2 = std::filesystem::temp_directory_path() / "test_tag_rt2.pdf";
    doc2->saveAs(tempPath2);
    auto doc3 = pdfe::core::Document::open(context_, tempPath2);
    ASSERT_NE(doc3, nullptr);

    EXPECT_GT(doc3->pageCount(), 0);

    doc3.reset();
    doc2.reset();
    std::filesystem::remove(tempPath1);
    std::filesystem::remove(tempPath2);
}
