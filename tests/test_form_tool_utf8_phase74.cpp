// -*- coding: utf-8 -*-
//
// Phase-74 (pipeline phase-21) — WIN-49 prerequisite: FormTool edited UTF-8 by
// the BYTE.
//
// `FormTool::editBuffer_` is a std::string holding UTF-8, and `cursorPosition_`
// was used as a raw byte index: Backspace erased one byte, Delete erased one
// byte, Left/Right stepped one byte, insert advanced by the byte length. Every
// character outside ASCII is 2-4 bytes, so ONE Backspace over "é" or "中" split
// the sequence and left invalid UTF-8 in the buffer — which commitEdit() then
// writes into the PDF field value.
//
// This is broken TODAY, with no IME involved. It has to be fixed before IME
// composition is wired in (phase-23), because a composition system that hands
// back multi-byte text would otherwise inherit a buffer that cannot represent
// it.
//
// WHY THESE TESTS ASSERT ON editBuffer_ DIRECTLY. The invariant is a property of
// the buffer at every instant, not of the value that eventually reaches the PDF.
// A test that only checked the committed value would pass on an implementation
// that corrupted the buffer and sanitised it on the way out — and would then be
// green while the on-screen text, the caret position and any future IME
// integration all still saw a broken buffer.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "core/Utf8.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"
#include "forms/JavaScriptEngine.h"

#include <QKeyEvent>
#include <QMouseEvent>

#include <filesystem>
#include <memory>
#include <string>

namespace {

// U+00E9 LATIN SMALL LETTER E WITH ACUTE — 2 bytes (C3 A9).
const std::string kEAcute = "\xC3\xA9";
// U+4E2D CJK IDEOGRAPH "middle" — 3 bytes (E4 B8 AD).
const std::string kCjk = "\xE4\xB8\xAD";
// U+1F600 GRINNING FACE — 4 bytes (F0 9F 98 80). Included because a 4-byte
// sequence is the one a "step two bytes" half-fix still splits.
const std::string kEmoji = "\xF0\x9F\x98\x80";

} // namespace

class FormToolUtf8Test : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        const std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        formPdf = fixturesDir / "test_form.pdf";
        if (!std::filesystem::exists(formPdf)) return;

        document = pdfe::core::Document::open(context, formPdf);
        manager = std::make_unique<pdfe::forms::FormManager>(context);
        jsEngine = std::make_unique<pdfe::forms::JavaScriptEngine>(context);
        undoManager = std::make_unique<pdfe::commands::UndoManager>();
        tool = std::make_unique<pdfe::forms::FormTool>(
            manager.get(), jsEngine.get(), undoManager.get());
        page = document->page(0);
        tool->activate();
        tool->setPage(page.get());
    }

    void TearDown() override {
        tool.reset();
        page.reset();
        undoManager.reset();
        jsEngine.reset();
        manager.reset();
        document.reset();
        context.reset();
    }

    /// Click into the first editable text field and begin editing. Returns its
    /// widget index, or -1 if the fixture has none.
    int beginEditingTextField()
    {
        const auto fields = manager->getFormFields(page.get());
        for (const auto& f : fields) {
            if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
                QMouseEvent mouseEvent(QEvent::MouseButtonPress, QPointF(0, 0),
                                       Qt::LeftButton, Qt::LeftButton,
                                       Qt::NoModifier);
                tool->onMousePress(&mouseEvent, 0, f.rect.center());
                return f.widgetIndex;
            }
        }
        return -1;
    }

    /// Type text through the same route the UI uses.
    void type(const QString& s)
    {
        QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, s);
        tool->onKeyPress(&ev);
    }

    void pressKey(int key)
    {
        QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
        tool->onKeyPress(&ev);
    }

    const std::string& buffer() const { return tool->editBufferForTesting(); }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path formPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::forms::FormManager> manager;
    std::unique_ptr<pdfe::forms::JavaScriptEngine> jsEngine;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager;
    std::unique_ptr<pdfe::forms::FormTool> tool;
    std::unique_ptr<pdfe::core::Page> page;
};

// =====================================================================
// The reproducing cases: one keystroke must consume one CHARACTER.
// =====================================================================

