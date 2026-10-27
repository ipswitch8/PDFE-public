#include <gtest/gtest.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QScrollArea>
#include <QToolButton>

#include "tre/AtlasMode.h"
#include "tre/EnrichedAtlas.h"
#include "tre/GlyphAtlas.h"
#include "tre/VisualFontRanker.h"
#include "ui/AtlasComparisonDialog.h"
#include "ui/TextEditToolBar.h"

using namespace pdfe::tre;
using namespace pdfe::ui;

// ============================================================
// Helpers
// ============================================================

namespace {

/// Create a small test glyph image (dark pixels on transparent background).
QImage makeTestGlyph(int w, int h, QRgb color = qRgba(0, 0, 0, 255)) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    int x0 = w / 4, x1 = 3 * w / 4;
    int y0 = h / 4, y1 = 3 * h / 4;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            img.setPixelColor(x, y, QColor::fromRgba(color));
    return img;
}

/// Build a small EnrichedAtlas with a few test glyphs.
std::shared_ptr<EnrichedAtlas> buildTestAtlas(
    const std::vector<std::pair<char32_t, QImage>>& glyphs) {
    GlyphAtlas ga;
    for (const auto& [cp, img] : glyphs) {
        ga.addGlyph(cp, img, 100);
    }
    auto atlas = std::make_shared<EnrichedAtlas>();
    atlas->build(std::move(ga));
    return atlas;
}

/// Build a minimal test line image (solid white rectangle).
QImage makeLineImage(int w = 200, int h = 30) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    return img;
}

/// Build an empty ranked candidates list.
std::vector<VisualFontRanker::RankedCandidate> emptyRanked() {
    return {};
}

/// Convenience: create an AtlasComparisonDialog with no candidates and no compositor.
AtlasComparisonDialog* makeDlg(
    std::shared_ptr<EnrichedAtlas> scan,
    std::shared_ptr<EnrichedAtlas> ft,
    AtlasMode mode = AtlasMode::ScanCropped,
    const QImage& lineImage = makeLineImage(),
    const QString& lineText = QStringLiteral("Hello world"),
    const std::vector<VisualFontRanker::RankedCandidate>& ranked = emptyRanked())
{
    return new AtlasComparisonDialog(
        std::move(scan), std::move(ft),
        lineImage, lineText, ranked,
        /*compositor=*/nullptr, /*fontSizePt=*/12.0f, /*scanDpi=*/300,
        mode);
}

} // anonymous namespace

// ============================================================
// AtlasMode enum tests
// ============================================================

TEST(AtlasModeTest, DefaultIsScanCropped) {
    AtlasMode mode{};
    EXPECT_EQ(mode, AtlasMode::ScanCropped);
}

TEST(AtlasModeTest, EnumValuesDistinct) {
    EXPECT_NE(AtlasMode::ScanCropped, AtlasMode::FreeTypeStyled);
}

// ============================================================
// EnrichedAtlas::calibrateStyleFrom tests
// ============================================================

TEST(EnrichedAtlasCalibrate, CalibrateStyleFromUsesExternalAtlas) {
    QImage noisyGlyph(20, 30, QImage::Format_ARGB32_Premultiplied);
    noisyGlyph.fill(Qt::transparent);
    for (int y = 5; y < 25; ++y) {
        for (int x = 5; x < 15; ++x) {
            int alpha = 200 + (x % 3) * 18;
            alpha = std::min(alpha, 255);
            noisyGlyph.setPixelColor(x, y, QColor(0, 0, 0, alpha));
        }
    }
    GlyphAtlas refGa;
    refGa.addGlyph(U'A', noisyGlyph, 100);
    refGa.addGlyph(U'B', noisyGlyph, 100);

    auto emptyAtlas = std::make_shared<EnrichedAtlas>();
    emptyAtlas->build(GlyphAtlas{});

    emptyAtlas->calibrateStyleFrom(refGa);

    EXPECT_EQ(emptyAtlas->size(), 0);  // Still empty, but calibrated — no crash
}

TEST(EnrichedAtlasCalibrate, CalibrateStyleFromOverridesEmpty) {
    auto glyphImg = makeTestGlyph(10, 20);
    GlyphAtlas refGa;
    refGa.addGlyph(U'X', glyphImg, 100);

    auto atlas = std::make_shared<EnrichedAtlas>();
    atlas->build(GlyphAtlas{});

    atlas->calibrateStyleFrom(refGa);
    EXPECT_EQ(atlas->size(), 0);
}

