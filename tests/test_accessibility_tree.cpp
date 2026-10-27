// -*- coding: utf-8 -*-
// test_accessibility_tree.cpp
//
// C-06 (coverage-expansion phase-3): accessibility / AT TREE oracle.
//
// The pre-existing a11y tests (test_accessibility.cpp, test_accessible_names.cpp)
// assert NAMES and EXISTENCE. Screen-reader usability, however, depends on the
// QAccessible tree exposing the correct ROLE, the correct PARENT/CHILD
// hierarchy, and the correct VALUE/STATE — a widget with a perfect name but the
// wrong role (a canvas reported as a plain pane, a button reported as static
// text) is still unusable with an AT. This suite asserts those three properties
// for the PdfCanvas a11y subtree and for a representative dialog surface, plus a
// tagged-PDF -> reading-order oracle.
//
// Non-vacuity is proven per-section in the comments and, where possible, by a
// discrimination assertion (role() actually distinguishes control kinds; a
// reversed reading order would fail the strict-ordering assertions).

#include <gtest/gtest.h>

#include "ui/PdfCanvasAccessible.h"
#include "ui/PdfCanvas.h"
#include "ui/MainWindow.h"
#include "tre/TreEditDialog.h"
#include "tre/RegionReconstructor.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QAccessible>
#include <QAccessibleInterface>
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QRadioButton>
#include <QPushButton>
#include <QTextEdit>
#include <QToolButton>
#include <QString>
#include <QList>

#include <filesystem>
#include <memory>

using namespace pdfe::ui;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// A canonical accessible interface for a widget (factory must be installed).
QAccessibleInterface* iface(QWidget* w) {
    return QAccessible::queryAccessibleInterface(w);
}

} // namespace

// ───────────────────────────── Canvas subtree ──────────────────────────────

class A11yTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        QAccessible::installFactory(pdfCanvasAccessibleFactory);

        threePage_ = fixturesDir() / "test_3page.pdf";
        tagged_ = fixturesDir() / "test_tagged_multipage.pdf";
        ASSERT_TRUE(std::filesystem::exists(threePage_))
            << "test_3page.pdf fixture required";
        ASSERT_TRUE(std::filesystem::exists(tagged_))
            << "test_tagged_multipage.pdf fixture required";

        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
    }

    std::unique_ptr<pdfe::core::Document> open(const std::filesystem::path& p) {
        auto d = pdfe::core::Document::open(ctx_, p);
        EXPECT_NE(d, nullptr) << "failed to open " << p.string();
        return d;
    }

    std::filesystem::path threePage_;
    std::filesystem::path tagged_;
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
};

// ROLE: the canvas is a Canvas (Document/Graphic-class) node, and each page
// child is StaticText. Non-vacuity: a screen reader that saw ROLE_SYSTEM_PANE
// (the default QWidget role) instead of Canvas would fail the first EXPECT; a
// page child reported with the canvas's own role (a constant-role bug) would
// fail the StaticText EXPECT. The two roles differ, proving role() discriminates.
TEST_F(A11yTreeTest, CanvasAndPageChildrenExposeCorrectRoles) {
    auto doc = open(threePage_);
    ASSERT_NE(doc, nullptr);
    PdfCanvas canvas;
    canvas.setDocument(doc.get(), nullptr, nullptr);

    QAccessibleInterface* c = iface(&canvas);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->role(), QAccessible::Canvas)
        << "PdfCanvas must expose the Canvas role, not a generic pane";

    ASSERT_EQ(c->childCount(), doc->pageCount());
    for (int i = 0; i < c->childCount(); ++i) {
        QAccessibleInterface* child = c->child(i);
        ASSERT_NE(child, nullptr) << "missing page child " << i;
        EXPECT_EQ(child->role(), QAccessible::StaticText)
            << "page child " << i << " must be a StaticText node";
        // Discrimination: child role differs from the parent's Canvas role.
        EXPECT_NE(child->role(), c->role());
    }
}

