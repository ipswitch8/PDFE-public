// test_writing_mode_autoplumb.cpp
//
// R2-21a: auto-plumbing of writing mode from extraction into the text-edit
// overlay, WITHOUT any manual setter.
//
// The reconciled product edge runs entirely on the live text-edit (vector text)
// pipeline — the path a user actually edits — not the OCR/bitmap-composite path
// (which has no writing-mode source: HOCR carries none, and nothing sets a
// vertical FontMatch). The closed edge is:
//
//   SOURCE : core::TextLine::writingMode   (populated in
//            src/core/TextExtractor.cpp from MuPDF fz_stext_line::wmode)
//   CARRIER: textedit::TextSpan::writingMode  (copied in the TextEditModel ctor)
//   SINK   : TextEditTool::selectSpan(), which calls
//            setOverlayVerticalMode(span.writingMode == 1) so the overlay's
//            vertical/RTL caret + selection geometry (R2-21b) is selected
//            automatically on span activation — no caller flips a seam by hand.
//
// Two oracles pin the two links of the edge:
//   (1) SOURCE -> CARRIER, headless: a TextLine's writingMode propagates onto
//       the TextSpan the model builds from it.
//   (2) CARRIER -> SINK, through the REAL tool: selecting a span drives the
//       overlay writing mode from that span's carried writingMode. Proven with
//       a pre-forced stale vertical mode + a horizontal fixture span, so the
//       wire's ABSENCE (mutation) leaves the stale mode and reddens the test.
//
// UTF-8 source; all non-ASCII codepoints written as \u escapes.

#include <gtest/gtest.h>

#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/TextBlock.h"
#include "core/TextLine.h"
#include "core/TextChar.h"

#include <QRectF>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::TextBlock;
using pdfe::core::TextChar;
using pdfe::core::TextLine;
using pdfe::textedit::TextEditModel;
using pdfe::textedit::TextEditTool;

namespace {

std::string fixturesDir() { return TEST_FIXTURES_DIR; }

// Build a one-line TextBlock carrying the given writing mode. Two same-font
// chars make one span (the model groups a run of equal-font chars into a span).
TextBlock makeLineBlock(int writingMode) {
    TextChar a;
    a.codepoint = U'A';
    a.fontName = "Helvetica";
    a.fontSize = 12.0f;
    a.quad = QRectF(0, 0, 6, 12);
    TextChar b = a;
    b.codepoint = U'B';
    b.quad = QRectF(6, 0, 6, 12);

    TextLine line;
    line.writingMode = writingMode;
    line.chars = {a, b};
    line.boundingBox = QRectF(0, 0, 12, 12);

    TextBlock block;
    block.lines = {line};
    block.boundingBox = line.boundingBox;
    return block;
}

} // namespace

// ---------------------------------------------------------------------------
// (1) SOURCE -> CARRIER: TextLine::writingMode propagates onto TextSpan.
// ---------------------------------------------------------------------------

TEST(WritingModeCarrier, VerticalLinePropagatesToSpan) {
    TextEditModel model({makeLineBlock(/*writingMode=*/1)});
    ASSERT_GT(model.spanCount(), 0) << "one-line block must yield >=1 span";
    EXPECT_EQ(model.span(0).writingMode, 1)
        << "a vertical (wmode==1) source line must carry writingMode onto its span";
}

TEST(WritingModeCarrier, HorizontalLinePropagatesToSpan) {
    TextEditModel model({makeLineBlock(/*writingMode=*/0)});
    ASSERT_GT(model.spanCount(), 0);
    EXPECT_EQ(model.span(0).writingMode, 0)
        << "a horizontal (wmode==0) source line must leave the span horizontal";
}

// Differential: identical block except for the source line's writing mode ->
// isolates the carrier as the sole cause of the span's writingMode value.
TEST(WritingModeCarrier, DiffersOnlyByLineWritingMode) {
    TextEditModel vModel({makeLineBlock(1)});
    TextEditModel hModel({makeLineBlock(0)});
    ASSERT_GT(vModel.spanCount(), 0);
    ASSERT_GT(hModel.spanCount(), 0);
    EXPECT_NE(vModel.span(0).writingMode, hModel.span(0).writingMode)
        << "only the source line writing mode changed; the span must reflect it";
}

// ---------------------------------------------------------------------------
// (2) CARRIER -> SINK, through the real TextEditTool: selecting a span drives
//     the overlay writing mode from the span's carried writingMode.
// ---------------------------------------------------------------------------

class WritingModeSinkTest : public ::testing::Test {
protected:
    void SetUp() override { context_ = std::make_shared<MuPdfContext>(); }
    std::shared_ptr<MuPdfContext> context_;
};

// A horizontal vector-text fixture: after pre-forcing a STALE vertical overlay
// mode, selecting a (horizontal) span must reset the overlay to horizontal, and
// in general the overlay mode must equal (span.writingMode == 1). Removing the
// selectSpan wire leaves the forced-true mode in place -> this reddens.
TEST_F(WritingModeSinkTest, SelectSpanDrivesOverlayModeFromSpanWritingMode) {
    auto path = std::filesystem::path(fixturesDir()) / "invoice_vector_heading.pdf";
    ASSERT_TRUE(std::filesystem::exists(path))
        << "fixture invoice_vector_heading.pdf must exist";

    auto doc = Document::open(context_, path);
    ASSERT_NE(doc, nullptr);

    TextEditTool tool(doc.get(), context_);
    tool.activate();
    tool.setPage(0);

    const int spans = tool.spanCountForTesting();
    ASSERT_GT(spans, 0)
        << "invoice_vector_heading.pdf must expose hit-testable vector-text spans";

    // Pre-poison the overlay with a stale vertical mode. A live selectSpan wire
    // must overwrite it from the span's own writing mode.
    tool.setOverlayVerticalMode(true);
    ASSERT_TRUE(tool.overlayVerticalMode());

    ASSERT_TRUE(tool.selectSpanForTesting(0)) << "selecting span 0 must succeed";

    const bool spanIsVertical = tool.spanWritingModeForTesting(0) == 1;
    EXPECT_EQ(tool.overlayVerticalMode(), spanIsVertical)
        << "selectSpan must set the overlay writing mode from the span's carried "
           "writingMode (drop of the wire leaves the stale forced-true mode)";

    // This fixture is horizontal, so the concrete expectation is: reset to false.
    EXPECT_FALSE(spanIsVertical)
        << "invoice_vector_heading.pdf is horizontal text (wmode 0)";
    EXPECT_FALSE(tool.overlayVerticalMode())
        << "a horizontal span must reset the stale vertical overlay mode to false";

    tool.deactivate();
}
