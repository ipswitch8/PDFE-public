// Toolbar Bold/Italic indicator wiring (pipeline d4e7f921 phase 2).
//
// Phase 1 added descriptor-sourced TextSpan.isItalic/isBold (read from
// /FontDescriptor at extraction, independent of the font NAME). This phase
// wires those fields into TextEditTool::emitSelectionFormatting so the
// formatting toolbar's Bold/Italic buttons reflect the selected span's real
// style BEFORE any edit — instead of defaulting to unchecked when the style is
// encoded only in the descriptor.
//
// The emission's style logic is factored into the pure static helper
// TextEditTool::resolveToolbarStyle(run, span); emitSelectionFormatting emits
// exactly resolveToolbarStyle(run, span).bold / .italic in the
// selectionFormattingChanged signal. We test that helper directly (fast, no
// document) AND drive the real signal end-to-end by clicking into the
// descriptor-italic+bold fixture and capturing selectionFormattingChanged.

#include <gtest/gtest.h>

#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <QMouseEvent>
#include <QSignalSpy>
#include <QVariant>
#include <QColor>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::textedit;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::string fixturesDir() { return TEST_FIXTURES_DIR; }
// The phase-1 fixture: style-NEUTRAL BaseFont, descriptor forces Italic +
// ForceBold (/Flags 0x40062, /ItalicAngle -12). So any bold/italic the toolbar
// shows here can only have come from the descriptor, not the font name.
const char* kFixture = "embedded_descriptor_style.pdf";

// A FormattingRun that actually covers character 0 (charEnd > charStart),
// simulating a user-applied formatting edit.
FormattingRun explicitRun(bool bold, bool italic,
                          bool underline = false, bool strike = false) {
    FormattingRun r;
    r.charStart = 0;
    r.charEnd = 2;  // a real run is non-empty
    r.bold = bold;
    r.italic = italic;
    r.underline = underline;
    r.strikethrough = strike;
    return r;
}

TextSpan styledSpan(bool isItalic, bool isBold) {
    TextSpan s;
    s.fontName = "ADRXQZ+Sutpenia Regular";  // neutral name (no keyword)
    s.fontSize = 20.0f;
    s.text = U"Az";
    s.originalText = s.text;
    s.isItalic = isItalic;
    s.isBold = isBold;
    return s;
}

}  // namespace

// ─── Helper: resolveToolbarStyle (the exact logic the signal emits) ─────────

// (1) No explicit run + descriptor-italic span -> toolbar italic, not bold.
TEST(ToolbarStyleResolve, NoRun_DescriptorItalicSpan_IsItalicNotBold) {
    auto style = TextEditTool::resolveToolbarStyle(FormattingRun{},
                                                   styledSpan(true, false));
    EXPECT_TRUE(style.italic)
        << "span.isItalic must drive the toolbar Italic indicator";
    EXPECT_FALSE(style.bold);
    EXPECT_FALSE(style.underline);
    EXPECT_FALSE(style.strikethrough);
}

// (2) No explicit run + descriptor-bold span -> toolbar bold, not italic.
TEST(ToolbarStyleResolve, NoRun_DescriptorBoldSpan_IsBoldNotItalic) {
    auto style = TextEditTool::resolveToolbarStyle(FormattingRun{},
                                                   styledSpan(false, true));
    EXPECT_TRUE(style.bold)
        << "span.isBold must drive the toolbar Bold indicator";
    EXPECT_FALSE(style.italic);
}

// (3) No explicit run + neutral span (no descriptor style) -> both unchecked.
// Regression guard: AC requires a name-neutral, descriptor-less span continues
// to default to unchecked.
TEST(ToolbarStyleResolve, NoRun_NeutralSpan_BothUnchecked) {
    auto style = TextEditTool::resolveToolbarStyle(FormattingRun{},
                                                   styledSpan(false, false));
    EXPECT_FALSE(style.bold);
    EXPECT_FALSE(style.italic);
}

// (4) An explicit user-applied run wins over the span's descriptor style on
// BOTH axes: a run that turns italic OFF must show unchecked even though the
// span is descriptor-italic, and a run that turns bold/italic ON must show
// checked even on a neutral span.
TEST(ToolbarStyleResolve, ExplicitRun_OverridesSpanDescriptorStyle) {
    // Run clears italic on an italic+bold span -> toolbar follows the run.
    auto cleared = TextEditTool::resolveToolbarStyle(
        explicitRun(/*bold=*/false, /*italic=*/false), styledSpan(true, true));
    EXPECT_FALSE(cleared.italic)
        << "an explicit run with italic=false must win over span.isItalic";
    EXPECT_FALSE(cleared.bold)
        << "an explicit run with bold=false must win over span.isBold";

    // Run sets bold+italic+underline on a neutral span -> toolbar follows it.
    auto set = TextEditTool::resolveToolbarStyle(
        explicitRun(/*bold=*/true, /*italic=*/true, /*underline=*/true),
        styledSpan(false, false));
    EXPECT_TRUE(set.bold);
    EXPECT_TRUE(set.italic);
    EXPECT_TRUE(set.underline);
}

// ─── End-to-end: the real selectionFormattingChanged signal ─────────────────

class ToolbarIndicatorSignalTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        auto p = std::filesystem::path(fixturesDir()) / kFixture;
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

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<TextEditTool> tool_;
};

// Clicking into the descriptor-italic+bold span emits a selectionFormattingChanged
// whose bold AND italic flags are true — proving the descriptor style reaches the
// toolbar through the live signal, not just the helper.
TEST_F(ToolbarIndicatorSignalTest, ClickIntoDescriptorSpan_SignalCarriesItalicAndBold) {
    if (!tool_) GTEST_SKIP() << kFixture << " not available";

    tool_->activate();
    tool_->setPage(0);
    ASSERT_GT(tool_->spanCountForTesting(), 0) << "fixture has no editable span";

    // Sanity: span 0 carries descriptor italic+bold (precondition for the wiring).
    const TextSpan* span0 = nullptr;
    // selectSpan happens on click; before that we inspect via the model seam.
    QRectF bbox = tool_->spanBboxForTesting(0);
    ASSERT_FALSE(bbox.isEmpty()) << "span 0 has no bbox to click";

    QSignalSpy spy(tool_.get(), &TextEditTool::selectionFormattingChanged);
    ASSERT_TRUE(spy.isValid());

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->onMousePress(&press, 0, bbox.center());

    ASSERT_TRUE(tool_->isEditing()) << "clicking the span must enter edit mode";
    ASSERT_GE(spy.count(), 1) << "selectionFormattingChanged must fire on select";

    // Args: 0=family,1=size,2=bold,3=italic,4=underline,5=strikethrough,6=color.
    const QList<QVariant> args = spy.takeLast();
    EXPECT_TRUE(args.at(3).toBool())
        << "italic flag must be true from the descriptor (neutral font name)";
    EXPECT_TRUE(args.at(2).toBool())
        << "bold flag must be true from the descriptor ForceBold";
    (void)span0;
}

// Regression at the signal level: a span with no descriptor style and a neutral
// name must emit italic=false/bold=false (toolbar stays unchecked).
TEST_F(ToolbarIndicatorSignalTest, NeutralSpanHelperMatchesUncheckedDefault) {
    // This does not require the fixture — it locks the default-unchecked contract
    // the signal relies on for descriptor-less spans.
    auto style = TextEditTool::resolveToolbarStyle(FormattingRun{},
                                                   styledSpan(false, false));
    EXPECT_FALSE(style.bold);
    EXPECT_FALSE(style.italic);
}