// HIERARCHY: full parent/child round-trip. child(i)->parent() resolves back to
// the canvas interface, and parent()->indexOfChild(child) == i. Non-vacuity: a
// child whose parent() returned nullptr, or an indexOfChild that returned a
// constant, would break the round-trip identity below.
TEST_F(A11yTreeTest, CanvasChildParentRoundTrip) {
    auto doc = open(threePage_);
    ASSERT_NE(doc, nullptr);
    PdfCanvas canvas;
    canvas.setDocument(doc.get(), nullptr, nullptr);

    QAccessibleInterface* c = iface(&canvas);
    ASSERT_NE(c, nullptr);
    const int n = c->childCount();
    ASSERT_EQ(n, 3);

    for (int i = 0; i < n; ++i) {
        QAccessibleInterface* child = c->child(i);
        ASSERT_NE(child, nullptr);

        // parent() must resolve back to the canvas interface.
        QAccessibleInterface* p = child->parent();
        ASSERT_NE(p, nullptr) << "child " << i << " has no parent";
        EXPECT_EQ(p->object(), c->object())
            << "child " << i << " parent is not the canvas widget";

        // indexOfChild round-trips on BOTH the canvas and the resolved parent.
        EXPECT_EQ(c->indexOfChild(child), i);
        EXPECT_EQ(p->indexOfChild(child), i);
    }

    // An interface that is not a child must yield indexOfChild == -1 (proves the
    // round-trip is a real lookup, not "return the query index").
    EXPECT_EQ(c->indexOfChild(c), -1);
}

// STATE + VALUE: each page child is read-only + multi-line; the canvas's
// Value/Description reflects LIVE state (page number, zoom, rotation) and
// updates when the view changes. Non-vacuity: a constant description string
// would not change across goToPage()/setZoom(); we assert it does.
TEST_F(A11yTreeTest, PageChildStateAndCanvasLiveValue) {
    auto doc = open(threePage_);
    ASSERT_NE(doc, nullptr);
    PdfCanvas canvas;
    canvas.setDocument(doc.get(), nullptr, nullptr);

    QAccessibleInterface* c = iface(&canvas);
    ASSERT_NE(c, nullptr);
    ASSERT_GT(c->childCount(), 0);

    // Page-child state flags.
    QAccessibleInterface* child = c->child(0);
    ASSERT_NE(child, nullptr);
    const QAccessible::State st = child->state();
    EXPECT_TRUE(st.readOnly)  << "page content must be read-only to an AT";
    EXPECT_TRUE(st.multiLine) << "page text is multi-line";

    // Canvas Name == filename when a document is loaded.
    EXPECT_EQ(c->text(QAccessible::Name), QStringLiteral("test_3page.pdf"));

    // Value/Description reflects page 1 of 3 initially.
    const QString v0 = c->text(QAccessible::Value);
    EXPECT_TRUE(v0.contains("PDF viewer")) << v0.toStdString();
    EXPECT_TRUE(v0.contains("page 1 of 3")) << v0.toStdString();

    // Navigate and re-read: the value must now report page 2. This is the
    // live-state proof — a hard-coded string could not follow the navigation.
    canvas.goToPage(1);
    const QString v1 = c->text(QAccessible::Value);
    EXPECT_TRUE(v1.contains("page 2 of 3")) << v1.toStdString();
    EXPECT_NE(v0, v1) << "canvas a11y value did not track the page change";

    // Zoom also reflected.
    const int zoomBefore = 0;
    (void)zoomBefore;
    canvas.setZoom(canvas.zoom() * 2.0f);
    const QString v2 = c->text(QAccessible::Value);
    EXPECT_TRUE(v2.contains("zoom")) << v2.toStdString();
    EXPECT_NE(v1, v2) << "canvas a11y value did not track the zoom change";
}

// No-document canvas: Name is the "No document" sentinel and there are zero
// children (leaf). Guards the empty-state branch of the tree.
TEST_F(A11yTreeTest, NoDocumentCanvasIsNamedLeaf) {
    PdfCanvas canvas;
    QAccessibleInterface* c = iface(&canvas);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->role(), QAccessible::Canvas);
    EXPECT_EQ(c->childCount(), 0);
    EXPECT_EQ(c->text(QAccessible::Name), QStringLiteral("No document"));
    EXPECT_TRUE(c->text(QAccessible::Value).contains("no document loaded"));
}

// ─────────────────────── Tagged-PDF reading-order oracle ────────────────────

