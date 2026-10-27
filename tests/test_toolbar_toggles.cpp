#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include <filesystem>

class ToolbarTogglesTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_single.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = pdfe::core::Document::open(context, testPdf);
        }
    }

    void TearDown() override {
        document.reset();
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<pdfe::core::Document> document;
};

TEST_F(ToolbarTogglesTest, AnnotationFilter_DefaultShowAll) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // New document should have ShowAll filter by default
    pdfe::core::AnnotationFilter filter = document->annotationFilter();
    EXPECT_EQ(filter, pdfe::core::AnnotationFilter::ShowAll);
}

TEST_F(ToolbarTogglesTest, AnnotationFilter_SetHideStickyNotes) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Set the filter to HideStickyNotes
    document->setAnnotationFilter(pdfe::core::AnnotationFilter::HideStickyNotes);

    // Verify it was set correctly
    pdfe::core::AnnotationFilter filter = document->annotationFilter();
    EXPECT_EQ(filter, pdfe::core::AnnotationFilter::HideStickyNotes);
}

TEST_F(ToolbarTogglesTest, AnnotationFilter_FilteredDisplayListCreated) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Set filter to HideStickyNotes
    document->setAnnotationFilter(pdfe::core::AnnotationFilter::HideStickyNotes);

    // Create display list with filter applied
    void* displayList = document->createDisplayList(0);

    // Verify display list was created (non-null)
    ASSERT_NE(displayList, nullptr);

    // Clean up
    document->releaseDisplayList(displayList);
}

TEST_F(ToolbarTogglesTest, AnnotationFilter_ShowAllDisplayListCreated) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Ensure filter is ShowAll (default)
    document->setAnnotationFilter(pdfe::core::AnnotationFilter::ShowAll);

    // Create display list with all annotations visible
    void* displayList = document->createDisplayList(0);

    // Verify display list was created (non-null)
    ASSERT_NE(displayList, nullptr);

    // Clean up
    document->releaseDisplayList(displayList);
}

TEST_F(ToolbarTogglesTest, AnnotationFilter_ToggleBetweenStates) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Toggle between filters
    document->setAnnotationFilter(pdfe::core::AnnotationFilter::HideStickyNotes);
    EXPECT_EQ(document->annotationFilter(), pdfe::core::AnnotationFilter::HideStickyNotes);

    document->setAnnotationFilter(pdfe::core::AnnotationFilter::ShowAll);
    EXPECT_EQ(document->annotationFilter(), pdfe::core::AnnotationFilter::ShowAll);

    document->setAnnotationFilter(pdfe::core::AnnotationFilter::HideStickyNotes);
    EXPECT_EQ(document->annotationFilter(), pdfe::core::AnnotationFilter::HideStickyNotes);
}

TEST_F(ToolbarTogglesTest, AnnotationFilter_MultipleDisplayListsWithDifferentFilters) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Create display list with ShowAll
    document->setAnnotationFilter(pdfe::core::AnnotationFilter::ShowAll);
    void* displayList1 = document->createDisplayList(0);
    ASSERT_NE(displayList1, nullptr);

    // Create display list with HideStickyNotes
    document->setAnnotationFilter(pdfe::core::AnnotationFilter::HideStickyNotes);
    void* displayList2 = document->createDisplayList(0);
    ASSERT_NE(displayList2, nullptr);

    // Both should be valid (though potentially different)
    EXPECT_NE(displayList1, nullptr);
    EXPECT_NE(displayList2, nullptr);

    // Clean up
    document->releaseDisplayList(displayList1);
    document->releaseDisplayList(displayList2);
}

TEST_F(ToolbarTogglesTest, AnnotationFilter_ResetOnNewDocument) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Set filter to HideStickyNotes
    document->setAnnotationFilter(pdfe::core::AnnotationFilter::HideStickyNotes);
    EXPECT_EQ(document->annotationFilter(), pdfe::core::AnnotationFilter::HideStickyNotes);

    // Close and reopen document
    document.reset();
    document = pdfe::core::Document::open(context, testPdf);

    if (!document) {
        GTEST_SKIP() << "Could not reopen test PDF";
    }

    // Filter should be reset to default (ShowAll)
    pdfe::core::AnnotationFilter filter = document->annotationFilter();
    EXPECT_EQ(filter, pdfe::core::AnnotationFilter::ShowAll);
}

TEST_F(ToolbarTogglesTest, DisplayList_ReleaseNullIsNoOp) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Releasing null display list should not crash
    EXPECT_NO_THROW(document->releaseDisplayList(nullptr));
}
