// -*- coding: utf-8 -*-
// Pipeline a3f8e21d phase 2 — text-edit UX regressions reported against the
// Examplecorp RTV PDF:
//   (3) "no option to set text color while editing"
//   (4) "edit mode starts with all text selected"
//   (5) "Ctrl+A has no effect while editing"
//
// These are UI-behaviour bugs, so the tests exercise the real production
// objects (TextEditTool / TextEditToolBar / MainWindow) rather than a mock.
// The tests are written to assert the *desired* behaviour; when run against a
// tree where the behaviour is already correct they pass (regression guard),
// and where it is broken they fail RED first. The RED/GREEN status recorded
// at authoring time is noted per group.
//
// Fixtures are customer-data-free: embedded_descriptor_style.pdf is a synthetic
// native-text page that the toolbar-indicator suite already relies on for a
// hit-testable vector span (spanCount > 0).

#include <gtest/gtest.h>

#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "ui/TextEditToolBar.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QToolButton>

#include <filesystem>
#include <memory>
#include <string>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::textedit;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::filesystem::path fixturesDir() { return std::filesystem::path(TEST_FIXTURES_DIR); }

// Native-text fixture with at least one hit-testable vector span.
const char* kSpanFixture = "embedded_descriptor_style.pdf";

}  // namespace

// ─── Headless TextEditTool harness (bugs 4 + 3-behaviour) ───────────────────

class TextEditUxToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        auto p = fixturesDir() / kSpanFixture;
        if (std::filesystem::exists(p)) {
            doc_ = Document::open(ctx_, p);
            if (doc_ && doc_->pageCount() > 0)
                tool_ = std::make_unique<TextEditTool>(doc_.get(), ctx_);
        }
    }
    void TearDown() override {
        tool_.reset();
        doc_.reset();
        ctx_.reset();
    }

    // Enter edit mode by clicking the centre of span 0. Returns false if the
    // fixture / span is unavailable (caller SKIPs — the fixture ships in-repo
    // so this is a genuine-absence guard, not a way to dodge a red test).
    bool enterSpan0() {
        if (!tool_) return false;
        tool_->activate();
        tool_->setPage(0);
        if (tool_->spanCountForTesting() <= 0) return false;
        QRectF bbox = tool_->spanBboxForTesting(0);
        if (bbox.isEmpty()) return false;
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->onMousePress(&press, 0, bbox.center());
        return tool_->isEditing();
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<TextEditTool> tool_;
};

// Bug 4 — entering a span places a caret at the clicked character, NOT a
// full-span selection. Authoring status: GREEN (already correct in HEAD; the
// onMousePress path clears the selection). This locks that contract so a future
// change that re-introduces select-on-enter fails.
TEST_F(TextEditUxToolTest, EnteringSpanPlacesCaretNotFullSelection) {
    if (!enterSpan0()) GTEST_SKIP() << kSpanFixture << " span 0 not available";

    const int selStart = tool_->selectionStartForTesting();
    const int selEnd = tool_->selectionEndForTesting();

    // A live selection is (start >= 0 && start != end). Entering must not have one.
    const bool hasSelection = (selStart >= 0 && selStart != selEnd);
    EXPECT_FALSE(hasSelection)
        << "entering a span must place a caret, not select the whole span "
           "(selStart=" << selStart << " selEnd=" << selEnd << ")";
    EXPECT_EQ(selStart, -1)
        << "selectionStart_ must be the no-selection sentinel on edit-enter";
}

// Bug 3 (behaviour) — applyTextColor with only a caret (no range selection)
// recolors the whole active span. Authoring status: GREEN (applyFormattingField
// falls back to the full span when there is no selection). Proves the color
// control has a real effect during editing even before the user drags a
// selection.
TEST_F(TextEditUxToolTest, ApplyTextColorWithCaretRecolorsWholeSpan) {
    if (!enterSpan0()) GTEST_SKIP() << kSpanFixture << " span 0 not available";

    ASSERT_EQ(tool_->selectionStartForTesting(), -1)
        << "precondition: caret only, no range selection";

    tool_->applyTextColor(QColor(0xC0, 0x10, 0x20));  // a distinctive red

    const TextSpan* s = tool_->activeSpan();
    ASSERT_NE(s, nullptr) << "an active span must exist while editing";
    ASSERT_FALSE(s->formattingRuns.empty())
        << "applyTextColor must create a formatting run even with no selection";

    bool foundColor = false;
    for (const auto& r : s->formattingRuns) {
        if ((r.textColor & 0x00FFFFFFu) == 0x00C01020u &&
            r.charEnd > r.charStart) {
            foundColor = true;
            // The run must span the whole span text (0 .. len) when applied
            // with only a caret.
            EXPECT_EQ(r.charStart, 0);
            EXPECT_EQ(r.charEnd, static_cast<int>(s->text.size()));
        }
    }
    EXPECT_TRUE(foundColor)
        << "the caret-applied color must land on a run covering the span";
}

// ─── TextEditToolBar widget test (bug 3 — control exists) ───────────────────

