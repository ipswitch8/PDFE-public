#include <gtest/gtest.h>
#include "annotations/SelectTool.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationCommands.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <filesystem>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QImage>

class SelectToolEnhancedTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_single.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = pdfe::core::Document::open(context, testPdf);
            manager = std::make_unique<pdfe::annotations::AnnotationManager>(context);

            if (document && document->pageCount() > 0) {
                pagePtr = document->page(0);
                page = pagePtr.get();
            }
        }
    }

    // Helper to create and select an annotation
    void createAndSelectAnnotation(pdfe::annotations::AnnotationType type, QRectF rect) {
        if (!page) return;

        // Create annotation
        annotData = manager->createAnnotation(page, type, rect);

        // The newly created annotation is the last one in the list
        auto annotations = manager->getAnnotations(page);
        ASSERT_GT(annotations.size(), 0u);
        annotIndex = static_cast<int>(annotations.size()) - 1;
        // Update rect to what MuPDF actually stored (may differ slightly)
        annotData = annotations[annotIndex];

        // Initialize tool and paint once to set lastZoom_
        tool = std::make_unique<pdfe::annotations::SelectTool>(manager.get());
        tool->setPage(page);

        QImage image(800, 600, QImage::Format_ARGB32);
        QPainter painter(&image);
        tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);

        // Select the annotation by clicking on it
        QPointF center = rect.center();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(center.x(), center.y()),
                         QPointF(center.x(), center.y()),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool->onMousePress(&press, 0, center);

        ASSERT_EQ(tool->selectedAnnotIndex(), annotIndex);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::annotations::AnnotationManager> manager;
    std::unique_ptr<pdfe::core::Page> pagePtr;
    pdfe::core::Page* page = nullptr;
    std::unique_ptr<pdfe::annotations::SelectTool> tool;
    pdfe::annotations::AnnotationData annotData;
    int annotIndex = -1;
};

TEST_F(SelectToolEnhancedTest, HandleCornerResize_TopLeft) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));
    QRectF actualRect = annotData.rect; // Use MuPDF-stored rect

    bool moveCallbackFired = false;
    QRectF oldRect, newRect;

    tool->annotationMoved = [&](int pageIdx, int annotIdx, QRectF oldR, QRectF newR) {
        moveCallbackFired = true;
        oldRect = oldR;
        newRect = newR;
    };

    // Simulate dragging the top-left corner
    QPointF topLeft = actualRect.topLeft();
    QPointF newTopLeft(topLeft.x() + 10, topLeft.y() + 10);

    QMouseEvent press(QEvent::MouseButtonPress, topLeft, topLeft,
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, newTopLeft, newTopLeft,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, newTopLeft, newTopLeft,
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

    tool->onMousePress(&press, 0, topLeft);
    tool->onMouseMove(&move, 0, newTopLeft);
    tool->onMouseRelease(&release, 0, newTopLeft);

    EXPECT_TRUE(moveCallbackFired);
    EXPECT_EQ(oldRect, actualRect);
    // Top-left corner moved, so rect should be smaller
    EXPECT_GT(newRect.left(), actualRect.left());
    EXPECT_GT(newRect.top(), actualRect.top());
}

TEST_F(SelectToolEnhancedTest, HandleCornerResize_BottomRight) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));
    QRectF actualRect = annotData.rect;

    bool moveCallbackFired = false;
    QRectF newRect;

    tool->annotationMoved = [&](int pageIdx, int annotIdx, QRectF oldR, QRectF newR) {
        moveCallbackFired = true;
        newRect = newR;
    };

    // Simulate dragging the bottom-right corner outward
    QPointF bottomRight = actualRect.bottomRight();
    QPointF newBottomRight(bottomRight.x() + 20, bottomRight.y() + 20);

    QMouseEvent press(QEvent::MouseButtonPress, bottomRight, bottomRight,
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, newBottomRight, newBottomRight,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, newBottomRight, newBottomRight,
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

    tool->onMousePress(&press, 0, bottomRight);
    tool->onMouseMove(&move, 0, newBottomRight);
    tool->onMouseRelease(&release, 0, newBottomRight);

    EXPECT_TRUE(moveCallbackFired);
    // Bottom-right corner moved outward, so rect should be larger
    EXPECT_GT(newRect.width(), actualRect.width());
    EXPECT_GT(newRect.height(), actualRect.height());
}

