#include "ui/AtlasComparisonDialog.h"
#include "tre/EnrichedAtlas.h"
#include "tre/BitmapCompositor.h"
#include "tre/TreTypes.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

namespace {

/// Build the printable character set: full ASCII printable range plus any
/// additional codepoints from the scan atlas (e.g. non-ASCII glyphs on the page).
std::vector<char32_t> buildUnionCodepoints(
    const std::shared_ptr<pdfe::tre::EnrichedAtlas>& atlas)
{
    std::set<char32_t> allCps;
    // Always include full printable ASCII (space excluded — not useful to display)
    for (char32_t cp = U'!'; cp <= U'~'; ++cp)
        allCps.insert(cp);
    if (atlas) {
        for (char32_t cp : atlas->codepoints())
            allCps.insert(cp);
    }
    return {allCps.begin(), allCps.end()};
}

/// Gaussian blur with configurable radius on a grayscale image.
/// Uses separable 1D convolution for efficiency.
QImage gaussianBlur(const QImage& gray, int radius, float sigma)
{
    if (gray.isNull() || gray.width() < 3 || gray.height() < 3)
        return gray;

    // Build 1D Gaussian kernel
    int kernelSize = radius * 2 + 1;
    std::vector<float> kernel(static_cast<size_t>(kernelSize));
    float kernelSum = 0.0f;
    for (int i = 0; i < kernelSize; ++i) {
        float x = static_cast<float>(i - radius);
        kernel[static_cast<size_t>(i)] = std::exp(-(x * x) / (2.0f * sigma * sigma));
        kernelSum += kernel[static_cast<size_t>(i)];
    }
    for (auto& k : kernel)
        k /= kernelSum;

    const int w = gray.width(), h = gray.height();

    // Horizontal pass
    QImage temp(gray.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        const uchar* src = gray.constScanLine(y);
        uchar* dst = temp.scanLine(y);
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int sx = std::clamp(x + k, 0, w - 1);
                sum += static_cast<float>(src[sx]) * kernel[static_cast<size_t>(k + radius)];
            }
            dst[x] = static_cast<uchar>(std::clamp(static_cast<int>(sum + 0.5f), 0, 255));
        }
    }

    // Vertical pass
    QImage result(gray.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* dst = result.scanLine(y);
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int sy = std::clamp(y + k, 0, h - 1);
                sum += static_cast<float>(temp.constScanLine(sy)[x])
                       * kernel[static_cast<size_t>(k + radius)];
            }
            dst[x] = static_cast<uchar>(std::clamp(static_cast<int>(sum + 0.5f), 0, 255));
        }
    }
    return result;
}

/// Morphological dilation on a grayscale image (darkens — ink spread).
/// Uses a circular structuring element of the given radius.
QImage morphDilate(const QImage& gray, int radius)
{
    if (gray.isNull() || radius <= 0)
        return gray;

    const int w = gray.width(), h = gray.height();
    QImage result(gray.size(), QImage::Format_Grayscale8);
    result.fill(255);

    for (int y = 0; y < h; ++y) {
        uchar* out = result.scanLine(y);
        for (int x = 0; x < w; ++x) {
            int minVal = 255;
            for (int dy = -radius; dy <= radius; ++dy) {
                int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                const uchar* row = gray.constScanLine(ny);
                for (int dx = -radius; dx <= radius; ++dx) {
                    // Circular structuring element
                    if (dx * dx + dy * dy > radius * radius) continue;
                    int nx = x + dx;
                    if (nx < 0 || nx >= w) continue;
                    minVal = std::min(minVal, static_cast<int>(row[nx]));
                }
            }
            out[x] = static_cast<uchar>(minVal);
        }
    }
    return result;
}

/// Fixed glyph display area height for character set grids.
constexpr int kGlyphAreaH = 40;
/// Fixed preview row height (compact — glyphs are recognisable at this size).
constexpr int kPreviewH = 36;
/// Canvas height for rendering font glyphs — larger than display size so
/// FreeType has room to draw full ascenders/descenders before we scale down.
constexpr int kFontRenderH = 120;

/// Pixel brightness below which a pixel is considered content (not background).
constexpr int kWhiteBgThreshold = 250;

/// Crop a white-background image to its non-white content bounding box.
QImage cropToContentWhiteBg(const QImage& img, int threshold = kWhiteBgThreshold)
{
    QImage gray = img.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull())
        return img;

    int top = gray.height(), bottom = -1, left = gray.width(), right = -1;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (row[x] < threshold) {
                if (y < top) top = y;
                if (y > bottom) bottom = y;
                if (x < left) left = x;
                if (x > right) right = x;
            }
        }
    }
    if (top > bottom || left > right)
        return img;  // no content found — return original
    // Add 1px margin
    int l = std::max(0, left - 1);
    int t = std::max(0, top - 1);
    int r = std::min(img.width() - 1, right + 1);
    int b = std::min(img.height() - 1, bottom + 1);
    return img.copy(l, t, r - l + 1, b - t + 1);
}

/// Find the vertical content extent (top and bottom ink rows) in a white-bg image.
/// Returns {top, bottom} or {-1, -1} if no content found.
std::pair<int, int> findVerticalInkBounds(const QImage& img, int threshold = kWhiteBgThreshold)
{
    QImage gray = img.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull())
        return {-1, -1};
    int top = -1, bottom = -1;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (row[x] < threshold) {
                if (top < 0) top = y;
                bottom = y;
                break;
            }
        }
    }
    return {top, bottom};
}

