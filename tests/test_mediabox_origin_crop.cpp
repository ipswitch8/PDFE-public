// R3-12 — MediaBox-origin crop correctness for copy-selection-as-image.
//
// copySelectionAsImage() renders the whole page and crops it to the selection.
// The pre-fix code assumed a ZERO-origin MediaBox: it did pxX = selX*scale
// (ignoring pageBox.x()) and Y-flipped pxY = (pageH - selY - selH)*scale. For a
// page whose MediaBox has a non-zero origin (here [100 100 700 900]) BOTH axes
// were wrong — the crop landed on the wrong part of the page. The shipped R3-12
// tests only asserted the crop SIZE (80x60pt * 2x), never its CONTENT, so this
// origin/flip defect was invisible.
//
// This suite is a class-level oracle: it opens a fixture with two solid,
// saturated colour blocks at KNOWN PDF coordinates inside an offset MediaBox,
// selects the sub-rect covering ONE block, and asserts the cropped clipboard
// image is that block's colour and NOT the other block's colour. The colour
// distance thresholds tolerate 2x scale + anti-aliasing (we sample a small
// window at the crop centre, deep inside a solid block).
//
// RED proof (why this reds pre-fix, greens post-fix):
//   * If fz_bound_page preserves the MediaBox origin (pageBox.x()/y() != 0), the
//     pre-fix pxX = selX*scale is offset by the origin (100pt*2x = 200px) AND
//     the flip mirrors an already-Y-DOWN selection — the crop lands on empty
//     white page, so the dominant colour is neither red nor blue → assertion
//     fails.
//   * If fz_bound_page roots at (0,0), the X term happens to be right, but the
//     Y-flip still mirrors the selection vertically — the red selection's mirror
//     lands at the red block's X but the wrong Y (white, since the blue block is
//     at a different X) → still not red → assertion fails.
//   Either way the post-fix code (subtract pageBox origin on both axes, no flip)
//   crops the correct block → assertion passes.
//
// Mutation note: reverting either the pageBox-origin subtraction (drop the
// `- pageBox.x()` / `- pageBox.y()`) or re-introducing the Y-flip in
// MainWindow::copySelectionAsImage re-reds this test.

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QThread>

#include "ui/MainWindow.h"
#include "core/Document.h"
#include "core/Page.h"
#include "support/ClipboardTestUtil.h"

#include <cmath>
#include <filesystem>
#include <memory>

using namespace pdfe::ui;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Clipboard helpers live in tests/support/ClipboardTestUtil.h — see the header
// for why a clipboard round-trip is racy on an RDP host and why the WRITE must
// be retried, not only the read.

template <typename Pred>
bool spinUntil(Pred pred, int budgetMs = 5000, int stepMs = 20) {
    for (int waited = 0; waited <= budgetMs; waited += stepMs) {
        if (pred()) return true;
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents,
                                        stepMs);
        QThread::msleep(stepMs);
    }
    return pred();
}

// Fixture geometry (must match tests/fixtures/gen_mediabox_offset.py):
//   MediaBox [100 100 700 900]  → origin (100,100), extent 600 x 800.
constexpr double kMediaX0 = 100.0;
constexpr double kMediaY0 = 100.0;
// RED  block  PDF user-space (Y-up): x[140..280], y[640..760]
// BLUE block  PDF user-space (Y-up): x[420..560], y[190..310]
// Sub-rects fully inside each block (>=20pt margin) that we actually select:
//   RED  sel  PDF Y-up: x[160..260], y[660..740]
//   BLUE sel  PDF Y-up: x[440..540], y[210..290]

// Convert a PDF user-space (Y-up) rect to the page's fitz frame (Y-DOWN, the
// frame boundingBox()/copySelectionRect_ live in), using the LIVE pageBox so
// the conversion is agnostic to whether fz_bound_page roots at (0,0) or
// preserves the MediaBox origin. This mirrors the production selection frame
// (SelectTool feeds pdf_bound_annot rects, which are already in this frame).
//
//   fitz_x = pageBox.x() + (pdf_x - MediaBox.x0)
//   fitz_y = pageBox.y() + pageBox.height() - (pdf_top - MediaBox.y0)   (top edge)
QRectF pdfUpToFitz(const QRectF& pageBox, double px0, double pyBottom,
                   double w, double h) {
    const double pdfTop = pyBottom + h;  // Y-up top edge
    const double fx = pageBox.x() + (px0 - kMediaX0);
    const double fy = pageBox.y() + pageBox.height() - (pdfTop - kMediaY0);
    return QRectF(fx, fy, w, h);
}