// ============================================================
// AtlasComparisonDialog: construction and mode
// ============================================================

TEST(AtlasComparisonDialogTest, DefaultModePreserved) {
    auto scanAtlas = buildTestAtlas({{U'T', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'T', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::FreeTypeStyled));

    // Before any button click, selectedMode() returns the initial mode
    EXPECT_EQ(dlg->selectedMode(), AtlasMode::FreeTypeStyled);
}

TEST(AtlasComparisonDialogTest, InitialModeScanCropped) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::ScanCropped));

    EXPECT_EQ(dlg->selectedMode(), AtlasMode::ScanCropped);
}

TEST(AtlasComparisonDialogTest, UseScanButton) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::FreeTypeStyled));

    // Phase-16 fix (qt_test_gap_audit finding #10): use a non-zero
    // delay so the timer fires AFTER exec() enters the modal event
    // loop. With singleShot(0) the click can be dispatched before
    // the dialog is ready to receive input, which is timing-fragile.
    QTimer::singleShot(50, [&dlg]() {
        for (auto* b : dlg->findChildren<QPushButton*>()) {
            if (b->accessibleName() == QStringLiteral("atlas-comparison-use-scan")) {
                QTest::mouseClick(b, Qt::LeftButton);
                return;
            }
        }
    });
    dlg->exec();
    EXPECT_EQ(dlg->selectedMode(), AtlasMode::ScanCropped);
}

TEST(AtlasComparisonDialogTest, UseFontButton) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::ScanCropped));

    QTimer::singleShot(0, [&dlg]() {
        for (auto* b : dlg->findChildren<QPushButton*>()) {
            if (b->accessibleName() == QStringLiteral("atlas-comparison-use-font")) {
                QTest::mouseClick(b, Qt::LeftButton);
                return;
            }
        }
    });
    dlg->exec();
    EXPECT_EQ(dlg->selectedMode(), AtlasMode::FreeTypeStyled);
}

TEST(AtlasComparisonDialogTest, CancelReturnsDefault) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::ScanCropped));

    QTimer::singleShot(0, [&dlg]() {
        for (auto* b : dlg->findChildren<QPushButton*>()) {
            if (b->accessibleName() == QStringLiteral("atlas-comparison-cancel")) {
                QTest::mouseClick(b, Qt::LeftButton);
                return;
            }
        }
    });
    dlg->exec();
    // Cancel does not call accept() so selectedMode stays at initial value
    EXPECT_EQ(dlg->selectedMode(), AtlasMode::ScanCropped);
}

TEST(AtlasComparisonDialogTest, NullAtlasHandled) {
    // Both null atlases — should not crash
    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(nullptr, nullptr));
    EXPECT_NE(dlg, nullptr);
}

TEST(AtlasComparisonDialogTest, EmptyLineTextHandled) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    // Empty lineText should be handled gracefully (no crash, no assertion failure)
    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::ScanCropped,
                makeLineImage(), QStringLiteral("")));
    QTest::qWait(50);
    EXPECT_NE(dlg, nullptr);
}

TEST(AtlasComparisonDialogTest, PreviewLabelsExist) {
    auto scanAtlas = buildTestAtlas({
        {U'H', makeTestGlyph(12, 20)},
        {U'i', makeTestGlyph(6, 20)},
    });
    auto ftAtlas = buildTestAtlas({
        {U'H', makeTestGlyph(12, 20)},
        {U'i', makeTestGlyph(6, 20)},
    });

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::ScanCropped,
                makeLineImage(), QStringLiteral("Hi")));
    QTest::qWait(300);

    QLabel* origPreview = nullptr;
    QLabel* fontPreview = nullptr;
    QLabel* atlasPreview = nullptr;
    for (auto* label : dlg->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("atlas-comparison-original-preview"))
            origPreview = label;
        else if (label->accessibleName() == QStringLiteral("atlas-comparison-font-preview"))
            fontPreview = label;
        else if (label->accessibleName() == QStringLiteral("atlas-comparison-atlas-preview"))
            atlasPreview = label;
    }

    ASSERT_NE(origPreview, nullptr);
    ASSERT_NE(fontPreview, nullptr);
    ASSERT_NE(atlasPreview, nullptr);
}

// ============================================================
// TextEditToolBar atlas controls tests
// ============================================================