/// Crop an image to a common vertical window [cropTop..cropBot], trimming
/// horizontal whitespace individually.  All glyphs cropped to the same
/// vertical window preserve their relative vertical position.
QImage cropToCommonVertical(const QImage& img, int cropTop, int cropBot,
                            int threshold = kWhiteBgThreshold)
{
    if (img.isNull() || cropTop < 0 || cropBot < cropTop)
        return img;
    cropTop = std::max(0, cropTop);
    cropBot = std::min(img.height() - 1, cropBot);

    QImage gray = img.convertToFormat(QImage::Format_Grayscale8);
    int left = gray.width(), right = -1;
    for (int y = cropTop; y <= cropBot; ++y) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (row[x] < threshold) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    if (left > right) {
        // No horizontal content — return thin vertical slice
        int midX = img.width() / 2;
        return img.copy(midX, cropTop, 1, cropBot - cropTop + 1);
    }
    left = std::max(0, left - 1);
    right = std::min(img.width() - 1, right + 1);
    return img.copy(left, cropTop, right - left + 1, cropBot - cropTop + 1);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

AtlasComparisonDialog::AtlasComparisonDialog(
    std::shared_ptr<pdfe::tre::EnrichedAtlas> scanAtlas,
    std::shared_ptr<pdfe::tre::EnrichedAtlas> ftAtlas,
    const QImage& lineImage,
    const QString& lineText,
    const std::vector<pdfe::tre::VisualFontRanker::RankedCandidate>& rankedCandidates,
    pdfe::tre::BitmapCompositor* compositor,
    float fontSizePt,
    int scanDpi,
    pdfe::tre::AtlasMode currentMode,
    bool initialFillMissing,
    QWidget* parent)
    : QDialog(parent)
    , scanAtlas_(std::move(scanAtlas))
    , ftAtlas_(std::move(ftAtlas))
    , lineImage_(lineImage)
    , lineText_(lineText)
    , rankedCandidates_(rankedCandidates)
    , compositor_(compositor)
    , fontSizePt_(fontSizePt)
    , scanDpi_(scanDpi)
    , selectedMode_(currentMode)
    , debounceTimer_(nullptr)
{
    setupUi();

    // Cache codepoints once
    unionCodepoints_ = buildUnionCodepoints(scanAtlas_);

    debounceTimer_ = new QTimer(this);
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(120);
    connect(debounceTimer_, &QTimer::timeout,
            this, &AtlasComparisonDialog::updatePreviews);

    connect(fontCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AtlasComparisonDialog::onFontChanged);
    connect(fillMissingCheck_, &QCheckBox::toggled,
            this, &AtlasComparisonDialog::onFillMissingToggled);
    connect(useScanButton_, &QPushButton::clicked,
            this, &AtlasComparisonDialog::onUseScan);
    connect(useFontButton_, &QPushButton::clicked,
            this, &AtlasComparisonDialog::onUseFont);
    connect(cancelButton_, &QPushButton::clicked,
            this, &QDialog::reject);

    // Apply persisted fill-missing state. Done AFTER the toggled
    // signal is connected so onFillMissingToggled fires and populates
    // atlasGlyphOverride_ from unionCodepoints_. If false (default),
    // the box is already unchecked from setupUi() and no signal fires.
    if (initialFillMissing)
        fillMissingCheck_->setChecked(true);

    // Install event filter on atlas charset label for click-to-toggle
    atlasCharsetLabel_->installEventFilter(this);

    // Initial render (sizes may not be final yet; showEvent re-renders)
    updatePreviews();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

AtlasComparisonDialog::~AtlasComparisonDialog() = default;

bool AtlasComparisonDialog::fillMissingChecked() const
{
    return fillMissingCheck_ && fillMissingCheck_->isChecked();
}

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------

std::string AtlasComparisonDialog::selectedFontFilePath() const
{
    int idx = fontCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(rankedCandidates_.size()))
        return {};
    return rankedCandidates_[static_cast<size_t>(idx)].candidate.filePath;
}

std::string AtlasComparisonDialog::selectedFontFamily() const
{
    int idx = fontCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(rankedCandidates_.size()))
        return {};
    return rankedCandidates_[static_cast<size_t>(idx)].candidate.profile.familyName;
}

// ---------------------------------------------------------------------------
// Event overrides
// ---------------------------------------------------------------------------

void AtlasComparisonDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // Re-render after the layout has resolved so label sizes are correct
    QTimer::singleShot(0, this, &AtlasComparisonDialog::updatePreviews);
}

void AtlasComparisonDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    // Guard: resizeEvent can fire from setupUi()'s resize() before debounceTimer_ exists
    if (debounceTimer_)
        debounceTimer_->start();
}

