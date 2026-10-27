#include "TextEditTool.h"
#include "ContentStreamEditor.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontFamilyResolver.h"
#include "fonts/FontMetricProfile.h"
#include "fonts/TextShaper.h"
#include "ui/AccessibilityHelper.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QSet>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <QWindow>
#include <QDebug>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <unordered_map>

// Platform-appropriate default font family names. On Windows/macOS, use the
// Microsoft / Apple names directly because they're guaranteed installed and
// Qt's font matching tracks them. On Linux, Microsoft fonts usually aren't
// installed and Qt6 ignores fontconfig substitution rules for QFontInfo —
// so use the metric-compatible Liberation families which IS installed and
// matches glyph widths exactly (same PDF layout).
#if defined(_WIN32) || defined(__APPLE__)
    #define PDFE_FAMILY_SANS    QStringLiteral("Arial")
    #define PDFE_FAMILY_SERIF   QStringLiteral("Times New Roman")
    #define PDFE_FAMILY_MONO    QStringLiteral("Courier New")
    #define PDFE_FAMILY_CALIBRI QStringLiteral("Calibri")
    #define PDFE_FAMILY_CAMBRIA QStringLiteral("Cambria")
#else
    #define PDFE_FAMILY_SANS    QStringLiteral("Liberation Sans")
    #define PDFE_FAMILY_SERIF   QStringLiteral("Liberation Serif")
    #define PDFE_FAMILY_MONO    QStringLiteral("Liberation Mono")
    #define PDFE_FAMILY_CALIBRI QStringLiteral("Carlito")
    #define PDFE_FAMILY_CAMBRIA QStringLiteral("Caladea")
#endif

namespace pdfe::textedit {

namespace {

/// Read QClipboard::text() with retries to absorb transient empty
/// reads under Windows RDP / remote sessions, where the OS briefly
/// transfers clipboard ownership to the session manager during a
/// read. Without retries, a genuine paste can silently no-op because
/// the first read returned empty.
///
/// Spins at most `maxAttempts` times, calling processEvents() and
/// sleeping briefly between attempts. Returns the first non-empty
/// value, or the empty string if every attempt failed.
QString readClipboardTextWithRetry(int maxAttempts = 5,
                                   int delayMs = 15) {
    QClipboard* cb = QApplication::clipboard();
    if (!cb) return {};
    for (int i = 0; i < maxAttempts; ++i) {
        QString s = cb->text();
        if (!s.isEmpty()) return s;
        if (i + 1 < maxAttempts) {
            QCoreApplication::processEvents(
                QEventLoop::ExcludeUserInputEvents, delayMs);
            QThread::msleep(delayMs);
        }
    }
    return {};
}

/// Write text to QClipboard::setText() with a verify-then-retry
/// loop. Returns true when clipboard->text() reads back the expected
/// value within the retry budget. Guards against the RDP race where
/// another session process overwrites the clipboard between our
/// setText and the caller's subsequent read.
bool writeClipboardTextWithVerify(const QString& text,
                                  int maxAttempts = 5,
                                  int delayMs = 15) {
    QClipboard* cb = QApplication::clipboard();
    if (!cb) return false;
    for (int i = 0; i < maxAttempts; ++i) {
        cb->setText(text);
        QCoreApplication::processEvents(
            QEventLoop::ExcludeUserInputEvents, delayMs);
        if (cb->text() == text) return true;
        if (i + 1 < maxAttempts) {
            QThread::msleep(delayMs);
        }
    }
    return false;
}

} // namespace

/// Detect whether a PDF font name has a subset prefix (e.g., "REFXWK+Liberation Serif").
/// Subset prefixes indicate a real embedded font with correct metrics, as opposed to
/// OCR placeholder fonts like "GlyphLessFont" which have meaningless size/metrics.
static bool hasSubsetPrefix(const std::string& fontName) {
    if (fontName.size() < 8) return false;  // Need at least "ABCDEF+X"
    if (fontName[6] != '+') return false;
    for (int i = 0; i < 6; ++i) {
        if (fontName[i] < 'A' || fontName[i] > 'Z') return false;
    }
    return true;
}

int TextEditTool::charIndexFromPdfX(const TextSpan& span, double pdfX) const
{
    if (span.text.empty()) return 0;

    double relativeX = pdfX - span.boundingBox.left();
    double spanWidth = span.boundingBox.width();
    int textLen = static_cast<int>(span.text.size());

    // When atlas overlay is active, use atlas glyph widths for hit-testing
    // to match the rendering layout exactly.
    if (overlayEnrichedAtlas_ && overlayEnrichedAtlas_->size() > 0 && span.formattingRuns.empty()) {
        // Compute total atlas advance at zoom=1 (PDF coords)
        float bboxH = static_cast<float>(span.boundingBox.height());
        float scale = atlasScaleToScreen(span, bboxH);
        // atlasAdvanceForChars returns screen pixels at the given scale;
        // for PDF-space comparison, we need to map through the span width ratio
        float totalAtlasAdv = atlasAdvanceForChars(span, textLen, scale, 1.0f);
        double pdfScale = (totalAtlasAdv > 0.001f)
            ? (spanWidth / static_cast<double>(totalAtlasAdv)) : 1.0;

        int bestIndex = 0;
        double bestDist = std::abs(relativeX);
        for (int i = 1; i <= textLen; ++i) {
            double charRight = static_cast<double>(
                atlasAdvanceForChars(span, i, scale, 1.0f)) * pdfScale;
            double dist = std::abs(relativeX - charRight);
            if (dist < bestDist) {
                bestDist = dist;
                bestIndex = i;
            }
        }
        return bestIndex;
    }

    // System font mode: use Qt font metrics
    QFont font = buildOverlayFont(span, 1.0f, overlayFontOverride_, overlayFontSizePtOverride_, visibleExtentRatioForFont(span.fontName), serifClassForFont(span.fontName));
    QFontMetricsF fm(font);

    QString displayText = QString::fromUcs4(
        span.text.data(), static_cast<qsizetype>(span.text.size()));
    double totalAdvance = fm.horizontalAdvance(displayText);

    // Scale factor from font-metric space to PDF space
    double scale = (totalAdvance > 0.001) ? (spanWidth / totalAdvance) : 1.0;

    // Linear scan for the character boundary closest to relativeX
    int bestIndex = 0;
    double bestDist = std::abs(relativeX);

    for (int i = 1; i <= textLen; ++i) {
        double charRight = fm.horizontalAdvance(displayText.left(i)) * scale;
        double dist = std::abs(relativeX - charRight);
        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = i;
        }
    }

    return bestIndex;
}

// ===== Writing-mode / bidi overlay geometry (R2-21b) =======================

namespace {

// Cumulative advance along the writing axis for the first `count` logical
// characters. Consumes TextShaper::caretAdvance() to pick the axis per glyph:
// vertical text advances in Y (dy), horizontal in X (dx). This is the single
// place the caret model decides its axis, so vertical and horizontal share one
// accumulation path.
float axisCumulative(const std::vector<float>& advances, int count, bool vertical)
{
    float sum = 0.0f;
    const int n = std::min(count, static_cast<int>(advances.size()));
    for (int i = 0; i < n; ++i) {
        pdfe::fonts::ShapedGlyph g;
        g.xAdvance = advances[i];
        g.yAdvance = advances[i];
        const pdfe::fonts::CaretAdvance ca =
            pdfe::fonts::TextShaper::caretAdvance(g, vertical);
        sum += vertical ? ca.dy : ca.dx;
    }
    return sum;
}

} // namespace

bool TextEditTool::overlayBaseIsRtl(const std::u32string& logical)
{
    // The shaper's bidi partitions the string into level runs; the first run's
    // direction is the paragraph base direction (UAX#9-lite, default LTR).
    const auto runs = pdfe::fonts::TextShaper::bidiRuns(logical);
    return !runs.empty() && runs.front().rtl;
}

std::pair<QPointF, QPointF> TextEditTool::overlayCaretLine(
    const std::vector<float>& advances,
    const QRectF& screenBbox,
    int logicalIndex,
    OverlayGeometryParams params)
{
    const float left = static_cast<float>(screenBbox.left());
    const float right = static_cast<float>(screenBbox.right());
    const float top = static_cast<float>(screenBbox.top());
    const float bottom = static_cast<float>(screenBbox.bottom());

    const int clamped = std::max(0,
        std::min(logicalIndex, static_cast<int>(advances.size())));
    const float pre = axisCumulative(advances, clamped, params.vertical);

    if (params.vertical) {
        // TTB: caret steps down in Y; the column stacks in X. The caret is a
        // horizontal stroke spanning the column width at the current Y.
        const float y = top + pre;
        return {QPointF(left, y), QPointF(right, y)};
    }
    if (params.rtl) {
        // RTL: logical index 0 sits at the RIGHT visual edge; increasing index
        // moves the caret leftward.
        const float x = right - pre;
        return {QPointF(x, top), QPointF(x, bottom)};
    }
    // LTR (regression-locked): caret grows rightward from the left edge.
    const float x = left + pre;
    return {QPointF(x, top), QPointF(x, bottom)};
}

std::vector<QRectF> TextEditTool::overlaySelectionRects(
    const std::vector<float>& advances,
    const QRectF& screenBbox,
    int startLogical, int endLogical,
    OverlayGeometryParams params)
{
    std::vector<QRectF> rects;
    int start = std::min(startLogical, endLogical);
    int end = std::max(startLogical, endLogical);
    start = std::max(0, std::min(start, static_cast<int>(advances.size())));
    end = std::max(0, std::min(end, static_cast<int>(advances.size())));
    if (start >= end)
        return rects;

    const float left = static_cast<float>(screenBbox.left());
    const float right = static_cast<float>(screenBbox.right());
    const float top = static_cast<float>(screenBbox.top());
    const float width = static_cast<float>(screenBbox.width());
    const float height = static_cast<float>(screenBbox.height());

    const float a = axisCumulative(advances, start, params.vertical);
    const float b = axisCumulative(advances, end, params.vertical);

    if (params.vertical) {
        // Selection stacks vertically: [top+a, top+b) across the full column.
        rects.emplace_back(left, top + a, width, b - a);
    } else if (params.rtl) {
        // Mirrored: the selected logical prefix occupies the RIGHT visual span.
        rects.emplace_back(right - b, top, b - a, height);
    } else {
        rects.emplace_back(left + a, top, b - a, height);
    }
    return rects;
}

std::vector<float> TextEditTool::systemFontAdvances(const TextSpan& span,
                                                    float zoom) const
{
    std::vector<float> advances;
    const int n = static_cast<int>(span.text.size());
    if (n == 0)
        return advances;

    const QString displayText = QString::fromUcs4(
        span.text.data(), static_cast<qsizetype>(span.text.size()));
    QFont font = buildOverlayFont(span, zoom, overlayFontOverride_,
                                  overlayFontSizePtOverride_,
                                  visibleExtentRatioForFont(span.fontName),
                                  serifClassForFont(span.fontName));
    QFontMetricsF fm(font);

    // Per-char advance as the delta of cumulative substring widths, so the
    // running sum reproduces fm.horizontalAdvance(left(N)) exactly (kerning
    // included) — this keeps LTR geometry byte-identical to the prior code.
    float prev = 0.0f;
    for (int i = 1; i <= n; ++i) {
        const float cur = static_cast<float>(
            fm.horizontalAdvance(displayText.left(i)));
        advances.push_back(cur - prev);
        prev = cur;
    }
    return advances;
}