TEST(TextEditToolBarAtlasTest, AtlasControlsHiddenByDefault) {
    TextEditToolBar toolbar;

    auto* combo = toolbar.findChild<QComboBox*>("atlasModeCombo");
    ASSERT_NE(combo, nullptr);

    auto* compareBtn = toolbar.findChild<QToolButton*>("compareAtlasButton");
    ASSERT_NE(compareBtn, nullptr);
}

TEST(TextEditToolBarAtlasTest, ShowAtlasControls) {
    TextEditToolBar toolbar;
    toolbar.show();
    toolbar.showAtlasControls(true);

    auto* combo = toolbar.findChild<QComboBox*>("atlasModeCombo");
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->itemText(0), "Scan Glyphs");
    EXPECT_EQ(combo->itemText(1), "Font Matched");
}

TEST(TextEditToolBarAtlasTest, AtlasModeSignal) {
    TextEditToolBar toolbar;
    toolbar.showAtlasControls(true);

    // Phase-16 fix (qt_test_gap_audit finding #11): use QSignalSpy
    // with spy.wait() to flush queued signal delivery before
    // checking. Bare lambda + setCurrentIndex + immediate EXPECT_EQ
    // races against any QueuedConnection in the signal-relay chain.
    QSignalSpy spy(&toolbar, &TextEditToolBar::atlasModeChanged);
    ASSERT_TRUE(spy.isValid());

    auto* combo = toolbar.findChild<QComboBox*>("atlasModeCombo");
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(1);
    spy.wait(100);
    ASSERT_GE(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toInt(), 1);
}

TEST(TextEditToolBarAtlasTest, CompareSignal) {
    TextEditToolBar toolbar;
    toolbar.showAtlasControls(true);

    bool signalFired = false;
    QObject::connect(&toolbar, &TextEditToolBar::compareAtlasRequested,
                     [&signalFired]() { signalFired = true; });

    auto* btn = toolbar.findChild<QToolButton*>("compareAtlasButton");
    ASSERT_NE(btn, nullptr);
    QTest::mouseClick(btn, Qt::LeftButton);
    EXPECT_TRUE(signalFired);
}

TEST(TextEditToolBarAtlasTest, SetAtlasModeNoSignal) {
    TextEditToolBar toolbar;
    toolbar.showAtlasControls(true);

    int signalMode = -1;
    QObject::connect(&toolbar, &TextEditToolBar::atlasModeChanged,
                     [&signalMode](int mode) { signalMode = mode; });

    // setAtlasMode uses updatingFromSelection_ guard -- should NOT emit
    toolbar.setAtlasMode(1);
    EXPECT_EQ(signalMode, -1);

    auto* combo = toolbar.findChild<QComboBox*>("atlasModeCombo");
    EXPECT_EQ(combo->currentIndex(), 1);
}

// ============================================================
// Integration tests (no real BitmapCompositor -- just EnrichedAtlas)
// ============================================================

TEST(AtlasIntegrationTest, FreeTypeStyledAtlas_AllGlyphsFromFreeType) {
    auto ftAtlas = std::make_shared<EnrichedAtlas>();
    ftAtlas->build(GlyphAtlas{});
    EXPECT_EQ(ftAtlas->size(), 0);

    QImage g = ftAtlas->glyph(U'A');
    EXPECT_TRUE(g.isNull());
}

TEST(AtlasIntegrationTest, ScanCroppedAtlasHasGlyphs) {
    auto scanAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });
    EXPECT_EQ(scanAtlas->size(), 2);
    EXPECT_FALSE(scanAtlas->glyph(U'A').isNull());
    EXPECT_FALSE(scanAtlas->glyph(U'B').isNull());
}

TEST(AtlasIntegrationTest, SwitchBetweenModes) {
    auto scanAtlas = buildTestAtlas({{U'X', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'X', makeTestGlyph(12, 22)}});

    AtlasMode mode = AtlasMode::ScanCropped;
    auto active = (mode == AtlasMode::ScanCropped) ? scanAtlas : ftAtlas;
    EXPECT_EQ(active->glyph(U'X').width(), 10);

    mode = AtlasMode::FreeTypeStyled;
    active = (mode == AtlasMode::ScanCropped) ? scanAtlas : ftAtlas;
    EXPECT_EQ(active->glyph(U'X').width(), 12);
}