TEST_F(SelectToolEnhancedTest, HandleCornerResize_MinSize) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));
    QRectF actualRect = annotData.rect;

    QRectF finalRect;

    tool->annotationMoved = [&](int pageIdx, int annotIdx, QRectF oldR, QRectF newR) {
        finalRect = newR;
    };

    // Try to resize to smaller than minimum (10pt)
    QPointF bottomRight = actualRect.bottomRight();
    QPointF tinySize(actualRect.left() + 5, actualRect.top() + 5); // 5x5, below minimum

    QMouseEvent press(QEvent::MouseButtonPress, bottomRight, bottomRight,
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, tinySize, tinySize,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, tinySize, tinySize,
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

    tool->onMousePress(&press, 0, bottomRight);
    tool->onMouseMove(&move, 0, tinySize);
    tool->onMouseRelease(&release, 0, tinySize);

    // The minimum size should be enforced (10pt)
    EXPECT_GE(finalRect.width(), 10.0f);
    EXPECT_GE(finalRect.height(), 10.0f);
}

TEST_F(SelectToolEnhancedTest, MoveAnnotation_FiresCallback) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));
    QRectF actualRect = annotData.rect;

    bool moveCallbackFired = false;
    int movedPageIndex = -1;
    int movedAnnotIndex = -1;
    QRectF oldRect, newRect;

    tool->annotationMoved = [&](int pageIdx, int annotIdx, QRectF oldR, QRectF newR) {
        moveCallbackFired = true;
        movedPageIndex = pageIdx;
        movedAnnotIndex = annotIdx;
        oldRect = oldR;
        newRect = newR;
    };

    // Move annotation by dragging from center
    QPointF center = actualRect.center();
    QPointF newCenter(center.x() + 30, center.y() + 30);

    QMouseEvent press(QEvent::MouseButtonPress, center, center,
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, newCenter, newCenter,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, newCenter, newCenter,
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

    tool->onMousePress(&press, 0, center);
    tool->onMouseMove(&move, 0, newCenter);
    tool->onMouseRelease(&release, 0, newCenter);

    EXPECT_TRUE(moveCallbackFired);
    EXPECT_EQ(movedPageIndex, 0);
    EXPECT_EQ(movedAnnotIndex, annotIndex);
    EXPECT_EQ(oldRect, actualRect);
    EXPECT_NE(newRect, oldRect);
}

TEST_F(SelectToolEnhancedTest, DeleteKey_DeletesSelection) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));

    bool deleteCallbackFired = false;
    int deletedPageIndex = -1;
    int deletedAnnotIndex = -1;

    tool->annotationDeleted = [&](int pageIdx, int annotIdx) {
        deleteCallbackFired = true;
        deletedPageIndex = pageIdx;
        deletedAnnotIndex = annotIdx;
    };

    // Simulate Delete key press
    QKeyEvent deleteKey(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    tool->onKeyPress(&deleteKey);

    EXPECT_TRUE(deleteCallbackFired);
    EXPECT_EQ(deletedPageIndex, 0);
    EXPECT_EQ(deletedAnnotIndex, annotIndex);
}

