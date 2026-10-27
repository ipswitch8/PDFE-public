// PHASE3_ROUNDTRIP_ANNOTATIONS: mutate -> save -> reopen -> assert
// tests for the annotation subsystem. Closes the gap that the audit
// found in test_annotation_manager.cpp: those tests assert internal
// AnnotationData fields after createAnnotation / setContents / etc.
// but never re-open the saved PDF and verify the annotation
// actually landed in the bytes. This file does.
//
// Pattern across every TEST_F here:
//   1. Open a fresh fixture document.
//   2. Mutate (create / modify / delete an annotation).
//   3. Save via SaveAndReopen helper.
//   4. Re-query the AnnotationManager on the reopened document.
//   5. Assert the mutation survived the save round-trip.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "helpers/RoundTripHelper.h"

#include <filesystem>
#include <memory>

using pdfe::annotations::AnnotationData;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;

namespace {

class AnnotationRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_))
            << "test_3page.pdf fixture missing at " << fixture_;
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<AnnotationManager>(ctx_);

        // Per-test temp dir under the system temp.
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_annot_roundtrip" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        manager_.reset();
        document_.reset();
        ctx_.reset();
        // Leave temp files behind for post-mortem; OS will reap.
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<AnnotationManager> manager_;
};

// ─── Sticky note ─────────────────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, StickyNoteCreateSurvivesSave) {
    auto page = document_->page(0);
    QRectF rect(100, 100, 50, 50);
    AnnotationData created = manager_->createAnnotation(
        page.get(), AnnotationType::Text, rect);
    manager_->setContents(page.get(), created.annotIndex,
                          "round-trip sticky");
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);

    auto reopenedAnnots = manager_->getAnnotations(reopened->page(0).get());
    ASSERT_GE(reopenedAnnots.size(), 1u);
    bool found = false;
    for (const auto& a : reopenedAnnots) {
        if (a.type == AnnotationType::Text &&
            a.contents == "round-trip sticky") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Sticky-note contents did not survive the save "
                          "round-trip";
}

TEST_F(AnnotationRoundTripTest, StickyNoteColorSurvivesSave) {
    auto page = document_->page(0);
    AnnotationData created = manager_->createAnnotation(
        page.get(), AnnotationType::Text, QRectF(100, 100, 50, 50));
    manager_->setColor(page.get(), created.annotIndex,
                       QColor(255, 128, 64));
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto annots = manager_->getAnnotations(reopened->page(0).get());
    ASSERT_GE(annots.size(), 1u);
    EXPECT_EQ(annots[0].color.red(),   255);
    EXPECT_EQ(annots[0].color.green(), 128);
    EXPECT_EQ(annots[0].color.blue(),  64);
}

// ─── Highlight ───────────────────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, HighlightCreateSurvivesSave) {
    auto page = document_->page(0);
    AnnotationData created = manager_->createAnnotation(
        page.get(), AnnotationType::Highlight,
        QRectF(50, 200, 100, 20));
    manager_->setContents(page.get(), created.annotIndex,
                          "highlighted note");
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto annots = manager_->getAnnotations(reopened->page(0).get());

    bool foundHighlight = false;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::Highlight) {
            foundHighlight = true;
            EXPECT_EQ(a.contents, "highlighted note");
            break;
        }
    }
    EXPECT_TRUE(foundHighlight)
        << "Highlight annotation did not survive the save round-trip";
}

// ─── FreeText ────────────────────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, FreeTextCreateSurvivesSave) {
    auto page = document_->page(0);
    AnnotationData created = manager_->createAnnotation(
        page.get(), AnnotationType::FreeText,
        QRectF(50, 250, 200, 40));
    manager_->setContents(page.get(), created.annotIndex,
                          "free text content");
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto annots = manager_->getAnnotations(reopened->page(0).get());

    bool found = false;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::FreeText &&
            a.contents == "free text content") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "FreeText annotation did not survive save";
}

// ─── Ink ─────────────────────────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, InkAnnotationSurvivesSave) {
    auto page = document_->page(0);
    AnnotationData created = manager_->createAnnotation(
        page.get(), AnnotationType::Ink,
        QRectF(60, 300, 150, 100));
    std::vector<std::vector<QPointF>> strokes = {
        {QPointF(60, 300), QPointF(80, 320), QPointF(100, 310)}
    };
    manager_->setInkStrokes(page.get(), created.annotIndex, strokes);
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto annots = manager_->getAnnotations(reopened->page(0).get());
    bool found = false;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::Ink) {
            found = true;
            EXPECT_GE(a.inkStrokes.size(), 1u)
                << "Ink stroke count must survive save";
            break;
        }
    }
    EXPECT_TRUE(found) << "Ink annotation did not survive save";
}

// ─── Square ─────────────────────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, ShapeSquareSurvivesSave) {
    auto page = document_->page(0);
    AnnotationData created = manager_->createAnnotation(
        page.get(), AnnotationType::Square,
        QRectF(200, 200, 100, 80));
    manager_->setColor(page.get(), created.annotIndex, QColor(0, 200, 50));
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto annots = manager_->getAnnotations(reopened->page(0).get());

    bool found = false;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::Square) {
            found = true;
            EXPECT_EQ(a.color.green(), 200);
            break;
        }
    }
    EXPECT_TRUE(found) << "Square shape did not survive save";
}