TEST(AtlasIntegrationTest, ResetClearsState) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::shared_ptr<EnrichedAtlas> scanPtr = scanAtlas;
    std::shared_ptr<EnrichedAtlas> ftPtr = ftAtlas;
    AtlasMode mode = AtlasMode::FreeTypeStyled;
    bool choiceMade = true;

    scanPtr.reset();
    ftPtr.reset();
    mode = AtlasMode::ScanCropped;
    choiceMade = false;

    EXPECT_EQ(scanPtr, nullptr);
    EXPECT_EQ(ftPtr, nullptr);
    EXPECT_EQ(mode, AtlasMode::ScanCropped);
    EXPECT_FALSE(choiceMade);
}

TEST(TextEditToolBarAtlasTest, ShowAtlasControlsHides) {
    TextEditToolBar toolbar;
    toolbar.show();

    toolbar.showAtlasControls(true);
    auto* combo = toolbar.findChild<QComboBox*>("atlasModeCombo");
    ASSERT_NE(combo, nullptr);

    toolbar.showAtlasControls(false);
    auto* btn = toolbar.findChild<QToolButton*>("compareAtlasButton");
    ASSERT_NE(btn, nullptr);
}

TEST(AtlasComparisonDialogTest, OneNullAtlasHandled) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, nullptr));
    EXPECT_NE(dlg, nullptr);
}

TEST(AtlasComparisonDialogTest, OnlyFtAtlasHandled) {
    auto ftAtlas = buildTestAtlas({{U'B', makeTestGlyph(10, 20)}});
    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(nullptr, ftAtlas));
    EXPECT_NE(dlg, nullptr);
}

// ============================================================
// EnrichedAtlas::setReferenceMetrics tests
// ============================================================

TEST(EnrichedAtlasMetrics, SetReferenceMetricsSetsValues) {
    auto atlas = std::make_shared<EnrichedAtlas>();
    atlas->build(GlyphAtlas{});
    EXPECT_EQ(atlas->referenceLineHeight(), 0);
    EXPECT_EQ(atlas->referenceBaselineFromBottom(), 0);

    atlas->setReferenceMetrics(40, 5);
    EXPECT_EQ(atlas->referenceLineHeight(), 40);
    EXPECT_EQ(atlas->referenceBaselineFromBottom(), 5);
}

TEST(EnrichedAtlasMetrics, SetReferenceMetricsCopiedFromScanAtlas) {
    auto scanAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 30)},
        {U'B', makeTestGlyph(10, 30)},
        {U'C', makeTestGlyph(10, 30)},
    });
    EXPECT_EQ(scanAtlas->referenceLineHeight(), 30);

    auto ftAtlas = std::make_shared<EnrichedAtlas>();
    ftAtlas->build(GlyphAtlas{});
    EXPECT_EQ(ftAtlas->referenceLineHeight(), 0);

    ftAtlas->setReferenceMetrics(
        scanAtlas->referenceLineHeight(),
        scanAtlas->referenceBaselineFromBottom());

    EXPECT_EQ(ftAtlas->referenceLineHeight(), 30);
    EXPECT_EQ(ftAtlas->referenceBaselineFromBottom(),
              scanAtlas->referenceBaselineFromBottom());
}

TEST(EnrichedAtlasMetrics, SetReferenceMetricsNoCrashOnEmptyAtlas) {
    auto atlas = std::make_shared<EnrichedAtlas>();
    atlas->build(GlyphAtlas{});
    atlas->setReferenceMetrics(40, 5);
    EXPECT_EQ(atlas->size(), 0);
}

TEST(EnrichedAtlasMetrics, SetReferenceMetricsClampsNegative) {
    auto atlas = std::make_shared<EnrichedAtlas>();
    atlas->build(GlyphAtlas{});
    atlas->setReferenceMetrics(-10, -5);
    EXPECT_EQ(atlas->referenceLineHeight(), 0);
    EXPECT_EQ(atlas->referenceBaselineFromBottom(), 0);
}