TEST_F(FormToolUtf8Test, BackspaceRemovesAWholeCodepointNotOneByte) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    ASSERT_GE(beginEditingTextField(), 0)
        << "test_form.pdf must contain an editable text field";

    type(QString::fromStdString(kEAcute));
    ASSERT_EQ(buffer(), kEAcute);
    ASSERT_TRUE(pdfe::core::utf8::isValid(buffer()));

    pressKey(Qt::Key_Backspace);

    // Under byte indexing this left a lone 0xC3 lead byte: length 1, and
    // INVALID UTF-8. Both halves are asserted, because a fix that emptied the
    // buffer by luck would satisfy only the first.
    EXPECT_TRUE(buffer().empty())
        << "one Backspace must remove the whole 2-byte character, leaving "
           "nothing; buffer holds " << buffer().size() << " byte(s)";
    EXPECT_TRUE(pdfe::core::utf8::isValid(buffer()))
        << "Backspace split a multi-byte sequence and left invalid UTF-8";
    EXPECT_EQ(tool->cursorPositionForTesting(), 0);
}

TEST_F(FormToolUtf8Test, BackspaceOverThreeAndFourByteCharacters) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    ASSERT_GE(beginEditingTextField(), 0);

    // A 3-byte and a 4-byte character: a "step two bytes" half-fix passes the
    // é case above and fails here, which is exactly why both are present.
    type(QString::fromStdString(kCjk + kEmoji));
    ASSERT_EQ(buffer(), kCjk + kEmoji);
    ASSERT_TRUE(pdfe::core::utf8::isValid(buffer()));

    pressKey(Qt::Key_Backspace);
    EXPECT_EQ(buffer(), kCjk) << "Backspace must remove the whole 4-byte emoji";
    EXPECT_TRUE(pdfe::core::utf8::isValid(buffer()));

    pressKey(Qt::Key_Backspace);
    EXPECT_EQ(buffer(), "") << "Backspace must remove the whole 3-byte CJK char";
    EXPECT_TRUE(pdfe::core::utf8::isValid(buffer()));
}

TEST_F(FormToolUtf8Test, DeleteRemovesAWholeCodepointNotOneByte) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    ASSERT_GE(beginEditingTextField(), 0);

    type(QString::fromStdString(kCjk + "A"));
    ASSERT_EQ(buffer(), kCjk + "A");

    pressKey(Qt::Key_Home);
    ASSERT_EQ(tool->cursorPositionForTesting(), 0);

    pressKey(Qt::Key_Delete);
    EXPECT_EQ(buffer(), "A")
        << "Delete must remove the whole 3-byte character at the cursor";
    EXPECT_TRUE(pdfe::core::utf8::isValid(buffer()));
}

TEST_F(FormToolUtf8Test, LeftAndRightStepWholeCodepoints) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    ASSERT_GE(beginEditingTextField(), 0);

    type(QString::fromStdString(kEAcute + kCjk));
    const std::string expected = kEAcute + kCjk;
    ASSERT_EQ(buffer(), expected);
    ASSERT_EQ(tool->cursorPositionForTesting(), 5);  // 2 + 3 bytes

    // Left twice: past the CJK char (3 bytes) then past é (2 bytes). Under byte
    // stepping these landed on offsets 4 and 3 — both INSIDE a sequence.
    pressKey(Qt::Key_Left);
    EXPECT_EQ(tool->cursorPositionForTesting(), 2)
        << "Left must move to the start of the 3-byte character, not into it";
    pressKey(Qt::Key_Left);
    EXPECT_EQ(tool->cursorPositionForTesting(), 0);

    // And back.
    pressKey(Qt::Key_Right);
    EXPECT_EQ(tool->cursorPositionForTesting(), 2);
    pressKey(Qt::Key_Right);
    EXPECT_EQ(tool->cursorPositionForTesting(), 5);

    // Neither movement may alter the text.
    EXPECT_EQ(buffer(), expected);
}

TEST_F(FormToolUtf8Test, CursorNeverRestsInsideASequenceWhileTyping) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    ASSERT_GE(beginEditingTextField(), 0);

    // Mixed-width text, then walk the whole buffer in both directions checking
    // the invariant at every stop. This is the general property; the cases
    // above are the specific ones a reader can reason about.
    type(QString::fromStdString("a" + kEAcute + "b" + kCjk + kEmoji + "c"));
    ASSERT_TRUE(pdfe::core::utf8::isValid(buffer()));

    pressKey(Qt::Key_End);
    for (int i = 0; i < 12; ++i) {
        EXPECT_TRUE(pdfe::core::utf8::isBoundary(
            buffer(), static_cast<std::size_t>(tool->cursorPositionForTesting())))
            << "cursor rested inside a multi-byte sequence at byte "
            << tool->cursorPositionForTesting() << " after " << i << " Left(s)";
        pressKey(Qt::Key_Left);
    }
    for (int i = 0; i < 12; ++i) {
        EXPECT_TRUE(pdfe::core::utf8::isBoundary(
            buffer(), static_cast<std::size_t>(tool->cursorPositionForTesting())))
            << "cursor rested inside a multi-byte sequence at byte "
            << tool->cursorPositionForTesting() << " after " << i << " Right(s)";
        pressKey(Qt::Key_Right);
    }
    EXPECT_EQ(buffer(), "a" + kEAcute + "b" + kCjk + kEmoji + "c");
}