// Average colour over a small centred window of the image (robust to AA/rounding).
QColor sampleCentre(const QImage& img, int half = 4) {
    const int cx = img.width() / 2;
    const int cy = img.height() / 2;
    long r = 0, g = 0, b = 0, n = 0;
    for (int y = cy - half; y <= cy + half; ++y) {
        for (int x = cx - half; x <= cx + half; ++x) {
            if (x < 0 || y < 0 || x >= img.width() || y >= img.height())
                continue;
            const QColor c = img.pixelColor(x, y);
            r += c.red();
            g += c.green();
            b += c.blue();
            ++n;
        }
    }
    if (n == 0) return QColor(0, 0, 0);
    return QColor(int(r / n), int(g / n), int(b / n));
}

double colourDist(const QColor& a, const QColor& b) {
    const double dr = a.red() - b.red();
    const double dg = a.green() - b.green();
    const double db = a.blue() - b.blue();
    return std::sqrt(dr * dr + dg * dg + db * db);
}

} // namespace

class MediaBoxOriginCrop : public ::testing::Test {
protected:
    void SetUp() override {
        pdf_ = fixturesDir() / "mediabox_offset.pdf";
        mw_ = std::make_unique<MainWindow>();
    }
    void TearDown() override {
        if (auto* cb = QApplication::clipboard()) cb->clear();
        mw_.reset();
    }

    // Opens the fixture, selects `pdfUpRect` (converted into the live fitz page
    // frame) and returns the cropped clipboard image.
    QImage cropForPdfSelection(double px0, double pyBottom, double w, double h) {
        mw_->openFile(QString::fromStdString(pdf_.string()));
        EXPECT_TRUE(spinUntil(
            [&] { return mw_->documentForTesting() != nullptr; }));
        auto* doc = mw_->documentForTesting();
        if (!doc) return {};
        auto page = doc->page(0);
        EXPECT_NE(page, nullptr);
        if (!page) return {};

        const QRectF pageBox = page->boundingBox();
        // Sanity: the fixture MediaBox is 600 x 800.
        EXPECT_NEAR(pageBox.width(), 600.0, 1.0);
        EXPECT_NEAR(pageBox.height(), 800.0, 1.0);

        const QRectF sel = pdfUpToFitz(pageBox, px0, pyBottom, w, h);

        auto* cb = QApplication::clipboard();
        EXPECT_NE(cb, nullptr);

        mw_->setCopySelectionRect(0, sel);
        EXPECT_TRUE(mw_->hasCopySelection());

        // Re-drive the COPY, not just the read. A clipboard write swallowed by
        // an rdpclip ownership window leaves nothing to re-read, so retrying
        // the read alone can never recover — that was the actual cause of this
        // test flaking in-shard while passing 12/12 standalone.
        return pdfe::testsupport::rdpSafeCopyToClipboardImage(
            cb, [this] { return mw_->copySelectionAsImage(); });
    }

    std::unique_ptr<MainWindow> mw_;
    std::filesystem::path pdf_;
};

// Selecting the RED block's sub-rect must crop RED pixels (not blue, not white).
TEST_F(MediaBoxOriginCrop, RedSelectionCropsRedNotBlue) {
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "mediabox_offset.pdf fixture missing";
    }
    QImage out = cropForPdfSelection(160.0, 660.0, 100.0, 80.0);
    ASSERT_FALSE(out.isNull())
        << "copy of the red selection must place an image on the clipboard";
    // Size sanity: 100pt x 80pt at 2x ~= 200 x 160 px.
    EXPECT_NEAR(out.width(), 200, 12);
    EXPECT_NEAR(out.height(), 160, 12);

    const QColor c = sampleCentre(out);
    const double dRed = colourDist(c, QColor(255, 0, 0));
    const double dBlue = colourDist(c, QColor(0, 0, 255));
    EXPECT_LT(dRed, 60.0)
        << "crop centre (" << c.red() << "," << c.green() << "," << c.blue()
        << ") must be RED — proves the MediaBox origin offset is applied";
    EXPECT_GT(dBlue, 120.0)
        << "the red-region crop must NOT contain the blue block's colour";
}

// Selecting the BLUE block's sub-rect must crop BLUE pixels (not red, not white).
TEST_F(MediaBoxOriginCrop, BlueSelectionCropsBlueNotRed) {
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "mediabox_offset.pdf fixture missing";
    }
    QImage out = cropForPdfSelection(440.0, 210.0, 100.0, 80.0);
    ASSERT_FALSE(out.isNull())
        << "copy of the blue selection must place an image on the clipboard";
    EXPECT_NEAR(out.width(), 200, 12);
    EXPECT_NEAR(out.height(), 160, 12);

    const QColor c = sampleCentre(out);
    const double dBlue = colourDist(c, QColor(0, 0, 255));
    const double dRed = colourDist(c, QColor(255, 0, 0));
    EXPECT_LT(dBlue, 60.0)
        << "crop centre (" << c.red() << "," << c.green() << "," << c.blue()
        << ") must be BLUE — proves the MediaBox origin offset is applied";
    EXPECT_GT(dRed, 120.0)
        << "the blue-region crop must NOT contain the red block's colour";
}