float TextEditTool::atlasAdvanceForChars(const TextSpan& span, int charCount,
                                          float scaleToScreen, float zoom) const
{
    if (charCount <= 0) return 0.0f;
    int textLen = static_cast<int>(span.text.size());
    int n = std::min(charCount, textLen);

    // Compute average glyph metrics from the atlas for spacing
    float avgW = 0.0f;
    float avgH = 0.0f;
    int glyphCount = 0;
    for (char32_t c : span.text) {
        if (c == U' ') continue;
        QImage g = overlayEnrichedAtlas_->glyph(c);
        if (!g.isNull()) {
            avgW += static_cast<float>(g.width());
            avgH += static_cast<float>(g.height());
            ++glyphCount;
        }
    }
    avgW = glyphCount > 0 ? avgW / static_cast<float>(glyphCount) : 10.0f;
    avgH = glyphCount > 0 ? avgH / static_cast<float>(glyphCount) : 20.0f;
    float spaceWidth = avgW * 0.6f;

    // Build a fallback font sized to match atlas glyph height (in screen pixels).
    // setPixelSize ensures the font renders at the same visual height regardless
    // of screen DPI, matching the atlas glyph scaling exactly.
    int fallbackPixelSize = std::max(1, static_cast<int>(avgH * scaleToScreen));
    QFont fallbackFont;
    if (!overlayFontOverride_.isEmpty()) {
        fallbackFont = QFont(overlayFontOverride_);
    } else {
        fallbackFont = QFont(PDFE_FAMILY_SANS);
    }
    fallbackFont.setPixelSize(fallbackPixelSize);
    QFontMetricsF fallbackFm(fallbackFont);

    // Walk through characters, accumulating advance using atlas widths
    float advance = 0.0f;
    for (int i = 0; i < n; ++i) {
        char32_t c = span.text[i];
        if (c == U' ') {
            advance += spaceWidth * scaleToScreen;
            continue;
        }
        QImage glyphImg = overlayEnrichedAtlas_->glyph(c);
        if (!glyphImg.isNull()) {
            advance += static_cast<float>(glyphImg.width()) * scaleToScreen;
        } else {
            // Fallback: sized to match atlas glyph height
            QString ch = QString::fromUcs4(&c, 1);
            advance += static_cast<float>(fallbackFm.horizontalAdvance(ch));
        }
    }
    return advance;
}

float TextEditTool::atlasScaleToScreen(const TextSpan& span, float bboxH) const
{
    if (!overlayEnrichedAtlas_ || overlayEnrichedAtlas_->size() == 0 || span.text.empty())
        return 1.0f;

    // Use the canonical reference line height from the atlas. All atlas
    // glyphs are normalized to refLineH (via baseline-aligned padding for
    // shorter glyphs, scaling for taller). Simple division gives correct scale.
    int refLineH = overlayEnrichedAtlas_->referenceLineHeight();
    if (refLineH < 1) return 1.0f;

    float scale = bboxH / static_cast<float>(refLineH);

    if (scaleLogCount_++ % 60 == 0) {
        qDebug() << "[TextEdit] atlasScaleToScreen: bboxH=" << bboxH
                 << "refLineH=" << refLineH
                 << "scale=" << scale;
    }
    return scale;
}

void TextEditTool::renderAtlasOverlay(
    QPainter& painter, const TextSpan& span,
    const QRectF& atlasScreenBbox, const QRectF& screenBbox,
    float zoom, bool logChars)
{
    float penX = static_cast<float>(atlasScreenBbox.left());
    float bboxH = static_cast<float>(atlasScreenBbox.height());
    float drawTop = static_cast<float>(atlasScreenBbox.top());

    // Compute average glyph width for space sizing
    float avgW = 0.0f;
    int glyphCount = 0;
    for (char32_t c : span.text) {
        if (c == U' ' || c == U'\n' || c == U'\r' || c == U'\t') continue;
        QImage g = overlayEnrichedAtlas_->glyph(c);
        if (!g.isNull()) {
            avgW += static_cast<float>(g.width());
            ++glyphCount;
        }
    }
    avgW = glyphCount > 0 ? avgW / glyphCount : bboxH * 0.4f;
    float spaceWidth = avgW * 0.6f;
    float scaleToScreen = atlasScaleToScreen(span, bboxH);

    // Compute baseline position from atlas reference metrics (matches
    // GlyphAtlasCompositor and AtlasComparisonDialog grid rendering).
    float baselineFrac = 0.75f;  // default: 75% from top
    int refLineH = overlayEnrichedAtlas_->referenceLineHeight();
    int refBLBottom = overlayEnrichedAtlas_->referenceBaselineFromBottom();
    if (refLineH > 0 && refBLBottom > 0) {
        baselineFrac = static_cast<float>(refLineH - refBLBottom)
                     / static_cast<float>(refLineH);
    }
    float baselineY = drawTop + bboxH * baselineFrac;

    int fallbackPixelSize = std::max(1, static_cast<int>(bboxH * 0.7f));
    QFont fallbackFont;
    if (!overlayFontOverride_.isEmpty()) {
        fallbackFont = QFont(overlayFontOverride_);
    } else {
        fallbackFont = QFont(PDFE_FAMILY_SANS);
    }
    fallbackFont.setPixelSize(fallbackPixelSize);
    QFontMetricsF fallbackFm(fallbackFont);

    // White background covering both PDF bbox and HOCR bbox
    float totalAdvance = atlasAdvanceForChars(
        span, static_cast<int>(span.text.size()), scaleToScreen, zoom);
    float fillWidth = std::max({
        static_cast<float>(screenBbox.width()),
        static_cast<float>(atlasScreenBbox.width()),
        totalAdvance}) * 1.2f;
    float fillTop = std::min(static_cast<float>(screenBbox.top()),
                             static_cast<float>(atlasScreenBbox.top()));
    float fillBottom = std::max(static_cast<float>(screenBbox.bottom()),
                                static_cast<float>(atlasScreenBbox.bottom()));
    constexpr float kVerticalMargin = 2.0f;
    QRectF fillRect(std::min(screenBbox.left(), atlasScreenBbox.left()),
                    fillTop - kVerticalMargin,
                    static_cast<qreal>(fillWidth),
                    fillBottom - fillTop + 2.0 * kVerticalMargin);
    painter.fillRect(fillRect, Qt::white);

    for (char32_t c : span.text) {
        if (c == U' ') {
            penX += spaceWidth * scaleToScreen;
            continue;
        }

        QImage glyphImg = overlayEnrichedAtlas_->glyph(c);
        if (!glyphImg.isNull()) {
            float scaledW = static_cast<float>(glyphImg.width()) * scaleToScreen;
            float scaledH = static_cast<float>(glyphImg.height()) * scaleToScreen;
            // Baseline-anchored: glyph bottom aligns at baseline + descender.
            // EnrichedAtlas normalizes all glyphs to refLineH with baseline at
            // (refLineH - refBaselineFromBottom). The glyph's bottom edge
            // corresponds to the descender line, so place bottom at bbox bottom.
            float glyphTop = drawTop + bboxH - scaledH;
            // Adjust: anchor glyph bottom at baseline + proportional descender
            // rather than hard bbox bottom, matching the atlas grid rendering.
            if (refLineH > 0 && refBLBottom > 0) {
                float descFrac = static_cast<float>(refBLBottom) / static_cast<float>(refLineH);
                float descSpace = bboxH * descFrac;
                glyphTop = baselineY + descSpace - scaledH;
            }
            QRectF targetRect(penX, glyphTop, scaledW, scaledH);
            painter.drawImage(targetRect, glyphImg);
            if (logChars) {
                qDebug() << "[TextEdit] AtlasChar:"
                         << QString::fromUcs4(&c, 1)
                         << "U+" << QString::number(c, 16)
                         << "imgW=" << glyphImg.width()
                         << "imgH=" << glyphImg.height()
                         << "scaledW=" << scaledW;
            }
            penX += scaledW;
        } else {
            painter.setFont(fallbackFont);
            painter.setPen(Qt::black);
            QString ch = QString::fromUcs4(&c, 1);
            float fontBaselineY = drawTop + bboxH * 0.75f;
            painter.drawText(QPointF(penX, fontBaselineY), ch);
            if (logChars) {
                qDebug() << "[TextEdit] AtlasChar FALLBACK:"
                         << ch << "U+" << QString::number(c, 16);
            }
            penX += static_cast<float>(fallbackFm.horizontalAdvance(ch));
        }
    }
}

/// Reliable mapping from PDF font names to Qt family names.
/// Qt's font alias system is unreliable on Windows (e.g. QFont("Helvetica")
/// silently resolves to a serif font), and Qt6 on Linux ignores fontconfig
/// substitutions for QFontInfo. The PDFE_FAMILY_SANS/SERIF/MONO macros (top
/// of file) pick platform-appropriate names that QFontDatabase actually has.
const QString& mapPdfFontToQtFamily(const QString& strippedName)
{
    // Table maps lowercased, hyphen/space-stripped PDF names → Qt family names.
    // Built once, reused on every paint frame.
    static const std::unordered_map<std::string, QString> mapping = {
        // PDF Standard 14 fonts
        {"helvetica",           PDFE_FAMILY_SANS},
        {"helveticabold",       PDFE_FAMILY_SANS},
        {"helveticaoblique",    PDFE_FAMILY_SANS},
        {"helveticaboldoblique",PDFE_FAMILY_SANS},
        {"arialmt",             PDFE_FAMILY_SANS},
        {"arial",               PDFE_FAMILY_SANS},
        {"timesroman",          PDFE_FAMILY_SERIF},
        {"timesnewromanpsmt",   PDFE_FAMILY_SERIF},
        {"timesnewroman",       PDFE_FAMILY_SERIF},
        {"timesbold",           PDFE_FAMILY_SERIF},
        {"timesitalic",         PDFE_FAMILY_SERIF},
        {"timesbolditalic",     PDFE_FAMILY_SERIF},
        {"courier",             PDFE_FAMILY_MONO},
        {"couriernew",          PDFE_FAMILY_MONO},
        {"couriernewpsmt",      PDFE_FAMILY_MONO},
        {"courierbold",         PDFE_FAMILY_MONO},
        {"courieroblique",      PDFE_FAMILY_MONO},
        {"courierboldoblique",  PDFE_FAMILY_MONO},
        {"symbol",              QStringLiteral("Symbol")},
        {"zapfdingbats",        QStringLiteral("Wingdings")},
        // Common PDF producer fonts
        {"georgia",             QStringLiteral("Georgia")},
        {"verdana",             QStringLiteral("Verdana")},
        {"tahoma",              QStringLiteral("Tahoma")},
        {"trebuchetms",         QStringLiteral("Trebuchet MS")},
        {"calibri",             PDFE_FAMILY_CALIBRI},
        {"cambria",             PDFE_FAMILY_CAMBRIA},
        {"consolas",            QStringLiteral("Consolas")},
        {"palatino",            QStringLiteral("Palatino Linotype")},
        {"palatinolinotype",    QStringLiteral("Palatino Linotype")},
        {"garamond",            QStringLiteral("Garamond")},
        {"bookantiqua",         QStringLiteral("Book Antiqua")},
        {"centurygothic",       QStringLiteral("Century Gothic")},
        {"franklingothic",      QStringLiteral("Franklin Gothic Medium")},
        {"liberationsans",      PDFE_FAMILY_SANS},
        {"liberationserif",     PDFE_FAMILY_SERIF},
        {"liberationmono",      PDFE_FAMILY_MONO},
        {"dejavusans",          PDFE_FAMILY_SANS},
        {"dejavuserif",         PDFE_FAMILY_SERIF},
        {"dejavusansmono",      PDFE_FAMILY_MONO},
        {"nimbussans",          PDFE_FAMILY_SANS},
        {"nimbusroman",         PDFE_FAMILY_SERIF},
        {"nimbusmono",          PDFE_FAMILY_MONO},
        // SIL Open Font Library fonts (used by Adobe-Bridge / O'Reilly / etc.)
        {"charissil",           QStringLiteral("Times New Roman")},
        {"charissilregular",    QStringLiteral("Times New Roman")},
        {"charissilbold",       QStringLiteral("Times New Roman")},
        {"charissilitalic",     QStringLiteral("Times New Roman")},
        {"charissilcompact",    QStringLiteral("Times New Roman")},
        {"doulossil",           QStringLiteral("Times New Roman")},
        {"gentium",             QStringLiteral("Times New Roman")},
        {"gentiumplus",         QStringLiteral("Times New Roman")},
        {"gentiumbasic",        QStringLiteral("Times New Roman")},
        {"andika",              QStringLiteral("Verdana")},
        // OCR / invisible-text fonts → sans-serif
        {"glyphlessfont",       QStringLiteral("Arial")},
        {"invisiblefont",       QStringLiteral("Arial")},
        {"hiddenocrfont",       QStringLiteral("Arial")},
        {"ocrfont",             QStringLiteral("Arial")},
        {"adobeblank",          QStringLiteral("Arial")},
        {"finereaderfont",      QStringLiteral("Arial")},
        {"ocra",                QStringLiteral("Consolas")},
        {"ocrb",                QStringLiteral("Consolas")},
    };

    // Normalize: lowercase, strip hyphens and spaces
    std::string key = strippedName.toLower().toStdString();
    key.erase(std::remove_if(key.begin(), key.end(),
        [](char c) { return c == '-' || c == ' '; }), key.end());

    auto it = mapping.find(key);
    if (it != mapping.end()) {
        return it->second;
    }

    // Partial-match fallback: check if any mapping key is a substring of the
    // normalized name (handles variants like "HelveticaNeue", "ArialNarrow", etc.)
    for (const auto& [pattern, family] : mapping) {
        if (key.find(pattern) != std::string::npos) {
            return family;
        }
    }

    static const QString empty;
    return empty;
}