TEST_F(SelectToolEnhancedTest, CopyPaste_DuplicatesAnnotation) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));
    QRectF actualRect = annotData.rect;

    bool pasteCallbackFired = false;
    pdfe::annotations::AnnotationData pastedData;

    tool->annotationPasted = [&](int pageIdx, pdfe::annotations::AnnotationData data) {
        pasteCallbackFired = true;
        pastedData = data;
    };

    // Simulate Ctrl+C (Copy)
    QKeyEvent copyKey(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, "c");
    tool->onKeyPress(&copyKey);

    // Simulate Ctrl+V (Paste)
    QKeyEvent pasteKey(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier, "v");
    tool->onKeyPress(&pasteKey);

    EXPECT_TRUE(pasteCallbackFired);
    EXPECT_EQ(pastedData.type, pdfe::annotations::AnnotationType::Square);
    // Pasted annotation should be offset from original
    EXPECT_NE(pastedData.rect, actualRect);
}

TEST_F(SelectToolEnhancedTest, DoubleClick_StickyNote_FiresCallback) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Text, QRectF(100, 100, 20, 20));
    QRectF actualRect = annotData.rect;

    bool doubleClickFired = false;
    int clickedPageIndex = -1;
    int clickedAnnotIndex = -1;
    pdfe::annotations::AnnotationType clickedType = pdfe::annotations::AnnotationType::Unknown;

    tool->stickyNoteDoubleClicked = [&](int pageIdx, int annotIdx, pdfe::annotations::AnnotationData data) {
        doubleClickFired = true;
        clickedPageIndex = pageIdx;
        clickedAnnotIndex = annotIdx;
        clickedType = data.type;
    };

    // Simulate double-click on sticky note
    QPointF center = actualRect.center();
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, center, center,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMouseDoubleClick(&doubleClick, 0, center);

    EXPECT_TRUE(doubleClickFired);
    EXPECT_EQ(clickedPageIndex, 0);
    EXPECT_EQ(clickedAnnotIndex, annotIndex);
    EXPECT_EQ(clickedType, pdfe::annotations::AnnotationType::Text);
}

TEST_F(SelectToolEnhancedTest, DoubleClick_NonStickyNote_NoCallback) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));
    QRectF actualRect = annotData.rect;

    bool doubleClickFired = false;

    tool->stickyNoteDoubleClicked = [&](int pageIdx, int annotIdx, pdfe::annotations::AnnotationData data) {
        doubleClickFired = true;
    };

    // Simulate double-click on square annotation (not a sticky note)
    QPointF center = actualRect.center();
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, center, center,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMouseDoubleClick(&doubleClick, 0, center);

    // Should NOT fire for non-sticky note
    EXPECT_FALSE(doubleClickFired);
}

TEST_F(SelectToolEnhancedTest, ContextMenu_HasThreeActions) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));
    QRectF actualRect = annotData.rect;

    QPointF center = actualRect.center();
    QMouseEvent rightClick(QEvent::MouseButtonPress, center, center,
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);

    QMenu* menu = tool->onContextMenu(&rightClick, 0, center);

    ASSERT_NE(menu, nullptr);

    // Verify menu has at least 3 actions (Copy, Delete, Properties)
    QList<QAction*> actions = menu->actions();
    EXPECT_GE(actions.size(), 3);

    // Check for expected action texts
    bool hasCopy = false, hasDelete = false, hasProperties = false;
    for (QAction* action : actions) {
        QString text = action->text().toLower();
        if (text.contains("copy")) hasCopy = true;
        if (text.contains("delete")) hasDelete = true;
        if (text.contains("properties")) hasProperties = true;
    }

    EXPECT_TRUE(hasCopy);
    EXPECT_TRUE(hasDelete);
    EXPECT_TRUE(hasProperties);

    delete menu;
}

TEST_F(SelectToolEnhancedTest, DoubleClick_FreeText_FiresCallback) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::FreeText, QRectF(100, 100, 80, 40));
    QRectF actualRect = annotData.rect;

    bool doubleClickFired = false;
    pdfe::annotations::AnnotationType clickedType = pdfe::annotations::AnnotationType::Unknown;

    tool->stickyNoteDoubleClicked = [&](int pageIdx, int annotIdx, pdfe::annotations::AnnotationData data) {
        doubleClickFired = true;
        clickedType = data.type;
    };

    // Simulate double-click on FreeText annotation
    QPointF center = actualRect.center();
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, center, center,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMouseDoubleClick(&doubleClick, 0, center);

    // Should fire for FreeText
    EXPECT_TRUE(doubleClickFired);
    EXPECT_EQ(clickedType, pdfe::annotations::AnnotationType::FreeText);
}

