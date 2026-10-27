// -*- coding: utf-8 -*-
// test_ui_pdfcanvas_accessible_text.cpp
//
// R2-18 (accessibility) correctness oracle.
//
// Acceptance criterion:
//   "PdfCanvasAccessible implements QAccessibleTextInterface exposing per-page
//    text (and ideally form-field/link/annotation children); a test asserts
//    childCount()>0 and that extracted page text is reachable via the
//    accessibility tree for a representative corpus."
//
// Against the PRE-FIX implementation this suite is RED:
//   * PdfCanvasAccessible::childCount() hard-returned 0  -> every childCount>0
//     assertion fails.
//   * PdfCanvasAccessible exposed no QAccessibleTextInterface -> interface_cast
//     / queryAccessibleInterface().textInterface() is null and the page text is
//     unreachable through the a11y tree.
//
// After the fix (per-page PdfPageAccessible children + a whole-document
// QAccessibleTextInterface) it is GREEN.

#include <gtest/gtest.h>

#include "ui/PdfCanvasAccessible.h"
#include "ui/PdfCanvas.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QAccessible>
#include <QAccessibleInterface>
#include <QApplication>
#include <QString>

#include <filesystem>
#include <memory>

using namespace pdfe::ui;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Known text baked into tests/fixtures/test_3page.pdf (one distinctive line
// per page — see the fixture's decoded content streams).
const char* kPage1Marker = "Page 1 - Title Page";
const char* kPage2Marker = "Page 2 - Body Content";
const char* kPage3Marker = "Page 3 - Final Page";

} // namespace

class PdfCanvasAccessibleTextTest : public ::testing::Test {
protected:
    void SetUp() override {
        QAccessible::installFactory(pdfCanvasAccessibleFactory);

        fixture_ = fixturesDir() / "test_3page.pdf";
        if (!std::filesystem::exists(fixture_)) {
            GTEST_SKIP() << "test_3page.pdf fixture not available";
        }
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        doc_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(doc_, nullptr);
        ASSERT_GE(doc_->pageCount(), 3) << "fixture must have 3 pages";
    }

    std::filesystem::path fixture_;
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::core::Document> doc_;
};

// (a) childCount() > 0 for a loaded document, and equals the page count.
TEST_F(PdfCanvasAccessibleTextTest, ChildCountEqualsPageCountForLoadedDoc) {
    PdfCanvas canvas;
    canvas.setDocument(doc_.get(), nullptr, nullptr);

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&canvas);
    ASSERT_NE(iface, nullptr);

    EXPECT_GT(iface->childCount(), 0)
        << "a loaded document must expose per-page accessibility children";
    EXPECT_EQ(iface->childCount(), doc_->pageCount())
        << "one accessibility child per document page";
}

// A canvas with NO document exposes zero children (leaf), preserving the prior
// contract relied on by the click hit-test path.
TEST_F(PdfCanvasAccessibleTextTest, NoDocumentHasZeroChildren) {
    PdfCanvas canvas; // no setDocument
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&canvas);
    ASSERT_NE(iface, nullptr);
    EXPECT_EQ(iface->childCount(), 0);
}

// (b) The extracted page text is reachable through the WHOLE-CANVAS
// QAccessibleTextInterface (all pages concatenated).
TEST_F(PdfCanvasAccessibleTextTest, CanvasTextInterfaceExposesAllPageText) {
    PdfCanvas canvas;
    canvas.setDocument(doc_.get(), nullptr, nullptr);

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&canvas);
    ASSERT_NE(iface, nullptr);

    QAccessibleTextInterface* textIface = iface->textInterface();
    ASSERT_NE(textIface, nullptr)
        << "PdfCanvasAccessible must implement QAccessibleTextInterface";

    EXPECT_GT(textIface->characterCount(), 0);

    const QString all = textIface->text(0, textIface->characterCount());
    EXPECT_TRUE(all.contains(kPage1Marker))
        << "page 1 text unreachable via a11y text interface";
    EXPECT_TRUE(all.contains(kPage2Marker))
        << "page 2 text unreachable via a11y text interface";
    EXPECT_TRUE(all.contains(kPage3Marker))
        << "page 3 text unreachable via a11y text interface";
}