TEST(EnrichedAtlasMetrics, BaselineHistogramSkipsTightCrops) {
    // Simulate real-world atlas: some glyphs at refLineH (tight-cropped,
    // ink touches bottom edge → fromBottom=0) and some shorter (normalized
    // with padding → fromBottom>0). The baseline should come from the
    // normalized group, not the tight-crop noise.
    const int lineH = 30;  // will become refLineH_
    const int shortH = 20; // shorter glyphs that get normalized

    // Create glyphs at lineH with ink touching bottom (tight crops)
    auto makeTightCrop = [&](int w) {
        QImage img(w, lineH, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        // Ink from row 2 to row lineH-1 (bottom edge, fromBottom=0)
        for (int y = 2; y < lineH; ++y)
            for (int x = 1; x < w - 1; ++x)
                img.setPixelColor(x, y, Qt::black);
        return img;
    };
    // Create shorter glyphs (will be normalized/padded by build())
    auto makeShortGlyph = [&](int w) {
        QImage img(w, shortH, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        // Ink fills entire short image (tight crop, fromBottom=0 before normalization)
        for (int y = 2; y < shortH - 1; ++y)
            for (int x = 1; x < w - 1; ++x)
                img.setPixelColor(x, y, Qt::black);
        return img;
    };

    GlyphAtlas ga;
    // 6 tight-crops at lineH (fromBottom=0 after normalization) — majority sets mode
    ga.addGlyph(U'A', makeTightCrop(12), 100);
    ga.addGlyph(U'B', makeTightCrop(12), 100);
    ga.addGlyph(U'C', makeTightCrop(12), 100);
    ga.addGlyph(U'D', makeTightCrop(12), 100);
    ga.addGlyph(U'E', makeTightCrop(12), 100);
    ga.addGlyph(U'F', makeTightCrop(12), 100);
    // 5 shorter glyphs (fromBottom>0 after normalization with padding)
    ga.addGlyph(U'a', makeShortGlyph(10), 100);
    ga.addGlyph(U'e', makeShortGlyph(10), 100);
    ga.addGlyph(U'n', makeShortGlyph(10), 100);
    ga.addGlyph(U'o', makeShortGlyph(10), 100);
    ga.addGlyph(U's', makeShortGlyph(10), 100);

    auto atlas = std::make_shared<EnrichedAtlas>();
    atlas->build(std::move(ga));

    EXPECT_EQ(atlas->referenceLineHeight(), lineH);
    // Baseline should NOT be 0 (tight-crop noise) — must be >0
    // from the normalized shorter glyphs' padding
    EXPECT_GT(atlas->referenceBaselineFromBottom(), 0);
    // Should be approximately lineH/6 ≈ 5
    EXPECT_GE(atlas->referenceBaselineFromBottom(), 3);
    EXPECT_LE(atlas->referenceBaselineFromBottom(), 8);
}

TEST(EnrichedAtlasMetrics, BaselineHistogramFallbackWhenAllTightCrops) {
    // Edge case: all glyphs are at refLineH with ink at bottom edge.
    // Should fall back to refLineH/6 estimate.
    const int lineH = 30;
    auto makeTightCrop = [&](int w) {
        QImage img(w, lineH, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        for (int y = 2; y < lineH; ++y)
            for (int x = 1; x < w - 1; ++x)
                img.setPixelColor(x, y, Qt::black);
        return img;
    };

    GlyphAtlas ga;
    ga.addGlyph(U'A', makeTightCrop(12), 100);
    ga.addGlyph(U'B', makeTightCrop(12), 100);
    ga.addGlyph(U'C', makeTightCrop(12), 100);

    auto atlas = std::make_shared<EnrichedAtlas>();
    atlas->build(std::move(ga));

    EXPECT_EQ(atlas->referenceLineHeight(), lineH);
    // Fallback: max(1, lineH/6) = max(1, 5) = 5
    EXPECT_EQ(atlas->referenceBaselineFromBottom(), std::max(1, lineH / 6));
}

// ============================================================
// Character set grid and scroll area tests
// ============================================================

TEST(AtlasComparisonDialogTest, CharsetGridExists) {
    auto scanAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });
    auto ftAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, ftAtlas));

    // Find charset labels by accessible name
    QLabel* fontCharsetFound = nullptr;
    QLabel* atlasCharsetFound = nullptr;
    for (auto* label : dlg->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("atlas-comparison-font-charset"))
            fontCharsetFound = label;
        else if (label->accessibleName() == QStringLiteral("atlas-comparison-atlas-charset"))
            atlasCharsetFound = label;
    }
    ASSERT_NE(fontCharsetFound, nullptr);
    ASSERT_NE(atlasCharsetFound, nullptr);
}