QFont TextEditTool::buildOverlayFont(const TextSpan& span, float zoom,
                                      const QString& overlayOverride,
                                      float fontSizeOverride,
                                      float visibleExtentRatio,
                                      int profileSerif)
{
    QString fontFamily;

    // When the span has a real embedded font (subset prefix like "REFXWK+Liberation Serif"),
    // trust the span's own fontName and fontSize over FontMatcher's OCR-based detection.
    // FontMatcher can produce significantly wrong values on pages with real embedded fonts.
    bool spanHasRealFont = hasSubsetPrefix(span.fontName) && span.fontSize > 4.0f;

    if (!overlayOverride.isEmpty() && !spanHasRealFont) {
        // FontMatcher-determined override (true OCR pages without real embedded fonts)
        fontFamily = overlayOverride;
    } else {
        // Map PDF font name to a reliable Qt family name
        QString strippedName = QString::fromStdString(span.fontName);
        int plusIdx = strippedName.indexOf('+');
        if (plusIdx >= 0) {
            strippedName = strippedName.mid(plusIdx + 1);
        }

        const QString& mapped = mapPdfFontToQtFamily(strippedName);
        if (!mapped.isEmpty()) {
            fontFamily = mapped;
        } else {
            // Not in our mapping — try using the PDF name directly, but validate
            fontFamily = strippedName;
        }
    }

    // For real embedded fonts, always use span.fontSize (correct from PDF text matrix).
    // FontMatcher's size detection is only useful for true OCR pages.
    float baseSizePt = (fontSizeOverride > 0.0f && !spanHasRealFont)
                           ? fontSizeOverride : span.fontSize;
    // Match the rendered ink height of the substitute font to the rendered
    // ink height of the source (body) font. Both are measured the same way
    // (top-of-tallest-glyph minus bottom-of-deepest-descender, divided by
    // pixelSize), so the result is robust across:
    //   - source fonts with loose metrics (CharisSIL: ink/em ≈ 0.61)
    //   - source fonts with tight metrics (Times-derived: ink/em ≈ 0.85)
    //   - substitute fonts with metric leading (Qt's QFontMetricsF::ascent
    //     for Times reports 1.11N at pixelSize=N, but actual ink only
    //     spans 0.91N — using ascent+descent overshoots).
    //
    // visibleExtentRatio is the SOURCE font's measured ink extent / em
    // (from EmbeddedFontAnalyzer::measureRenderedExtent).
    // The substitute's ink extent is measured here via tightBoundingRect
    // on a representative reference string with full ascenders ('l','b','d')
    // and descenders ('p','q','y','j','g'). Using ascent+descent would
    // include typographic leading and undersize the result.
    float targetExtentPx = baseSizePt * zoom * visibleExtentRatio;
    constexpr int kProbePx = 100;
    QFont probe(fontFamily);
    probe.setPixelSize(kProbePx);
    QFontMetricsF probeFm(probe);
    // tightBoundingRect returns the actual ink-occupied rectangle.
    // Falls back to ascent+descent if the rect is degenerate (e.g.,
    // font missing the reference glyphs entirely).
    QRectF probeInk = probeFm.tightBoundingRect(QStringLiteral("HMlbdfhpqgyEjk"));
    float probeExtent = static_cast<float>(probeInk.height());
    if (probeExtent < 1.0f)
        probeExtent = static_cast<float>(probeFm.ascent() + probeFm.descent());
    float scale = (probeExtent > 1.0f)
        ? (targetExtentPx / probeExtent)
        : (targetExtentPx / static_cast<float>(kProbePx));
    int pixelSize = static_cast<int>(static_cast<float>(kProbePx) * scale + 0.5f);
    if (pixelSize < 1) pixelSize = 1;
    QFont font(fontFamily);
    font.setPixelSize(pixelSize);

    // Apply slant/weight from the source font name so the overlay matches
    // the underlying face. The substitute QFont above is built family+size
    // only; without this the overlay silently drops the source's italic/bold
    // (e.g. a "...-BoldItalic" subset renders upright/regular over slanted,
    // heavier body text — the overlay-vs-underlay style mismatch). Name-parse
    // mirrors AutoMissingGlyphResolver::parseFontStyle, the same derivation the
    // new-character path uses, so the overlay and inserted glyphs agree on
    // style. Set before the override early-return and the serif/sans fallback
    // below; QFont::setFamily preserves bold/italic, so the fallback keeps it.
    {
        // Prefer the name-INDEPENDENT descriptor flags (TextSpan.isItalic/
        // isBold, sourced from /FontDescriptor during extraction). They catch
        // styles the name does not spell out ("-BdIt", numeric weights,
        // non-English). The font-NAME keyword scan is kept only as the fallback
        // when the descriptor is silent (both false — e.g. no /FontDescriptor),
        // preserving the pre-descriptor behaviour for synthetic spans and
        // descriptor-less fonts.
        bool italic = span.isItalic;
        bool bold = span.isBold;
        if (!italic && !bold) {
            QString styleName = QString::fromStdString(span.fontName);
            int stylePlus = styleName.indexOf('+');
            if (stylePlus >= 0) styleName = styleName.mid(stylePlus + 1);
            const QString styleLower = styleName.toLower();
            if (styleLower.contains("bold")) bold = true;
            if (styleLower.contains("italic") || styleLower.contains("oblique"))
                italic = true;
        }
        if (bold) font.setBold(true);
        if (italic) font.setItalic(true);
    }

    qDebug() << "[TextEdit] buildOverlayFont:"
             << "family=" << fontFamily
             << "span.fontName=" << QString::fromStdString(span.fontName)
             << "span.fontSize=" << span.fontSize
             << "fontSizeOverride=" << fontSizeOverride
             << "spanHasRealFont=" << spanHasRealFont
             << "baseSizePt=" << baseSizePt
             << "zoom=" << zoom
             << "visibleExtentRatio=" << visibleExtentRatio
             << "targetExtentPx=" << targetExtentPx
             << "probeExtent=" << probeExtent
             << "scale=" << scale
             << "pixelSize=" << pixelSize
             << "bold=" << font.bold()
             << "italic=" << font.italic()
             << "overlayOverride=" << overlayOverride;

    // Validate: QFontInfo tells us what Qt actually resolved to.
    // If the resolved family doesn't match what we requested, the font
    // isn't really available — fall back to Arial.
    if (!overlayOverride.isEmpty()) {
        // Trust the override (came from FontMatcher)
        return font;
    }

    QFontInfo fi(font);
    QString resolvedFamily = fi.family();

    // Check for misresolution: we asked for a sans-serif but got a serif,
    // or the resolved name doesn't even contain what we asked for.
    // Common misresolution: "Helvetica" → "Times New Roman" on Windows.
    bool looksWrong = false;

    // If we have a mapped family (from our table), verify the resolution matches
    QString strippedName = QString::fromStdString(span.fontName);
    int plusIdx = strippedName.indexOf('+');
    if (plusIdx >= 0) strippedName = strippedName.mid(plusIdx + 1);
    const QString& mapped = mapPdfFontToQtFamily(strippedName);

    if (!mapped.isEmpty()) {
        // We explicitly mapped this font — verify Qt resolved to the right family
        if (!resolvedFamily.contains(mapped, Qt::CaseInsensitive) &&
            !mapped.contains(resolvedFamily, Qt::CaseInsensitive)) {
            looksWrong = true;
        }
    } else {
        // Unknown font — check if Qt silently substituted something unexpected.
        // If the resolved name doesn't resemble what we asked for, it's a miss.
        if (!resolvedFamily.contains(fontFamily, Qt::CaseInsensitive) &&
            !fontFamily.contains(resolvedFamily, Qt::CaseInsensitive)) {
            looksWrong = true;
        }
    }

    if (looksWrong) {
        // Prefer a serif fallback when the source font is serif, so a serif
        // body (e.g. Palatino) does not get a sans (Arial) overlay.
        //
        // FIRST consult the EmbeddedFontAnalyzer FontMetricProfile serif
        // classification supplied by the caller (profileSerif: PANOSE byte 1 /
        // measured serif extent — name-INDEPENDENT, so it catches serif fonts
        // whose name carries no recognisable keyword and rejects sans fonts
        // whose name happens to contain one). The font-name fragment scan below
        // is the FALLBACK, used only when no profile is available (profileSerif
        // == -1) — e.g. system fonts or image-replaced text the analyzer never
        // saw. (pipeline d4e7f921 phase 3)
        bool looksSerif;
        if (profileSerif >= 0) {
            looksSerif = (profileSerif == 1);
        } else {
            // Name-fragment heuristic. Common serif fragments: "Serif",
            // "Times", "Roman", "Garamond", "Caslon", "Baskerville",
            // "Palatino", "Georgia", "Cambria", "Charis", "Doulos", "Gentium".
            QString lower = QString::fromStdString(span.fontName).toLower();
            looksSerif =
                lower.contains("serif") || lower.contains("times") ||
                lower.contains("roman") || lower.contains("garamond") ||
                lower.contains("caslon") || lower.contains("baskerville") ||
                lower.contains("palatino") || lower.contains("georgia") ||
                lower.contains("cambria") || lower.contains("charis") ||
                lower.contains("doulos") || lower.contains("gentium") ||
                lower.contains("minion") || lower.contains("goudy") ||
                lower.contains("century");
        }
        font.setFamily(looksSerif ? PDFE_FAMILY_SERIF : PDFE_FAMILY_SANS);
    }

    return font;
}

const QScreen* TextEditTool::screenForWidget(const QWidget* widget)
{
    // WIN-33: resolve through the widget's own top-level window so a window
    // living on a secondary monitor with different scaling is sized for THAT
    // monitor. Mirrors PdfCanvas::updateDevicePixelRatio().
    if (!widget) return nullptr;
    const QWidget* win = widget->window();
    if (!win) return nullptr;
    const QWindow* handle = win->windowHandle();
    if (!handle) return nullptr;
    return handle->screen();
}

float TextEditTool::devicePixelRatioForScreen(const QScreen* screen)
{
    float dpr = screen ? static_cast<float>(screen->devicePixelRatio()) : 1.0f;
    if (dpr < 0.1f) dpr = 1.0f;   // degenerate / unset ratio
    return dpr;
}

float TextEditTool::devicePixelRatioForWidget(const QWidget* widget)
{
    if (const QScreen* screen = screenForWidget(widget)) {
        return devicePixelRatioForScreen(screen);
    }
    // No window handle yet (widget never shown, or no widget context at all):
    // the primary screen is the only thing available. Same shape as
    // StickyNotePopup::mouseMoveEvent — primary is the FALLBACK, never the
    // unconditional answer.
    return devicePixelRatioForScreen(QGuiApplication::primaryScreen());
}

int TextEditTool::overlayPixelSizeForDpr(float sizePt, float zoom, float dpr)
{
    if (dpr < 0.1f) dpr = 1.0f;
    int pixelSize = static_cast<int>(sizePt * zoom / dpr + 0.5f);
    if (pixelSize < 1) pixelSize = 1;
    return pixelSize;
}

QFont TextEditTool::buildRunFont(const FormattingRun& run, const TextSpan& span,
                                  float zoom, const QString& overlayOverride,
                                  float fontSizeOverride,
                                  const QWidget* contextWidget)
{
    QString fontFamily;

    // Same subset prefix check as buildOverlayFont — trust real embedded fonts
    bool spanHasRealFont = hasSubsetPrefix(span.fontName) && span.fontSize > 4.0f;

    if (!run.fontFamily.empty()) {
        // Run specifies an explicit Qt font family
        fontFamily = QString::fromStdString(run.fontFamily);
    } else if (!overlayOverride.isEmpty() && !spanHasRealFont) {
        fontFamily = overlayOverride;
    } else {
        // Fall back to span's default font mapping
        QString strippedName = QString::fromStdString(span.fontName);
        int plusIdx = strippedName.indexOf('+');
        if (plusIdx >= 0) strippedName = strippedName.mid(plusIdx + 1);
        const QString& mapped = mapPdfFontToQtFamily(strippedName);
        fontFamily = mapped.isEmpty() ? strippedName : mapped;
    }

    float baseFontSize = (fontSizeOverride > 0.0f && !spanHasRealFont)
                              ? fontSizeOverride : span.fontSize;
    float size = (run.fontSize > 0.0f) ? run.fontSize : baseFontSize;
    // See buildOverlayFont for the DPR compensation rationale.
    // WIN-33: the ratio comes from the screen the CONTEXT WIDGET's window is
    // on, not from the primary screen — on a mixed-scaling multi-monitor
    // desktop the overlay used to be sized for the primary monitor no matter
    // which display the window had been dragged to.
    const float dpr = devicePixelRatioForWidget(contextWidget);
    const int pixelSize = overlayPixelSizeForDpr(size, zoom, dpr);
    QFont font(fontFamily);
    font.setPixelSize(pixelSize);
    font.setBold(run.bold);
    font.setItalic(run.italic);
    font.setUnderline(run.underline);
    font.setStrikeOut(run.strikethrough);

    qDebug() << "[TextEdit] buildRunFont:"
             << "family=" << fontFamily
             << "run.fontFamily=" << QString::fromStdString(run.fontFamily)
             << "run.fontSize=" << run.fontSize
             << "span.fontSize=" << span.fontSize
             << "fontSizeOverride=" << fontSizeOverride
             << "baseFontSize=" << baseFontSize
             << "size=" << size
             << "pixelSize=" << pixelSize
             << "zoom=" << zoom
             << "bold=" << run.bold << "italic=" << run.italic;

    return font;
}

