// tests/test_text_edit_empty_area_insert.cpp
//
// Phase-11 regression locks for bug 9: "when using the Edit Text function it
// is not possible to select an empty area (e.g. a blank line) to start adding
// text there."
//
// CONFIRMED mechanism (triage): TextEditTool::onMousePress computed
// spanIndex = model_->spanAtPoint(pdfPoint); when spanIndex < 0 it only
// committed the active edit and RETURNED — there was no path to create a new
// text run at an empty location. These tests prove BOTH halves of the change:
//
//   BEFORE  — a point in empty page space has no editable span
//             (spanIndexAtPoint < 0): the exact condition that used to cause
//             the early return and reject the click.
//   AFTER   — a left-click there now begins a NEW text edit (an empty span is
//             created and made active), typed text lands in it, and on commit
//             the new run is WRITTEN to the page content stream via
//             InsertTextRunCommand so it round-trips save→reopen as REAL page
//             text a subsequent Edit Text click can select as a span (NOT a
//             form widget).
//
// Real oracles throughout: span existence/active state, span text, and a
// save→reopen re-extraction that finds the typed token as a page span.

#include <gtest/gtest.h>

#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "tre/TreCommands.h"
#include "tre/PdfContentWriter.h"
#include "tre/TreTypes.h"
#include "fonts/PdfFontEmbedder.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/TextExtractor.h"
#include "core/UndoManager.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QRectF>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::textedit;
using namespace pdfe::core;
using namespace pdfe::commands;

namespace {

class EmptyAreaInsertTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        fixtureSrc_ = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(fixtureSrc_)) {
            GTEST_SKIP() << "Test fixture not available: " << fixtureSrc_;
        }
        document_ = Document::open(context_, fixtureSrc_);
        if (!document_ || document_->pageCount() == 0) {
            GTEST_SKIP() << "Fixture failed to open";
        }
        tool_ = std::make_unique<TextEditTool>(document_.get(), context_);
        tool_->activate();
        tool_->setPage(0);

        const auto& info = *::testing::UnitTest::GetInstance()->current_test_info();
        tempOut_ = std::filesystem::temp_directory_path() /
                   ("pdfe_empty_insert_" + std::string(info.name()) + ".pdf");
        std::error_code ec;
        std::filesystem::remove(tempOut_, ec);
    }

    void TearDown() override {
        tool_.reset();
        document_.reset();
        std::error_code ec;
        std::filesystem::remove(tempOut_, ec);
    }

    // A left-button press (the real empty-area click path).
    void leftClickAt(const QPointF& pdfPoint) {
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(50, 50),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->onMousePress(&ev, 0, pdfPoint);
    }

    // Find a point on page 0 that is below all existing text (so genuinely
    // empty) yet still within the page box (so writeTextBlock places it on the
    // page). Returns {point, pageHeight}. Fails the test if no span geometry.
    QPointF emptyPointBelowText(double* pageHOut = nullptr) {
        auto page = document_->page(0);
        EXPECT_NE(page, nullptr);
        double pageH = page->boundingBox().height();
        if (pageHOut) *pageHOut = pageH;

        int n = tool_->spanCountForTesting();
        EXPECT_GT(n, 0) << "Fixture page 0 must have hit-testable vector text.";
        double maxBottom = 0.0;
        double anyLeft = 72.0;
        for (int i = 0; i < n; ++i) {
            QRectF b = tool_->spanBboxForTesting(i);
            if (b.bottom() > maxBottom) maxBottom = b.bottom();
            if (i == 0 && b.left() > 0) anyLeft = b.left();
        }
        // 20pt below the lowest text, left-aligned with it.
        return QPointF(anyLeft, maxBottom + 20.0);
    }

    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path fixtureSrc_;
    std::filesystem::path tempOut_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<TextEditTool> tool_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// BEFORE: the empty location has no editable span. This is the precise
// precondition (spanAtPoint < 0) that used to reject the click.
// ─────────────────────────────────────────────────────────────────────
TEST_F(EmptyAreaInsertTest, EmptyLocationHasNoSpan) {
    double pageH = 0.0;
    QPointF empty = emptyPointBelowText(&pageH);
    ASSERT_LT(empty.y(), pageH)
        << "Chosen empty point must still be on the page.";
    EXPECT_LT(tool_->spanIndexAtPoint(empty), 0)
        << "The chosen point must be empty page space (no existing span) — "
           "the condition that formerly caused onMousePress to bail.";
    EXPECT_FALSE(tool_->isEditing());
}