// =====================================================================
// Controls — a fix that breaks ASCII editing must not pass.
// =====================================================================

TEST_F(FormToolUtf8Test, AsciiEditingIsUnchanged) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    ASSERT_GE(beginEditingTextField(), 0);

    type("abc");
    EXPECT_EQ(buffer(), "abc");
    EXPECT_EQ(tool->cursorPositionForTesting(), 3);

    pressKey(Qt::Key_Backspace);
    EXPECT_EQ(buffer(), "ab");

    pressKey(Qt::Key_Left);
    EXPECT_EQ(tool->cursorPositionForTesting(), 1);
    pressKey(Qt::Key_Delete);
    EXPECT_EQ(buffer(), "a");
}

TEST_F(FormToolUtf8Test, BackspaceAtStartAndDeleteAtEndAreNoOps) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    ASSERT_GE(beginEditingTextField(), 0);

    // The boundary helpers must not walk off either end. An off-by-one here is
    // a crash, not a wrong character.
    type(QString::fromStdString(kCjk));
    pressKey(Qt::Key_Home);
    pressKey(Qt::Key_Backspace);
    EXPECT_EQ(buffer(), kCjk) << "Backspace at offset 0 must do nothing";

    pressKey(Qt::Key_End);
    pressKey(Qt::Key_Delete);
    EXPECT_EQ(buffer(), kCjk) << "Delete at end-of-buffer must do nothing";
    EXPECT_TRUE(pdfe::core::utf8::isValid(buffer()));
}

TEST_F(FormToolUtf8Test, InsertingMidStringLandsOnABoundary) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    ASSERT_GE(beginEditingTextField(), 0);

    type(QString::fromStdString(kCjk + kCjk));
    pressKey(Qt::Key_Left);              // between the two CJK characters
    ASSERT_EQ(tool->cursorPositionForTesting(), 3);

    type("X");
    EXPECT_EQ(buffer(), kCjk + "X" + kCjk)
        << "insert must land between characters, not inside one";
    EXPECT_TRUE(pdfe::core::utf8::isValid(buffer()));
}

// =====================================================================
// Criterion 4 — the value that actually reaches the PDF survives.
// =====================================================================

TEST_F(FormToolUtf8Test, CommittedValueRoundTripsThroughTheDocument) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    const int idx = beginEditingTextField();
    ASSERT_GE(idx, 0);

    const std::string typed = kEAcute + kCjk + kEmoji;
    type(QString::fromStdString(typed));
    ASSERT_EQ(buffer(), typed);

    // Commit the way the UI does: deactivating the tool commits the edit.
    tool->deactivate();

    const auto after = manager->getFormField(page.get(), idx);
    EXPECT_EQ(after.value, typed)
        << "the committed field value is not the exact codepoint sequence typed";
    EXPECT_TRUE(pdfe::core::utf8::isValid(after.value))
        << "the value written into the document is not valid UTF-8";
}