TextEditTool::TextEditTool(
    pdfe::core::Document* document,
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    QObject* parent)
    : QObject(parent)
    , document_(document)
    , context_(std::move(context))
    , fontResolver_(std::make_unique<pdfe::fonts::FontFamilyResolver>())
    , fontAnalyzer_(std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get()))
{
    blinkTimer_ = new QTimer(this);
    blinkTimer_->setInterval(500);
    connect(blinkTimer_, &QTimer::timeout, this, [this]() {
        cursorVisible_ = !cursorVisible_;
        emit repaintRequested();
    });
}

TextEditTool::~TextEditTool()
{
    if (isEditing()) {
        cancelEdit();
    }
}

void TextEditTool::activate()
{
    activeSpanIndex_ = -1;
    cursorPosition_ = 0;
    selectionStart_ = -1;
    selectionEnd_ = -1;
}

void TextEditTool::deactivate()
{
    if (isEditing()) {
        commitEdit();
    }
    model_.reset();
    activeSpanIndex_ = -1;
    currentPageIndex_ = -1;
    preeditText_.clear();
    preeditCursor_ = 0;
    lastCaretScreenRectValid_ = false;
    // Drop per-document font analysis when the tool deactivates so a
    // subsequent activation on a different document starts fresh.
    visibleExtentCache_.clear();
    ascenderRatioCache_.clear();
    serifCache_.clear();
    analyzedPages_.clear();
}

void TextEditTool::setPage(int pageIndex)
{
    if (pageIndex == currentPageIndex_ && model_)
        return;

    if (isEditing()) {
        commitEdit();
    }

    currentPageIndex_ = pageIndex;
    rebuildModel();
}

void TextEditTool::rebuildModel()
{
    model_.reset();
    if (currentPageIndex_ < 0 || !document_)
        return;

    auto page = document_->page(currentPageIndex_);
    if (!page)
        return;

    auto blocks = page->extractText();
    model_ = std::make_unique<TextEditModel>(blocks);

    // Pre-warm the visible-extent cache so the first paint after page
    // entry doesn't pay the EmbeddedFontAnalyzer cost while the user
    // is already typing.
    warmFontCacheForPage(currentPageIndex_);
}

void TextEditTool::warmFontCacheForPage(int pageIndex)
{
    if (pageIndex < 0 || !document_ || !fontAnalyzer_)
        return;
    if (analyzedPages_.count(pageIndex))
        return;

    auto page = document_->page(pageIndex);
    if (!page)
        return;

    auto profiles = fontAnalyzer_->analyzePageFonts(page.get(), document_);
    for (const auto& p : profiles) {
        if (p.postScriptName.empty())
            continue;

        // Prefer the actually-rendered visible extent over OS/2 declarations.
        // Many fonts (CharisSIL, Doulos SIL, Gentium, etc.) declare large
        // ascender + descender to reserve space for stacked diacritics, but
        // their rendered Latin glyphs only span ~60% of em. The rendered
        // measurement is the truth — OS/2 just shapes the line-box.
        float ratio = p.renderedVisibleExtent;
        const char* source = "rendered";

        if (ratio <= 0.0f) {
            // Fallback: OS/2 sCapHeight + |descender|, with defensive
            // defaults for fonts that don't expose sCapHeight.
            float capH = p.capHeight;
            float desc = std::abs(p.descender);
            if (capH <= 0.0f || capH > 1.0f) capH = 0.7f;
            if (desc <= 0.0f || desc > 0.5f) desc = 0.18f;
            ratio = capH + desc;
            source = "OS2";
        }

        // Clamp. 0.5 is a hard floor (any font tighter than that would
        // render essentially as a thin band — likely a measurement
        // artifact); 1.0 is the metric em ceiling.
        if (ratio < 0.5f) ratio = 0.5f;
        if (ratio > 1.0f) ratio = 1.0f;
        visibleExtentCache_[p.postScriptName] = ratio;

        // Cache the baseline-from-bbox-top ratio. MuPDF positions text
        // using the font's metric ascender (the declared distance from
        // the line bbox top to the baseline), NOT the actual cap-top of
        // rendered glyphs. For CharisSIL the metric ascender is 0.73 em
        // even though caps only reach 0.46 em above baseline — the
        // 0.27 em above caps is unused space the font reserves for
        // diacritics. The body bbox includes that space, so the baseline
        // sits at 0.73 × bbox.height down from bbox.top.
        //
        // We need to position the overlay's baseline at the SAME absolute
        // y as MuPDF's body baseline, so use the source font's declared
        // ascender. Fall back through capHeight + a typical descent
        // estimate, then to a generic Latin default if nothing's known.
        float ascRatio = p.ascender;
        if (ascRatio <= 0.0f) {
            // ascender wasn't populated; fall back to a synthetic
            // ascender = capHeight + ~10% above caps.
            ascRatio = p.capHeight > 0.0f ? (p.capHeight + 0.10f) : 0.75f;
        }
        // Sanity clamps. Real-world ascender ratios fall in [0.65, 0.95].
        if (ascRatio < 0.55f) ascRatio = 0.55f;
        if (ascRatio > 0.98f) ascRatio = 0.98f;
        ascenderRatioCache_[p.postScriptName] = ascRatio;

        // Cache the serif classification (PANOSE byte 1 / measured serif
        // extent, decided in EmbeddedFontAnalyzer::analyzeOwnedFace) so the
        // overlay's serif/sans fallback can prefer the real classification over
        // the font-name heuristic. (pipeline d4e7f921 phase 3)
        serifCache_[p.postScriptName] = p.isSerif ? 1 : 0;

        qDebug() << "[TextEdit] visibleExtent cached:"
                 << "name=" << QString::fromStdString(p.postScriptName)
                 << "rendered=" << p.renderedVisibleExtent
                 << "renderedMaxTop=" << p.renderedMaxTop
                 << "renderedMinBottom=" << p.renderedMinBottom
                 << "capH=" << p.capHeight << "desc=" << p.descender
                 << "ratio=" << ratio << "ascRatio=" << ascRatio
                 << "source=" << source;
    }
    analyzedPages_.insert(pageIndex);
}

float TextEditTool::visibleExtentRatioForFont(const std::string& fontName) const
{
    // Default: typical Latin metrics. Used as fallback for system fonts,
    // image-replaced text, or fonts whose OS/2 table couldn't be read.
    constexpr float kDefaultRatio = 0.85f;

    if (fontName.empty()) return kDefaultRatio;

    // Direct match (PDF /BaseFont name == EmbeddedFontAnalyzer postScriptName)
    auto it = visibleExtentCache_.find(fontName);
    if (it != visibleExtentCache_.end()) return it->second;

    // Subset prefix stripped match (e.g., span = "REFXWK+CharisSILRegular",
    // analyzer returns "CharisSILRegular"). PDF subset prefixes are 6 caps
    // followed by '+' followed by the actual postScript name.
    auto plus = fontName.find('+');
    if (plus != std::string::npos && plus + 1 < fontName.size()) {
        auto stripped = fontName.substr(plus + 1);
        auto it2 = visibleExtentCache_.find(stripped);
        if (it2 != visibleExtentCache_.end()) return it2->second;
    }

    // Reverse: span has plain name, analyzer returned subset-prefixed.
    // Iterate cache once to find a postfix match.
    for (const auto& [name, ratio] : visibleExtentCache_) {
        auto p = name.find('+');
        if (p != std::string::npos && p + 1 < name.size()
            && name.substr(p + 1) == fontName) {
            return ratio;
        }
    }

    return kDefaultRatio;
}

float TextEditTool::ascenderRatioForFont(const std::string& fontName) const
{
    // Default: typical Latin metric ascender at ~75% of em. This is the
    // baseline-from-bbox-top ratio, not the visible cap-top — see
    // warmFontCacheForPage for the distinction. Applies when no
    // measurement is available (system fonts, image-replaced text).
    constexpr float kDefaultRatio = 0.75f;

    if (fontName.empty()) return kDefaultRatio;

    auto it = ascenderRatioCache_.find(fontName);
    if (it != ascenderRatioCache_.end()) return it->second;

    auto plus = fontName.find('+');
    if (plus != std::string::npos && plus + 1 < fontName.size()) {
        auto stripped = fontName.substr(plus + 1);
        auto it2 = ascenderRatioCache_.find(stripped);
        if (it2 != ascenderRatioCache_.end()) return it2->second;
    }

    for (const auto& [name, ratio] : ascenderRatioCache_) {
        auto p = name.find('+');
        if (p != std::string::npos && p + 1 < name.size()
            && name.substr(p + 1) == fontName) {
            return ratio;
        }
    }

    // No measured profile for this font. Before the generic default, consult
    // the per-family table — for common system fonts a representative
    // hhea-ascender/em is far closer than the blind 0.75. (phase 3)
    const float tabled = tabledAscenderRatio(fontName);
    if (tabled > 0.0f)
        return tabled;

    return kDefaultRatio;
}

float TextEditTool::tabledAscenderRatio(const std::string& fontName)
{
    if (fontName.empty()) return -1.0f;

    // Lowercase + strip a 6-char subset prefix ("ABCDEF+") so embedded subset
    // names still match the family fragment.
    std::string lower = fontName;
    auto plus = lower.find('+');
    if (plus != std::string::npos && plus + 1 < lower.size())
        lower = lower.substr(plus + 1);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Representative hhea-ascender / unitsPerEm for well-known families. These
    // mirror the value the profile path caches (face->ascender / units_per_EM),
    // clamped to the same [0.55, 0.98] sanity band used in warmFontCacheForPage.
    // Order matters: more-specific fragments first. Values are approximate (the
    // exact figure varies by font version), so they are a heuristic improvement
    // on 0.75, NOT a precise metric — the real measured profile always wins
    // above this tier.
    struct Entry { const char* frag; float ratio; };
    static const Entry kTable[] = {
        // Sans-serif
        {"liberationsans",  0.905f},
        {"liberation sans", 0.905f},
        {"helvetica",       0.905f},
        {"arial",           0.905f},
        {"verdana",         0.905f},
        {"tahoma",          0.905f},
        {"segoe",           0.905f},
        {"calibri",         0.952f},
        {"dejavusans",      0.928f},
        {"dejavu sans",     0.928f},
        // Serif
        {"liberationserif", 0.891f},
        {"liberation serif",0.891f},
        {"times new roman", 0.891f},
        {"timesnewroman",   0.891f},
        {"times",           0.891f},
        {"nimbus roman",    0.891f},
        {"nimbusroman",     0.891f},
        {"tinos",           0.891f},
        {"georgia",         0.917f},
        {"cambria",         0.950f},
        {"garamond",        0.800f},
        {"palatino",        0.800f},
        {"palladio",        0.800f},
        {"book antiqua",    0.800f},
        {"minion",          0.800f},
        // Monospace
        {"courier",         0.833f},
        {"consolas",        0.879f},
    };
    for (const auto& e : kTable) {
        if (lower.find(e.frag) != std::string::npos) {
            float r = e.ratio;
            if (r < 0.55f) r = 0.55f;
            if (r > 0.98f) r = 0.98f;
            return r;
        }
    }
    return -1.0f;
}