// The AT reads the document by walking the ordered page children and, within
// each, the page's extracted text (MuPDF stext reading order). This oracle
// asserts BOTH granularities of order match the document's true reading order:
//
//   cross-page : Alpha (p1) < Bravo (p2) < Charlie (p3) in the concatenated
//                whole-document text, AND child(0/1/2) carry exactly their own
//                page's heading with no cross-page leakage.
//   intra-page : on test_3page page 3, "Item 1" < "Item 2" < "Item 3".
//
// Non-vacuity: were the pages exposed in reverse (or a child returned the whole
// document), Charlie would precede Alpha / a child would carry a foreign
// heading, failing the strict-order and no-leak assertions below.
TEST_F(A11yTreeTest, TaggedPdfExposesCorrectReadingOrder) {
    auto doc = open(tagged_);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3);

    PdfCanvas canvas;
    canvas.setDocument(doc.get(), nullptr, nullptr);
    QAccessibleInterface* c = iface(&canvas);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->childCount(), 3);

    // Per-page children carry exactly their own heading, in order.
    const char* headings[3] = {"Alpha", "Bravo", "Charlie"};
    for (int i = 0; i < 3; ++i) {
        QAccessibleInterface* child = c->child(i);
        ASSERT_NE(child, nullptr);
        QAccessibleTextInterface* t = child->textInterface();
        ASSERT_NE(t, nullptr);
        const QString pageText = t->text(0, t->characterCount());
        EXPECT_TRUE(pageText.contains(headings[i]))
            << "child " << i << " missing its heading '" << headings[i] << "'";
        for (int j = 0; j < 3; ++j) {
            if (j == i) continue;
            EXPECT_FALSE(pageText.contains(headings[j]))
                << "child " << i << " leaked heading '" << headings[j] << "'";
        }
    }

    // Cross-page reading order in the whole-document text.
    auto* ci = dynamic_cast<PdfCanvasAccessible*>(c);
    ASSERT_NE(ci, nullptr);
    const QString all = ci->allPagesText();
    const int iAlpha = all.indexOf("Alpha");
    const int iBravo = all.indexOf("Bravo");
    const int iCharlie = all.indexOf("Charlie");
    ASSERT_GE(iAlpha, 0);
    ASSERT_GE(iBravo, 0);
    ASSERT_GE(iCharlie, 0);
    EXPECT_LT(iAlpha, iBravo) << "reading order broken: Alpha must precede Bravo";
    EXPECT_LT(iBravo, iCharlie) << "reading order broken: Bravo must precede Charlie";
}

TEST_F(A11yTreeTest, IntraPageReadingOrderIsPreserved) {
    auto doc = open(threePage_);
    ASSERT_NE(doc, nullptr);
    PdfCanvas canvas;
    canvas.setDocument(doc.get(), nullptr, nullptr);
    QAccessibleInterface* c = iface(&canvas);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->childCount(), 3);

    QAccessibleInterface* page3 = c->child(2);
    ASSERT_NE(page3, nullptr);
    QAccessibleTextInterface* t = page3->textInterface();
    ASSERT_NE(t, nullptr);
    const QString txt = t->text(0, t->characterCount());

    const int i1 = txt.indexOf("Item 1");
    const int i2 = txt.indexOf("Item 2");
    const int i3 = txt.indexOf("Item 3");
    ASSERT_GE(i1, 0) << txt.toStdString();
    ASSERT_GE(i2, 0);
    ASSERT_GE(i3, 0);
    EXPECT_LT(i1, i2) << "intra-page reading order broken (Item 1 before Item 2)";
    EXPECT_LT(i2, i3) << "intra-page reading order broken (Item 2 before Item 3)";
}

// ─────────────────────── Dialog surface roles/hierarchy ─────────────────────

// A representative dialog (TreEditDialog) must expose the correct control roles
// so an AT can announce each widget kind. Non-vacuity: the four asserted roles
// are mutually distinct (Dialog / EditableText / RadioButton / Button /
// StaticText), so a stubbed role() returning a single constant could not satisfy
// all of them simultaneously — the DiscriminatesControlRoles check makes that
// explicit.
class A11yDialogRolesTest : public ::testing::Test {
protected:
    void SetUp() override {
        QAccessible::installFactory(pdfCanvasAccessibleFactory);
    }
};