bool AtlasComparisonDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == atlasCharsetLabel_ && event->type() == QEvent::MouseButtonPress) {
        if (atlasCols_ <= 0 || atlasCellW_ <= 0 || atlasCellH_ <= 0)
            return QDialog::eventFilter(watched, event);

        auto* me = static_cast<QMouseEvent*>(event);
        int col = me->pos().x() / atlasCellW_;
        int row = me->pos().y() / atlasCellH_;
        int idx = row * atlasCols_ + col;
        if (idx >= 0 && idx < static_cast<int>(unionCodepoints_.size())) {
            char32_t cp = unionCodepoints_[static_cast<size_t>(idx)];
            // Toggle between 0 (original) and 1 (font-matched)
            int current = 0;
            auto it = atlasGlyphOverride_.find(cp);
            if (it != atlasGlyphOverride_.end())
                current = it->second;
            atlasGlyphOverride_[cp] = (current == 0) ? 1 : 0;
            refreshAtlasGrid();
        }
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void AtlasComparisonDialog::onFontChanged(int /*index*/)
{
    // Clear overrides when font changes since font-matched glyphs are different
    atlasGlyphOverride_.clear();
    if (debounceTimer_)
        debounceTimer_->start();
}

void AtlasComparisonDialog::updatePreviews()
{
    int maxW = originalPreviewLabel_->width();
    if (maxW < 50) maxW = 600;

    // Fit image to preview area — crop to content then scale to fit.
    auto fitToPreview = [&](const QImage& img) -> QPixmap {
        if (img.isNull())
            return {};
        QImage cropped = cropToContentWhiteBg(img);
        return QPixmap::fromImage(cropped).scaled(
            maxW, kPreviewH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    };

    // --- Row 1: original scan ---
    // Measure scan content height so we can match the font preview to it.
    int scanContentH = 0;
    if (!lineImage_.isNull()) {
        QImage scanCropped = cropToContentWhiteBg(lineImage_);
        scanContentH = scanCropped.height();
        originalPreviewLabel_->setPixmap(
            QPixmap::fromImage(scanCropped).scaled(
                maxW, kPreviewH, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        originalPreviewLabel_->clear();
        originalPreviewLabel_->setText(tr("(no scan image)"));
    }

    // --- Row 2: font-matched rendering ---
    // After content-cropping the font render, scale it to match the scan's
    // content height before fitting to preview.  This ensures the font text
    // appears at the same visual size as the scan text, regardless of whether
    // fontSizePt_ produces taller/shorter glyphs.
    int fontIdx = fontCombo_->currentIndex();
    if (fontIdx >= 0 && fontIdx < static_cast<int>(rankedCandidates_.size())
        && compositor_ && !lineText_.isEmpty())
    {
        const auto& rc = rankedCandidates_[static_cast<size_t>(fontIdx)];
        QImage fontImg = renderFontPreview(rc.candidate.filePath, fontSizePt_);
        if (!fontImg.isNull()) {
            QImage scanStyled = applyScanAppearance(fontImg);
            QImage fontCropped = cropToContentWhiteBg(scanStyled);
            // Match font content height to scan content height
            if (scanContentH > 0 && fontCropped.height() > 0
                && fontCropped.height() != scanContentH) {
                float hScale = static_cast<float>(scanContentH)
                             / static_cast<float>(fontCropped.height());
                int newW = std::max(1, static_cast<int>(fontCropped.width() * hScale));
                fontCropped = fontCropped.scaled(
                    newW, scanContentH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            fontPreviewLabel_->setPixmap(
                QPixmap::fromImage(fontCropped).scaled(
                    maxW, kPreviewH, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            fontPreviewLabel_->clear();
            fontPreviewLabel_->setText(tr("(render failed)"));
        }
    } else {
        fontPreviewLabel_->clear();
        fontPreviewLabel_->setText(tr("(no font selected)"));
    }

    // --- Row 3: atlas-based rendering ---
    QImage atlasImg = renderAtlasPreview();
    if (!atlasImg.isNull()) {
        atlasPreviewLabel_->setPixmap(fitToPreview(atlasImg));
    } else {
        atlasPreviewLabel_->clear();
        atlasPreviewLabel_->setText(tr("(no atlas)"));
    }

    // --- Character set grids ---
    int fontGridW = fontCharsetScroll_->viewport()->width();
    if (fontGridW < 100) fontGridW = 350;
    int atlasGridW = atlasCharsetScroll_->viewport()->width();
    if (atlasGridW < 100) atlasGridW = 350;

    // Font charset: render each char using BitmapCompositor
    if (fontIdx >= 0 && fontIdx < static_cast<int>(rankedCandidates_.size()) && compositor_) {
        const auto& rc = rankedCandidates_[static_cast<size_t>(fontIdx)];
        QImage fontGrid = renderFontCharsetGrid(rc.candidate.filePath, fontSizePt_, fontGridW);
        if (!fontGrid.isNull()) {
            fontCharsetLabel_->setPixmap(QPixmap::fromImage(fontGrid));
            fontCharsetLabel_->resize(fontGrid.size());
        } else {
            fontCharsetLabel_->clear();
            fontCharsetLabel_->resize(fontGridW, 0);
        }
    } else {
        fontCharsetLabel_->clear();
        fontCharsetLabel_->resize(fontGridW, 0);
    }

    // Atlas charset: color-coded scan atlas
    refreshAtlasGrid();
}

void AtlasComparisonDialog::onUseScan()
{
    selectedMode_ = pdfe::tre::AtlasMode::ScanCropped;
    accept();
}

void AtlasComparisonDialog::onUseFont()
{
    selectedMode_ = pdfe::tre::AtlasMode::FreeTypeStyled;
    accept();
}

void AtlasComparisonDialog::onFillMissingToggled(bool checked)
{
    if (!scanAtlas_)
        return;

    // Set or clear font-matched override for all missing codepoints
    for (char32_t cp : unionCodepoints_) {
        bool isAtlas = scanAtlas_->atlas().contains(cp);
        bool isSynth = !isAtlas && !scanAtlas_->glyphFromScan(cp).isNull();
        bool isMissing = !isAtlas && !isSynth;

        if (isMissing) {
            atlasGlyphOverride_[cp] = checked ? 1 : 0;
        }
    }

    refreshAtlasGrid();
}

// ---------------------------------------------------------------------------
// setupUi
// ---------------------------------------------------------------------------

void AtlasComparisonDialog::setupUi()
{
    setWindowTitle(tr("Atlas Comparison"));
    setMinimumSize(700, 500);
    resize(1100, 850);
    setObjectName(QStringLiteral("AtlasComparisonDialog"));
    setSizeGripEnabled(true);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(6);

    // -----------------------------------------------------------------------
    // Row 1: Original scan image
    // -----------------------------------------------------------------------
    auto* origRow = new QHBoxLayout;
    auto* origTitleLabel = new QLabel(tr("Original (scan):"));
    origTitleLabel->setFixedWidth(120);
    origRow->addWidget(origTitleLabel);

    originalPreviewLabel_ = new QLabel;
    originalPreviewLabel_->setFixedHeight(kPreviewH);
    originalPreviewLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    originalPreviewLabel_->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    originalPreviewLabel_->setObjectName(QStringLiteral("atlasOriginalPreview"));
    originalPreviewLabel_->setAccessibleName(QStringLiteral("atlas-comparison-original-preview"));
    origRow->addWidget(originalPreviewLabel_, 1);
    mainLayout->addLayout(origRow);

    // -----------------------------------------------------------------------
    // Row 2: Font-matched rendering with dropdown on the RIGHT
    // -----------------------------------------------------------------------
    auto* fontRow = new QHBoxLayout;

    auto* fontMatchLabel = new QLabel(tr("Font match:"));
    fontMatchLabel->setFixedWidth(120);
    fontRow->addWidget(fontMatchLabel);

    fontPreviewLabel_ = new QLabel;
    fontPreviewLabel_->setFixedHeight(kPreviewH);
    fontPreviewLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    fontPreviewLabel_->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    fontPreviewLabel_->setObjectName(QStringLiteral("atlasFontPreview"));
    fontPreviewLabel_->setAccessibleName(QStringLiteral("atlas-comparison-font-preview"));
    fontRow->addWidget(fontPreviewLabel_, 1);

    // Combo on the right side so previews align
    fontCombo_ = new QComboBox;
    fontCombo_->setObjectName(QStringLiteral("atlasFontCombo"));
    fontCombo_->setAccessibleName(QStringLiteral("atlas-comparison-font-combo"));
    fontCombo_->setFixedWidth(320);
    fontCombo_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    // Populate from ranked candidates
    for (size_t i = 0; i < rankedCandidates_.size(); ++i) {
        const auto& rc = rankedCandidates_[i];
        QString label = QString::fromUtf8(
            (rc.candidate.profile.familyName + " (" + rc.candidate.profile.styleName + ")").c_str());
        label += QString(" \u2014 v:%1 m:%2")
                     .arg(static_cast<double>(rc.visualScore), 0, 'f', 2)
                     .arg(static_cast<double>(rc.metricScore), 0, 'f', 1);
        fontCombo_->addItem(label);
    }
    if (rankedCandidates_.empty()) {
        fontCombo_->addItem(tr("(no candidates)"));
        fontCombo_->setEnabled(false);
    }

    fontRow->addWidget(fontCombo_);
    mainLayout->addLayout(fontRow);

    // -----------------------------------------------------------------------
    // Row 3: Atlas rendering
    // -----------------------------------------------------------------------
    auto* atlasRow = new QHBoxLayout;
    auto* atlasRowLabel = new QLabel(tr("Atlas (scan glyphs):"));
    atlasRowLabel->setFixedWidth(120);
    atlasRow->addWidget(atlasRowLabel);

    atlasPreviewLabel_ = new QLabel;
    atlasPreviewLabel_->setFixedHeight(kPreviewH);
    atlasPreviewLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    atlasPreviewLabel_->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    atlasPreviewLabel_->setObjectName(QStringLiteral("atlasAtlasPreview"));
    atlasPreviewLabel_->setAccessibleName(QStringLiteral("atlas-comparison-atlas-preview"));
    atlasRow->addWidget(atlasPreviewLabel_, 1);
    mainLayout->addLayout(atlasRow);

    // -----------------------------------------------------------------------
    // Character set section: font charset (left) | atlas charset (right)
    // -----------------------------------------------------------------------
    auto* charsetRow = new QHBoxLayout;

    // Left: font charset
    auto* fontCharsetCol = new QVBoxLayout;
    fontCharsetCol->addWidget(new QLabel(tr("Font Character Set:")));
    fontCharsetScroll_ = new QScrollArea;
    fontCharsetScroll_->setWidgetResizable(false);
    fontCharsetScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    fontCharsetScroll_->setObjectName(QStringLiteral("atlasFontCharsetScroll"));
    fontCharsetScroll_->setAccessibleName(QStringLiteral("atlas-comparison-font-charset-scroll"));
    fontCharsetLabel_ = new QLabel;
    fontCharsetLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    fontCharsetLabel_->setObjectName(QStringLiteral("atlasFontCharsetLabel"));
    fontCharsetLabel_->setAccessibleName(QStringLiteral("atlas-comparison-font-charset"));
    fontCharsetScroll_->setWidget(fontCharsetLabel_);
    fontCharsetCol->addWidget(fontCharsetScroll_, 1);

    auto* fontBtnRow = new QHBoxLayout;
    fontBtnRow->addStretch();
    useFontButton_ = new QPushButton(tr("Use Font Matched"));
    useFontButton_->setObjectName(QStringLiteral("atlasUseFontButton"));
    useFontButton_->setAccessibleName(QStringLiteral("atlas-comparison-use-font"));
    fontBtnRow->addWidget(useFontButton_);
    fontBtnRow->addStretch();
    fontCharsetCol->addLayout(fontBtnRow);

    charsetRow->addLayout(fontCharsetCol);

    // Separator
    auto* sep = new QFrame;
    sep->setFrameStyle(QFrame::VLine | QFrame::Sunken);
    charsetRow->addWidget(sep);

    // Right: atlas charset (color-coded, clickable)
    auto* atlasCharsetCol = new QVBoxLayout;

    // Title row with legend and fill-missing checkbox
    auto* atlasTitleRow = new QHBoxLayout;
    auto* atlasCharsetTitle = new QLabel(
        tr("<b>Atlas Character Set</b> "
           "<small>"
           "<span style='background:#dcffdc;'>&nbsp;Atlas&nbsp;</span> "
           "<span style='background:#ffffd2;'>&nbsp;Synthesized&nbsp;</span> "
           "<span style='background:#ffdcdc;'>&nbsp;Missing&nbsp;</span> "
           "<span style='background:#dce8ff;'>&nbsp;Font&nbsp;</span>"
           "</small>"));
    atlasCharsetTitle->setTextFormat(Qt::RichText);
    atlasTitleRow->addWidget(atlasCharsetTitle);

    atlasTitleRow->addStretch();

    fillMissingCheck_ = new QCheckBox(tr("Fill missing from font"));
    fillMissingCheck_->setObjectName(QStringLiteral("atlasFillMissingCheck"));
    fillMissingCheck_->setAccessibleName(QStringLiteral("atlas-comparison-fill-missing"));
    fillMissingCheck_->setToolTip(tr("Replace missing (red) characters with font-matched glyphs\n"
                                     "that have scan-like appearance applied."));
    atlasTitleRow->addWidget(fillMissingCheck_);

    atlasCharsetCol->addLayout(atlasTitleRow);

    atlasCharsetScroll_ = new QScrollArea;
    atlasCharsetScroll_->setWidgetResizable(false);
    atlasCharsetScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    atlasCharsetScroll_->setObjectName(QStringLiteral("atlasAtlasCharsetScroll"));
    atlasCharsetScroll_->setAccessibleName(QStringLiteral("atlas-comparison-atlas-charset-scroll"));
    atlasCharsetLabel_ = new QLabel;
    atlasCharsetLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    atlasCharsetLabel_->setObjectName(QStringLiteral("atlasAtlasCharsetLabel"));
    atlasCharsetLabel_->setAccessibleName(QStringLiteral("atlas-comparison-atlas-charset"));
    atlasCharsetLabel_->setCursor(Qt::PointingHandCursor);
    atlasCharsetScroll_->setWidget(atlasCharsetLabel_);
    atlasCharsetCol->addWidget(atlasCharsetScroll_, 1);

    auto* atlasBtnRow = new QHBoxLayout;
    atlasBtnRow->addStretch();
    useScanButton_ = new QPushButton(tr("Use Built Atlas"));
    useScanButton_->setObjectName(QStringLiteral("atlasUseScanButton"));
    useScanButton_->setAccessibleName(QStringLiteral("atlas-comparison-use-scan"));
    atlasBtnRow->addWidget(useScanButton_);
    atlasBtnRow->addStretch();
    atlasCharsetCol->addLayout(atlasBtnRow);

    charsetRow->addLayout(atlasCharsetCol);

    mainLayout->addLayout(charsetRow, 1);

    // -----------------------------------------------------------------------
    // Cancel button (far right)
    // -----------------------------------------------------------------------
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    cancelButton_ = new QPushButton(tr("Cancel"));
    cancelButton_->setObjectName(QStringLiteral("atlasCancelButton"));
    cancelButton_->setAccessibleName(QStringLiteral("atlas-comparison-cancel"));
    buttonRow->addWidget(cancelButton_);
    mainLayout->addLayout(buttonRow);
}

void AtlasComparisonDialog::refreshAtlasGrid()
{
    int gridW = atlasCharsetScroll_->viewport()->width();
    if (gridW < 100) gridW = 350;
    QImage atlasGrid = renderAtlasCharsetGrid(gridW);
    if (!atlasGrid.isNull()) {
        atlasCharsetLabel_->setPixmap(QPixmap::fromImage(atlasGrid));
        atlasCharsetLabel_->resize(atlasGrid.size());
    } else {
        atlasCharsetLabel_->clear();
        atlasCharsetLabel_->resize(gridW, 0);
    }
}

// ---------------------------------------------------------------------------
// Scan appearance simulation
// ---------------------------------------------------------------------------

void AtlasComparisonDialog::measureScanContrast() const
{
    if (scanContrastMeasured_ || lineImage_.isNull())
        return;

    QImage scanGray = lineImage_.convertToFormat(QImage::Format_Grayscale8);

    // Build histogram, then use percentiles to find typical fg/bg values.
    // Absolute min/max is unreliable because scan noise creates outlier pixels
    // near 0 and 255, making the contrast remap nearly a no-op.
    int histogram[256] = {};
    int totalPixels = 0;
    for (int y = 0; y < scanGray.height(); ++y) {
        const uchar* row = scanGray.constScanLine(y);
        for (int x = 0; x < scanGray.width(); ++x) {
            histogram[row[x]]++;
            totalPixels++;
        }
    }

    if (totalPixels == 0) {
        scanContrastMeasured_ = true;
        return;
    }

    // 5th percentile = foreground (dark text), 95th percentile = background (paper)
    int cumulative = 0;
    int p5 = 0, p95 = 255;
    for (int i = 0; i < 256; ++i) {
        cumulative += histogram[i];
        if (cumulative * 100 / totalPixels >= 5 && p5 == 0)
            p5 = i;
        if (cumulative * 100 / totalPixels >= 95) {
            p95 = i;
            break;
        }
    }

    scanFgVal_ = std::clamp(p5, 20, 100);
    scanBgVal_ = std::clamp(p95, 160, 230);
    // Guard against inversion or near-identity mapping
    if (scanFgVal_ >= scanBgVal_ - 40) {
        scanFgVal_ = 50;
        scanBgVal_ = 210;
    }
    scanContrastMeasured_ = true;
}

QImage AtlasComparisonDialog::applyScanAppearance(const QImage& composited) const
{
    if (composited.isNull())
        return composited;

    measureScanContrast();

    // Convert to grayscale
    QImage gray = composited.convertToFormat(QImage::Format_Grayscale8);

    const int w = gray.width(), h = gray.height();

    // --- Stage 1: Morphological dilation (ink spread) ---
    // Real scanners cause ink to bleed slightly, thickening strokes.
    // Radius 1 at 40px display height is proportional to ~1-2px ink spread
    // at 300 DPI scan resolution.
    gray = morphDilate(gray, 1);

    // --- Stage 2: Contrast remap with stronger reduction ---
    // Remap pixel values: 0 (black text) → scanFgVal_, 255 (white bg) → scanBgVal_
    float fg = static_cast<float>(scanFgVal_);
    float bg = static_cast<float>(scanBgVal_);

    // Deterministic hash-based noise — two components:
    // 1. Fine grain noise (per-pixel): simulates sensor noise
    // 2. Low-frequency spatial variation: simulates paper texture / uneven lighting
    auto fineNoise = [](int x, int y) -> int {
        unsigned h = static_cast<unsigned>(x * 374761393 + y * 668265263);
        h = (h ^ (h >> 13)) * 1274126177;
        return static_cast<int>((h >> 16) & 0xFF) % 21 - 10;  // range [-10, +10]
    };

    auto paperTexture = [](int x, int y) -> int {
        // Low-frequency variation: divide coords by 4-8px blocks for spatial coherence
        int bx = x / 5, by = y / 5;
        unsigned h = static_cast<unsigned>(bx * 2654435761u + by * 2246822519u);
        h = (h ^ (h >> 15)) * 2246822519u;
        return static_cast<int>((h >> 16) & 0xFF) % 13 - 6;  // range [-6, +6]
    };

    for (int y2 = 0; y2 < h; ++y2) {
        uchar* row = gray.scanLine(y2);
        for (int x2 = 0; x2 < w; ++x2) {
            float t = static_cast<float>(row[x2]) / 255.0f;
            int val = static_cast<int>(fg + t * (bg - fg));
            // Apply noise: fine grain everywhere + paper texture on lighter areas
            int noise = fineNoise(x2, y2);
            if (t > 0.5f) {
                // Paper texture is more visible on background areas
                noise += paperTexture(x2, y2);
            }
            row[x2] = static_cast<uchar>(std::clamp(val + noise, 0, 255));
        }
    }

    // --- Stage 3: Gaussian blur (scanner optics) ---
    // Sigma 1.0 with radius 2 gives a 5x5 kernel — noticeably softens edges
    // at 40px display height without destroying legibility.
    gray = gaussianBlur(gray, 2, 1.0f);

    // --- Stage 4: Edge roughening ---
    // Scanners produce irregular glyph edges.  Add slight per-pixel perturbation
    // along edges (pixels with neighbors of significantly different brightness).
    {
        QImage edgeRoughened = gray.copy();
        for (int y2 = 1; y2 < h - 1; ++y2) {
            const uchar* prevRow = gray.constScanLine(y2 - 1);
            const uchar* curRow  = gray.constScanLine(y2);
            const uchar* nextRow = gray.constScanLine(y2 + 1);
            uchar* out = edgeRoughened.scanLine(y2);
            for (int x2 = 1; x2 < w - 1; ++x2) {
                int c = curRow[x2];
                // Detect edge: large gradient in any direction
                int maxDiff = std::max({
                    std::abs(c - static_cast<int>(curRow[x2 - 1])),
                    std::abs(c - static_cast<int>(curRow[x2 + 1])),
                    std::abs(c - static_cast<int>(prevRow[x2])),
                    std::abs(c - static_cast<int>(nextRow[x2]))
                });
                if (maxDiff > 30) {
                    // Edge pixel — randomly shift toward darker or lighter neighbor
                    unsigned hash = static_cast<unsigned>(x2 * 1103515245 + y2 * 12345);
                    hash = (hash ^ (hash >> 11)) * 2654435761u;
                    int shift = static_cast<int>((hash >> 16) & 0xFF) % 25 - 12; // [-12, +12]
                    out[x2] = static_cast<uchar>(std::clamp(c + shift, 0, 255));
                }
            }
        }
        gray = edgeRoughened;
    }

    return gray;
}

QImage AtlasComparisonDialog::renderFontGlyphWithScanStyle(char32_t cp) const
{
    int fontIdx = fontCombo_->currentIndex();
    if (!compositor_ || fontIdx < 0
        || fontIdx >= static_cast<int>(rankedCandidates_.size()))
        return {};

    const auto& rc = rankedCandidates_[static_cast<size_t>(fontIdx)];

    // Render at a generous canvas so FreeType has room for full glyph
    int canvasH = kFontRenderH;

    std::u32string cpStr = {cp};
    pdfe::tre::RgbColor black{0, 0, 0};

    QImage glyph = compositor_->renderText(
        cpStr, rc.candidate.filePath, fontSizePt_, black,
        canvasH * 3, canvasH, scanDpi_);

    if (glyph.isNull())
        return {};

    // Composite onto white
    QImage whiteBg(glyph.size(), QImage::Format_ARGB32_Premultiplied);
    whiteBg.fill(Qt::white);
    QPainter p(&whiteBg);
    p.drawImage(0, 0, glyph);
    p.end();

    // --- Baseline-anchored rendering approach ---
    // Render 'H' to find baseline position, then extract a vertical window
    // that preserves the glyph's position relative to the baseline.
    // The window uses the same vertical extent as the tallest ink (ascender
    // to descender), keeping quotes high and descenders low.

    // Find font baseline and cap-top from a reference character (H, M, N, W...),
    // used as SHARED vertical window for ALL glyphs.
    int fontBaseline = -1;
    int fontCapTop = -1;
    for (char32_t refCp : {U'H', U'M', U'N', U'W', U'E', U'T', U'B', U'D'}) {
        QImage refGlyph = compositor_->renderText(
            std::u32string{refCp}, rc.candidate.filePath, fontSizePt_, black,
            canvasH * 3, canvasH, scanDpi_);
        if (refGlyph.isNull())
            continue;
        QImage refBg(refGlyph.size(), QImage::Format_ARGB32_Premultiplied);
        refBg.fill(Qt::white);
        QPainter rp(&refBg);
        rp.drawImage(0, 0, refGlyph);
        rp.end();
        auto [rTop, rBot] = findVerticalInkBounds(refBg);
        if (rTop >= 0 && rBot >= 0) {
            fontCapTop = rTop;
            fontBaseline = rBot;
            break;
        }
    }

    if (fontBaseline < 0 || fontCapTop < 0) {
        QImage cropped = cropToContentWhiteBg(whiteBg);
        if (cropped.isNull() || cropped.size() == whiteBg.size())
            return {};
        return cropped;
    }

    // Shared vertical window: extends from above cap-top (for ascenders on
    // ?, @, &, etc.) to baseline + descender space.  All glyphs use the same
    // window so vertical positions are consistent (quotes stay high,
    // dash/period stay at mid-height, descenders go low).
    int capH = fontBaseline - fontCapTop;
    int ascenderSpace = std::max(2, capH / 4);   // ~25% above cap for ?, @, &, etc.
    int descenderSpace = std::max(2, capH / 3);  // ~33% of cap-height for descenders
    int windowTop = fontCapTop - ascenderSpace;
    int windowBot = fontBaseline + descenderSpace;

    windowTop = std::max(0, windowTop);
    windowBot = std::min(whiteBg.height() - 1, windowBot);
    if (windowBot <= windowTop)
        return {};

    QImage windowed = cropToCommonVertical(whiteBg, windowTop, windowBot);
    if (windowed.isNull())
        return {};

    // Scale to match atlas glyph size.  Compare the font window's TOTAL height
    // to the atlas reference character's FULL image height.  Using cap-height
    // alone would leave the font window 58% taller (due to ascender+descender
    // space), making font-matched glyphs appear larger in the grid.
    if (scanAtlas_ && windowed.height() > 0) {
        int atlasRefH = 0;
        for (char32_t refCp : {U'H', U'M', U'N', U'W', U'E', U'T', U'B', U'D'}) {
            QImage atlasRef = scanAtlas_->glyphFromScan(refCp);
            if (atlasRef.isNull())
                continue;
            atlasRefH = atlasRef.height();
            break;
        }
        if (atlasRefH > 0) {
            float sizeScale = static_cast<float>(atlasRefH)
                            / static_cast<float>(windowed.height());
            sizeScale = std::clamp(sizeScale, 0.3f, 3.0f);
            if (std::abs(sizeScale - 1.0f) > 0.05f) {
                // Scale preserving aspect ratio — IgnoreAspectRatio was
                // compressing width, making glyphs (especially narrow ones
                // like 'I') visibly narrower than in the font charset grid.
                int newH = std::max(1, static_cast<int>(windowed.height() * sizeScale));
                windowed = windowed.scaledToHeight(newH, Qt::SmoothTransformation);
            }
        }
    }

    return windowed;
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

QImage AtlasComparisonDialog::renderFontPreview(
    const std::string& fontFilePath, float fontSizePt) const
{
    if (!compositor_ || lineText_.isEmpty() || fontFilePath.empty())
        return {};

    std::u32string u32text = lineText_.toStdU32String();
    int canvasH = lineImage_.height() > 0 ? lineImage_.height() : 50;
    int canvasW = std::max(lineImage_.width(), canvasH * 12);

    pdfe::tre::RgbColor black{0, 0, 0};
    QImage rendered = compositor_->renderText(
        u32text, fontFilePath, fontSizePt, black, canvasW, canvasH, scanDpi_);

    if (rendered.isNull())
        return {};

    // Composite onto white background (caller applies scan appearance at display resolution)
    QImage whiteBg(rendered.size(), QImage::Format_ARGB32_Premultiplied);
    whiteBg.fill(Qt::white);
    QPainter painter(&whiteBg);
    painter.drawImage(0, 0, rendered);
    painter.end();

    return whiteBg;
}

QImage AtlasComparisonDialog::renderAtlasPreview() const
{
    if (!scanAtlas_ || lineText_.isEmpty())
        return {};

    std::u32string u32text = lineText_.toStdU32String();

    int lineH = scanAtlas_->referenceLineHeight();
    if (lineH <= 0) lineH = 30;

    int baselineFromBottom = scanAtlas_->referenceBaselineFromBottom();
    if (baselineFromBottom <= 0) baselineFromBottom = lineH / 5;

    struct GlyphInfo { QImage img; int width; };
    std::vector<GlyphInfo> glyphs;
    glyphs.reserve(u32text.size());
    int totalW = 0;
    int spacing = std::max(1, lineH / 10);

    for (char32_t cp : u32text) {
        if (cp == U' ') {
            int spaceW = std::max(1, lineH / 3);
            glyphs.push_back({{}, spaceW});
            totalW += spaceW + spacing;
            continue;
        }
        // scanOnly=true: use only atlas+synthesized glyphs, no FreeType fallback
        QImage g = scanAtlas_->glyphFromScan(cp);
        int w = g.isNull() ? std::max(1, lineH / 2) : g.width();
        glyphs.push_back({std::move(g), w});
        totalW += w + spacing;
    }

    if (totalW <= 0)
        return {};

    int canvasH = lineH + 4;
    int canvasW = totalW + spacing;
    QImage canvas(canvasW, canvasH, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);

    QPainter painter(&canvas);

    int baselineY = std::clamp(canvasH - baselineFromBottom - 2, 0, canvasH - 1);
    painter.setPen(QPen(QColor(200, 100, 100, 120), 1, Qt::DashLine));
    painter.drawLine(0, baselineY, canvasW, baselineY);

    int penX = 0;
    for (const auto& gi : glyphs) {
        if (gi.img.isNull()) {
            penX += gi.width + spacing;
            continue;
        }
        int yOff = canvasH - gi.img.height();
        painter.drawImage(penX, std::max(0, yOff), gi.img);
        penX += gi.width + spacing;
    }

    painter.end();
    return canvas;
}

QImage AtlasComparisonDialog::renderFontCharsetGrid(
    const std::string& fontFilePath, float fontSizePt, int gridWidth) const
{
    if (!compositor_ || fontFilePath.empty() || unionCodepoints_.empty())
        return {};

    QFont labelFont;
    labelFont.setFamilies({QStringLiteral("Consolas"), QStringLiteral("DejaVu Sans Mono"),
                           QStringLiteral("monospace")});
    labelFont.setPointSize(8);
    QFontMetrics labelFm(labelFont);
    int labelH = labelFm.height();
    int cellPad = 4;
    int cellH = kGlyphAreaH + labelH + cellPad * 3;
    int cellW = std::max(kGlyphAreaH + cellPad * 2,
                         labelFm.horizontalAdvance(QStringLiteral("W")) + cellPad * 4);
    cellW = std::max(cellW, 24);

    int cols = std::max(1, gridWidth / cellW);
    int cpCount = static_cast<int>(unionCodepoints_.size());
    int rows = (cpCount + cols - 1) / cols;

    constexpr int kMaxCanvasH = 8000;
    int canvasW = cols * cellW;
    int canvasH = rows * cellH;
    int maxCps = cpCount;
    if (canvasH > kMaxCanvasH) {
        int maxRows = kMaxCanvasH / cellH;
        maxCps = maxRows * cols;
        rows = maxRows;
        canvasH = rows * cellH;
    }
    if (canvasH <= 0 || canvasW <= 0)
        return {};

    QImage canvas(canvasW, canvasH, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);

    bool truncated = (maxCps < cpCount);

    // --- Pass 1: render all glyphs on same-height canvas, find common vertical
    //     ink window across ALL glyphs so vertical positions are preserved.
    //     (Quote marks stay high, descenders stay low, baseline stays consistent.)
    struct RenderedGlyph { QImage whiteBg; };
    std::vector<RenderedGlyph> rendered(static_cast<size_t>(maxCps));
    int globalTop = kFontRenderH;   // will shrink to topmost ink
    int globalBot = 0;              // will grow to bottommost ink

    for (int i = 0; i < maxCps && i < cpCount; ++i) {
        char32_t cp = unionCodepoints_[static_cast<size_t>(i)];
        if (cp == U' ')
            continue;

        std::u32string cpStr = {cp};
        pdfe::tre::RgbColor black{0, 0, 0};
        QImage glyph = compositor_->renderText(
            cpStr, fontFilePath, fontSizePt, black,
            kFontRenderH * 2, kFontRenderH, scanDpi_);

        if (!glyph.isNull()) {
            QImage whiteBg(glyph.size(), QImage::Format_ARGB32_Premultiplied);
            whiteBg.fill(Qt::white);
            QPainter gp(&whiteBg);
            gp.drawImage(0, 0, glyph);
            gp.end();

            auto [top, bot] = findVerticalInkBounds(whiteBg);
            if (top >= 0) {
                globalTop = std::min(globalTop, top);
                globalBot = std::max(globalBot, bot);
            }
            rendered[static_cast<size_t>(i)].whiteBg = std::move(whiteBg);
        }
    }
    if (globalTop > globalBot) {
        globalTop = 0;
        globalBot = kFontRenderH - 1;
    }

    // Find baseline: render 'H' and find its bottom ink row within the common window
    int baselineInWindow = -1;
    {
        std::u32string hStr = {U'H'};
        pdfe::tre::RgbColor black{0, 0, 0};
        QImage hGlyph = compositor_->renderText(
            hStr, fontFilePath, fontSizePt, black,
            kFontRenderH * 2, kFontRenderH, scanDpi_);
        if (!hGlyph.isNull()) {
            QImage hBg(hGlyph.size(), QImage::Format_ARGB32_Premultiplied);
            hBg.fill(Qt::white);
            QPainter hp(&hBg);
            hp.drawImage(0, 0, hGlyph);
            hp.end();
            auto [hTop, hBot] = findVerticalInkBounds(hBg);
            if (hBot >= 0)
                baselineInWindow = hBot - globalTop;  // relative to common window top
        }
    }

    int commonH = globalBot - globalTop + 1;
    float uniformScale = static_cast<float>(kGlyphAreaH) / static_cast<float>(commonH);
    int maxGlyphW = cellW - cellPad * 2;
    int baselineY = (baselineInWindow >= 0)
                        ? static_cast<int>(baselineInWindow * uniformScale)
                        : -1;

    // --- Pass 2: crop each glyph to the common vertical window, scale, draw ---
    QPainter painter(&canvas);
    painter.setFont(labelFont);

    for (int i = 0; i < maxCps && i < cpCount; ++i) {
        char32_t cp = unionCodepoints_[static_cast<size_t>(i)];
        int col = i % cols;
        int row = i / cols;
        int cx = col * cellW;
        int cy = row * cellH;

        painter.fillRect(cx + 1, cy + 1, cellW - 2, cellH - 2, QColor(255, 255, 255));
        painter.setPen(QColor(200, 200, 200));
        painter.drawRect(cx, cy, cellW - 1, cellH - 1);

        // Draw baseline indicator
        if (baselineY >= 0 && baselineY < kGlyphAreaH) {
            painter.setPen(QPen(QColor(200, 80, 80, 160), 1, Qt::DashLine));
            int by = cy + cellPad + baselineY;
            painter.drawLine(cx + 2, by, cx + cellW - 3, by);
        }

        const auto& rg = rendered[static_cast<size_t>(i)];
        if (cp == U' ' || rg.whiteBg.isNull()) {
            if (rg.whiteBg.isNull() && cp != U' ') {
                painter.setPen(QColor(180, 180, 180));
                QRect glyphRect(cx + cellPad, cy + cellPad, maxGlyphW, kGlyphAreaH);
                painter.drawText(glyphRect, Qt::AlignCenter, QStringLiteral("?"));
            }
        } else {
            // Crop to common vertical window (preserves relative position)
            QImage cropped = cropToCommonVertical(rg.whiteBg, globalTop, globalBot);
            // Scale uniformly — all cropped images have the same height
            int scaledW = std::max(1, static_cast<int>(cropped.width() * uniformScale));
            int scaledH = kGlyphAreaH;  // commonH * uniformScale == kGlyphAreaH
            if (scaledW > maxGlyphW)
                scaledW = maxGlyphW;

            QImage scaled = cropped.scaled(scaledW, scaledH,
                                           Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation);

            // Apply scan appearance at display resolution (after scaling) so
            // blur/noise are visible.  Per-glyph application preserves crisp
            // labels, grid lines, and baseline indicators.
            scaled = applyScanAppearance(scaled);

            // All images are kGlyphAreaH tall — position at top of glyph area
            painter.drawImage(
                cx + cellPad + (maxGlyphW - scaled.width()) / 2,
                cy + cellPad,
                scaled);
        }

        // Character label below glyph area
        painter.setPen(Qt::black);
        QString label = QString::fromUcs4(&cp, 1);
        QRect labelRect(cx, cy + cellPad + kGlyphAreaH + cellPad, cellW, labelH);
        painter.drawText(labelRect, Qt::AlignCenter, label);
    }

    if (truncated) {
        QFont noteFont = labelFont;
        noteFont.setItalic(true);
        painter.setFont(noteFont);
        painter.setPen(QColor(128, 0, 0));
        QString note = QString("(truncated \u2014 %1 of %2 glyphs shown)")
                           .arg(maxCps).arg(cpCount);
        painter.drawText(QRect(0, canvasH - cellH, canvasW, cellH),
                         Qt::AlignCenter, note);
    }

    painter.end();
    return canvas;
}

QImage AtlasComparisonDialog::renderAtlasCharsetGrid(int gridWidth)
{
    if (!scanAtlas_ || unionCodepoints_.empty())
        return {};

    QFont labelFont;
    labelFont.setFamilies({QStringLiteral("Consolas"), QStringLiteral("DejaVu Sans Mono"),
                           QStringLiteral("monospace")});
    labelFont.setPointSize(8);
    QFontMetrics labelFm(labelFont);
    int labelH = labelFm.height();
    int cellPad = 4;
    int cellH = kGlyphAreaH + labelH + cellPad * 3;
    int cellW = std::max(kGlyphAreaH + cellPad * 2,
                         labelFm.horizontalAdvance(QStringLiteral("W")) + cellPad * 4);
    cellW = std::max(cellW, 24);

    int cols = std::max(1, gridWidth / cellW);
    int cpCount = static_cast<int>(unionCodepoints_.size());
    int rows = (cpCount + cols - 1) / cols;

    constexpr int kMaxCanvasH = 8000;
    int canvasW = cols * cellW;
    int canvasH = rows * cellH;
    int maxCps = cpCount;
    if (canvasH > kMaxCanvasH) {
        int maxRows = kMaxCanvasH / cellH;
        maxCps = maxRows * cols;
        rows = maxRows;
        canvasH = rows * cellH;
    }
    if (canvasH <= 0 || canvasW <= 0)
        return {};

    // Store layout info for click detection
    atlasCols_ = cols;
    atlasCellW_ = cellW;
    atlasCellH_ = cellH;

    QImage canvas(canvasW, canvasH, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);

    bool truncated = (maxCps < cpCount);

    // --- Pass 1: gather all display glyphs and find tallest for uniform scaling ---
    enum class GlyphSource { Atlas, Synthesized, Missing };
    struct AtlasGlyphInfo {
        QImage img;
        GlyphSource source = GlyphSource::Missing;
        bool fontMatched = false;
    };
    std::vector<AtlasGlyphInfo> glyphInfos(static_cast<size_t>(maxCps));
    int tallestH = 1;

    // Sub-pass 1a: collect atlas/synthesized glyphs and determine tallestH.
    // tallestH is ALWAYS computed from the atlas/synthesized images, even for
    // cells currently toggled to font-matched.  This ensures toggling a single
    // cell never changes the scaling of all other cells.
    for (int i = 0; i < maxCps && i < cpCount; ++i) {
        char32_t cp = unionCodepoints_[static_cast<size_t>(i)];
        auto& gi = glyphInfos[static_cast<size_t>(i)];

        if (scanAtlas_->atlas().contains(cp)) {
            gi.img = scanAtlas_->glyphFromScan(cp);
            gi.source = GlyphSource::Atlas;
        } else {
            gi.img = scanAtlas_->glyphFromScan(cp);
            if (!gi.img.isNull())
                gi.source = GlyphSource::Synthesized;
        }

        // Check if this codepoint has a font-matched override
        auto overIt = atlasGlyphOverride_.find(cp);
        if (overIt != atlasGlyphOverride_.end() && overIt->second == 1)
            gi.fontMatched = true;

        // ALL atlas/synthesized images contribute to tallestH regardless of
        // font-matched state — the original image height is the stable reference.
        if (!gi.img.isNull())
            tallestH = std::max(tallestH, gi.img.height());
    }

    // Sub-pass 1b: render font-matched glyphs.  These do NOT contribute to
    // tallestH — the H-correction in renderFontGlyphWithScanStyle already
    // scales them to match atlas glyph size.  If font-matched glyphs changed
    // tallestH, toggling a single cell would resize ALL cells (visual bug).
    for (int i = 0; i < maxCps && i < cpCount; ++i) {
        auto& gi = glyphInfos[static_cast<size_t>(i)];
        if (!gi.fontMatched)
            continue;
        char32_t cp = unionCodepoints_[static_cast<size_t>(i)];
        QImage fontGlyph = renderFontGlyphWithScanStyle(cp);
        if (!fontGlyph.isNull() && fontGlyph.height() > 0) {
            gi.img = std::move(fontGlyph);
            // Do NOT update tallestH — font glyphs are already H-corrected
        }
        // If render failed, keep the original atlas/synthesized glyph
    }

    // Uniform scale: tallest glyph fills kGlyphAreaH, others scale proportionally
    float uniformScale = static_cast<float>(kGlyphAreaH) / static_cast<float>(tallestH);
    int maxGlyphW = cellW - cellPad * 2;

    // Estimate baseline position from atlas reference metrics
    int atlasBaselineY = -1;
    {
        int refLineH = scanAtlas_->referenceLineHeight();
        int baseFromBot = scanAtlas_->referenceBaselineFromBottom();
        // Fallback: if baseFromBot is 0 (small refLineH or not computed), use ~20% from bottom
        if (refLineH > 0 && baseFromBot <= 0)
            baseFromBot = std::max(1, refLineH / 5);
        if (refLineH > 0 && baseFromBot > 0) {
            float baseFrac = static_cast<float>(refLineH - baseFromBot)
                           / static_cast<float>(refLineH);
            atlasBaselineY = std::min(kGlyphAreaH - 1,
                                      static_cast<int>(kGlyphAreaH * baseFrac));
        }
    }

    // --- Pass 2: draw grid with uniform scaling ---
    QPainter painter(&canvas);
    painter.setFont(labelFont);

    for (int i = 0; i < maxCps && i < cpCount; ++i) {
        char32_t cp = unionCodepoints_[static_cast<size_t>(i)];
        int col = i % cols;
        int row = i / cols;
        int cx = col * cellW;
        int cy = row * cellH;

        const auto& gi = glyphInfos[static_cast<size_t>(i)];

        // Background color
        QColor bgColor;
        if (gi.fontMatched) {
            bgColor = QColor(220, 232, 255);  // light blue for font-matched
        } else {
            switch (gi.source) {
            case GlyphSource::Atlas:       bgColor = QColor(220, 255, 220); break;
            case GlyphSource::Synthesized: bgColor = QColor(255, 255, 210); break;
            case GlyphSource::Missing:     bgColor = QColor(255, 220, 220); break;
            }
        }

        painter.fillRect(cx + 1, cy + 1, cellW - 2, cellH - 2, bgColor);
        painter.setPen(QColor(200, 200, 200));
        painter.drawRect(cx, cy, cellW - 1, cellH - 1);

        // Draw baseline indicator
        if (atlasBaselineY >= 0 && atlasBaselineY < kGlyphAreaH) {
            painter.setPen(QPen(QColor(200, 80, 80, 160), 1, Qt::DashLine));
            int by = cy + cellPad + atlasBaselineY;
            painter.drawLine(cx + 2, by, cx + cellW - 3, by);
        }

        if (!gi.img.isNull()) {
            // Apply uniform scale — preserves relative glyph proportions
            int scaledW = std::max(1, static_cast<int>(gi.img.width() * uniformScale));
            int scaledH = std::max(1, static_cast<int>(gi.img.height() * uniformScale));
            if (scaledW > maxGlyphW) {
                float wScale = static_cast<float>(maxGlyphW) / static_cast<float>(scaledW);
                scaledW = maxGlyphW;
                scaledH = std::max(1, static_cast<int>(scaledH * wScale));
            }

            QImage scaled = gi.img.scaled(scaledW, scaledH,
                                           Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            // Apply scan appearance to font-matched glyphs (at display size,
            // after scaling — produces visible noise/blur at the right resolution)
            if (gi.fontMatched)
                scaled = applyScanAppearance(scaled);
            // Bottom-align within glyph area
            painter.drawImage(
                cx + cellPad + (maxGlyphW - scaled.width()) / 2,
                cy + cellPad + (kGlyphAreaH - scaled.height()),
                scaled);
        } else {
            painter.setPen(QColor(180, 180, 180));
            QRect glyphRect(cx + cellPad, cy + cellPad, maxGlyphW, kGlyphAreaH);
            painter.drawText(glyphRect, Qt::AlignCenter, QStringLiteral("?"));
        }

        painter.setPen(Qt::black);
        QString label = QString::fromUcs4(&cp, 1);
        QRect labelRect(cx, cy + cellPad + kGlyphAreaH + cellPad, cellW, labelH);
        painter.drawText(labelRect, Qt::AlignCenter, label);
    }

    if (truncated) {
        QFont noteFont = labelFont;
        noteFont.setItalic(true);
        painter.setFont(noteFont);
        painter.setPen(QColor(128, 0, 0));
        QString note = QString("(truncated \u2014 %1 of %2 glyphs shown)")
                           .arg(maxCps).arg(cpCount);
        painter.drawText(QRect(0, canvasH - cellH, canvasW, cellH),
                         Qt::AlignCenter, note);
    }

    painter.end();
    return canvas;
}

} // namespace pdfe::ui