TEST_F(SelectToolEnhancedTest, SingleClick_StickyNote_FiresPreview) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Create a Text annotation (sticky note)
    if (!page) return;
    annotData = manager->createAnnotation(page, pdfe::annotations::AnnotationType::Text, QRectF(200, 200, 20, 20));
    auto annotations = manager->getAnnotations(page);
    ASSERT_GT(annotations.size(), 0u);
    annotIndex = static_cast<int>(annotations.size()) - 1;
    annotData = annotations[annotIndex];

    tool = std::make_unique<pdfe::annotations::SelectTool>(manager.get());
    tool->setPage(page);

    QImage image(800, 600, QImage::Format_ARGB32);
    QPainter painter(&image);
    tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);

    bool previewFired = false;
    int previewAnnotIndex = -1;

    tool->annotationPreviewRequested = [&](int pageIdx, int annotIdx, pdfe::annotations::AnnotationData data) {
        previewFired = true;
        previewAnnotIndex = annotIdx;
    };

    // Single-click on the sticky note
    QPointF center = annotData.rect.center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(center.x(), center.y()),
                     QPointF(center.x(), center.y()),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, center);

    EXPECT_TRUE(previewFired);
    EXPECT_EQ(previewAnnotIndex, annotIndex);
}

TEST_F(SelectToolEnhancedTest, SingleClick_Square_NoPreview) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));

    bool previewFired = false;

    tool->annotationPreviewRequested = [&](int pageIdx, int annotIdx, pdfe::annotations::AnnotationData data) {
        previewFired = true;
    };

    // Click on the square — no preview should fire (not Text/FreeText)
    // Note: the annotation is already selected from createAndSelectAnnotation,
    // so we click somewhere else first to clear, then click on it again
    QPointF empty(400, 400);
    QMouseEvent clearClick(QEvent::MouseButtonPress, empty, empty,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&clearClick, 0, empty);

    QPointF center = annotData.rect.center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(center.x(), center.y()),
                     QPointF(center.x(), center.y()),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, center);

    EXPECT_FALSE(previewFired);
}

TEST_F(SelectToolEnhancedTest, ClearSelection_FiresCallback) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));

    ASSERT_NE(tool->selectedAnnotIndex(), -1);

    bool clearCallbackFired = false;

    tool->selectionCleared = [&]() {
        clearCallbackFired = true;
    };

    // Click on empty space to clear selection
    QPointF emptySpace(300, 300); // Far from the annotation
    QMouseEvent click(QEvent::MouseButtonPress, emptySpace, emptySpace,
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&click, 0, emptySpace);

    EXPECT_TRUE(clearCallbackFired);
    EXPECT_EQ(tool->selectedAnnotIndex(), -1);
}

// ─────────────────────────────────────────────────────────────────────
// UI-CTX-03 — annotation right-click context menu must offer
// Properties / Delete / Reply for comment-style annotations (Text /
// FreeText). Reply must NOT appear for non-comment types (Square,
// Ink, Highlight, etc.) since /IRT replies have no PDF semantics
// for those.
// ─────────────────────────────────────────────────────────────────────
TEST_F(SelectToolEnhancedTest, CTX03_StickyNoteContextMenuOffersReply) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Sticky note (PDF AnnotationType::Text) — Reply must appear.
    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Text,
                              QRectF(120, 120, 20, 20));
    QPointF center = annotData.rect.center();
    QMouseEvent rightClick(QEvent::MouseButtonPress, center, center,
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QMenu* menu = tool->onContextMenu(&rightClick, 0, center);
    ASSERT_NE(menu, nullptr);

    auto* replyAction = menu->findChild<QAction*>("contextReplyAction");
    ASSERT_NE(replyAction, nullptr)
        << "UI-CTX-03: Reply action must appear on sticky-note "
           "right-click — /IRT chains attach to Text annotations.";
    EXPECT_TRUE(replyAction->text().toLower().contains("reply"));

    delete menu;
}