// ─────────────────────────────────────────────────────────────────────
// AFTER: a left-click on that empty location now BEGINS a new text edit.
// ─────────────────────────────────────────────────────────────────────
TEST_F(EmptyAreaInsertTest, EmptyAreaLeftClickStartsNewEdit) {
    QPointF empty = emptyPointBelowText();
    ASSERT_LT(tool_->spanIndexAtPoint(empty), 0);

    int spansBefore = tool_->spanCountForTesting();
    ASSERT_FALSE(tool_->isEditing());

    leftClickAt(empty);

    EXPECT_TRUE(tool_->isEditing())
        << "Clicking empty page space must start a new text run (bug 9).";
    int active = tool_->activeSpanIndexForTesting();
    ASSERT_GE(active, 0);
    EXPECT_EQ(tool_->spanCountForTesting(), spansBefore + 1)
        << "A new empty span must have been appended to the model.";

    // The new active span must be a new-insertion span, initially empty.
    EXPECT_EQ(tool_->spanTextForTesting(active), QString());

    // Typing now lands in the new span.
    tool_->onTextInput(QStringLiteral("Hi"));
    EXPECT_EQ(tool_->spanTextForTesting(active), QStringLiteral("Hi"));
}

// ─────────────────────────────────────────────────────────────────────
// Gating: a RIGHT-click on empty space must NOT create a run (it raises the
// context menu). Confirms the new path is scoped to a deliberate left-click.
// ─────────────────────────────────────────────────────────────────────
TEST_F(EmptyAreaInsertTest, EmptyAreaRightClickDoesNotInsert) {
    QPointF empty = emptyPointBelowText();
    int spansBefore = tool_->spanCountForTesting();

    QMouseEvent ev(QEvent::MouseButtonPress, QPointF(50, 50),
                   Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    tool_->onMousePress(&ev, 0, empty);

    EXPECT_FALSE(tool_->isEditing())
        << "A right-click must not begin a text insertion.";
    EXPECT_EQ(tool_->spanCountForTesting(), spansBefore)
        << "A right-click must not append a span.";
}

// ─────────────────────────────────────────────────────────────────────
// Model unit: addEmptySpan marks the span isNewInsertion and leaves it empty
// (so an untouched new span commits nothing via isModified()).
// ─────────────────────────────────────────────────────────────────────
TEST_F(EmptyAreaInsertTest, AddEmptySpanIsNewInsertionAndEmpty) {
    auto page = document_->page(0);
    auto blocks = page->extractText();
    TextEditModel model(blocks);
    int before = model.spanCount();

    int idx = model.addEmptySpan(QRectF(100, 100, 120, 15), "Helvetica", 12.0f);
    ASSERT_EQ(idx, before);
    ASSERT_EQ(model.spanCount(), before + 1);
    const TextSpan& s = model.span(idx);
    EXPECT_TRUE(s.isNewInsertion);
    EXPECT_TRUE(s.text.empty());
    EXPECT_FALSE(s.isModified())
        << "An untouched new span must not report as modified (commits nothing).";
    EXPECT_EQ(s.fontSize, 12.0f);
    EXPECT_EQ(s.fontName, "Helvetica");
}

// ─────────────────────────────────────────────────────────────────────
// Round-trip: empty-area click → type → commit WRITES a new content-stream
// run that survives save→reopen and is re-extractable as a REAL page span
// (selectable by a subsequent Edit Text click), NOT a form widget.
// ─────────────────────────────────────────────────────────────────────
TEST_F(EmptyAreaInsertTest, NewRunRoundTripsAsSelectablePageText) {
    const QString token = QStringLiteral("EMPTYAREATOKEN42");

    // Wire the commit path exactly as MainWindow does: a new-insertion span
    // routes to InsertTextRunCommand (content-stream WRITE), not EditTextCommand.
    auto embedder = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
    auto writer = std::make_unique<pdfe::tre::PdfContentWriter>(
        context_.get(), embedder.get());
    UndoManager undo;

    QRectF insertedBbox;
    tool_->textEdited = [&](int pageIndex, int /*spanIndex*/,
                            std::u32string /*oldText*/, std::u32string newText,
                            TextSpan spanInfo) {
        ASSERT_TRUE(spanInfo.isNewInsertion)
            << "Commit of an empty-area edit must carry isNewInsertion.";
        insertedBbox = spanInfo.boundingBox;
        const QString qNew = QString::fromUcs4(
            reinterpret_cast<const char32_t*>(newText.data()),
            static_cast<int>(newText.size()));
        pdfe::tre::PdfRect rect;
        rect.x0 = spanInfo.boundingBox.left();
        rect.y0 = spanInfo.boundingBox.top();
        rect.x1 = spanInfo.boundingBox.right();
        rect.y1 = spanInfo.boundingBox.bottom();
        pdfe::tre::RgbColor color;  // default black
        auto cmd = std::make_unique<pdfe::tre::InsertTextRunCommand>(
            document_.get(), context_, writer.get(), pageIndex, rect,
            qNew.toStdString(),
            spanInfo.fontName.empty() ? std::string("Helvetica") : spanInfo.fontName,
            spanInfo.fontSize > 0.0f ? spanInfo.fontSize : 12.0f, color);
        undo.execute(std::move(cmd));
    };

    QPointF empty = emptyPointBelowText();
    ASSERT_LT(tool_->spanIndexAtPoint(empty), 0);

    leftClickAt(empty);
    ASSERT_TRUE(tool_->isEditing());
    tool_->onTextInput(token);
    tool_->commitEdit();

    ASSERT_GT(undo.undoStackSize(), 0)
        << "Commit must have produced an InsertTextRunCommand on the stack.";

    // Save and reopen.
    document_->saveAs(tempOut_);
    tool_.reset();
    document_.reset();

    auto reopened = Document::open(context_, tempOut_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    ASSERT_NE(rePage, nullptr);

    // Proof 1: the token is present as PAGE text (TextExtractor only sees page
    // content-stream text, never a form-widget appearance stream). That it is
    // extracted at all is the "it is real page text, not a widget" proof.
    TextExtractor extractor(context_);
    std::string plain = extractor.extractPlainText(rePage.get());
    EXPECT_NE(plain.find(token.toStdString()), std::string::npos)
        << "Inserted run did not survive save→reopen as page text. plain="
        << plain.substr(0, 400);

    // Proof 2: it is selectable as a span — a TextEditModel built on reopen has
    // a span whose text contains the token (what a subsequent Edit Text click
    // would hit-test and select).
    auto blocks = extractor.extract(rePage.get());
    TextEditModel model2(blocks);
    bool foundSpan = false;
    for (int i = 0; i < model2.spanCount(); ++i) {
        const QString t = QString::fromUcs4(
            reinterpret_cast<const char32_t*>(model2.span(i).text.data()),
            static_cast<int>(model2.span(i).text.size()));
        if (t.contains(token)) { foundSpan = true; break; }
    }
    EXPECT_TRUE(foundSpan)
        << "Inserted text is not re-extractable as an editable span.";
}

// ─────────────────────────────────────────────────────────────────────
// Undo of the insertion removes the new run (lossless /Contents restore).
// ─────────────────────────────────────────────────────────────────────
TEST_F(EmptyAreaInsertTest, UndoRemovesInsertedRun) {
    const std::string token = "UNDOINSERTTOKEN77";

    auto embedder = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
    auto writer = std::make_unique<pdfe::tre::PdfContentWriter>(
        context_.get(), embedder.get());

    auto page = document_->page(0);
    double pageH = page->boundingBox().height();
    double maxBottom = 0.0;
    for (int i = 0; i < tool_->spanCountForTesting(); ++i)
        maxBottom = std::max(maxBottom, tool_->spanBboxForTesting(i).bottom());
    ASSERT_LT(maxBottom + 20.0, pageH);

    pdfe::tre::PdfRect rect;
    rect.x0 = 72.0; rect.y0 = maxBottom + 20.0;
    rect.x1 = 272.0; rect.y1 = maxBottom + 35.0;
    pdfe::tre::RgbColor color;

    pdfe::tre::InsertTextRunCommand cmd(
        document_.get(), context_, writer.get(), 0, rect, token,
        "Helvetica", 12.0f, color);
    cmd.execute();
    ASSERT_TRUE(cmd.succeeded());

    TextExtractor extractor(context_);
    {
        auto p = document_->page(0);
        std::string plain = extractor.extractPlainText(p.get());
        ASSERT_NE(plain.find(token), std::string::npos)
            << "Run must be present after execute().";
    }

    cmd.undo();
    {
        auto p = document_->page(0);
        std::string plain = extractor.extractPlainText(p.get());
        EXPECT_EQ(plain.find(token), std::string::npos)
            << "Undo must remove the inserted run from the page content.";
    }
}