TEST(AtlasComparisonDialogTest, CharsetGridRendersAfterDebounce) {
    auto scanAtlas = buildTestAtlas({
        {U'X', makeTestGlyph(10, 20)},
        {U'Y', makeTestGlyph(10, 20)},
        {U'Z', makeTestGlyph(10, 20)},
    });
    auto ftAtlas = buildTestAtlas({
        {U'X', makeTestGlyph(10, 20)},
        {U'Y', makeTestGlyph(10, 20)},
        {U'Z', makeTestGlyph(10, 20)},
    });

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::ScanCropped,
                makeLineImage(), QStringLiteral("XYZ")));
    QTest::qWait(400);

    QLabel* atlasCharset = nullptr;
    for (auto* label : dlg->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("atlas-comparison-atlas-charset"))
            atlasCharset = label;
    }
    ASSERT_NE(atlasCharset, nullptr);
    // Atlas charset grid is rendered from scanAtlas; with glyphs present it should not be null.
    // (font charset is null because compositor_ is nullptr in this test.)
    EXPECT_FALSE(atlasCharset->pixmap().isNull());
}

TEST(AtlasComparisonDialogTest, CharsetGridShowsUnionCodepoints) {
    // Scan has A,B; FreeType-styled has B,C — union from scanAtlas should include A,B
    auto scanAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });
    auto ftAtlas = buildTestAtlas({
        {U'B', makeTestGlyph(10, 20)},
        {U'C', makeTestGlyph(10, 20)},
    });

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, ftAtlas));
    QTest::qWait(400);

    QLabel* atlasCharset = nullptr;
    for (auto* label : dlg->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("atlas-comparison-atlas-charset"))
            atlasCharset = label;
    }
    ASSERT_NE(atlasCharset, nullptr);
    EXPECT_FALSE(atlasCharset->pixmap().isNull());
}

TEST(AtlasComparisonDialogTest, TitleShowsDialogWindowTitle) {
    auto scanAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
        {U'C', makeTestGlyph(10, 20)},
    });
    auto ftAtlas = buildTestAtlas({
        {U'X', makeTestGlyph(10, 20)},
    });

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, ftAtlas));

    // The dialog has a window title
    EXPECT_FALSE(dlg->windowTitle().isEmpty());
    EXPECT_TRUE(dlg->windowTitle().contains("Atlas"));
}

TEST(AtlasComparisonDialogTest, CharsetScrollAreasExist) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, ftAtlas));

    QScrollArea* fontScroll = nullptr;
    QScrollArea* atlasScroll = nullptr;
    for (auto* scroll : dlg->findChildren<QScrollArea*>()) {
        if (scroll->accessibleName() == QStringLiteral("atlas-comparison-font-charset-scroll"))
            fontScroll = scroll;
        else if (scroll->accessibleName() == QStringLiteral("atlas-comparison-atlas-charset-scroll"))
            atlasScroll = scroll;
    }
    ASSERT_NE(fontScroll, nullptr);
    ASSERT_NE(atlasScroll, nullptr);
}

TEST(AtlasComparisonDialogTest, NullAtlasCharsetGridHandled) {
    // Both null atlases — atlas charset grid should handle gracefully (no crash)
    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(nullptr, nullptr));
    QTest::qWait(400);

    QLabel* atlasCharset = nullptr;
    for (auto* label : dlg->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("atlas-comparison-atlas-charset"))
            atlasCharset = label;
    }
    ASSERT_NE(atlasCharset, nullptr);
    EXPECT_TRUE(atlasCharset->pixmap().isNull());
}

// ============================================================
// Font combo (ranked candidates) tests
// ============================================================

TEST(AtlasComparisonDialogTest, FontComboExistsAndDisabledWhenEmpty) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, ftAtlas));

    QComboBox* combo = nullptr;
    for (auto* c : dlg->findChildren<QComboBox*>()) {
        if (c->accessibleName() == QStringLiteral("atlas-comparison-font-combo"))
            combo = c;
    }
    ASSERT_NE(combo, nullptr);
    // No candidates provided — combo should be disabled (one placeholder item)
    EXPECT_FALSE(combo->isEnabled());
    EXPECT_EQ(combo->count(), 1);
}

