#pragma once

#include "tre/AtlasMode.h"
#include "tre/VisualFontRanker.h"
#include "tre/TreTypes.h"
#include <QDialog>
#include <QImage>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

class QLabel;
class QCheckBox;
class QComboBox;
class QPushButton;
class QScrollArea;
class QTimer;

namespace pdfe::tre {
class EnrichedAtlas;
class BitmapCompositor;
}

namespace pdfe::ui {

/// Comparison dialog showing three vertically-stacked previews of the clicked line:
/// 1. Original scan image
/// 2. Font-matched rendering (selectable from CNN ranked candidates dropdown)
/// 3. Atlas-built rendering
/// Plus a character set grid for the selected font (left) and the atlas (right, color-coded).
/// Atlas grid cells are clickable to toggle between original and font-matched glyphs.
class AtlasComparisonDialog : public QDialog {
    Q_OBJECT

public:
    /// @param scanAtlas         Scan-cropped atlas (level 1-4 fallback chain).
    /// @param ftAtlas           FreeType-rendered atlas (with scan appearance applied).
    /// @param lineImage         Cropped image of the clicked line from the page.
    /// @param lineText          HOCR text of the clicked line.
    /// @param rankedCandidates  CNN-ranked font candidates (sorted best-first).
    /// @param compositor        BitmapCompositor for rendering font previews.
    /// @param fontSizePt        Detected font size in points.
    /// @param scanDpi           Scan DPI for rendering.
    /// @param currentMode       Currently active atlas mode.
    /// @param initialFillMissing  Initial state for the "Fill missing
    ///                            from font" checkbox (caller persists
    ///                            this across dialog instances; see
    ///                            MainWindow::fillMissingPersisted_).
    /// @param parent            Parent widget.
    explicit AtlasComparisonDialog(
        std::shared_ptr<pdfe::tre::EnrichedAtlas> scanAtlas,
        std::shared_ptr<pdfe::tre::EnrichedAtlas> ftAtlas,
        const QImage& lineImage,
        const QString& lineText,
        const std::vector<pdfe::tre::VisualFontRanker::RankedCandidate>& rankedCandidates,
        pdfe::tre::BitmapCompositor* compositor,
        float fontSizePt,
        int scanDpi,
        pdfe::tre::AtlasMode currentMode = pdfe::tre::AtlasMode::ScanCropped,
        bool initialFillMissing = false,
        QWidget* parent = nullptr);
    ~AtlasComparisonDialog() override;

    /// Pipeline a7c4f891 phase-5 (D1 / AC1): returns true when both
    /// atlases passed to the constructor are non-null (the dialog is
    /// ready for downstream operations). Gated by PDFE_TEST_PIPES via
    /// the caller (MainWindow) — not re-checked here since the dialog
    /// only exists when MainWindow constructs it.
    Q_INVOKABLE bool isAtlasReady() const {
        return scanAtlas_ != nullptr && ftAtlas_ != nullptr;
    }

    /// The selected atlas mode (valid after exec() returns Accepted).
    [[nodiscard]] pdfe::tre::AtlasMode selectedMode() const { return selectedMode_; }

    /// The current state of the fillMissing checkbox. Caller saves
    /// this back to its persistence slot after dlg.exec() returns
    /// (regardless of Accepted/Rejected) so the user's UI choice
    /// survives dialog close+reopen within the session.
    [[nodiscard]] bool fillMissingChecked() const;

    /// The selected font file path (from dropdown), or empty if no candidates.
    [[nodiscard]] std::string selectedFontFilePath() const;

    /// The selected font family name, or empty if no candidates.
    [[nodiscard]] std::string selectedFontFamily() const;

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onFontChanged(int index);
    void updatePreviews();
    void onUseScan();
    void onUseFont();
    void onFillMissingToggled(bool checked);

private:
    void setupUi();
    QImage renderFontPreview(const std::string& fontFilePath, float fontSizePt) const;
    QImage renderAtlasPreview() const;
    QImage renderFontCharsetGrid(const std::string& fontFilePath, float fontSizePt,
                                 int gridWidth) const;
    QImage renderAtlasCharsetGrid(int gridWidth);
    void refreshAtlasGrid();

    /// Apply scan-like appearance to a white-background composited grayscale image.
    /// Remaps contrast range and adds blur to match the original scan line.
    QImage applyScanAppearance(const QImage& composited) const;

    /// Render a single font glyph for atlas grid substitution (content-cropped,
    /// H-corrected to match atlas glyph size; scan appearance applied by caller).
    QImage renderFontGlyphWithScanStyle(char32_t cp) const;

    /// Measure scan line contrast range (cached after first call).
    void measureScanContrast() const;

    std::shared_ptr<pdfe::tre::EnrichedAtlas> scanAtlas_;
    std::shared_ptr<pdfe::tre::EnrichedAtlas> ftAtlas_;
    QImage lineImage_;
    QString lineText_;
    std::vector<pdfe::tre::VisualFontRanker::RankedCandidate> rankedCandidates_;
    pdfe::tre::BitmapCompositor* compositor_;
    float fontSizePt_;
    int scanDpi_;
    pdfe::tre::AtlasMode selectedMode_;
    std::vector<char32_t> unionCodepoints_;  ///< Cached union of atlas + ASCII codepoints

    /// Per-codepoint display source override for the atlas grid.
    /// 0 = default (atlas/synthesized/missing), 1 = font-matched with scan style.
    std::unordered_map<char32_t, int> atlasGlyphOverride_;

    /// Cached scan contrast measurements
    mutable int scanFgVal_ = 50;   ///< Darkest pixel in scan (foreground)
    mutable int scanBgVal_ = 210;  ///< Lightest pixel in scan (background)
    mutable bool scanContrastMeasured_ = false;

    /// Atlas grid layout info for click detection
    int atlasCols_ = 0;
    int atlasCellW_ = 0;
    int atlasCellH_ = 0;

    // UI widgets
    QLabel* originalPreviewLabel_;
    QLabel* fontPreviewLabel_;
    QLabel* atlasPreviewLabel_;
    QComboBox* fontCombo_;
    QLabel* fontCharsetLabel_;
    QLabel* atlasCharsetLabel_;
    QScrollArea* fontCharsetScroll_;
    QScrollArea* atlasCharsetScroll_;
    QCheckBox* fillMissingCheck_;
    QPushButton* useScanButton_;
    QPushButton* useFontButton_;
    QPushButton* cancelButton_;
    QTimer* debounceTimer_;
};

} // namespace pdfe::ui