int TextEditTool::serifClassForFont(const std::string& fontName) const
{
    if (fontName.empty()) return -1;

    auto it = serifCache_.find(fontName);
    if (it != serifCache_.end()) return it->second;

    auto plus = fontName.find('+');
    if (plus != std::string::npos && plus + 1 < fontName.size()) {
        auto stripped = fontName.substr(plus + 1);
        auto it2 = serifCache_.find(stripped);
        if (it2 != serifCache_.end()) return it2->second;
    }

    for (const auto& [name, cls] : serifCache_) {
        auto p = name.find('+');
        if (p != std::string::npos && p + 1 < name.size()
            && name.substr(p + 1) == fontName) {
            return cls;
        }
    }

    return -1;
}

void TextEditTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Right-click: emit context menu signal (MainWindow handles the menu)
    if (event && event->button() == Qt::RightButton) {
        emit contextMenuRequested(event->globalPosition().toPoint());
        return;
    }

    if (pageIndex != currentPageIndex_) {
        setPage(pageIndex);
    }

    if (!model_)
        return;

    int spanIndex = model_->spanAtPoint(pdfPoint);

    if (spanIndex < 0) {
        // Clicked outside any text -- commit current edit if active
        if (isEditing()) {
            commitEdit();
        }
        return;
    }

    if (spanIndex != activeSpanIndex_) {
        // Switching to a different span -- commit old edit
        if (isEditing()) {
            commitEdit();
        }
    }

    selectSpan(spanIndex);

    // Use font-metrics-based hit testing for modified spans (charQuads are stale)
    const auto& clickedSpan = model_->span(spanIndex);
    if (clickedSpan.isModified()) {
        cursorPosition_ = charIndexFromPdfX(clickedSpan, pdfPoint.x());
    } else {
        cursorPosition_ = model_->charIndexAtPoint(spanIndex, pdfPoint);
    }
    selectionStart_ = -1;
    selectionEnd_ = -1;
    emitSelectionFormatting();
}

void TextEditTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    (void)event;
    (void)pageIndex;

    // Drag to extend selection
    if (isEditing() && (event->buttons() & Qt::LeftButton)) {
        const auto& dragSpan = model_->span(activeSpanIndex_);
        int charIdx = dragSpan.isModified()
            ? charIndexFromPdfX(dragSpan, pdfPoint.x())
            : model_->charIndexAtPoint(activeSpanIndex_, pdfPoint);
        selectionEnd_ = charIdx;
        if (selectionStart_ < 0) {
            selectionStart_ = cursorPosition_;
        }
        cursorPosition_ = charIdx;
    }
}

void TextEditTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    (void)event;
    (void)pageIndex;
    (void)pdfPoint;
}

void TextEditTool::onMouseDoubleClick(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    (void)event;

    if (pageIndex != currentPageIndex_) {
        setPage(pageIndex);
    }

    if (!model_)
        return;

    int spanIndex = model_->spanAtPoint(pdfPoint);
    if (spanIndex < 0)
        return;

    selectSpan(spanIndex);

    // Select word at cursor position
    const auto& span = model_->span(spanIndex);
    int charIdx = span.isModified()
        ? charIndexFromPdfX(span, pdfPoint.x())
        : model_->charIndexAtPoint(spanIndex, pdfPoint);

    // Find word boundaries
    int textLen = static_cast<int>(span.text.size());
    int wordStart = std::min(charIdx, textLen - 1);
    int wordEnd = wordStart;

    if (wordStart >= 0 && wordStart < textLen) {
        // Expand left to word start
        while (wordStart > 0 && !std::iswspace(static_cast<wint_t>(span.text[wordStart - 1]))) {
            --wordStart;
        }
        // Expand right to word end
        while (wordEnd < textLen && !std::iswspace(static_cast<wint_t>(span.text[wordEnd]))) {
            ++wordEnd;
        }
    }

    selectionStart_ = wordStart;
    selectionEnd_ = wordEnd;
    cursorPosition_ = wordEnd;
}

void TextEditTool::onKeyPress(QKeyEvent* event)
{
    // Up/Down are ignored unconditionally — TextEditTool has no vertical
    // cursor navigation (spans are single-line).  Ignoring here regardless of
    // isEditing() ensures PdfCanvas::keyPressEvent always falls through to the
    // canvas scroll handler for these keys, even when no span is being edited
    // (e.g. tool installed but span scrolled off-screen between key presses).
    // Without this guard the early `return` below (for !isEditing()) left the
    // event as accepted=true (Qt default), silently swallowing the key and
    // preventing canvas scroll.  Pipeline a7f3d2c9 phase-2 (GAP-23 fix).
    if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
        event->ignore();
        return;
    }

    if (!isEditing() || !model_)
        return;

    // Ctrl shortcuts
    if (event->modifiers() & Qt::ControlModifier) {
        switch (event->key()) {
        case Qt::Key_C:
            copySelection();   // self-gates via copyDenied() (live perm + toggle)
            event->accept();
            return;
        case Qt::Key_X:
            cutSelection();    // self-gates via copyDenied()
            event->accept();
            return;
        case Qt::Key_V: pasteAtCursor(); event->accept(); return;
        case Qt::Key_A: selectAll(); event->accept(); return;
        default: break;
        }
    }

    resetCursorBlink();

    // Pipeline b3f19d72 phase-3 (P7-BUG-1): snapshot span text before
    // the switch so we can detect which keys mutated the model and
    // push an incremental edit immediately afterwards.  This keeps
    // UndoManager / undoAction's enabled state in sync with typed
    // input on a per-character basis (mergeId collapses the resulting
    // commands into one undo step for consecutive characters on the
    // same span — see EditTextCommand::mergeWith).
    const std::u32string textBeforeKey =
        (activeSpanIndex_ >= 0 && model_)
            ? model_->span(activeSpanIndex_).text
            : std::u32string{};

    switch (event->key()) {
    case Qt::Key_Escape:
        cancelEdit();
        break;

    case Qt::Key_Return:
    case Qt::Key_Enter:
        commitEdit();
        break;

    case Qt::Key_Backspace:
        if (selectionStart_ >= 0 && selectionStart_ != selectionEnd_) {
            // Delete selection
            int start = std::min(selectionStart_, selectionEnd_);
            int end = std::max(selectionStart_, selectionEnd_);
            model_->replaceCharRange(activeSpanIndex_, start, end, U"");
            cursorPosition_ = start;
            selectionStart_ = -1;
            selectionEnd_ = -1;
        } else {
            deleteCharAtCursor(false);
        }
        break;

    case Qt::Key_Delete:
        if (selectionStart_ >= 0 && selectionStart_ != selectionEnd_) {
            int start = std::min(selectionStart_, selectionEnd_);
            int end = std::max(selectionStart_, selectionEnd_);
            model_->replaceCharRange(activeSpanIndex_, start, end, U"");
            cursorPosition_ = start;
            selectionStart_ = -1;
            selectionEnd_ = -1;
        } else {
            deleteCharAtCursor(true);
        }
        break;

    case Qt::Key_Left:
        // Anchor the selection start at the current cursor position on the
        // first Shift+arrow keystroke.  Without this anchor, selectionStart_
        // stays -1 and no selection is ever painted (the canvas paint path
        // requires selectionStart_ >= 0 && selectionStart_ != selectionEnd_).
        if ((event->modifiers() & Qt::ShiftModifier) && selectionStart_ < 0) {
            selectionStart_ = cursorPosition_;
        }
        if (cursorPosition_ > 0) {
            --cursorPosition_;
        }
        if (!(event->modifiers() & Qt::ShiftModifier)) {
            selectionStart_ = -1;
            selectionEnd_ = -1;
        } else {
            selectionEnd_ = cursorPosition_;
        }
        break;

    case Qt::Key_Right: {
        // Anchor selection start before moving (same rationale as Key_Left).
        if ((event->modifiers() & Qt::ShiftModifier) && selectionStart_ < 0) {
            selectionStart_ = cursorPosition_;
        }
        const auto& span = model_->span(activeSpanIndex_);
        if (cursorPosition_ < static_cast<int>(span.text.size())) {
            ++cursorPosition_;
        }
        if (!(event->modifiers() & Qt::ShiftModifier)) {
            selectionStart_ = -1;
            selectionEnd_ = -1;
        } else {
            selectionEnd_ = cursorPosition_;
        }
        break;
    }

    case Qt::Key_Home:
        // Anchor selection start before jumping to column 0.
        if ((event->modifiers() & Qt::ShiftModifier) && selectionStart_ < 0) {
            selectionStart_ = cursorPosition_;
        }
        cursorPosition_ = 0;
        if (!(event->modifiers() & Qt::ShiftModifier)) {
            selectionStart_ = -1;
            selectionEnd_ = -1;
        } else {
            selectionEnd_ = cursorPosition_;
        }
        break;

    case Qt::Key_End: {
        // Anchor selection start before jumping to end of span.
        if ((event->modifiers() & Qt::ShiftModifier) && selectionStart_ < 0) {
            selectionStart_ = cursorPosition_;
        }
        const auto& span = model_->span(activeSpanIndex_);
        cursorPosition_ = static_cast<int>(span.text.size());
        if (!(event->modifiers() & Qt::ShiftModifier)) {
            selectionStart_ = -1;
            selectionEnd_ = -1;
        } else {
            selectionEnd_ = cursorPosition_;
        }
        break;
    }

    default:
        // Regular text input
        if (!event->text().isEmpty()) {
            onTextInput(event->text());
        }
        break;
    }

    // Pipeline b3f19d72 phase-3 (P7-BUG-1): if the span text changed
    // as a result of this key (and we're still editing the same span —
    // i.e. it wasn't Escape/Enter, which reset activeSpanIndex_), push
    // the change to UndoManager via the textEdited callback so the
    // undoAction enabled state and accessible-state-change notification
    // fire immediately.  Consecutive characters merge into one undo
    // step (EditTextCommand::mergeWith).
    if (activeSpanIndex_ >= 0 && model_) {
        const auto& spanAfter = model_->span(activeSpanIndex_);
        if (spanAfter.text != textBeforeKey) {
            pushIncrementalEdit();
        }
    }

    emitSelectionFormatting();
    event->accept();
}

void TextEditTool::onTextInput(const QString& text)
{
    if (!isEditing() || !model_ || text.isEmpty())
        return;

    resetCursorBlink();

    const auto ucs4 = text.toUcs4();
    std::u32string u32Text(ucs4.begin(), ucs4.end());

    // Replace selection if active
    if (selectionStart_ >= 0 && selectionStart_ != selectionEnd_) {
        int start = std::min(selectionStart_, selectionEnd_);
        int end = std::max(selectionStart_, selectionEnd_);
        model_->replaceCharRange(activeSpanIndex_, start, end, u32Text);
        cursorPosition_ = start + static_cast<int>(u32Text.size());
        selectionStart_ = -1;
        selectionEnd_ = -1;
    } else {
        insertTextAtCursor(u32Text);
    }
}

// ─── Input-method (IME) support — WIN-49, follow-up phase-76 ───────────────

void TextEditTool::setPreeditText(const std::u32string& text, int cursorInPreedit)
{
    if (!isEditing() || !model_) {
        // Not editing: there is nothing to compose into. Drop any stale
        // composition rather than leaving it to be painted over no caret.
        preeditText_.clear();
        preeditCursor_ = 0;
        return;
    }

    // NOTE: model_ is deliberately NOT touched here. See TextEditTool.h.
    preeditText_ = text;
    preeditCursor_ = std::clamp(cursorInPreedit, 0,
                                static_cast<int>(preeditText_.size()));
    resetCursorBlink();
    emit repaintRequested();
}

void TextEditTool::clearPreedit()
{
    if (preeditText_.empty() && preeditCursor_ == 0)
        return;
    preeditText_.clear();
    preeditCursor_ = 0;
    emit repaintRequested();
}