TEST_F(SelectToolEnhancedTest, CTX03_FreeTextContextMenuOffersReply) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::FreeText,
                              QRectF(150, 150, 80, 40));
    QPointF center = annotData.rect.center();
    QMouseEvent rightClick(QEvent::MouseButtonPress, center, center,
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QMenu* menu = tool->onContextMenu(&rightClick, 0, center);
    ASSERT_NE(menu, nullptr);

    auto* replyAction = menu->findChild<QAction*>("contextReplyAction");
    ASSERT_NE(replyAction, nullptr)
        << "UI-CTX-03: FreeText annotations also accept /IRT "
           "replies; Reply action must appear.";

    delete menu;
}

TEST_F(SelectToolEnhancedTest, CTX03_SquareContextMenuOmitsReply) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Square is a non-comment annotation — Reply has no semantic
    // meaning and must NOT appear.
    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square,
                              QRectF(100, 100, 50, 50));
    QPointF center = annotData.rect.center();
    QMouseEvent rightClick(QEvent::MouseButtonPress, center, center,
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QMenu* menu = tool->onContextMenu(&rightClick, 0, center);
    ASSERT_NE(menu, nullptr);

    auto* replyAction = menu->findChild<QAction*>("contextReplyAction");
    EXPECT_EQ(replyAction, nullptr)
        << "UI-CTX-03: Reply action must NOT appear on non-comment "
           "annotations like Square; /IRT replies have no PDF spec "
           "meaning for those.";

    // Properties + Delete + Copy are still expected.
    EXPECT_NE(menu->findChild<QAction*>("contextCopyAction"), nullptr);
    EXPECT_NE(menu->findChild<QAction*>("contextDeleteAction"), nullptr);
    EXPECT_NE(menu->findChild<QAction*>("contextPropertiesAction"), nullptr);

    delete menu;
}

TEST_F(SelectToolEnhancedTest, CTX03_StickyNoteReplyTriggersCallback) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Text,
                              QRectF(120, 120, 20, 20));

    int firedPage = -999;
    int firedAnnot = -999;
    tool->replyRequested = [&](int p, int a) {
        firedPage = p;
        firedAnnot = a;
    };

    QPointF center = annotData.rect.center();
    QMouseEvent rightClick(QEvent::MouseButtonPress, center, center,
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QMenu* menu = tool->onContextMenu(&rightClick, 0, center);
    ASSERT_NE(menu, nullptr);

    auto* replyAction = menu->findChild<QAction*>("contextReplyAction");
    ASSERT_NE(replyAction, nullptr);

    replyAction->trigger();

    EXPECT_EQ(firedPage, 0);
    EXPECT_EQ(firedAnnot, annotIndex)
        << "Reply action must invoke replyRequested with the selected "
           "annotation's pageIndex+annotIndex so MainWindow can wire "
           "it through AnnotationManager::setInReplyTo (UI-CTX-03).";

    delete menu;
}

