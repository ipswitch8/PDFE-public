#include <gtest/gtest.h>
#include "annotations/AnnotationTool.h"
#include "annotations/StickyNoteTool.h"
#include "annotations/TextMarkupTool.h"
#include "annotations/InkTool.h"
#include "annotations/ShapeTool.h"
#include "annotations/FreeTextTool.h"
#include "annotations/StampTool.h"
#include "annotations/FileAttachmentTool.h"
#include "annotations/AnnotationManager.h"
#include "annotations/TextSelector.h"
#include "measure/MeasureTool.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>

class AnnotationToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = pdfe::core::Document::open(context, testPdf);
            manager = std::make_unique<pdfe::annotations::AnnotationManager>(context);
            selector = std::make_shared<pdfe::annotations::TextSelector>(context);

            if (document && document->pageCount() > 0) {
                pagePtr = document->page(0);
                page = pagePtr.get();
            }
        }
    }

    // Helper to create mouse events
    QMouseEvent* createMouseEvent(QEvent::Type type, Qt::MouseButton button, QPoint pos) {
        return new QMouseEvent(type, pos, button, button, Qt::NoModifier);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::annotations::AnnotationManager> manager;
    std::shared_ptr<pdfe::annotations::TextSelector> selector;
    std::unique_ptr<pdfe::core::Page> pagePtr;
    pdfe::core::Page* page = nullptr;
};

TEST_F(AnnotationToolsTest, StickyNoteCreation) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    pdfe::annotations::StickyNoteTool tool(manager.get());
    tool.setPage(page);

    // Track created annotations
    bool annotationWasCreated = false;
    int createdPageIndex = -1;
    pdfe::annotations::AnnotationType createdType = pdfe::annotations::AnnotationType::Unknown;

    tool.annotationCreated = [&](int pageIndex, pdfe::annotations::AnnotationData data) {
        annotationWasCreated = true;
        createdPageIndex = pageIndex;
        createdType = data.type;
    };

    EXPECT_EQ(tool.toolType(), pdfe::annotations::AnnotationType::Text);

    // Simulate mouse press to create sticky note
    QPointF pdfPoint(100.0, 100.0);
    auto* event = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));

    tool.onMousePress(event, 0, pdfPoint);

    delete event;

    // Verify annotation was created
    EXPECT_TRUE(annotationWasCreated);
    EXPECT_EQ(createdPageIndex, 0);
    EXPECT_EQ(createdType, pdfe::annotations::AnnotationType::Text);
}

TEST_F(AnnotationToolsTest, HighlightFromSelection) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    QColor yellow(255, 255, 0);
    pdfe::annotations::TextMarkupTool tool(
        manager.get(),
        selector,
        pdfe::annotations::AnnotationType::Highlight,
        yellow
    );
    tool.setPage(page);

    EXPECT_EQ(tool.toolType(), pdfe::annotations::AnnotationType::Highlight);

    // Track created annotations
    bool annotationWasCreated = false;
    pdfe::annotations::AnnotationType createdType = pdfe::annotations::AnnotationType::Unknown;

    tool.annotationCreated = [&](int pageIndex, pdfe::annotations::AnnotationData data) {
        annotationWasCreated = true;
        createdType = data.type;
    };

    // Simulate text selection: press, move, release
    QPointF startPoint(50.0, 50.0);
    QPointF endPoint(200.0, 50.0);

    auto* pressEvent = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
    auto* moveEvent = createMouseEvent(QEvent::MouseMove, Qt::LeftButton, QPoint(0, 0));
    auto* releaseEvent = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));

    tool.onMousePress(pressEvent, 0, startPoint);
    tool.onMouseMove(moveEvent, 0, endPoint);
    tool.onMouseRelease(releaseEvent, 0, endPoint);

    delete pressEvent;
    delete moveEvent;
    delete releaseEvent;

    // Note: Annotation is only created if text was actually selected
    // If no text was selected (empty quads), no annotation is created
    if (annotationWasCreated) {
        EXPECT_EQ(createdType, pdfe::annotations::AnnotationType::Highlight);
    }
}