// (b) The extracted page text is reachable through each PER-PAGE child's
// QAccessibleTextInterface, and each child carries the text of its own page.
TEST_F(PdfCanvasAccessibleTextTest, EachChildExposesItsOwnPageText) {
    PdfCanvas canvas;
    canvas.setDocument(doc_.get(), nullptr, nullptr);

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&canvas);
    ASSERT_NE(iface, nullptr);
    ASSERT_EQ(iface->childCount(), 3);

    const char* markers[3] = { kPage1Marker, kPage2Marker, kPage3Marker };

    for (int i = 0; i < 3; ++i) {
        QAccessibleInterface* childIface = iface->child(i);
        ASSERT_NE(childIface, nullptr) << "missing child " << i;

        QAccessibleTextInterface* childText = childIface->textInterface();
        ASSERT_NE(childText, nullptr)
            << "page child " << i << " must expose QAccessibleTextInterface";

        const QString pageStr = childText->text(0, childText->characterCount());
        EXPECT_TRUE(pageStr.contains(markers[i]))
            << "child " << i << " does not carry page " << (i + 1) << " text";

        // A page's child must NOT carry a different page's marker — proves the
        // per-page split rather than every child returning the whole document.
        for (int j = 0; j < 3; ++j) {
            if (j == i) continue;
            EXPECT_FALSE(pageStr.contains(markers[j]))
                << "child " << i << " leaked page " << (j + 1) << " text";
        }

        // indexOfChild round-trips.
        EXPECT_EQ(iface->indexOfChild(childIface), i);

        // Stable, unique accessible name for automation friendliness.
        EXPECT_EQ(childIface->text(QAccessible::Name),
                  QStringLiteral("Page %1").arg(i + 1));
    }
}

// textAtOffset with a character boundary returns exactly that character —
// exercises the QAccessibleTextInterface boundary path on a child.
TEST_F(PdfCanvasAccessibleTextTest, ChildTextAtOffsetCharBoundary) {
    PdfCanvas canvas;
    canvas.setDocument(doc_.get(), nullptr, nullptr);

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&canvas);
    ASSERT_NE(iface, nullptr);
    QAccessibleInterface* childIface = iface->child(0);
    ASSERT_NE(childIface, nullptr);
    QAccessibleTextInterface* childText = childIface->textInterface();
    ASSERT_NE(childText, nullptr);
    ASSERT_GT(childText->characterCount(), 0);

    int startOffset = -1, endOffset = -1;
    const QString one =
        childText->textAtOffset(0, QAccessible::CharBoundary, &startOffset, &endOffset);
    EXPECT_EQ(startOffset, 0);
    EXPECT_EQ(endOffset, 1);
    EXPECT_EQ(one.length(), 1);
    EXPECT_EQ(one, childText->text(0, 1));
}

// Regression: the page child interfaces must be owned by Qt's QAccessibleCache
// (registered), NOT raw-deleted by PdfCanvasAccessible. Raw-deleting a cached
// interface leaves a dangling entry that QApplication teardown's
// cleanupAccessibleCache() dereferences -> the shutdown use-after-free
// (call [rax+0x10] with rax=0xdddddddddddddddd) captured in ui-out/crashdumps.
// After a document change, the stale child's id must be GONE from the cache
// (removed via QAccessible::deleteAccessibleInterface), not pointing at freed
// memory. On the pre-fix code the id still resolves to the freed pointer, so
// this reds.
TEST_F(PdfCanvasAccessibleTextTest, PageChildrenAreCacheOwnedNotRawDeleted) {
    PdfCanvas canvas;
    canvas.setDocument(doc_.get(), nullptr, nullptr);

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&canvas);
    ASSERT_NE(iface, nullptr);
    ASSERT_GT(iface->childCount(), 0);
    QAccessibleInterface* child0 = iface->child(0);
    ASSERT_NE(child0, nullptr);

    const QAccessible::Id id0 = QAccessible::uniqueId(child0);
    EXPECT_EQ(QAccessible::accessibleInterface(id0), child0)
        << "page child must be registered in the QAccessibleCache";

    // Force a rebuild via a document change (pageCount differs -> children retire).
    canvas.setDocument(nullptr, nullptr, nullptr);
    EXPECT_EQ(iface->childCount(), 0);  // triggers rebuildChildrenIfNeeded()

    EXPECT_EQ(QAccessible::accessibleInterface(id0), nullptr)
        << "stale page child left DANGLING in QAccessibleCache after a document "
           "change — it was raw-deleted instead of removed via "
           "QAccessible::deleteAccessibleInterface(). That dangling entry is what "
           "cleanupAccessibleCache() faults on at QApplication teardown (the "
           "shutdown UAF).";
}