// Regression (paint-time use-after-free): onDocumentClosing() must drop the
// cached raw Page* and the multi-selection, so a repaint queued after the
// document's pages are freed cannot deref a dangling page_ in onPaintOverlay
// (SelectTool::onPaintOverlay -> AnnotationManager::getAnnotation ->
// Page::fzPage on 0xdd… freed memory — the 2 residual crash dumps). Pre-fix,
// SelectTool had no onDocumentClosing override (base no-op) so the selection
// survived and the next paint re-queried the freed page -> this reds.
TEST_F(SelectToolEnhancedTest, OnDocumentClosingClearsSelectionSoPaintIsPageFree) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }
    // Two annotations so a marquee yields a MULTI-selection (the only paint path
    // that re-queries the manager with the cached raw Page*).
    manager->createAnnotation(page, pdfe::annotations::AnnotationType::Square, QRectF(100, 100, 50, 50));
    manager->createAnnotation(page, pdfe::annotations::AnnotationType::Square, QRectF(200, 100, 50, 50));

    tool = std::make_unique<pdfe::annotations::SelectTool>(manager.get());
    tool->setPage(page);
    { QImage img(800, 600, QImage::Format_ARGB32); QPainter p(&img);
      tool->onPaintOverlay(p, 0, 1.0f, QPointF(0, 0), 792.0f); }

    // Rubber-band marquee over the whole page (press starts on empty space).
    const QPointF start(10, 10), end(600, 700);
    QMouseEvent press(QEvent::MouseButtonPress, start, start,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, end, end,
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, end, end,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, start);
    tool->onMouseMove(&move, 0, end);
    tool->onMouseRelease(&release, 0, end);

    ASSERT_GE(tool->selectedAnnotIndices().size(), 1u)
        << "marquee should have produced a multi-selection exercising the "
           "page_-dependent paint path";

    // Document is closing: the tool MUST drop the cached page + selection.
    tool->onDocumentClosing();
    EXPECT_TRUE(tool->selectedAnnotIndices().empty())
        << "onDocumentClosing() did not clear the multi-selection — a repaint "
           "after the pages are freed would re-query the dangling page_ in "
           "onPaintOverlay (the paint-time use-after-free).";

    // A repaint after close must be a safe no-op (page_ null, selection empty).
    QImage img2(800, 600, QImage::Format_ARGB32);
    QPainter p2(&img2);
    EXPECT_NO_THROW(tool->onPaintOverlay(p2, 0, 1.0f, QPointF(0, 0), 792.0f));
}

// ---------------------------------------------------------------------------
// Selecting an already-rotated annotation must adopt its angle.
//
// The rotate handle's angle used to live only in SelectTool and was reset to 0
// on every selection, because there was nowhere to read it back from --
// AnnotationData had no rotation field until the fix for the 2026-08-28 field
// report (defect 2). That left two visible problems once annotations could
// actually be rotated:
//
//   * the selection outline drew upright over rotated text, and
//   * dragStartRotation_ started at 0, so the next drag of the handle applied
//     its delta from upright and the box JUMPED to the delta instead of
//     continuing from where it was.
//
// The second is the one a user notices: rotate to 45, click away, click back,
// nudge the handle, and the box snaps to the nudge.
// ---------------------------------------------------------------------------

TEST_F(SelectToolEnhancedTest, SelectingARotatedAnnotationAdoptsItsStoredAngle) {
    ASSERT_NE(page, nullptr) << "fixture not loaded: " << testPdf.string();

    const QRectF rect(100, 100, 200, 60);
    createAndSelectAnnotation(pdfe::annotations::AnnotationType::FreeText, rect);
    ASSERT_NE(tool, nullptr);

    // Rotate it through the manager, as the committed command would.
    manager->setRotation(page, annotIndex, 33.0, rect);

    // Reset the tool's own state to zero, then select again. Using setPage()
    // rather than a stray click makes the assertion unambiguous: the angle can
    // only have come from the annotation, never from leftover tool state.
    tool->setPage(page);
    ASSERT_DOUBLE_EQ(tool->rotationDegrees(), 0.0) << "precondition";
    QImage image(800, 600, QImage::Format_ARGB32);
    QPainter painter(&image);
    tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);

    auto refreshed = manager->getAnnotations(page);
    ASSERT_GT(refreshed.size(), 0u);
    const QPointF centre = refreshed[annotIndex].rect.center();
    QMouseEvent press(QEvent::MouseButtonPress, centre, centre,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, centre);

    ASSERT_EQ(tool->selectedAnnotIndex(), annotIndex)
        << "the click did not reselect the annotation";
    EXPECT_NEAR(tool->rotationDegrees(), 33.0, 1e-3)
        << "selection reset the angle to 0, so the outline would draw upright "
           "and the next rotate-handle drag would jump";
}