// Regression for #48: TextMarkupTool::onMouseRelease used to write
// currentPageIndex_ = -1 AFTER firing annotationCreated. But creating a
// text-markup annotation makes MainWindow switch the active tool
// (onAnnotationCreatedByTool -> onAnnotationToolSelected -> clearCurrentTool),
// which DELETES the tool — so the post-callback member write was a
// heap-use-after-free (ASan: TextMarkupTool.cpp:93), whose heap corruption later
// surfaced as a QString/QTextEdit paint crash. The fix resets state BEFORE the
// callback and touches nothing after. This test reproduces the exact scenario:
// the callback (a) verifies our state is ALREADY reset when we're notified
// (deterministic ordering oracle), and (b) deletes the tool mid-onMouseRelease.
TEST_F(AnnotationToolsTest, MarkupOnMouseReleaseIsSafeIfCallbackDeletesTool) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    QColor yellow(255, 255, 0);
    auto tool = std::make_unique<pdfe::annotations::TextMarkupTool>(
        manager.get(), selector,
        pdfe::annotations::AnnotationType::Highlight, yellow);
    tool->setPage(page);
    pdfe::annotations::TextMarkupTool* raw = tool.get();

    bool fired = false;
    int pageIndexAtCallback = -999;
    raw->annotationCreated =
        [&](int /*pageIndex*/, pdfe::annotations::AnnotationData /*data*/) {
            fired = true;
            // Ordering oracle: the tool must have reset its own state BEFORE
            // notifying us. Pre-fix currentPageIndex_ was still the real page
            // index here (reset ran afterwards → the UAF). Post-fix it is -1.
            pageIndexAtCallback = raw->currentPageIndex();
            // Reproduce the real scenario: creating the annotation switches the
            // active tool, which destroys this tool. onMouseRelease must not
            // touch any member after this callback returns.
            tool.reset();
        };

    // Drive a real text selection so onMouseRelease reaches the callback.
    QPointF startPoint(50.0, 90.0);
    QPointF endPoint(400.0, 90.0);
    auto* pressEvent = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
    auto* moveEvent = createMouseEvent(QEvent::MouseMove, Qt::LeftButton, QPoint(0, 0));
    auto* releaseEvent = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));
    raw->onMousePress(pressEvent, 0, startPoint);
    raw->onMouseMove(moveEvent, 0, endPoint);
    // The callback deletes the tool mid-call; post-fix this returns cleanly,
    // pre-fix it writes to the freed tool (caught by ASan / the ordering oracle).
    raw->onMouseRelease(releaseEvent, 0, endPoint);
    delete pressEvent;
    delete moveEvent;
    delete releaseEvent;

    ASSERT_TRUE(fired)
        << "text selection did not produce quads on this fixture — adjust the "
           "selection coordinates so the #48 scenario is actually exercised";
    EXPECT_EQ(pageIndexAtCallback, -1)
        << "onMouseRelease must reset currentPageIndex_ BEFORE firing "
           "annotationCreated (which may delete the tool) — #48 UAF ordering";
}

TEST_F(AnnotationToolsTest, InkStrokeCapture) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    pdfe::annotations::InkTool tool(manager.get());
    tool.setPage(page);

    EXPECT_EQ(tool.toolType(), pdfe::annotations::AnnotationType::Ink);

    // Track created annotations
    bool annotationWasCreated = false;
    pdfe::annotations::AnnotationType createdType = pdfe::annotations::AnnotationType::Unknown;

    tool.annotationCreated = [&](int pageIndex, pdfe::annotations::AnnotationData data) {
        annotationWasCreated = true;
        createdType = data.type;
    };

    // Simulate drawing a stroke
    std::vector<QPointF> strokePoints = {
        QPointF(50.0, 50.0),
        QPointF(100.0, 60.0),
        QPointF(150.0, 55.0),
        QPointF(200.0, 50.0)
    };

    auto* pressEvent = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
    tool.onMousePress(pressEvent, 0, strokePoints[0]);
    delete pressEvent;

    for (size_t i = 1; i < strokePoints.size() - 1; ++i) {
        auto* moveEvent = createMouseEvent(QEvent::MouseMove, Qt::LeftButton, QPoint(0, 0));
        tool.onMouseMove(moveEvent, 0, strokePoints[i]);
        delete moveEvent;
    }

    auto* releaseEvent = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));
    tool.onMouseRelease(releaseEvent, 0, strokePoints.back());
    delete releaseEvent;

    // Verify annotation was created
    EXPECT_TRUE(annotationWasCreated);
    EXPECT_EQ(createdType, pdfe::annotations::AnnotationType::Ink);
}