// Bug 3 (control) — the edit toolbar exposes a color control with a stable
// objectName + accessibleName (automation-friendly per CLAUDE.md) that is
// enabled and emits textColorChanged. Authoring status: GREEN (textColorButton
// added in e425724). Locks the control's presence + wiring.
TEST(TextEditToolBarColorControl, HasEnabledColorButtonWithStableIds) {
    pdfe::ui::TextEditToolBar bar;

    auto* btn = bar.findChild<QToolButton*>("textColorButton");
    ASSERT_NE(btn, nullptr)
        << "the text-edit toolbar must expose a 'textColorButton' color control";
    EXPECT_TRUE(btn->isEnabled())
        << "the color control must be usable during text editing";
    EXPECT_FALSE(btn->accessibleName().isEmpty())
        << "the color control needs an accessibleName for UIA automation";

    // The textColorChanged signal is the tool-facing contract (MainWindow wires
    // it to TextEditTool::applyTextColor). It must exist and be spy-able.
    QSignalSpy spy(&bar, &pdfe::ui::TextEditToolBar::textColorChanged);
    EXPECT_TRUE(spy.isValid())
        << "TextEditToolBar must expose a textColorChanged(QColor) signal";
}

// ─── MainWindow routing test (bug 5 — Ctrl+A shadowing) ─────────────────────

class SelectAllRoutingTest : public ::testing::Test {
protected:
    void SetUp() override {
        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        QApplication::processEvents();
    }
    void TearDown() override {
        mw_.reset();
        QApplication::processEvents();
    }
    QAction* action(const QString& name) { return mw_->findChild<QAction*>(name); }
    pdfe::ui::PdfCanvas* canvas() {
        return mw_->findChild<pdfe::ui::PdfCanvas*>("pdfCanvas");
    }
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
};

// Bug 5 — while a text-edit session is active, triggering the global
// selectAllAction_ (which owns the QKeySequence::SelectAll shortcut, i.e. what
// Ctrl+A fires) must select all text in the edited span, NOT copy the whole
// page to the clipboard. Before the fix the global QAction shadows the tool:
// the page text lands on the clipboard and the tool selection stays empty (RED).
// Authoring status: RED first, GREEN after routing onSelectAll to the active
// tool.
TEST_F(SelectAllRoutingTest, CtrlAWhileEditingSelectsSpanNotPageCopy) {
    auto pdf = fixturesDir() / kSpanFixture;
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << kSpanFixture << " missing";

    mw_->openFile(QString::fromStdString(pdf.string()));
    QApplication::processEvents();

    auto* c = canvas();
    ASSERT_NE(c, nullptr) << "pdfCanvas must exist";
    ASSERT_NE(c->document(), nullptr) << "document must be loaded";

    auto* editAction = action("textEditToolAction");
    ASSERT_NE(editAction, nullptr);
    editAction->trigger();  // native text → installs TextEditTool
    QApplication::processEvents();

    auto* tool = c->textEditTool();
    ASSERT_NE(tool, nullptr) << "native text PDF must route to TextEditTool";

    // Enter edit mode on span 0.
    tool->setPage(0);
    ASSERT_GT(tool->spanCountForTesting(), 0) << "fixture must have a vector span";
    QRectF bbox = tool->spanBboxForTesting(0);
    ASSERT_FALSE(bbox.isEmpty());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, bbox.center());
    ASSERT_TRUE(tool->isEditing()) << "clicking the span must enter edit mode";

    const int spanLen = tool->spanTextForTesting(0).size();
    ASSERT_GT(spanLen, 0);

    // Sentinel clipboard content — the page-copy path would overwrite it.
    const QString sentinel = QStringLiteral("PDFE-CLIPBOARD-SENTINEL");
    QGuiApplication::clipboard()->setText(sentinel);

    auto* selAll = action("selectAllAction");
    ASSERT_NE(selAll, nullptr);
    ASSERT_TRUE(selAll->isEnabled()) << "selectAllAction is enabled when a doc is open";

    // Trigger exactly what pressing Ctrl+A fires (the QAction owns that shortcut).
    selAll->trigger();
    QApplication::processEvents();

    // Desired: the tool selected its whole span; the clipboard was NOT touched.
    EXPECT_EQ(tool->selectionStartForTesting(), 0)
        << "Ctrl+A while editing must select from the start of the span";
    EXPECT_EQ(tool->selectionEndForTesting(), spanLen)
        << "Ctrl+A while editing must select to the end of the span";
    EXPECT_EQ(QGuiApplication::clipboard()->text(), sentinel)
        << "Ctrl+A while editing must NOT copy the page text to the clipboard "
           "(global selectAllAction_ shadowed the text-edit tool)";
}

// Regression: when NO text-edit session is active, selectAllAction_ keeps its
// document-level behaviour (copy page text to clipboard). Ensures the bug-5 fix
// routes ONLY while editing and doesn't break the normal Select All.
TEST_F(SelectAllRoutingTest, SelectAllWithoutEditingCopiesPageText) {
    auto pdf = fixturesDir() / kSpanFixture;
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << kSpanFixture << " missing";

    mw_->openFile(QString::fromStdString(pdf.string()));
    QApplication::processEvents();

    QGuiApplication::clipboard()->setText(QStringLiteral("SENTINEL-NO-EDIT"));

    auto* selAll = action("selectAllAction");
    ASSERT_NE(selAll, nullptr);
    ASSERT_TRUE(selAll->isEnabled());
    selAll->trigger();
    QApplication::processEvents();

    // No text-edit session → page text must have been copied (clipboard changed).
    EXPECT_NE(QGuiApplication::clipboard()->text(),
              QStringLiteral("SENTINEL-NO-EDIT"))
        << "with no active edit, Select All must still copy the page text";
}