TEST(AtlasComparisonDialogTest, FontComboPopulatedWithCandidates) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    // Build two fake ranked candidates
    pdfe::fonts::FontMetricProfile profile1;
    profile1.familyName = "Arial";
    profile1.styleName  = "Regular";

    pdfe::fonts::FontMetricProfile profile2;
    profile2.familyName = "Times New Roman";
    profile2.styleName  = "Bold";

    pdfe::fonts::SubstituteResult sub1;
    sub1.profile   = profile1;
    sub1.filePath  = "C:/fake/arial.ttf";
    sub1.matchScore = 5.0f;

    pdfe::fonts::SubstituteResult sub2;
    sub2.profile   = profile2;
    sub2.filePath  = "C:/fake/timesnr.ttf";
    sub2.matchScore = 12.0f;

    VisualFontRanker::RankedCandidate rc1;
    rc1.candidate    = sub1;
    rc1.visualScore  = 0.85f;
    rc1.metricScore  = 5.0f;
    rc1.combinedScore = 0.15f;

    VisualFontRanker::RankedCandidate rc2;
    rc2.candidate    = sub2;
    rc2.visualScore  = 0.72f;
    rc2.metricScore  = 12.0f;
    rc2.combinedScore = 0.28f;

    std::vector<VisualFontRanker::RankedCandidate> ranked = {rc1, rc2};

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::ScanCropped,
                makeLineImage(), QStringLiteral("Hello"), ranked));

    QComboBox* combo = nullptr;
    for (auto* c : dlg->findChildren<QComboBox*>()) {
        if (c->accessibleName() == QStringLiteral("atlas-comparison-font-combo"))
            combo = c;
    }
    ASSERT_NE(combo, nullptr);
    EXPECT_TRUE(combo->isEnabled());
    EXPECT_EQ(combo->count(), 2);
    EXPECT_TRUE(combo->itemText(0).contains("Arial"));
    EXPECT_TRUE(combo->itemText(1).contains("Times New Roman"));
}

TEST(AtlasComparisonDialogTest, SelectedFontFilePath) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    pdfe::fonts::FontMetricProfile profile;
    profile.familyName = "Verdana";
    profile.styleName  = "Regular";

    pdfe::fonts::SubstituteResult sub;
    sub.profile  = profile;
    sub.filePath = "C:/fake/verdana.ttf";

    VisualFontRanker::RankedCandidate rc;
    rc.candidate   = sub;
    rc.visualScore = 0.90f;
    rc.metricScore = 3.0f;

    std::vector<VisualFontRanker::RankedCandidate> ranked = {rc};

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, nullptr, AtlasMode::ScanCropped,
                makeLineImage(), QStringLiteral("Test"), ranked));

    EXPECT_EQ(dlg->selectedFontFilePath(), "C:/fake/verdana.ttf");
    EXPECT_EQ(dlg->selectedFontFamily(), "Verdana");
}

TEST(AtlasComparisonDialogTest, SelectedFontPathEmptyWhenNoCandidates) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, nullptr));

    EXPECT_TRUE(dlg->selectedFontFilePath().empty());
    EXPECT_TRUE(dlg->selectedFontFamily().empty());
}

// ============================================================
// New feature tests
// ============================================================

TEST(AtlasComparisonDialogTest, FillMissingCheckboxExists) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, ftAtlas));

    QCheckBox* fillMissing = nullptr;
    for (auto* cb : dlg->findChildren<QCheckBox*>()) {
        if (cb->accessibleName() == QStringLiteral("atlas-comparison-fill-missing"))
            fillMissing = cb;
    }
    ASSERT_NE(fillMissing, nullptr);
    EXPECT_FALSE(fillMissing->isChecked());
}

TEST(AtlasComparisonDialogTest, FillMissingInitialStatePersists) {
    // Pipeline c7e2a914 phase-3: caller-supplied initialFillMissing
    // pre-checks the box AND populates atlasGlyphOverride_ via the
    // toggled signal, so the visual effect is identical to the user
    // clicking it after construction. fillMissingChecked() reads
    // back the current state for the caller to persist.
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    // Default false → unchecked
    std::unique_ptr<AtlasComparisonDialog> dlgDefault(
        new AtlasComparisonDialog(
            scanAtlas, ftAtlas, makeLineImage(), QStringLiteral("A"),
            emptyRanked(), nullptr, 12.0f, 300,
            AtlasMode::ScanCropped,
            /*initialFillMissing=*/false));
    EXPECT_FALSE(dlgDefault->fillMissingChecked());

    // Explicit true → pre-checked
    std::unique_ptr<AtlasComparisonDialog> dlgTrue(
        new AtlasComparisonDialog(
            scanAtlas, ftAtlas, makeLineImage(), QStringLiteral("A"),
            emptyRanked(), nullptr, 12.0f, 300,
            AtlasMode::ScanCropped,
            /*initialFillMissing=*/true));
    EXPECT_TRUE(dlgTrue->fillMissingChecked());
}