TEST_F(AnnotationToolsTest, ShapeToolRect) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    QColor red(255, 0, 0);
    pdfe::annotations::ShapeTool tool(
        manager.get(),
        pdfe::annotations::AnnotationType::Square,
        red
    );
    tool.setPage(page);

    EXPECT_EQ(tool.toolType(), pdfe::annotations::AnnotationType::Square);

    // Track created annotations
    bool annotationWasCreated = false;
    pdfe::annotations::AnnotationType createdType = pdfe::annotations::AnnotationType::Unknown;

    tool.annotationCreated = [&](int pageIndex, pdfe::annotations::AnnotationData data) {
        annotationWasCreated = true;
        createdType = data.type;
    };

    // Simulate dragging to create rectangle
    QPointF startPoint(50.0, 50.0);
    QPointF endPoint(150.0, 100.0);

    auto* pressEvent = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
    auto* moveEvent = createMouseEvent(QEvent::MouseMove, Qt::LeftButton, QPoint(0, 0));
    auto* releaseEvent = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));

    tool.onMousePress(pressEvent, 0, startPoint);
    tool.onMouseMove(moveEvent, 0, endPoint);
    tool.onMouseRelease(releaseEvent, 0, endPoint);

    delete pressEvent;
    delete moveEvent;
    delete releaseEvent;

    // Verify annotation was created
    EXPECT_TRUE(annotationWasCreated);
    EXPECT_EQ(createdType, pdfe::annotations::AnnotationType::Square);
}

TEST_F(AnnotationToolsTest, ShapeToolCircle) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    QColor blue(0, 0, 255);
    pdfe::annotations::ShapeTool tool(
        manager.get(),
        pdfe::annotations::AnnotationType::Circle,
        blue
    );
    tool.setPage(page);

    EXPECT_EQ(tool.toolType(), pdfe::annotations::AnnotationType::Circle);

    // Track created annotations
    bool annotationWasCreated = false;
    pdfe::annotations::AnnotationType createdType = pdfe::annotations::AnnotationType::Unknown;

    tool.annotationCreated = [&](int pageIndex, pdfe::annotations::AnnotationData data) {
        annotationWasCreated = true;
        createdType = data.type;
    };

    // Simulate dragging to create circle
    QPointF startPoint(50.0, 50.0);
    QPointF endPoint(150.0, 100.0);

    auto* pressEvent = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
    auto* moveEvent = createMouseEvent(QEvent::MouseMove, Qt::LeftButton, QPoint(0, 0));
    auto* releaseEvent = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));

    tool.onMousePress(pressEvent, 0, startPoint);
    tool.onMouseMove(moveEvent, 0, endPoint);
    tool.onMouseRelease(releaseEvent, 0, endPoint);

    delete pressEvent;
    delete moveEvent;
    delete releaseEvent;

    // Verify annotation was created
    EXPECT_TRUE(annotationWasCreated);
    EXPECT_EQ(createdType, pdfe::annotations::AnnotationType::Circle);
}