// ===========================================================================
// GitHub #186: an unhandled key must be IGNORED by every exit path.
//
// Qt delivers key events already ACCEPTED, and PdfCanvas::keyPressEvent takes
// its early-out on event->isAccepted() right after asking the tool. So a tool
// that returns without touching the flag has silently EATEN the key, and the
// canvas's own PageDown/PageUp/Home/End/arrow navigation never runs.
//
// That was fixed once, at the bottom of SelectTool::onKeyPress. The guard at
// the TOP kept the old bare `return`, so the same swallow survived for the
// case where the tool has no bound page — the state #186 suspects during a
// page transition. These pin both paths.
//
// The suite is the right place for this rather than a live-UI test: "was the
// event flag left set" is not observable through UIA at all, and a UI test
// could only ever see the downstream symptom (the page did not turn) which
// has a dozen other causes.
// ===========================================================================

TEST_F(SelectToolEnhancedTest, UnboundToolIgnoresNavigationKeysInsteadOfEatingThem) {
    if (!manager) GTEST_SKIP() << "fixture manager unavailable";
    // Constructed here rather than reused from the fixture: `tool` is only
    // created inside createAndSelectAnnotation(), which also BINDS a page.
    // This test needs the unbound state, so it never calls setPage().
    tool = std::make_unique<pdfe::annotations::SelectTool>(manager.get());

    for (int key : {static_cast<int>(Qt::Key_PageDown),
                    static_cast<int>(Qt::Key_PageUp),
                    static_cast<int>(Qt::Key_Home),
                    static_cast<int>(Qt::Key_End),
                    static_cast<int>(Qt::Key_Down)}) {
        QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
        // Qt's own delivery leaves the event accepted; reproduce that rather
        // than assume it, or the assertion below would pass on an event that
        // arrived un-accepted for an unrelated reason.
        ev.accept();
        ASSERT_TRUE(ev.isAccepted()) << "precondition: key " << key;

        tool->onKeyPress(&ev);

        EXPECT_FALSE(ev.isAccepted())
            << "SelectTool with no bound page CONSUMED key " << key
            << " — PdfCanvas::keyPressEvent takes its early-out on "
               "isAccepted(), so canvas navigation never runs and the key is "
               "silently swallowed (GitHub #186)";
    }
}

TEST_F(SelectToolEnhancedTest, BoundToolAlsoIgnoresKeysItDoesNotHandle) {
    // The control. Without it, the test above is satisfied by a tool that
    // ignores EVERY key, including Delete and Ctrl+C, which would be a worse
    // defect than the one being fixed.
    if (!page || !manager) GTEST_SKIP() << "fixture page unavailable";
    tool = std::make_unique<pdfe::annotations::SelectTool>(manager.get());
    tool->setPage(page);

    QKeyEvent nav(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
    nav.accept();
    tool->onKeyPress(&nav);
    EXPECT_FALSE(nav.isAccepted())
        << "a bound SelectTool swallowed PageDown";

    // ...and a key it DOES handle is still consumed.
    createAndSelectAnnotation(pdfe::annotations::AnnotationType::Square,
                              QRectF(100, 100, 80, 40));
    ASSERT_GE(tool->selectedAnnotIndex(), 0)
        << "precondition: an annotation must be selected for Delete to be "
           "handled, otherwise this control asserts nothing";
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    del.accept();
    tool->onKeyPress(&del);
    EXPECT_TRUE(del.isAccepted())
        << "Delete on a selected annotation was NOT consumed — the fix for "
           "the swallowed-navigation bug has over-corrected into ignoring "
           "keys the tool genuinely handles";
}