TEST_F(A11yDialogRolesTest, TreEditDialogControlsExposeCorrectRoles) {
    pdfe::tre::ReconstructionResult result;
    pdfe::tre::TreEditDialog dlg(result);

    QAccessibleInterface* d = iface(&dlg);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->role(), QAccessible::Dialog)
        << "the dialog root must be a Dialog for an AT";

    // Text editor -> EditableText.
    auto* edit = dlg.findChild<QTextEdit*>("treTextEdit");
    ASSERT_NE(edit, nullptr);
    ASSERT_NE(iface(edit), nullptr);
    EXPECT_EQ(iface(edit)->role(), QAccessible::EditableText);

    // Approach radios -> RadioButton.
    for (const char* obj : {"approachBRadio", "approachCRadio"}) {
        auto* r = dlg.findChild<QRadioButton*>(obj);
        ASSERT_NE(r, nullptr) << obj;
        ASSERT_NE(iface(r), nullptr);
        EXPECT_EQ(iface(r)->role(), QAccessible::RadioButton) << obj;
    }

    // A label -> StaticText.
    auto* label = dlg.findChild<QLabel*>("qualityLabel");
    ASSERT_NE(label, nullptr);
    ASSERT_NE(iface(label), nullptr);
    EXPECT_EQ(iface(label)->role(), QAccessible::StaticText);

    // OK/Cancel push buttons -> Button.
    const QList<QPushButton*> buttons = dlg.findChildren<QPushButton*>();
    ASSERT_GE(buttons.size(), 2) << "dialog button box must have OK + Cancel";
    int checked = 0;
    for (QPushButton* b : buttons) {
        QAccessibleInterface* bi = iface(b);
        ASSERT_NE(bi, nullptr);
        EXPECT_EQ(bi->role(), QAccessible::Button)
            << "push button '" << b->objectName().toStdString()
            << "' must expose the Button role";
        ++checked;
    }
    ASSERT_GT(checked, 0);
}

// Explicit discrimination: role() returns DIFFERENT values for different control
// kinds inside one dialog — the proof the assertions above are not vacuous
// against a constant-returning stub.
TEST_F(A11yDialogRolesTest, RoleDiscriminatesControlKinds) {
    pdfe::tre::ReconstructionResult result;
    pdfe::tre::TreEditDialog dlg(result);

    auto* edit = dlg.findChild<QTextEdit*>("treTextEdit");
    auto* radio = dlg.findChild<QRadioButton*>("approachBRadio");
    auto* label = dlg.findChild<QLabel*>("qualityLabel");
    ASSERT_NE(edit, nullptr);
    ASSERT_NE(radio, nullptr);
    ASSERT_NE(label, nullptr);

    const QAccessible::Role rEdit = iface(edit)->role();
    const QAccessible::Role rRadio = iface(radio)->role();
    const QAccessible::Role rLabel = iface(label)->role();

    EXPECT_NE(rEdit, rRadio);
    EXPECT_NE(rEdit, rLabel);
    EXPECT_NE(rRadio, rLabel);
}

// MainWindow toolbar buttons must expose an ACTIONABLE control role so an AT
// announces them as operable — never a passive Pane/StaticText/NoRole. The
// exact role depends on the button kind: a plain action button -> Button, a
// menu button -> ButtonMenu, and a CHECKABLE toggle (e.g. bold/italic) -> a
// CheckBox (Qt maps checkable QToolButtons to CheckBox, which is correct: the AT
// announces a checked/unchecked toggle). Non-vacuity: asserts against the ACTUAL
// role of every realised QToolButton, requires at least one to exist, requires
// at least one plain Button (proving the actionable set is genuinely reached),
// and explicitly rejects the passive roles a broken tree would report.
TEST_F(A11yDialogRolesTest, MainWindowToolButtonsExposeActionableRole) {
    MainWindow win;
    const QList<QToolButton*> toolButtons = win.findChildren<QToolButton*>();
    int checked = 0;
    int plainButtons = 0;
    for (QToolButton* b : toolButtons) {
        QAccessibleInterface* bi = iface(b);
        if (!bi) continue;  // some transient tool buttons may not be realised
        const QAccessible::Role role = bi->role();
        const bool actionable = role == QAccessible::Button
                             || role == QAccessible::ButtonMenu
                             || role == QAccessible::CheckBox;
        EXPECT_TRUE(actionable)
            << "tool button '" << b->objectName().toStdString()
            << "' exposed non-actionable role " << static_cast<int>(role);
        // A screen reader must never see a toolbar control as a passive node.
        EXPECT_NE(role, QAccessible::Pane);
        EXPECT_NE(role, QAccessible::StaticText);
        EXPECT_NE(role, QAccessible::NoRole);
        if (role == QAccessible::Button) ++plainButtons;
        ++checked;
    }
    EXPECT_GT(checked, 0)
        << "MainWindow must expose at least one accessible tool button";
    EXPECT_GT(plainButtons, 0)
        << "expected at least one plain action Button among the tool buttons "
           "(proves the actionable-role set is genuinely reached, not vacuous)";
}