TEST_F(AnnotationToolsTest, FreeTextCreation) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    pdfe::annotations::FreeTextTool tool(manager.get());
    tool.setPage(page);

    EXPECT_EQ(tool.toolType(), pdfe::annotations::AnnotationType::FreeText);

    // Track created annotations
    bool annotationWasCreated = false;
    pdfe::annotations::AnnotationType createdType = pdfe::annotations::AnnotationType::Unknown;

    tool.annotationCreated = [&](int pageIndex, pdfe::annotations::AnnotationData data) {
        annotationWasCreated = true;
        createdType = data.type;
    };

    // Simulate mouse press to create text box
    QPointF pdfPoint(100.0, 100.0);
    auto* event = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));

    tool.onMousePress(event, 0, pdfPoint);

    delete event;

    // Verify annotation was created
    EXPECT_TRUE(annotationWasCreated);
    EXPECT_EQ(createdType, pdfe::annotations::AnnotationType::FreeText);
}

TEST_F(AnnotationToolsTest, ToolActivateDeactivate) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Test that activate/deactivate methods run without crashing
    pdfe::annotations::StickyNoteTool stickyTool(manager.get());
    stickyTool.activate();
    stickyTool.deactivate();

    pdfe::annotations::InkTool inkTool(manager.get());
    inkTool.activate();
    inkTool.deactivate();

    pdfe::annotations::ShapeTool shapeTool(
        manager.get(),
        pdfe::annotations::AnnotationType::Square,
        Qt::red
    );
    shapeTool.activate();
    shapeTool.deactivate();

    pdfe::annotations::FreeTextTool textTool(manager.get());
    textTool.activate();
    textTool.deactivate();

    pdfe::annotations::TextMarkupTool markupTool(
        manager.get(),
        selector,
        pdfe::annotations::AnnotationType::Highlight,
        Qt::yellow
    );
    markupTool.activate();
    markupTool.deactivate();

    // If we get here without crashing, the test passes
    SUCCEED();
}

TEST_F(AnnotationToolsTest, ToolTypeReporting) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    pdfe::annotations::StickyNoteTool stickyTool(manager.get());
    EXPECT_EQ(stickyTool.toolType(), pdfe::annotations::AnnotationType::Text);

    pdfe::annotations::InkTool inkTool(manager.get());
    EXPECT_EQ(inkTool.toolType(), pdfe::annotations::AnnotationType::Ink);

    pdfe::annotations::FreeTextTool textTool(manager.get());
    EXPECT_EQ(textTool.toolType(), pdfe::annotations::AnnotationType::FreeText);

    pdfe::annotations::ShapeTool squareTool(
        manager.get(),
        pdfe::annotations::AnnotationType::Square,
        Qt::red
    );
    EXPECT_EQ(squareTool.toolType(), pdfe::annotations::AnnotationType::Square);

    pdfe::annotations::ShapeTool circleTool(
        manager.get(),
        pdfe::annotations::AnnotationType::Circle,
        Qt::blue
    );
    EXPECT_EQ(circleTool.toolType(), pdfe::annotations::AnnotationType::Circle);

    pdfe::annotations::ShapeTool lineTool(
        manager.get(),
        pdfe::annotations::AnnotationType::Line,
        Qt::green
    );
    EXPECT_EQ(lineTool.toolType(), pdfe::annotations::AnnotationType::Line);

    pdfe::annotations::TextMarkupTool highlightTool(
        manager.get(),
        selector,
        pdfe::annotations::AnnotationType::Highlight,
        Qt::yellow
    );
    EXPECT_EQ(highlightTool.toolType(), pdfe::annotations::AnnotationType::Highlight);

    pdfe::annotations::TextMarkupTool underlineTool(
        manager.get(),
        selector,
        pdfe::annotations::AnnotationType::Underline,
        Qt::blue
    );
    EXPECT_EQ(underlineTool.toolType(), pdfe::annotations::AnnotationType::Underline);

    pdfe::annotations::TextMarkupTool strikeoutTool(
        manager.get(),
        selector,
        pdfe::annotations::AnnotationType::StrikeOut,
        Qt::red
    );
    EXPECT_EQ(strikeoutTool.toolType(), pdfe::annotations::AnnotationType::StrikeOut);
}

