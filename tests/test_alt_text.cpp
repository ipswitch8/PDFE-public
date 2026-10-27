// -*- coding: utf-8 -*-
// Phase 8c: alt text editing — /Alt and /ActualText round-trip through
// save and reload on annotations.
#include <gtest/gtest.h>
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <memory>

using namespace pdfe::annotations;
using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path tempOut(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_alttext_tests";
    std::filesystem::create_directories(d);
    return d / name;
}

} // namespace

class AltTextTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto src = fixturesDir() / "test_single.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        work = tempOut("alt_work.pdf");
        std::filesystem::copy_file(src, work,
            std::filesystem::copy_options::overwrite_existing);
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path work;
};

TEST_F(AltTextTest, SetAndGetAltText)
{
    auto doc = Document::open(ctx, work);
    auto page = doc->page(0);
    AnnotationManager mgr(ctx);
    auto a = mgr.createAnnotation(page.get(), AnnotationType::Square,
                                  QRectF(50, 50, 100, 100));

    EXPECT_EQ(mgr.getAltText(page.get(), a.annotIndex), "");
    mgr.setAltText(page.get(), a.annotIndex, "A picture of a cat");
    EXPECT_EQ(mgr.getAltText(page.get(), a.annotIndex),
              std::string("A picture of a cat"));
}

TEST_F(AltTextTest, SetEmptyAltTextClearsIt)
{
    auto doc = Document::open(ctx, work);
    auto page = doc->page(0);
    AnnotationManager mgr(ctx);
    auto a = mgr.createAnnotation(page.get(), AnnotationType::Square,
                                  QRectF(50, 50, 100, 100));
    mgr.setAltText(page.get(), a.annotIndex, "temporary");
    ASSERT_EQ(mgr.getAltText(page.get(), a.annotIndex), "temporary");
    mgr.setAltText(page.get(), a.annotIndex, "");
    EXPECT_EQ(mgr.getAltText(page.get(), a.annotIndex), "");
}

TEST_F(AltTextTest, SetAndGetActualText)
{
    auto doc = Document::open(ctx, work);
    auto page = doc->page(0);
    AnnotationManager mgr(ctx);
    auto a = mgr.createAnnotation(page.get(), AnnotationType::Square,
                                  QRectF(50, 50, 100, 100));

    EXPECT_EQ(mgr.getActualText(page.get(), a.annotIndex), "");
    mgr.setActualText(page.get(), a.annotIndex, "H2O");
    EXPECT_EQ(mgr.getActualText(page.get(), a.annotIndex), std::string("H2O"));
}

TEST_F(AltTextTest, AltAndActualAreIndependentFields)
{
    auto doc = Document::open(ctx, work);
    auto page = doc->page(0);
    AnnotationManager mgr(ctx);
    auto a = mgr.createAnnotation(page.get(), AnnotationType::Square,
                                  QRectF(50, 50, 100, 100));
    mgr.setAltText(page.get(), a.annotIndex, "alt-value");
    mgr.setActualText(page.get(), a.annotIndex, "actual-value");

    EXPECT_EQ(mgr.getAltText(page.get(), a.annotIndex), "alt-value");
    EXPECT_EQ(mgr.getActualText(page.get(), a.annotIndex), "actual-value");

    // Clearing one should not affect the other.
    mgr.setAltText(page.get(), a.annotIndex, "");
    EXPECT_EQ(mgr.getAltText(page.get(), a.annotIndex), "");
    EXPECT_EQ(mgr.getActualText(page.get(), a.annotIndex), "actual-value");
}

TEST_F(AltTextTest, AltTextSurvivesSaveAndReload)
{
    int savedIdx = -1;
    {
        auto doc = Document::open(ctx, work);
        auto page = doc->page(0);
        AnnotationManager mgr(ctx);
        auto a = mgr.createAnnotation(page.get(), AnnotationType::Square,
                                      QRectF(50, 50, 100, 100));
        mgr.setAltText(page.get(), a.annotIndex, "Persistent alt text");
        mgr.setActualText(page.get(), a.annotIndex, "Persistent actual");
        savedIdx = a.annotIndex;

        auto saved = tempOut("alt_saved.pdf");
        doc->saveAs(saved);
        work = saved;
    }
    auto doc2 = Document::open(ctx, work);
    auto page2 = doc2->page(0);
    AnnotationManager mgr2(ctx);
    EXPECT_EQ(mgr2.getAltText(page2.get(), savedIdx),
              std::string("Persistent alt text"));
    EXPECT_EQ(mgr2.getActualText(page2.get(), savedIdx),
              std::string("Persistent actual"));
}

TEST_F(AltTextTest, SetAltTextOnBogusIndexThrows)
{
    auto doc = Document::open(ctx, work);
    auto page = doc->page(0);
    AnnotationManager mgr(ctx);
    EXPECT_THROW(mgr.setAltText(page.get(), 99, "x"), std::exception);
    EXPECT_THROW(mgr.setActualText(page.get(), 99, "y"), std::exception);
}