void TextEditTool::commitPreedit(const std::u32string& text,
                                 int replaceFrom, int replaceLength)
{
    if (!isEditing() || !model_)
        return;

    // The composition is over either way — clear it before mutating so a
    // repaint triggered from inside the edit path cannot draw a stale preedit.
    preeditText_.clear();
    preeditCursor_ = 0;

    // Same snapshot/compare shape as onKeyPress: an incremental edit is pushed
    // only when the span text actually changed, so an empty commit with no
    // replacement is a no-op on the UndoManager.
    const std::u32string textBefore = model_->span(activeSpanIndex_).text;

    if (replaceLength > 0) {
        const int len = static_cast<int>(textBefore.size());
        // Qt's replaceFrom is relative to the current cursor and may be
        // negative (the IME asking to replace text BEFORE the caret).
        const int start = std::clamp(cursorPosition_ + replaceFrom, 0, len);
        const int end = std::clamp(start + replaceLength, start, len);
        if (end > start) {
            model_->replaceCharRange(activeSpanIndex_, start, end, U"");
            cursorPosition_ = start;
            // A replacement supersedes any selection; leaving the old range
            // would make the insert below delete text the IME never named.
            selectionStart_ = -1;
            selectionEnd_ = -1;
        }
    }

    if (!text.empty()) {
        // Route through the ordinary text-input path so selection replacement,
        // cursor advance and blink reset behave exactly as for typed input.
        onTextInput(QString::fromUcs4(
            reinterpret_cast<const char32_t*>(text.data()),
            static_cast<int>(text.size())));
    }

    if (activeSpanIndex_ >= 0 && model_ &&
        model_->span(activeSpanIndex_).text != textBefore) {
        pushIncrementalEdit();
    }

    resetCursorBlink();
    emitSelectionFormatting();
    emit repaintRequested();
}

std::u32string TextEditTool::surroundingText() const
{
    if (!isEditing() || !model_)
        return {};
    return model_->span(activeSpanIndex_).text;
}

int TextEditTool::cursorPositionForIme() const
{
    if (!isEditing() || !model_)
        return 0;
    const int len = static_cast<int>(model_->span(activeSpanIndex_).text.size());
    return std::clamp(cursorPosition_, 0, len);
}

bool TextEditTool::caretScreenRect(QRectF& out) const
{
    if (!lastCaretScreenRectValid_)
        return false;
    out = lastCaretScreenRect_;
    return true;
}

void TextEditTool::onPaintOverlay(
    QPainter& painter, int pageIndex,
    float zoom, QPointF pageOrigin, float pageHeightPt)
{
    if (!isEditing() || pageIndex != currentPageIndex_ || !model_)
        return;

    const auto& span = model_->span(activeSpanIndex_);

    // WIN-33: the widget being painted supplies the monitor the overlay must
    // be sized for. QPaintDevice is not a QObject, so use Qt's own device-type
    // discriminator rather than a dynamic_cast. nullptr when the painter is
    // targeting an image/printer, in which case buildRunFont falls back.
    const QWidget* paintWidget =
        (painter.device() && painter.device()->devType() == QInternal::Widget)
            ? static_cast<const QWidget*>(painter.device())
            : nullptr;

    // Convert fitz-space bbox to screen coordinates (both Y-down, no flip needed)
    auto pdfToScreen = [&](const QRectF& pdfRect) -> QRectF {
        float x = static_cast<float>(pageOrigin.x()) + static_cast<float>(pdfRect.left()) * zoom;
        float y = static_cast<float>(pageOrigin.y()) + static_cast<float>(pdfRect.top()) * zoom;
        float w = static_cast<float>(pdfRect.width()) * zoom;
        float h = static_cast<float>(pdfRect.height()) * zoom;
        return QRectF(x, y, w, h);
    };

    static int paintLogCount = 0;
    if (paintLogCount++ % 60 == 0) { // Log every ~1 second at 60fps
        // Diagnose painter device characteristics. The overlay size
        // mismatch implies Qt is interpreting setPixelSize differently
        // than the canvas tile rendering.
        QPaintDevice* dev = painter.device();
        int devLogicalDpiX = dev ? dev->logicalDpiX() : 0;
        int devLogicalDpiY = dev ? dev->logicalDpiY() : 0;
        int devPhysicalDpiX = dev ? dev->physicalDpiX() : 0;
        qreal devDpr = dev ? dev->devicePixelRatioF() : 1.0;
        qDebug() << "[TextEdit] onPaintOverlay:"
                 << "zoom=" << zoom
                 << "span.bbox=" << span.boundingBox
                 << "span.fontSize=" << span.fontSize
                 << "span.fontName=" << QString::fromStdString(span.fontName)
                 << "isModified=" << span.isModified()
                 << "formattingRuns=" << span.formattingRuns.size()
                 << "overlayFontOverride=" << overlayFontOverride_
                 << "overlayFontSizeOverride=" << overlayFontSizePtOverride_
                 << "painter.device.logicalDpiX=" << devLogicalDpiX
                 << "painter.device.logicalDpiY=" << devLogicalDpiY
                 << "painter.device.physicalDpiX=" << devPhysicalDpiX
                 << "painter.device.devicePixelRatioF=" << devDpr;
        // Probe what QFontMetrics actually computes for the overlay font
        QFont probeFont = buildOverlayFont(span, zoom, overlayFontOverride_, overlayFontSizePtOverride_, visibleExtentRatioForFont(span.fontName), serifClassForFont(span.fontName));
        QFontMetricsF probeFm(probeFont, dev);
        qDebug() << "[TextEdit] onPaintOverlay font metrics:"
                 << "pixelSize_set=" << probeFont.pixelSize()
                 << "pointSize_resolved=" << probeFont.pointSize()
                 << "fm.height=" << probeFm.height()
                 << "fm.ascent=" << probeFm.ascent()
                 << "fm.descent=" << probeFm.descent()
                 << "fm.capHeight=" << probeFm.capHeight()
                 << "fm.xHeight=" << probeFm.xHeight()
                 << "overlayWordStyles=" << overlayWordStyles_.size();
    }

    // Draw span highlight
    QRectF screenBbox = pdfToScreen(span.boundingBox);

    // HOCR-adjusted bbox for atlas-mode rendering on scanned pages.
    // The PDF Tr=3 text layer is often vertically offset from the actual
    // scan text position. Compute an adjusted bbox that matches the HOCR line.
    QRectF atlasScreenBbox = screenBbox;  // default: same as PDF bbox
    if (overlayEnrichedAtlas_ && !ocrLineBboxesPdf_.empty()) {
        float bestOverlap = 0.0f;
        QRectF bestLine;
        for (const auto& lb : ocrLineBboxesPdf_) {
            QRectF overlap = lb.intersected(span.boundingBox);
            float overlapArea = static_cast<float>(overlap.width() * overlap.height());
            if (overlapArea > bestOverlap) {
                bestOverlap = overlapArea;
                bestLine = lb;
            }
        }
        if (bestOverlap > 0.0f) {
            atlasScreenBbox = pdfToScreen(bestLine);
        }
    }

    // If text has been modified, cover original text and render the new text
    if (span.isModified()) {
        // Render the modified text
        QString displayText = QString::fromUcs4(span.text.data(), static_cast<qsizetype>(span.text.size()));
        if (!displayText.isEmpty()) {
            if (overlayEnrichedAtlas_ && overlayEnrichedAtlas_->size() > 0 && span.formattingRuns.empty()) {
                // Atlas-based overlay for modified text
                bool doCharLog = !atlasCharLogged_;
                if (doCharLog) atlasCharLogged_ = true;
                renderAtlasOverlay(painter, span, atlasScreenBbox, screenBbox, zoom, doCharLog);
            } else {
                // System font overlay: used when no atlas is available or formatting runs exist
                painter.fillRect(screenBbox, Qt::white);
                QFont baseFont = buildOverlayFont(span, zoom, overlayFontOverride_, overlayFontSizePtOverride_, visibleExtentRatioForFont(span.fontName), serifClassForFont(span.fontName));
                painter.setPen(Qt::black);

                if (!span.formattingRuns.empty()) {
                    // Per-run formatting (user-applied font family/size/style changes)
                    //
                    // Position the baseline at the SOURCE font's measured
                    // ascender ratio × bbox.height — the SAME convention the
                    // no-formatting-run branch below uses. This branch
                    // previously used the substitute QFont's leading-inflated
                    // pixel ascent (max across runs), which overshoots the
                    // em-box-tall white cover band (e.g. 40px QFont ascent vs a
                    // 38.1px band), so the overlay dropped to/below the band's
                    // bottom and the original serif-italic glyphs peeked out
                    // along the top. ANY new/substituted character creates a
                    // formatting run and flipped the overlay onto that un-fixed
                    // baseline — the geometry mis-alignment. Use the source
                    // ratio so both branches share one baseline convention.
                    float topRatio = ascenderRatioForFont(span.fontName);
                    float textY = static_cast<float>(screenBbox.top())
                                  + topRatio * static_cast<float>(screenBbox.height());
                    float currentX = static_cast<float>(screenBbox.left());

                    int pos = 0;
                    int textLen = static_cast<int>(displayText.size());
                    while (pos < textLen) {
                        FormattingRun runFmt = model_->formattingAtChar(activeSpanIndex_, pos);
                        int runEnd;
                        QFont runFont;

                        if (runFmt.charEnd > 0 && pos >= runFmt.charStart) {
                            // Within a formatting run
                            runEnd = std::min(runFmt.charEnd, textLen);
                            runFont = buildRunFont(runFmt, span, zoom, overlayFontOverride_,
                                                   overlayFontSizePtOverride_, paintWidget);
                        } else {
                            // In a gap — find next run start
                            runEnd = textLen;
                            const auto nextRun = std::find_if(
                                span.formattingRuns.begin(),
                                span.formattingRuns.end(),
                                [pos](const auto& fr) { return fr.charStart > pos; });
                            if (nextRun != span.formattingRuns.end()) {
                                runEnd = std::min(runEnd, nextRun->charStart);
                            }
                            runFont = baseFont;
                        }

                        if (runEnd <= pos) runEnd = pos + 1;
                        QString runText = displayText.mid(pos, runEnd - pos);

                        painter.setFont(runFont);
                        painter.drawText(QPointF(currentX, textY), runText);
                        QFontMetricsF runFm(runFont);
                        currentX += static_cast<float>(runFm.horizontalAdvance(runText));
                        pos = runEnd;
                    }
                } else {
                    // Modified text with no formatting runs — single drawText with base font.
                    // OCR-based overlayWordStyles_ are intentionally NOT applied here:
                    // their character indices are computed from the original OCR text and
                    // would apply wrong styles at wrong positions after editing.
                    //
                    // Position the baseline at the SOURCE font's measured ascender
                    // ratio × bbox.height (not the substitute's leading-inflated
                    // QFont ascent). This keeps overlay caps aligned with body
                    // caps even when the substitute and source fonts have different
                    // ascender-to-em ratios — see the screenshot regression where
                    // Times's ascent overshot CharisSIL's actual cap-top.
                    painter.setFont(baseFont);
                    float topRatio = ascenderRatioForFont(span.fontName);
                    float textY = static_cast<float>(screenBbox.top())
                                  + topRatio * static_cast<float>(screenBbox.height());
                    painter.drawText(QPointF(screenBbox.left(), textY), displayText);
                }
            }
        }
    } else if (overlayEnrichedAtlas_ && overlayEnrichedAtlas_->size() > 0) {
        // Unmodified text with atlas: same rendering as modified path
        bool doCharLog = !atlasCharLogged_;
        if (doCharLog) atlasCharLogged_ = true;
        renderAtlasOverlay(painter, span, atlasScreenBbox, screenBbox, zoom, doCharLog);
    } else if (!overlayWordStyles_.empty()) {
        // Unmodified text on image-dominated pages WITHOUT atlas:
        // render with per-word OCR-detected styles.
        QString displayText = QString::fromUcs4(span.text.data(), static_cast<qsizetype>(span.text.size()));
        if (!displayText.isEmpty()) {
            QFont baseFont = buildOverlayFont(span, zoom, overlayFontOverride_, overlayFontSizePtOverride_, visibleExtentRatioForFont(span.fontName), serifClassForFont(span.fontName));
            painter.setPen(Qt::black);

            float currentX = static_cast<float>(screenBbox.left());
            QFontMetricsF baseFm(baseFont);
            // See parallel comment in the modified-text branch above:
            // baseline derived from source font's measured ascender ratio,
            // not the substitute font's leading-inflated QFont ascent.
            float topRatio = ascenderRatioForFont(span.fontName);
            float textY = static_cast<float>(screenBbox.top())
                          + topRatio * static_cast<float>(screenBbox.height());

            int pos = 0;
            while (pos < displayText.size()) {
                pdfe::tre::WordStyleFlags flags;
                int runEnd = static_cast<int>(displayText.size());

                for (const auto& ws : overlayWordStyles_) {
                    if (ws.charStart <= pos && pos < ws.charEnd) {
                        flags = ws.style;
                        runEnd = std::min(runEnd, ws.charEnd);
                        break;
                    }
                    if (ws.charStart > pos) {
                        runEnd = std::min(runEnd, ws.charStart);
                        break;
                    }
                }
                runEnd = std::min(runEnd, static_cast<int>(displayText.size()));
                if (runEnd <= pos) runEnd = pos + 1;

                QString runText = displayText.mid(pos, runEnd - pos);

                QFont runFont = baseFont;
                runFont.setBold(flags.isBold);
                runFont.setItalic(flags.isItalic);
                runFont.setUnderline(flags.isUnderlined);
                runFont.setStrikeOut(flags.isStrikethrough);

                painter.setFont(runFont);
                painter.drawText(QPointF(currentX, textY), runText);

                QFontMetricsF runFm(runFont);
                currentX += static_cast<float>(runFm.horizontalAdvance(runText));
                pos = runEnd;
            }
        }
    }

    // R3-17: the edit-region frame and selection fill are hardcoded blue; in
    // High-Contrast mode route them through the HC system Highlight color so
    // the overlay stays visible against the HC theme (alpha preserved).
    using pdfe::ui::AccessibilityHelper;
    painter.setPen(QPen(AccessibilityHelper::adaptColor(QColor(0, 120, 215), QPalette::Highlight), 1.5f));
    painter.setBrush(AccessibilityHelper::adaptColor(QColor(0, 120, 215, 30), QPalette::Highlight));
    painter.drawRect(screenBbox);

    // Draw selection highlight
    if (selectionStart_ >= 0 && selectionStart_ != selectionEnd_) {
        int start = std::min(selectionStart_, selectionEnd_);
        int end = std::max(selectionStart_, selectionEnd_);

        painter.setPen(Qt::NoPen);
        painter.setBrush(AccessibilityHelper::adaptColor(QColor(0, 120, 215, 80), QPalette::Highlight));

        if (span.isModified()) {
            int clampedStart = std::min(start, static_cast<int>(span.text.size()));
            int clampedEnd = std::min(end, static_cast<int>(span.text.size()));

            if (overlayEnrichedAtlas_ && overlayEnrichedAtlas_->size() > 0 && span.formattingRuns.empty()) {
                // Atlas mode (scanned/OCR overlay): horizontal HOCR-aligned bbox.
                float bboxH = static_cast<float>(atlasScreenBbox.height());
                float scale = atlasScaleToScreen(span, bboxH);
                float selLeft = static_cast<float>(atlasScreenBbox.left()) +
                    atlasAdvanceForChars(span, clampedStart, scale, zoom);
                float selRight = static_cast<float>(atlasScreenBbox.left()) +
                    atlasAdvanceForChars(span, clampedEnd, scale, zoom);
                QRectF selRect(selLeft, static_cast<float>(atlasScreenBbox.top()),
                               selRight - selLeft,
                               static_cast<float>(atlasScreenBbox.height()));
                painter.drawRect(selRect);
            } else {
                // System font mode: route through the writing-mode/bidi-aware
                // pure geometry so RTL selections cover the right visual span
                // and vertical selections stack in Y. LTR stays byte-identical
                // (systemFontAdvances reproduces horizontalAdvance(left(N))).
                std::vector<float> advances = systemFontAdvances(span, zoom);
                OverlayGeometryParams gp;
                gp.vertical = overlayVertical_;
                gp.rtl = overlayBaseIsRtl(span.text);
                for (const QRectF& r : overlaySelectionRects(
                         advances, screenBbox, clampedStart, clampedEnd, gp)) {
                    painter.drawRect(r);
                }
            }
        } else {
            // Unmodified text: use original charQuads
            for (int i = start; i < end && i < static_cast<int>(span.charQuads.size()); ++i) {
                QRectF charScreen = pdfToScreen(span.charQuads[i]);
                painter.drawRect(charScreen);
            }
        }
    }

    // Draw cursor (blinks via timer)
    if (cursorVisible_ && cursorPosition_ >= 0) {
        float cursorX;
        float cursorTop = static_cast<float>(screenBbox.top());
        float cursorBottom = static_cast<float>(screenBbox.bottom());

        if (span.isModified()) {
            int clampedPos = std::min(cursorPosition_, static_cast<int>(span.text.size()));

            if (overlayEnrichedAtlas_ && overlayEnrichedAtlas_->size() > 0 && span.formattingRuns.empty()) {
                // Atlas mode: use HOCR-aligned bbox (matches rendering)
                float bboxH = static_cast<float>(atlasScreenBbox.height());
                float scale = atlasScaleToScreen(span, bboxH);
                cursorX = static_cast<float>(atlasScreenBbox.left()) +
                    atlasAdvanceForChars(span, clampedPos, scale, zoom);
                cursorTop = static_cast<float>(atlasScreenBbox.top());
                cursorBottom = static_cast<float>(atlasScreenBbox.bottom());
            } else {
                // System font mode: route through the writing-mode/bidi-aware
                // pure caret model. LTR yields the same vertical line at
                // left+advance as before; RTL mirrors it; vertical returns a
                // horizontal stroke (a.y == b.y) stepping down the column.
                std::vector<float> advances = systemFontAdvances(span, zoom);
                OverlayGeometryParams gp;
                gp.vertical = overlayVertical_;
                gp.rtl = overlayBaseIsRtl(span.text);
                auto [ca, cb] = overlayCaretLine(advances, screenBbox, clampedPos, gp);
                painter.setPen(QPen(Qt::black, 2.0f));
                painter.drawLine(ca, cb);
                noteCaretAndDrawPreedit(painter, QRectF(ca, cb).normalized());
                return;
            }
        } else if (cursorPosition_ <= static_cast<int>(span.charQuads.size())) {
            // Unmodified text: use original charQuads (accurate for original layout)
            if (cursorPosition_ < static_cast<int>(span.charQuads.size())) {
                QRectF charScreen = pdfToScreen(span.charQuads[cursorPosition_]);
                cursorX = static_cast<float>(charScreen.left());
                cursorTop = static_cast<float>(charScreen.top());
                cursorBottom = static_cast<float>(charScreen.bottom());
            } else if (!span.charQuads.empty()) {
                QRectF charScreen = pdfToScreen(span.charQuads.back());
                cursorX = static_cast<float>(charScreen.right());
                cursorTop = static_cast<float>(charScreen.top());
                cursorBottom = static_cast<float>(charScreen.bottom());
            } else {
                return;
            }
        } else {
            return;
        }

        painter.setPen(QPen(Qt::black, 2.0f));
        painter.drawLine(QPointF(cursorX, cursorTop), QPointF(cursorX, cursorBottom));
        noteCaretAndDrawPreedit(
            painter, QRectF(QPointF(cursorX, cursorTop),
                            QPointF(cursorX, cursorBottom)).normalized());
    }
}