// ═════════════════════════════════════════════════════════════════════
// CLASS-LEVEL TEST for DCA 0009 (follow-up phase 42).
//
// The INSTANCE test above (MarkupOnMouseReleaseIsSafeIfCallbackDeletesTool)
// pins ONE tool and ONE member: TextMarkupTool::currentPageIndex_. Its oracle
// is `pageIndexAtCallback == -1`, which is specific to that member of that
// class and says nothing about the other seven annotation tools that fire the
// same callback from the same kind of handler.
//
// THE CLASS is: *an object that fires a callback which may destroy it, and
// then keeps using itself.* Every AnnotationTool is exposed to it — in
// production `annotationCreated` runs synchronously and reaches
// PdfCanvas::annotationCreated → MainWindow::onAnnotationCreatedByTool →
// onAnnotationToolSelected → PdfCanvas::clearCurrentTool →
// unique_ptr<AnnotationTool>::reset(), which destroys the tool whose handler is
// on the stack. Any member write after the callback is then a heap UAF.
//
// THE ORACLE is a byte-image comparison of the whole tool object, so it is
// blind to WHICH member is touched — that is what makes it class-level rather
// than a second copy of the instance test:
//
//   * the callback snapshots the tool's entire object representation at the
//     moment it is invoked;
//   * after the driving handler returns, the object image must be IDENTICAL.
//
// Any post-callback self-mutation — a scalar assignment, a vector clear, a new
// member added years from now — changes some byte and fails. Note this
// deliberately does NOT destroy the tool inside the callback: destroying the
// std::function that is mid-call is itself undefined behaviour, and it is not
// needed. "Does the tool write to itself after firing?" is the property; if the
// answer is no, destroying it there is safe by construction.
//
// WHAT IT CAUGHT (phase-42): InkTool::onMouseRelease still ran
// `allStrokes_.clear(); currentPageIndex_ = -1;` AFTER the callback — the exact
// DCA 0009 defect, surviving in a sibling because the 0009 fix was written
// against the single tool the crash dump named. Fixed in src/annotations/InkTool.cpp
// as part of this phase.
//
// COVERAGE BOUNDARY (stated, not hidden): every class that fires
// annotationCreated is driven here EXCEPT LinkTool, whose onMouseRelease calls
// promptForAction() — a modal QDialog::exec() that cannot be driven headlessly.
// LinkTool's fire is the last statement of its handler; that is established by
// inspection, not by this test.
//
// THE CORPUS IS DERIVED, NOT LISTED (phase-42 remediation). The first version
// of this test hand-assembled its rows from a directory listing of
// src/annotations/ — and MISSED src/measure/MeasureTool.cpp, which fires the
// same callback from the same kind of handler and had the same live UAF. A
// class-level oracle whose corpus is a hand-copied directory listing is not
// class-level; it is an instance test with more rows, and it fails exactly the
// way the defect it targets fails. ToolCorpusCoversEveryAnnotationCreatedFirer
// below now PARSES the product source for fire sites and requires this corpus
// to cover all of them, so a tenth tool in a tenth directory cannot escape.
TEST_F(AnnotationToolsTest, NoToolTouchesItselfAfterFiringAnnotationCreated) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available";
    }

    // Compare the tool's whole object representation before/after the callback.
    // `drive` must take the tool through to firing annotationCreated.
    auto expectNoSelfTouch = [&](const char* toolName, auto& tool,
                                 auto&& drive) {
        using ToolT = std::remove_reference_t<decltype(tool)>;
        const auto* raw = reinterpret_cast<const unsigned char*>(&tool);
        std::vector<unsigned char> imageAtCallback;
        bool fired = false;

        tool.annotationCreated =
            [&](int /*pageIndex*/, pdfe::annotations::AnnotationData /*data*/) {
                fired = true;
                // LAST thing the callback does: freeze the object image.
                imageAtCallback.assign(raw, raw + sizeof(ToolT));
            };

        drive(tool);

        EXPECT_TRUE(fired)
            << toolName << " never fired annotationCreated — the drive sequence "
               "did not reach the callback, so this row proves NOTHING. Fix the "
               "drive, do not drop the tool from the corpus.";
        if (!fired) return;
        ASSERT_EQ(imageAtCallback.size(), sizeof(ToolT));
        EXPECT_EQ(std::memcmp(raw, imageAtCallback.data(), sizeof(ToolT)), 0)
            << toolName << " modified itself AFTER firing annotationCreated. In "
               "production that callback destroys the tool, so every one of "
               "those bytes is a write to freed heap (DCA 0009). Move the state "
               "reset BEFORE the callback and make the callback the last "
               "statement.";
    };

    const QPointF a(60.0, 90.0);
    const QPointF b(400.0, 90.0);

    // 1. StickyNoteTool — fires from onMousePress.
    {
        pdfe::annotations::StickyNoteTool tool(manager.get());
        tool.setPage(page);
        expectNoSelfTouch("StickyNoteTool", tool, [&](auto& t) {
            auto* ev = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(ev, 0, a);
            delete ev;
        });
    }

    // 2. FreeTextTool — fires from onMousePress.
    {
        pdfe::annotations::FreeTextTool tool(manager.get());
        tool.setPage(page);
        expectNoSelfTouch("FreeTextTool", tool, [&](auto& t) {
            auto* ev = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(ev, 0, a);
            delete ev;
        });
    }

    // 3. InkTool — fires from onMouseRelease. THIS IS THE ROW THAT WAS RED.
    {
        pdfe::annotations::InkTool tool(manager.get());
        tool.setPage(page);
        expectNoSelfTouch("InkTool", tool, [&](auto& t) {
            auto* press = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            auto* move = createMouseEvent(QEvent::MouseMove, Qt::LeftButton, QPoint(0, 0));
            auto* rel = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(press, 0, a);
            t.onMouseMove(move, 0, QPointF(200.0, 120.0));
            t.onMouseRelease(rel, 0, b);
            delete press; delete move; delete rel;
        });
    }

    // 4. ShapeTool — fires from onMouseRelease.
    {
        pdfe::annotations::ShapeTool tool(manager.get(),
                                          pdfe::annotations::AnnotationType::Square,
                                          QColor(255, 0, 0));
        tool.setPage(page);
        expectNoSelfTouch("ShapeTool", tool, [&](auto& t) {
            auto* press = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            auto* move = createMouseEvent(QEvent::MouseMove, Qt::LeftButton, QPoint(0, 0));
            auto* rel = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(press, 0, a);
            t.onMouseMove(move, 0, b);
            t.onMouseRelease(rel, 0, QPointF(400.0, 200.0));
            delete press; delete move; delete rel;
        });
    }

    // 5. StampTool (text path) — fires from onMouseRelease.
    {
        pdfe::annotations::StampTool tool(manager.get());
        tool.setPage(page);
        tool.setStampName(QStringLiteral("APPROVED"));
        tool.setAuthor(QStringLiteral("phase-42"));
        expectNoSelfTouch("StampTool(text)", tool, [&](auto& t) {
            auto* press = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            auto* rel = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(press, 0, a);
            t.onMouseRelease(rel, 0, a);
            delete press; delete rel;
        });
    }

    // 6. StampTool (image path) — the SECOND fire site in StampTool.cpp.
    {
        pdfe::annotations::StampTool tool(manager.get());
        tool.setPage(page);
        QImage stamp(24, 12, QImage::Format_ARGB32);
        stamp.fill(Qt::green);
        tool.setImageStamp(QStringLiteral("phase42-stamp"), stamp);
        expectNoSelfTouch("StampTool(image)", tool, [&](auto& t) {
            auto* press = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            auto* rel = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(press, 0, a);
            t.onMouseRelease(rel, 0, a);
            delete press; delete rel;
        });
    }

    // 7. FileAttachmentTool — fires from onMousePress; needs a real source file.
    {
        auto attachDir = std::filesystem::temp_directory_path() / "pdfe_dca42_attach";
        std::filesystem::create_directories(attachDir);
        auto attachFile = attachDir / "phase42-attachment.txt";
        {
            std::ofstream out(attachFile, std::ios::binary);
            out << "phase-42 class-level attachment fixture";
        }
        pdfe::annotations::FileAttachmentTool tool(manager.get());
        tool.setPage(page);
        tool.setSourceFile(attachFile);
        expectNoSelfTouch("FileAttachmentTool", tool, [&](auto& t) {
            auto* ev = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(ev, 0, a);
            delete ev;
        });
        std::error_code ec;
        std::filesystem::remove(attachFile, ec);
    }

    // 8. TextMarkupTool — the DCA 0009 instance, re-checked by the class oracle
    //    rather than by its member-specific one.
    {
        pdfe::annotations::TextMarkupTool tool(
            manager.get(), selector,
            pdfe::annotations::AnnotationType::Highlight, QColor(255, 255, 0));
        tool.setPage(page);
        expectNoSelfTouch("TextMarkupTool", tool, [&](auto& t) {
            auto* press = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            auto* move = createMouseEvent(QEvent::MouseMove, Qt::LeftButton, QPoint(0, 0));
            auto* rel = createMouseEvent(QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(press, 0, QPointF(50.0, 90.0));
            t.onMouseMove(move, 0, b);
            t.onMouseRelease(rel, 0, b);
            delete press; delete move; delete rel;
        });
    }

    // 9. MeasureTool (distance) — src/measure/, NOT src/annotations/. This is
    //    the row the first phase-42 corpus missed, and it was RED: the tool ran
    //    points_.clear() after firing. Distance commits on the SECOND press.
    {
        pdfe::measure::MeasureTool tool(manager.get(),
                                        pdfe::measure::MeasureMode::Distance);
        tool.setPage(page);
        expectNoSelfTouch("MeasureTool(distance)", tool, [&](auto& t) {
            auto* p1 = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            auto* p2 = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(p1, 0, a);
            t.onMousePress(p2, 0, b);
            delete p1; delete p2;
        });
    }

    // 10. MeasureTool (area) — the SECOND fire site, on double-click.
    {
        pdfe::measure::MeasureTool tool(manager.get(),
                                        pdfe::measure::MeasureMode::Area);
        tool.setPage(page);
        expectNoSelfTouch("MeasureTool(area)", tool, [&](auto& t) {
            auto* p1 = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            auto* p2 = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            auto* p3 = createMouseEvent(QEvent::MouseButtonPress, Qt::LeftButton, QPoint(0, 0));
            auto* dbl = createMouseEvent(QEvent::MouseButtonDblClick, Qt::LeftButton, QPoint(0, 0));
            t.onMousePress(p1, 0, a);
            t.onMousePress(p2, 0, b);
            t.onMousePress(p3, 0, QPointF(200.0, 300.0));
            t.onMouseDoubleClick(dbl, 0, QPointF(200.0, 300.0));
            delete p1; delete p2; delete p3; delete dbl;
        });
    }
}