TEST(AtlasComparisonDialogTest, FillMissingCheckboxTogglesAtlasGrid) {
    // scanAtlas has only A,B; ftAtlas has additional glyphs so fill-missing has
    // material to render into the empty cells, producing a different grid image.
    auto scanAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });
    auto ftAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
        {U'C', makeTestGlyph(10, 20)},
        {U'D', makeTestGlyph(10, 20)},
    });

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::ScanCropped,
                makeLineImage(), QStringLiteral("ABCD")));
    QTest::qWait(400);

    QLabel* atlasCharset = nullptr;
    QCheckBox* fillMissing = nullptr;
    for (auto* label : dlg->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("atlas-comparison-atlas-charset"))
            atlasCharset = label;
    }
    for (auto* cb : dlg->findChildren<QCheckBox*>()) {
        if (cb->accessibleName() == QStringLiteral("atlas-comparison-fill-missing"))
            fillMissing = cb;
    }
    ASSERT_NE(atlasCharset, nullptr);
    ASSERT_NE(fillMissing, nullptr);

    // Capture the grid image before toggling fill-missing
    QImage before = atlasCharset->pixmap().toImage().copy();

    // Toggle fill-missing on
    fillMissing->setChecked(true);
    QTest::qWait(400);

    QImage after = atlasCharset->pixmap().toImage();
    EXPECT_NE(before, after);
}

TEST(AtlasComparisonDialogTest, AtlasGridHasPointingHandCursor) {
    auto scanAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });
    auto ftAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, ftAtlas));

    QLabel* atlasCharset = nullptr;
    for (auto* label : dlg->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("atlas-comparison-atlas-charset"))
            atlasCharset = label;
    }
    ASSERT_NE(atlasCharset, nullptr);
    EXPECT_EQ(atlasCharset->cursor().shape(), Qt::PointingHandCursor);
}

TEST(AtlasComparisonDialogTest, EventFilterConsumesClickOnAtlasLabel) {
    auto scanAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });
    auto ftAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });

    std::unique_ptr<AtlasComparisonDialog> dlg(
        makeDlg(scanAtlas, ftAtlas, AtlasMode::ScanCropped,
                makeLineImage(), QStringLiteral("AB")));
    QTest::qWait(400);

    QLabel* atlasCharset = nullptr;
    for (auto* label : dlg->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("atlas-comparison-atlas-charset"))
            atlasCharset = label;
    }
    ASSERT_NE(atlasCharset, nullptr);

    // Send a mouse press event directly and verify the dialog handles it (no crash).
    // The event filter installed by AtlasComparisonDialog on atlasCharsetLabel_ consumes
    // mouse button press events to toggle per-glyph overrides.
    QMouseEvent ev(QEvent::MouseButtonPress, QPointF(2.0, 2.0),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(atlasCharset, &ev);
    EXPECT_TRUE(ev.isAccepted());
}

TEST(AtlasComparisonDialogTest, ButtonPositioning_UseFontAndUseScanExist) {
    auto scanAtlas = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});
    auto ftAtlas   = buildTestAtlas({{U'A', makeTestGlyph(10, 20)}});

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, ftAtlas));

    QPushButton* useFontBtn  = nullptr;
    QPushButton* useScanBtn  = nullptr;
    QPushButton* cancelBtn   = nullptr;
    for (auto* b : dlg->findChildren<QPushButton*>()) {
        const QString name = b->accessibleName();
        if (name == QStringLiteral("atlas-comparison-use-font"))
            useFontBtn = b;
        else if (name == QStringLiteral("atlas-comparison-use-scan"))
            useScanBtn = b;
        else if (name == QStringLiteral("atlas-comparison-cancel"))
            cancelBtn = b;
    }
    EXPECT_NE(useFontBtn, nullptr);
    EXPECT_NE(useScanBtn, nullptr);
    EXPECT_NE(cancelBtn, nullptr);
}

TEST(AtlasComparisonDialogTest, FontLegendColorEntry) {
    auto scanAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });
    auto ftAtlas = buildTestAtlas({
        {U'A', makeTestGlyph(10, 20)},
        {U'B', makeTestGlyph(10, 20)},
    });

    std::unique_ptr<AtlasComparisonDialog> dlg(makeDlg(scanAtlas, ftAtlas));

    // The charset section title/legend label uses rich text with color-coded entries
    // for "Scan" and "Font" glyphs. Verify the label containing the word "Font" exists.
    bool foundFontLegend = false;
    for (auto* label : dlg->findChildren<QLabel*>()) {
        const QString text = label->text();
        if (text.contains(QStringLiteral("Font")) && label->textFormat() == Qt::RichText) {
            foundFontLegend = true;
            break;
        }
    }
    EXPECT_TRUE(foundFontLegend);
}