void TextEditTool::noteCaretAndDrawPreedit(QPainter& painter,
                                           const QRectF& caretRect)
{
    // A caret line is zero-width by construction; give the reported rectangle a
    // 1px extent so platform IMEs that position their candidate window from it
    // do not treat it as empty.
    QRectF r = caretRect;
    if (r.width() <= 0.0) r.setWidth(1.0);
    if (r.height() <= 0.0) r.setHeight(1.0);
    lastCaretScreenRect_ = r;
    lastCaretScreenRectValid_ = true;

    if (preeditText_.empty())
        return;

    // The composition is drawn, never inserted: it exists only on screen until
    // the IME commits it (see commitPreedit).
    const QString preedit = QString::fromUcs4(
        reinterpret_cast<const char32_t*>(preeditText_.data()),
        static_cast<int>(preeditText_.size()));

    painter.save();
    QFont f = painter.font();
    f.setUnderline(true);
    painter.setFont(f);
    painter.setPen(QPen(Qt::black, 1.0f));
    painter.drawText(QPointF(r.right(), r.bottom()), preedit);
    painter.restore();
}

void TextEditTool::commitEdit()
{
    if (!isEditing() || !model_)
        return;

    blinkTimer_->stop();
    cursorVisible_ = true;

    const auto& span = model_->span(activeSpanIndex_);
    if (span.isModified() && textEdited) {
        qDebug() << "[TextEdit] commitEdit: page=" << currentPageIndex_
                 << "span=" << activeSpanIndex_
                 << "fontName=" << QString::fromStdString(span.fontName)
                 << "fontSize=" << span.fontSize
                 << "formattingRuns=" << span.formattingRuns.size()
                 << "textChanged=" << (span.text != span.originalText);
        for (size_t i = 0; i < span.formattingRuns.size(); ++i) {
            const auto& fr = span.formattingRuns[i];
            qDebug() << "  [TextEdit] run[" << i << "]:"
                     << "chars=" << fr.charStart << "-" << fr.charEnd
                     << "family=" << QString::fromStdString(fr.fontFamily)
                     << "fontSize=" << fr.fontSize
                     << "fontFilePath=" << QString::fromStdString(fr.fontFilePath)
                     << "bold=" << fr.bold << "italic=" << fr.italic
                     << "underline=" << fr.underline << "strike=" << fr.strikethrough;
        }
        textEdited(currentPageIndex_, activeSpanIndex_,
                   span.originalText, span.text, span);
    }

    activeSpanIndex_ = -1;
    cursorPosition_ = 0;
    selectionStart_ = -1;
    selectionEnd_ = -1;
    // An uncommitted composition is abandoned when the edit ends — it was never
    // in the model, so there is nothing to roll back.
    preeditText_.clear();
    preeditCursor_ = 0;
    lastCaretScreenRectValid_ = false;
    emit editingStateChanged(false);
}

void TextEditTool::pushIncrementalEdit()
{
    // Pipeline b3f19d72 phase-3 (P7-BUG-1): mirror commitEdit's textEdited
    // callback path so each model mutation reaches the UndoManager
    // immediately, but DO NOT reset activeSpanIndex_ — the user is still
    // editing the same span.  Consecutive incremental edits on the same
    // span collapse into one undo step via EditTextCommand::mergeWith
    // (mergeId = 1000 + spanIndex).
    if (!isEditing() || !model_ || !textEdited) return;
    const auto& span = model_->span(activeSpanIndex_);
    if (!span.isModified()) return;
    textEdited(currentPageIndex_, activeSpanIndex_,
               span.originalText, span.text, span);
}

void TextEditTool::cancelEdit()
{
    if (!isEditing() || !model_)
        return;

    blinkTimer_->stop();
    cursorVisible_ = true;

    // Restore original text and clear any formatting runs
    auto& span = model_->mutableSpan(activeSpanIndex_);
    span.text = span.originalText;
    span.formattingRuns.clear();

    activeSpanIndex_ = -1;
    cursorPosition_ = 0;
    selectionStart_ = -1;
    selectionEnd_ = -1;
    preeditText_.clear();
    preeditCursor_ = 0;
    lastCaretScreenRectValid_ = false;
    emit editingStateChanged(false);
}

void TextEditTool::selectSpan(int spanIndex)
{
    if (spanIndex == activeSpanIndex_)
        return;

    activeSpanIndex_ = spanIndex;
    cursorPosition_ = 0;
    selectionStart_ = -1;
    selectionEnd_ = -1;
    // Switching spans abandons any composition in flight on the old one.
    preeditText_.clear();
    preeditCursor_ = 0;
    cursorVisible_ = true;
    atlasCharLogged_ = false;
    blinkTimer_->start();

    const auto& span = model_->span(spanIndex);

    // R2-21a SINK: auto-select the overlay writing mode from the span's carried
    // writing mode (sourced from core::TextLine::writingMode in extraction). A
    // vertical-wmode span drives the overlay's TTB caret/selection geometry
    // (R2-21b); a horizontal span resets it. This is the product edge that
    // closes extraction -> overlay with no manual setOverlayVerticalMode() call.
    setOverlayVerticalMode(span.writingMode == 1);

    qDebug() << "[TextEdit] selectSpan:" << spanIndex
             << "fontName=" << QString::fromStdString(span.fontName)
             << "fontSize=" << span.fontSize
             << "bbox=" << span.boundingBox
             << "bboxW=" << span.boundingBox.width()
             << "bboxH=" << span.boundingBox.height()
             << "textLen=" << span.text.size()
             << "overlayFontOverride=" << overlayFontOverride_
             << "overlayFontSizeOverride=" << overlayFontSizePtOverride_;

    // Signal MainWindow with the clicked span's position so it can build
    // or rebuild the atlas using the matching HOCR line as reference.
    // Emitted on every span click — MainWindow decides whether to rebuild
    // (different HOCR line than current reference) or keep the existing atlas.
    if (!span.boundingBox.isEmpty()) {
        emit spanActivated(span.boundingBox);
    }

    emit activeSpanChanged(QString::fromStdString(span.fontName), span.fontSize);
    emit editingStateChanged(true);
    emitSelectionFormatting();
}