// ═════════════════════════════════════════════════════════════════════
// CORPUS COMPLETENESS GUARD for the class-level test above (phase-42
// remediation).
//
// NoToolTouchesItselfAfterFiringAnnotationCreated is only class-level if its
// corpus is the WHOLE family. The first version assembled its rows by hand from
// a listing of src/annotations/ and therefore missed src/measure/MeasureTool.cpp
// — which fires the same callback and carried the same live use-after-free.
// That is the DCA 0009 failure mode reproduced inside DCA 0009's own class-level
// defence: a fix (or a test) scoped to where someone looked.
//
// So the corpus is not trusted to be complete; it is CHECKED. This parses every
// .cpp under src/ for real `annotationCreated(...)` INVOCATIONS and requires the
// set of firing translation units to equal the set the corpus drives, plus an
// explicit, justified exclusion list. A tenth tool in a tenth directory fails
// here on the day it lands.
//
// Two things are deliberately not matched: `if (annotationCreated)` (a null
// test, not a call) and `emit annotationCreated(...)` in PdfCanvas.cpp (a Qt
// SIGNAL of the same name, not an AnnotationTool member — including it would
// make this guard permanently and wrongly red).
TEST(AnnotationToolCorpus, CoversEveryAnnotationCreatedFirer) {
    namespace fs = std::filesystem;
    const fs::path srcRoot = fs::path(PDFE_REPO_ROOT) / "src";
    ASSERT_TRUE(fs::exists(srcRoot))
        << "cannot find " << srcRoot.string()
        << " — this guard must fail closed, not pass silently";

    // Tools the class-level test actually drives.
    const std::set<std::string> driven = {
        "StickyNoteTool", "FreeTextTool", "InkTool", "ShapeTool",
        "StampTool", "FileAttachmentTool", "TextMarkupTool", "MeasureTool",
    };
    // Excluded WITH a reason. LinkTool's fire is unreachable headlessly because
    // promptForAction() runs a modal QDialog::exec().
    const std::set<std::string> excludedWithReason = { "LinkTool" };

    std::set<std::string> firers;
    const std::regex fireRx(R"RX(^\s*(?:if\s*\([^)]*\)\s*)?annotationCreated\s*\()RX");
    for (const auto& entry : fs::recursive_directory_iterator(srcRoot)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".cpp") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in.good()) continue;
        std::string line;
        while (std::getline(in, line)) {
            const auto firstNonSpace = line.find_first_not_of(" \t\r");
            if (firstNonSpace != std::string::npos &&
                line.compare(firstNonSpace, 2, "//") == 0) {
                continue;                       // comment
            }
            if (line.find("emit annotationCreated") != std::string::npos) {
                continue;                       // Qt signal, not a tool fire
            }
            // A call is `annotationCreated(` or `cb(` after a local copy; match
            // the direct form plus the copy idiom this codebase now uses.
            const bool directCall =
                std::regex_search(line, fireRx) &&
                line.find("if (annotationCreated)") == std::string::npos;
            const bool copyCall =
                line.find("cb(") != std::string::npos &&
                line.find("annotationCreated") == std::string::npos &&
                line.find("auto cb = ") == std::string::npos;
            if (directCall || copyCall) {
                // Only count a `cb(` line in a file that also copies the
                // callback, so unrelated `cb(` identifiers elsewhere in src/
                // cannot inflate the set.
                if (copyCall) {
                    std::ifstream again(entry.path(), std::ios::binary);
                    std::string whole((std::istreambuf_iterator<char>(again)),
                                      std::istreambuf_iterator<char>());
                    if (whole.find("auto cb = annotationCreated") == std::string::npos)
                        continue;
                }
                firers.insert(entry.path().stem().string());
            }
        }
    }

    ASSERT_FALSE(firers.empty())
        << "parsed no annotationCreated fire sites at all under "
        << srcRoot.string() << " — the parser is broken and this guard would "
           "pass vacuously, which is worse than the gap it closes";

    std::set<std::string> expected;
    expected.insert(driven.begin(), driven.end());
    expected.insert(excludedWithReason.begin(), excludedWithReason.end());

    std::vector<std::string> uncovered;
    for (const auto& f : firers) {
        if (!expected.count(f)) uncovered.push_back(f);
    }
    EXPECT_TRUE(uncovered.empty())
        << "these classes fire annotationCreated but are NOT in the class-level "
           "corpus and are not on the justified-exclusion list: "
        << [&] {
               std::string s;
               for (const auto& u : uncovered) { s += u; s += " "; }
               return s;
           }()
        << "\nEvery firer must be driven by "
           "AnnotationToolsTest.NoToolTouchesItselfAfterFiringAnnotationCreated, "
           "or listed as excluded with a reason. A tool nobody drove is exactly "
           "how MeasureTool kept the DCA 0009 use-after-free through the fix "
           "AND through the first class-level test written to catch it.";

    // The reverse direction: a corpus row for a class that no longer fires is
    // dead weight and hides a shrinking corpus.
    for (const auto& d : driven) {
        EXPECT_TRUE(firers.count(d) > 0)
            << d << " is driven by the class-level corpus but no longer fires "
                    "annotationCreated anywhere in src/ — remove the stale row "
                    "or fix the parser.";
    }
}