TEST_F(FormToolUtf8Test, MultiByteValueSurvivesSaveAndReload)
{
    // CRITERION 4, LITERALLY. The test above commits and reads back through
    // FormManager, which returns the LIVE in-memory annotation -- so it never
    // touches the PDF's own string encoding. The `karen` gate failed this phase
    // for exactly that gap: the criterion says "round-trip save/reload", and
    // the place multi-byte corruption would actually appear is the encoding
    // step on the way to disk (PDF text strings are PDFDocEncoding or
    // UTF-16BE, neither of which is UTF-8).
    //
    // This one writes the file, reopens it, and reads the field out of the
    // reopened document.
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    const int idx = beginEditingTextField();
    ASSERT_GE(idx, 0);

    const std::string typed = kEAcute + kCjk + kEmoji;
    type(QString::fromStdString(typed));
    ASSERT_EQ(buffer(), typed);
    tool->deactivate();

    const std::filesystem::path out =
        std::filesystem::temp_directory_path() /
        "pdfe_phase74_utf8_roundtrip.pdf";
    std::error_code ec;
    std::filesystem::remove(out, ec);
    ASSERT_NO_THROW(document->save(out)) << "save failed";
    ASSERT_TRUE(std::filesystem::exists(out));

    {
        auto ctx2 = std::make_shared<pdfe::core::MuPdfContext>();
        auto reopened = pdfe::core::Document::open(ctx2, out);
        ASSERT_NE(reopened, nullptr) << "could not reopen the saved document";
        auto page2 = reopened->page(0);
        ASSERT_NE(page2, nullptr);

        pdfe::forms::FormManager mgr2(ctx2);
        const auto after = mgr2.getFormField(page2.get(), idx);
        EXPECT_EQ(after.value, typed)
            << "the codepoint sequence did not survive save + reload";
        EXPECT_TRUE(pdfe::core::utf8::isValid(after.value))
            << "the reloaded value is not valid UTF-8";
    }

    std::filesystem::remove(out, ec);
}

// =====================================================================
// The helper's own contract. These are what the mutation rows target.
// =====================================================================

TEST(Utf8Boundaries, ValidatorRejectsSplitAndOverlongSequences) {
    using namespace pdfe::core::utf8;

    EXPECT_TRUE(isValid(""));
    EXPECT_TRUE(isValid("plain ascii"));
    EXPECT_TRUE(isValid(kEAcute + kCjk + kEmoji));

    // A lone lead byte — exactly what byte-wise Backspace produced.
    EXPECT_FALSE(isValid("\xC3"));
    // A lone continuation byte.
    EXPECT_FALSE(isValid("\xA9"));
    // Truncated 3-byte sequence.
    EXPECT_FALSE(isValid("\xE4\xB8"));
    // Over-long encoding of '/' — the classic path-traversal smuggle. A
    // validator that only checks continuation-byte shape accepts this.
    EXPECT_FALSE(isValid("\xC0\xAF"));
    // Surrogate half, which is not encodable in UTF-8.
    EXPECT_FALSE(isValid("\xED\xA0\x80"));
    // Past U+10FFFF.
    EXPECT_FALSE(isValid("\xF5\x80\x80\x80"));
}

TEST(Utf8Boundaries, MovementTerminatesOnMalformedInput) {
    using namespace pdfe::core::utf8;

    // A buffer that is NOT valid UTF-8 must still be traversable: nextBoundary
    // has to make progress, or a cursor-movement loop hangs the UI on a damaged
    // value loaded from a hostile document.
    //
    // THE CASE LIST IS THE POINT, and it grew because a mutation survived. The
    // first version of this test used only "\xC3\xC3\xC3" — but 0xC3 is a LEAD
    // byte, so an implementation that trusts the lead byte's declared length
    // still advances on it, and the row that replaced the scanning loop with
    // exactly that implementation passed. The input that discriminates is one
    // beginning with a CONTINUATION byte, which such an implementation reports
    // as zero-length. Same shape as phase-72's IPv4-mapped survivor: the
    // mutation was not redundant, my examples were incomplete.
    const std::string bad[] = {
        "\xC3\xC3\xC3",       // lead bytes with no continuations
        "\xA9\xA9",           // STARTS with a continuation byte (the killer)
        "abc\x80\x80xyz",     // orphan continuations mid-string
        "\xF0\x9F",           // truncated 4-byte sequence
        "\xFF\xFE",           // bytes that are never valid UTF-8 at all
    };
    for (const auto& b : bad) {
        SCOPED_TRACE("malformed input of " + std::to_string(b.size()) + " byte(s)");
        std::size_t pos = 0;
        int steps = 0;
        while (pos < b.size() && steps < 100) {
            const std::size_t next = nextBoundary(b, pos);
            EXPECT_GT(next, pos) << "nextBoundary did not advance at byte " << pos;
            if (next <= pos) break;   // do not spin if the invariant is broken
            pos = next;
            ++steps;
        }
        EXPECT_LT(steps, 100) << "traversal of malformed input did not terminate";
    }

    EXPECT_EQ(codepointCount(""), 0u);
    EXPECT_EQ(codepointCount("abc"), 3u);
    EXPECT_EQ(codepointCount(kEAcute + kCjk + kEmoji), 3u);
}