void TextEditTool::insertTextAtCursor(const std::u32string& text)
{
    if (!model_ || activeSpanIndex_ < 0)
        return;

    model_->replaceCharRange(activeSpanIndex_, cursorPosition_, cursorPosition_, text);
    cursorPosition_ += static_cast<int>(text.size());
}

void TextEditTool::deleteCharAtCursor(bool forward)
{
    if (!model_ || activeSpanIndex_ < 0)
        return;

    const auto& span = model_->span(activeSpanIndex_);
    int textLen = static_cast<int>(span.text.size());

    if (forward) {
        if (cursorPosition_ < textLen) {
            model_->replaceCharRange(activeSpanIndex_, cursorPosition_, cursorPosition_ + 1, U"");
        }
    } else {
        if (cursorPosition_ > 0) {
            model_->replaceCharRange(activeSpanIndex_, cursorPosition_ - 1, cursorPosition_, U"");
            --cursorPosition_;
        }
    }
}

bool TextEditTool::hasTextSelection() const {
    return selectionStart_ >= 0 && selectionEnd_ >= 0 && selectionStart_ != selectionEnd_;
}

bool TextEditTool::copyDenied() const
{
    // Live document permission is the mandatory floor; the manual Prevent-Copy
    // toggle is additive. Either one denies extraction.
    if (document_ && !document_->copyAllowed())
        return true;
    return copyPrevented_;
}

void TextEditTool::copySelection()
{
    // R2-10: enforce copy/extract permission at THIS funnel so the context-menu
    // Copy action (which calls performCopy()->copySelection() directly) is gated
    // exactly like the global Copy menu — not just the keyboard shortcut branch.
    if (copyDenied())
        return;

    if (!isEditing() || !model_ || selectionStart_ < 0 || selectionStart_ == selectionEnd_)
        return;

    const auto& span = model_->span(activeSpanIndex_);
    int start = std::min(selectionStart_, selectionEnd_);
    int end = std::max(selectionStart_, selectionEnd_);
    start = std::clamp(start, 0, static_cast<int>(span.text.size()));
    end = std::clamp(end, start, static_cast<int>(span.text.size()));

    std::u32string selected(span.text.begin() + start, span.text.begin() + end);

    // Convert u32string to QString
    QString qstr = QString::fromUcs4(selected.data(), static_cast<qsizetype>(selected.size()));

    // RDP-safe write: verify the clipboard actually accepted our text.
    // Under Windows remote sessions the clipboard can be transiently
    // grabbed by the session manager — a silent failure here would
    // lose the user's copy.
    if (!writeClipboardTextWithVerify(qstr)) {
        qWarning() << "[TextEdit] copySelection: clipboard setText did "
                      "not verify after retries";
    }
}

void TextEditTool::cutSelection()
{
    // R2-10: a denied copy must refuse the WHOLE cut — never delete the span
    // without (or instead of) placing it on the clipboard.
    if (copyDenied())
        return;

    copySelection();
    // Delete selection
    if (selectionStart_ >= 0 && selectionStart_ != selectionEnd_) {
        int start = std::min(selectionStart_, selectionEnd_);
        int end = std::max(selectionStart_, selectionEnd_);
        model_->replaceCharRange(activeSpanIndex_, start, end, U"");
        cursorPosition_ = start;
        selectionStart_ = -1;
        selectionEnd_ = -1;
    }
}

void TextEditTool::pasteAtCursor()
{
    if (!isEditing() || !model_)
        return;

    // RDP-safe read: retry briefly to absorb transient empty reads.
    // Under Windows remote sessions QClipboard::text() can return ""
    // if the session manager has grabbed the clipboard at read time,
    // even though the user's setText() earlier in this test/session
    // succeeded. Without the retry, a real user paste silently
    // no-ops and the text-edit model stays unchanged.
    QString clipText = readClipboardTextWithRetry();
    if (clipText.isEmpty())
        return;

    // Convert QString to u32string
    const auto ucs4 = clipText.toUcs4();
    std::u32string u32Text(ucs4.begin(), ucs4.end());

    // Replace selection or insert at cursor
    if (selectionStart_ >= 0 && selectionStart_ != selectionEnd_) {
        int start = std::min(selectionStart_, selectionEnd_);
        int end = std::max(selectionStart_, selectionEnd_);
        model_->replaceCharRange(activeSpanIndex_, start, end, u32Text);
        cursorPosition_ = start + static_cast<int>(u32Text.size());
        selectionStart_ = -1;
        selectionEnd_ = -1;
    } else {
        insertTextAtCursor(u32Text);
    }
}

void TextEditTool::selectAll()
{
    if (!isEditing() || !model_)
        return;

    const auto& span = model_->span(activeSpanIndex_);
    selectionStart_ = 0;
    selectionEnd_ = static_cast<int>(span.text.size());
    cursorPosition_ = selectionEnd_;
}

void TextEditTool::resetCursorBlink()
{
    cursorVisible_ = true;
    blinkTimer_->start();
}

// ─── Formatting API ────────────────────────────────────────────────

void TextEditTool::applyFormattingField(std::function<void(FormattingRun&)> modifier)
{
    if (!isEditing() || !model_)
        return;

    const auto& span = model_->span(activeSpanIndex_);
    int textLen = static_cast<int>(span.text.size());
    if (textLen == 0)
        return;

    int start, end;
    if (selectionStart_ >= 0 && selectionStart_ != selectionEnd_) {
        start = std::min(selectionStart_, selectionEnd_);
        end = std::max(selectionStart_, selectionEnd_);
    } else {
        start = 0;
        end = textLen;
    }

    // Build a FormattingRun from the current state at cursor position,
    // then apply the modifier to change the desired field
    FormattingRun run = model_->formattingAtChar(activeSpanIndex_, start);
    run.charStart = start;
    run.charEnd = end;
    modifier(run);

    qDebug() << "[TextEdit] applyFormattingField:"
             << "chars=" << start << "-" << end
             << "family=" << QString::fromStdString(run.fontFamily)
             << "fontSize=" << run.fontSize
             << "fontFilePath=" << (run.fontFilePath.empty() ? "EMPTY" : QString::fromStdString(run.fontFilePath))
             << "bold=" << run.bold << "italic=" << run.italic;

    // UI-TE-UNDO-07: snapshot formattingRuns before and after the
    // mutation so MainWindow can build an ApplyFormattingCommand that
    // makes the change reversible via the UndoManager. Without these
    // snapshots, formatting toggles are silent on Ctrl+Z.
    std::vector<FormattingRun> oldRuns =
        model_->span(activeSpanIndex_).formattingRuns;
    model_->applyFormatting(activeSpanIndex_, start, end, run);
    std::vector<FormattingRun> newRuns =
        model_->span(activeSpanIndex_).formattingRuns;

    if (formattingApplied) {
        formattingApplied(model_.get(), activeSpanIndex_,
                          std::move(oldRuns), std::move(newRuns),
                          "Apply formatting");
    }

    emit repaintRequested();
    emitSelectionFormatting();
}

void TextEditTool::applyFontFamily(const QString& family)
{
    if (!isEditing() || !model_)
        return;

    std::string familyStd = family.toStdString();

    applyFormattingField([&](FormattingRun& run) {
        run.fontFamily = familyStd;
        // Resolve the font file path for embedding
        run.fontFilePath = fontResolver_->resolve(familyStd, run.bold, run.italic);
    });
}

void TextEditTool::applyFontSize(float size)
{
    applyFormattingField([size](FormattingRun& run) {
        run.fontSize = size;
    });
}

float TextEditTool::selectionRunFontSizeForTesting() const
{
    if (!isEditing() || !model_ || activeSpanIndex_ < 0)
        return 0.0f;
    const auto& span = model_->span(activeSpanIndex_);
    const int textLen = static_cast<int>(span.text.size());
    if (textLen == 0)
        return 0.0f;
    // Mirror applyFormattingField's run selection: the START of the selection
    // (or 0 when nothing is selected) is inside the run applyFontSize modified,
    // unlike emitSelectionFormatting which reads at cursorPosition_ (the
    // selection END, just past the modified run).
    const int start = (selectionStart_ >= 0 && selectionStart_ != selectionEnd_)
                          ? std::min(selectionStart_, selectionEnd_)
                          : 0;
    const FormattingRun run = model_->formattingAtChar(activeSpanIndex_, start);
    return (run.fontSize > 0.0f) ? run.fontSize : span.fontSize;
}

void TextEditTool::applyBold(bool bold)
{
    applyFormattingField([bold, this](FormattingRun& run) {
        run.bold = bold;
        // Re-resolve font path with new weight
        if (!run.fontFamily.empty()) {
            run.fontFilePath = fontResolver_->resolve(run.fontFamily, run.bold, run.italic);
        }
    });
}

void TextEditTool::applyItalic(bool italic)
{
    applyFormattingField([italic, this](FormattingRun& run) {
        run.italic = italic;
        if (!run.fontFamily.empty()) {
            run.fontFilePath = fontResolver_->resolve(run.fontFamily, run.bold, run.italic);
        }
    });
}

void TextEditTool::applyUnderline(bool underline)
{
    applyFormattingField([underline](FormattingRun& run) {
        run.underline = underline;
    });
}

void TextEditTool::applyStrikethrough(bool strikethrough)
{
    applyFormattingField([strikethrough](FormattingRun& run) {
        run.strikethrough = strikethrough;
    });
}

void TextEditTool::applyTextColor(const QColor& color)
{
    uint32_t rgb = static_cast<uint32_t>(color.rgb()) & 0x00FFFFFF;
    applyFormattingField([rgb](FormattingRun& run) {
        run.textColor = rgb;
    });
}

void TextEditTool::applyAlignment(Qt::Alignment alignment)
{
    currentAlignment_ = alignment;
    emit repaintRequested();
}

void TextEditTool::emitSelectionFormatting()
{
    if (!isEditing() || !model_)
        return;

    int charIdx = cursorPosition_;
    if (charIdx < 0) charIdx = 0;

    FormattingRun run = model_->formattingAtChar(activeSpanIndex_, charIdx);
    const auto& span = model_->span(activeSpanIndex_);

    // If run has no explicit font, use span defaults
    QString family = run.fontFamily.empty()
        ? QString::fromStdString(span.fontName)
        : QString::fromStdString(run.fontFamily);
    float size = (run.fontSize > 0.0f) ? run.fontSize : span.fontSize;

    // Resolve the Bold/Italic the toolbar shows: an explicit user-applied
    // formatting run wins; otherwise fall back to the span's descriptor-sourced
    // style so the toolbar reflects the underlying font before any edit
    // (pipeline d4e7f921 phase 2). This mirrors the family/size span-default
    // fallback above.
    const ToolbarStyle style = resolveToolbarStyle(run, span);

    qDebug() << "[TextEdit] emitSelectionFormatting:"
             << "family=" << family
             << "size=" << size
             << "run.fontFamily=" << QString::fromStdString(run.fontFamily)
             << "run.fontSize=" << run.fontSize
             << "span.fontName=" << QString::fromStdString(span.fontName)
             << "span.fontSize=" << span.fontSize
             << "span.isBold=" << span.isBold << "span.isItalic=" << span.isItalic
             << "bold=" << style.bold << "italic=" << style.italic;

    QColor runColor = QColor::fromRgb(run.textColor);
    emit selectionFormattingChanged(family, size,
                                    style.bold, style.italic,
                                    style.underline, style.strikethrough,
                                    runColor);
}

TextEditTool::ToolbarStyle
TextEditTool::resolveToolbarStyle(const FormattingRun& run, const TextSpan& span)
{
    ToolbarStyle s;
    // formattingAtChar returns a default-constructed FormattingRun (charEnd == 0)
    // when no explicit run covers the cursor char; a real run always has
    // charEnd > charStart. Only an explicit run carries user-set bold/italic.
    // For an unedited span we fall back to the descriptor-sourced span style so
    // a font whose italic/bold lives only in /FontDescriptor (neutral name)
    // still lights the toolbar B/I buttons.
    const bool hasExplicitRun = run.charEnd > run.charStart;
    if (hasExplicitRun) {
        s.bold = run.bold;
        s.italic = run.italic;
        s.underline = run.underline;
        s.strikethrough = run.strikethrough;
    } else {
        s.bold = span.isBold;
        s.italic = span.isItalic;
        // Underline/strikethrough are not encoded in the PDF font descriptor;
        // they stay unchecked until the user applies one (which creates a run).
        s.underline = false;
        s.strikethrough = false;
    }
    return s;
}

} // namespace pdfe::textedit