// ─── Delete + reopen ─────────────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, DeleteAnnotationVanishesAfterSave) {
    auto page = document_->page(0);
    auto a1 = manager_->createAnnotation(page.get(), AnnotationType::Text,
                                          QRectF(100, 100, 50, 50));
    auto a2 = manager_->createAnnotation(page.get(), AnnotationType::Text,
                                          QRectF(200, 100, 50, 50));
    manager_->setContents(page.get(), a1.annotIndex, "keep me");
    manager_->setContents(page.get(), a2.annotIndex, "delete me");

    manager_->deleteAnnotation(page.get(), a2.annotIndex);
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto annots = manager_->getAnnotations(reopened->page(0).get());
    bool sawKeep = false, sawDelete = false;
    for (const auto& a : annots) {
        if (a.contents == "keep me")   sawKeep = true;
        if (a.contents == "delete me") sawDelete = true;
    }
    EXPECT_TRUE(sawKeep)
        << "Surviving annotation must still be present after save";
    EXPECT_FALSE(sawDelete)
        << "Deleted annotation must NOT reappear in the saved PDF";
}

// ─── Modify contents -> reopen ───────────────────────────────────────

TEST_F(AnnotationRoundTripTest, ModifyContentsSurvivesSave) {
    auto page = document_->page(0);
    auto a = manager_->createAnnotation(page.get(), AnnotationType::Text,
                                         QRectF(100, 100, 50, 50));
    manager_->setContents(page.get(), a.annotIndex, "before");

    // Modify in place.
    manager_->setContents(page.get(), a.annotIndex, "after");
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto annots = manager_->getAnnotations(reopened->page(0).get());
    ASSERT_GE(annots.size(), 1u);
    bool sawAfter = false, sawBefore = false;
    for (const auto& x : annots) {
        if (x.contents == "after")  sawAfter = true;
        if (x.contents == "before") sawBefore = true;
    }
    EXPECT_TRUE(sawAfter)
        << "Modified contents must be present after save";
    EXPECT_FALSE(sawBefore)
        << "Original contents must be replaced, not duplicated";
}

// ─── Modify color -> reopen ──────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, ModifyColorSurvivesSave) {
    auto page = document_->page(0);
    auto a = manager_->createAnnotation(page.get(), AnnotationType::Text,
                                         QRectF(100, 100, 50, 50));
    manager_->setColor(page.get(), a.annotIndex, QColor(0, 0, 0));
    manager_->setColor(page.get(), a.annotIndex, QColor(255, 0, 255));
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto annots = manager_->getAnnotations(reopened->page(0).get());
    ASSERT_GE(annots.size(), 1u);
    EXPECT_EQ(annots[0].color.red(),   255);
    EXPECT_EQ(annots[0].color.green(), 0);
    EXPECT_EQ(annots[0].color.blue(),  255);
}

// ─── Multi-page ─────────────────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, MultiPageAnnotationsSurviveSave) {
    auto p0 = document_->page(0);
    auto p1 = document_->page(1);
    manager_->createAnnotation(p0.get(), AnnotationType::Text,
                                QRectF(50, 50, 30, 30));
    manager_->createAnnotation(p1.get(), AnnotationType::Highlight,
                                QRectF(50, 50, 100, 20));
    p0.reset();
    p1.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto a0 = manager_->getAnnotations(reopened->page(0).get());
    auto a1 = manager_->getAnnotations(reopened->page(1).get());
    EXPECT_GE(a0.size(), 1u) << "Page 0 annotation must survive";
    EXPECT_GE(a1.size(), 1u) << "Page 1 annotation must survive";
}

// ─── File size sanity ────────────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, SavedFileIsNonEmpty) {
    auto page = document_->page(0);
    manager_->createAnnotation(page.get(), AnnotationType::Text,
                                QRectF(100, 100, 50, 50));
    page.reset();

    auto result = pdfe::test::SaveAndReopenWithPath(*document_, ctx_,
                                                     tempDir_);
    ASSERT_NE(result.doc, nullptr);
    EXPECT_GT(std::filesystem::file_size(result.path), 0u)
        << "saveAs must produce a non-empty file";
    EXPECT_GE(result.doc->pageCount(), 3)
        << "Reopened document must preserve page count of fixture";
}

// ─── Idempotency ─────────────────────────────────────────────────────

TEST_F(AnnotationRoundTripTest, DoubleRoundTripIsIdempotent) {
    auto page = document_->page(0);
    auto a = manager_->createAnnotation(page.get(), AnnotationType::Text,
                                         QRectF(100, 100, 50, 50));
    manager_->setContents(page.get(), a.annotIndex, "idempotent");
    page.reset();

    auto first = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_,
                                            "first");
    ASSERT_NE(first, nullptr);
    auto second = pdfe::test::SaveAndReopen(*first, ctx_, tempDir_,
                                             "second");
    ASSERT_NE(second, nullptr);
    auto annots = manager_->getAnnotations(second->page(0).get());
    bool found = false;
    for (const auto& x : annots) {
        if (x.contents == "idempotent") { found = true; break; }
    }
    EXPECT_TRUE(found)
        << "Annotation must survive two save/reopen cycles unchanged";
}

} // namespace
