#include "FormTool.h"

#include "core/Utf8.h"

#include <cstddef>
#include "FormManager.h"
#include "FormCommands.h"
#include "JavaScriptEngine.h"
#include "core/Page.h"
#include "core/UndoManager.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>
#include <QMouseEvent>
#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QString>
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdfe::forms {

// ─── GitHub #459: live paint of the field being edited ──────────────────────
//
// While a text field is edited, the typed text lives only in editBuffer_ (and
// an IME composition in preeditText_); the document is written once, in
// commitEdit(). The page raster under the overlay comes from the widget's
// appearance stream, which is therefore stale for the whole edit. So the
// overlay paints the field itself: an opaque fill in the widget background,
// then the buffer and the composition in the widget's /DA font, size and
// colour, laid out the way MuPDF's write_variable_text (pdf-appearance.c)
// lays out the committed value, so the text does not jump on commit.
//
// Phase-3 security audit (hostile input). The /DA size, /MaxLen, /BS /W and
// the value itself come from the file, so this paint:
//   - re-sanitises the /DA size (FormManager already did, but a FormFieldData
//     can be hand-built) and clamps size x zoom to [1, kMaxEditFontPixelSize]
//     pixels BEFORE any float->int conversion;
//   - paints nothing for a non-finite or absurd zoom or field box, and for a
//     zoom below kMinZoom (a denormal zoom would make 1/zoom, and with it the
//     line height, infinite and the visible-line culling NaN);
//   - wraps in O(n) per layout (per-character advance sums, confirmed with an
//     exact measure of each line), caches the layout across repaints, and
//     draws only the lines (or, for a very long single line, the stretch of
//     it) that can reach the field box, and never more lines than the box
//     can show (a hard cap, a backstop should the culling range be wrong);
//   - draws at most /MaxLen comb cells, and at most one per pixel column.

namespace detail {

// The cached layout of the field being edited. Keyed on everything a layout
// depends on: the composed text (editBuffer_ with the preedit spliced in,
// password-masked), the font (QFont== covers family, style and pixel size),
// the content width and the layout mode. Any change of key rebuilds it.
struct EditLayoutCache {
    bool valid = false;
    bool multiline = false;
    QString text;
    QFont font;
    qreal width = 0.0;
    /// Multiline: wrapped lines as [start, end) UTF-16 ranges into `text`.
    std::vector<std::pair<qsizetype, qsizetype>> lines;
    /// Single line longer than kExactSingleLineLimit: the summed advance of
    /// text[0, i) at every UTF-16 index i (size text.size() + 1).
    std::vector<qreal> prefix;

    /// Auto-size (/DA size 0, single line): the text's advance per point.
    bool autoValid = false;
    QString autoText;
    QFont autoFont;
    qreal autoPerPt = 0.0;
};

} // namespace detail

namespace {

/// Largest font pixel size the live paint will ask Qt for (size x zoom).
constexpr int kMaxEditFontPixelSize = 10000;
/// Largest zoom the live paint accepts. The canvas stops at 10
/// (PdfCanvas::MAX_ZOOM); a hand-built call with a finite box but a zoom like
/// 1e30 would put the padding, and so every glyph, at coordinates Qt's
/// fixed-point rasteriser overflows.
constexpr float kMaxZoom = 1000.0f;
/// Smallest zoom the live paint accepts (the canvas stops at 0.1,
/// PdfCanvas::MIN_ZOOM). Phase-3 security audit, LOW: below it, a hand-built
/// call with a denormal zoom turns the 1-pixel font floor's 1/zoom into
/// infinity, so the line height is infinite and the culling range NaN.
constexpr float kMinZoom = 1.0e-3f;
/// The smallest line height, in pixels, the multiline cap divides by. The
/// clamps above keep a real line at 1.116 px or more (a font of at least one
/// pixel), so this never shortens a legitimate cap.
constexpr qreal kMinLineHeightPx = 1.0;
/// A field box beyond this many pixels in any coordinate is not painted: a PDF
/// page is at most 14400 pt, so even at zoom 64 a real field is < 1e6 px.
constexpr qreal kMaxBoxExtentPx = 1.0e7;
/// Single-line text up to this many UTF-16 units is laid out exactly as
/// before (shaped as one run). Longer text -- hostile or pasted, never typed
/// into a real single-line field -- uses cached per-codepoint advance sums and
/// draws only the visible stretch.
constexpr qsizetype kExactSingleLineLimit = 4096;
/// UTF-16 units wrapLines may shape exactly per layout (Qt shapes at roughly
/// 0.4 us a unit here, and each line is confirmed with about two shapes of
/// itself). Text up to ~32K units wraps exactly by the original rule; past
/// the budget, the line breaks are the scale-corrected per-codepoint
/// estimate, so a megabyte value lays out in tens of milliseconds.
constexpr qsizetype kExactWrapBudgetUnits = 65536;
/// Longest line wrapLines makes at all, in UTF-16 units.
constexpr qsizetype kMaxLineUnits = 65536;
/// Longest line wrapLines confirms exactly (a longer one is either a hostile
/// run of near-zero-advance codepoints or a font tiny against the field).
constexpr qsizetype kMaxExactLineUnits = 4096;
/// Most UTF-16 units drawn in one run. Reached only by a line of near-zero-
/// advance codepoints (a normal line is bounded by the field width).
constexpr qsizetype kMaxRunUnits = 16384;

// A PDF colour (gray, RGB or CMYK components in 0..1) as a QColor.
QColor colorFromPdf(int n, const std::array<float, 4>& c, const QColor& fallback) {
    // sanitizeColorComponent is std::clamp to [0, 1] plus NaN -> 0 (a NaN
    // passes straight through std::clamp and makes QColor invalid).
    auto f = [](float v) { return sanitizeColorComponent(v); };
    switch (n) {
        case 1: return QColor::fromRgbF(f(c[0]), f(c[0]), f(c[0]));
        case 3: return QColor::fromRgbF(f(c[0]), f(c[1]), f(c[2]));
        case 4: return QColor::fromCmykF(f(c[0]), f(c[1]), f(c[2]), f(c[3]));
        default: return fallback;
    }
}

// The /DA base-14 resource name as a QFont family. The family names are the
// PostScript ones; fontconfig (Linux) and Qt's Windows substitution table map
// them to the metric-compatible system faces (Arial / Liberation Sans /
// Nimbus Sans for Helvetica, and so on), and the style hint covers a host
// that has none of them.
QFont fontForDefaultAppearance(const std::string& daFont) {
    if (daFont == "TiRo") {
        QFont font(QStringLiteral("Times"));
        font.setStyleHint(QFont::Times);
        return font;
    }
    if (daFont == "Cour") {
        QFont font(QStringLiteral("Courier"));
        font.setStyleHint(QFont::Courier);
        font.setFixedPitch(true);
        return font;
    }
    if (daFont == "Helv") {
        QFont font(QStringLiteral("Helvetica"));
        font.setStyleHint(QFont::Helvetica);
        return font;
    }
    // Symb, ZaDb or anything else: a plain sans-serif face.
    QFont font(QStringLiteral("sans-serif"));
    font.setStyleHint(QFont::SansSerif);
    return font;
}

// Password fields show one bullet per codepoint, never the text.
QString maskedForPassword(const QString& s) {
    return QString(static_cast<qsizetype>(s.toUcs4().size()), QChar(0x2022));
}

// UTF-16 index just past the codepoint starting at `i`. A lone surrogate is a
// codepoint of its own.
qsizetype nextCodepoint(const QString& s, qsizetype i) {
    if (i + 1 < s.size() && s[i].isHighSurrogate() && s[i + 1].isLowSurrogate())
        return i + 2;
    return i + 1;
}

// Per-codepoint advances, memoised: ASCII in a table, the rest in a map. Used
// for ESTIMATES only (it does not shape across codepoints, so kerning is not
// in it); wrapLines confirms every line with an exact measurement.
class AdvanceTable {
public:
    explicit AdvanceTable(const QFontMetricsF& fm) : fm_(fm) { ascii_.fill(-1.0); }

    qreal at(const QString& s, qsizetype i, qsizetype next) {
        const char16_t u = s[i].unicode();
        if (next == i + 1 && u < 128) {
            qreal& a = ascii_[u];
            if (a < 0.0) a = fm_.horizontalAdvance(s[i]);
            return a;
        }
        const char32_t key = next == i + 2 ? QChar::surrogateToUcs4(s[i], s[i + 1])
                                           : static_cast<char32_t>(u);
        const auto it = other_.find(key);
        if (it != other_.end()) return it->second;
        const qreal a = fm_.horizontalAdvance(s.mid(i, next - i));
        other_.emplace(key, a);
        return a;
    }

private:
    const QFontMetricsF& fm_;
    std::array<qreal, 128> ascii_{};
    std::unordered_map<char32_t, qreal> other_;
};

// Greedy word wrap of `s` into lines no wider than `width`, honouring hard
// newlines. Each line is a [start, end) range of UTF-16 indices into `s`; a
// newline belongs to no line, so the line after it starts one past it.
//
// The rule (unchanged from the phase-3 original): a line starting at `pos`
// takes codepoints while the EXACT advance of s[pos, next) stays within
// `width`, always at least one; it then breaks after the last space in it,
// if there is one and the paragraph continues.
//
// Phase-3 security audit: the original found the break by re-measuring the
// growing substring once per codepoint -- O(line length) per codepoint, a
// QString allocation each, quadratic per line; a 1 MB value with a tiny font
// took minutes per paint. Now each line is O(its length): per-codepoint
// advance sums ESTIMATE the break, then the exact substring advance -- the
// very expression the original compared -- CONFIRMS it, usually in two
// measurements (the estimate is scaled by the exact/estimated ratio of the
// previous line, and it falls back to galloping + bisection when it is still
// off). The breaks are the original's whenever the exact prefix advance grows
// with each codepoint, which it does for any font whose kerning never cancels
// a whole glyph advance. Exact confirmation stops once kExactWrapBudgetUnits
// have been shaped; the rest of a huge value breaks at the estimate.
std::vector<std::pair<qsizetype, qsizetype>> wrapLines(const QString& s,
                                                       const QFontMetricsF& fm,
                                                       qreal width) {
    std::vector<std::pair<qsizetype, qsizetype>> lines;
    AdvanceTable adv(fm);
    std::vector<qsizetype> bounds;   // codepoint boundaries from the line start
    std::vector<qreal> sums;         // summed estimate at each boundary
    qreal scale = 1.0;               // exact / summed, from the last line
    qsizetype exactBudget = kExactWrapBudgetUnits;
    qsizetype paraStart = 0;
    for (;;) {
        const qsizetype nl = s.indexOf(QChar('\n'), paraStart);
        const qsizetype paraEnd = nl < 0 ? s.size() : nl;
        qsizetype pos = paraStart;
        if (pos == paraEnd) lines.emplace_back(pos, pos);
        while (pos < paraEnd) {
            // bounds[k] is the k-th codepoint boundary after pos (bounds[0] =
            // pos). Candidate breaks are k >= 2: the line holds bounds[0,k-1]
            // where k is the smallest with exact(pos, bounds[k]) > width.
            bounds.clear();
            bounds.push_back(pos);
            auto extendTo = [&](std::size_t k) {
                while (bounds.size() <= k && bounds.back() < paraEnd)
                    bounds.push_back(std::min(nextCodepoint(s, bounds.back()), paraEnd));
            };
            auto exact = [&](std::size_t k) {
                const qsizetype units = bounds[k] - pos;
                exactBudget -= std::min(exactBudget, units);
                return fm.horizontalAdvance(s.mid(pos, units));
            };
            auto over = [&](std::size_t k) { return exact(k) > width; };

            // Estimate: the first k >= 2 whose summed advance, corrected by
            // the exact/summed ratio learned on the previous line (hinted
            // single-codepoint advances can differ from shaped ones at small
            // pixel sizes), exceeds width.
            sums.clear();
            sums.push_back(0.0);
            std::size_t est = 0;
            bool estOverflows = false;
            qreal sum = 0.0;
            while (bounds.back() < paraEnd) {
                const qsizetype i = bounds.back();
                const qsizetype next = std::min(nextCodepoint(s, i), paraEnd);
                sum += adv.at(s, i, next);
                bounds.push_back(next);
                sums.push_back(sum);
                if (bounds.size() >= 3 && sum * scale > width) {
                    est = bounds.size() - 1;
                    estOverflows = true;
                    break;
                }
                // A line no width can bound (near-zero advances) is cut at
                // kMaxLineUnits, so no line's walk exceeds that.
                if (next - pos >= kMaxLineUnits) break;
            }
            if (est == 0) est = bounds.size() - 1;   // reached paraEnd or the cap

            // Confirm with exact measurements: lo is known not to overflow
            // (1 is never a candidate), hi (if set) is known to overflow.
            // Gallop from the estimate in whichever direction it was wrong,
            // then bisect. A line is confirmed only while it is affordable:
            // the layout's exact-shaping budget is not spent and the line is
            // at most kMaxExactLineUnits long; otherwise the estimate stands.
            auto affordable = [&](std::size_t k) {
                return exactBudget > 0 && bounds[k] - pos <= kMaxExactLineUnits;
            };
            std::size_t endK = est;   // the line is [pos, bounds[endK])
            if (est < 2) {
                endK = est;           // a single codepoint is always a line
            } else if (!affordable(est)) {
                endK = estOverflows ? est - 1 : est;
            } else {
                std::size_t lo = 1;
                std::size_t hi = 0;
                const qreal e = exact(est);
                if (sums[est] > 0.0 && e > 0.0) scale = e / sums[est];
                if (e > width) hi = est;
                else lo = est;
                if (hi != 0) {
                    for (std::size_t step = 1; hi - lo > 1 && exactBudget > 0; step *= 2) {
                        const std::size_t k = hi - std::min(step, hi - lo - 1);
                        if (over(k)) {
                            hi = k;
                        } else {
                            lo = k;
                            break;
                        }
                    }
                } else {
                    for (std::size_t step = 1; bounds.back() < paraEnd; step *= 2) {
                        extendTo(lo + step);
                        const std::size_t k = std::min(lo + step, bounds.size() - 1);
                        if (!affordable(k)) break;
                        if (over(k)) {
                            hi = k;
                            break;
                        }
                        lo = k;
                    }
                }
                // Smallest overflowing k in (lo, hi].
                while (hi != 0 && hi - lo > 1 && exactBudget > 0) {
                    const std::size_t mid = lo + (hi - lo) / 2;
                    if (over(mid)) hi = mid;
                    else lo = mid;
                }
                // hi == 0: everything measured fits, up to bounds[lo] (the
                // paragraph end, unless the budget stopped the gallop).
                endK = hi != 0 ? hi - 1 : lo;
            }
            qsizetype end = bounds[endK];

            // Break after the last space on the line, if the paragraph goes on.
            if (end < paraEnd) {
                for (qsizetype i = end - 1; i >= pos; --i) {
                    if (s[i] == QChar(' ')) {
                        end = i + 1;
                        break;
                    }
                }
            }
            lines.emplace_back(pos, end);
            pos = end;
        }
        if (nl < 0) break;
        paraStart = nl + 1;
    }
    return lines;
}

bool finiteBox(const QRectF& r) {
    for (const qreal v : {r.left(), r.top(), r.width(), r.height()})
        if (!std::isfinite(v) || std::abs(v) > kMaxBoxExtentPx) return false;
    return r.width() >= 0.0 && r.height() >= 0.0;
}

// Paint the field being edited into `box` (its screen rect) and return the
// caret rectangle, in screen coordinates, at which the caller draws the caret
// (a null rect when nothing could be painted).
//
// `buffer`/`cursorByte` are editBuffer_ and cursorPosition_ (a UTF-8 byte
// offset); `preedit` is the in-flight IME composition, shown at the caret and
// underlined. The caret sits after the composition. `cache` carries the
// layout between repaints; `stats` records what this paint did.
QRectF paintFieldEdit(QPainter& painter, const FormFieldData& field,
                      const QRectF& box, float zoom, const std::string& buffer,
                      int cursorByte, const std::string& preedit,
                      detail::EditLayoutCache& cache, FormTool::EditPaintStats& stats) {
    stats = FormTool::EditPaintStats{};
    // Hostile geometry: a zoom or a field box that is non-finite, non-positive
    // or absurd would feed NaN or out-of-range values to the layout and to
    // Qt's fixed-point rasteriser. Paint nothing; the caller records no caret.
    if (!std::isfinite(zoom) || zoom <= 0.0f || zoom < kMinZoom || zoom > kMaxZoom ||
        !finiteBox(box))
        return QRectF();

    const std::size_t at = pdfe::core::utf8::alignLeft(
        buffer, static_cast<std::size_t>(std::max(0, cursorByte)));
    QString before = QString::fromStdString(buffer.substr(0, at));
    QString pre = QString::fromStdString(preedit);
    QString after = QString::fromStdString(buffer.substr(at));
    if (field.isPassword) {
        before = maskedForPassword(before);
        pre = maskedForPassword(pre);
        after = maskedForPassword(after);
    }
    const QString text = before + pre + after;
    const qsizetype preStart = before.size();
    const qsizetype caretIdx = preStart + pre.size();

    // Opaque background: /MK /BG, or white when the widget has none. Opaque so
    // the stale appearance stream underneath cannot show through.
    const FormFieldBorderAppearance& ap = field.borderAppearance;
    painter.fillRect(box, colorFromPdf(ap.backgroundColorComponents,
                                       ap.backgroundColor, QColor(Qt::white)));

    // Layout constants after MuPDF's pdf_write_tx_widget_appearance: padding
    // is twice the border width (2pt for the usual 1pt border), a comb field
    // has none, and the text sits on MuPDF's baselines below.
    const bool multiline = field.isMultiline;
    const bool comb = !multiline && field.isComb && field.maxLen > 0;
    const float border = ap.hasBorderStyle ? sanitizeBorderWidth(ap.borderWidth) : 1.0f;
    const float padPt = comb ? 0.0f : 2.0f * border;
    const float contentWPt =
        std::max(0.0f, static_cast<float>(field.rect.width()) - 2.0f * padPt);
    const float contentHPt =
        std::max(0.0f, static_cast<float>(field.rect.height()) - 2.0f * padPt);

    QFont font = fontForDefaultAppearance(field.daFontName);
    // Clamp 1 of 2: the /DA size, again -- `field` may not have come through
    // FormManager. Non-finite or negative -> 0 (auto), cap 1000 pt.
    float sizePt = sanitizeDefaultAppearanceSize(field.daFontSize);
    if (sizePt <= 0.0f) {
        // /DA size 0 is auto-size. Like MuPDF: 12pt for multiline; otherwise
        // the field height, shrunk until the whole text fits the width.
        if (multiline) {
            sizePt = 12.0f;
        } else {
            sizePt = contentHPt;
            if (!text.isEmpty()) {
                QFont unit = font;
                unit.setPixelSize(100);
                if (!cache.autoValid || cache.autoFont != unit || cache.autoText != text) {
                    const QFontMetricsF um(unit);
                    qreal advance = 0.0;
                    if (text.size() <= kExactSingleLineLimit) {
                        advance = um.horizontalAdvance(text);
                    } else {
                        AdvanceTable table(um);
                        for (qsizetype i = 0; i < text.size();) {
                            const qsizetype next = nextCodepoint(text, i);
                            advance += table.at(text, i, next);
                            i = next;
                        }
                    }
                    cache.autoValid = true;
                    cache.autoFont = unit;
                    cache.autoText = text;
                    cache.autoPerPt = advance / 100.0;
                }
                const qreal perPt = cache.autoPerPt;
                if (perPt > 0.0)
                    sizePt = std::min(sizePt, static_cast<float>(contentWPt / perPt));
            }
        }
        sizePt = std::max(sizePt, 1.0f);
        sizePt = std::isfinite(sizePt) ? std::min(sizePt, kMaxDefaultAppearanceSizePt)
                                       : 12.0f;
    }
    // Clamp 2 of 2: the pixel size, size x zoom, clamped to
    // [1, kMaxEditFontPixelSize] while still a float -- qRound of NaN or of a
    // value outside int range is undefined behaviour. In range this is the
    // original max(1, qRound(sizePt * zoom)) bit for bit. Out of range the
    // layout follows the font actually used, so lines and baselines stay
    // consistent with the glyphs.
    const float pxF = sizePt * zoom;
    int pixelSize = 1;
    if (pxF >= static_cast<float>(kMaxEditFontPixelSize)) {
        pixelSize = kMaxEditFontPixelSize;   // also +inf
    } else if (pxF >= 1.0f) {
        pixelSize = std::max(1, qRound(pxF));
    }
    if (!(pxF >= 1.0f && pxF < static_cast<float>(kMaxEditFontPixelSize)))
        sizePt = static_cast<float>(pixelSize) / zoom;
    font.setPixelSize(pixelSize);
    stats.fontPixelSize = pixelSize;
    const QFontMetricsF fm(font);

    const qreal z = zoom;
    const qreal pad = padPt * z;
    const qreal contentLeft = box.left() + pad;
    const qreal contentW = std::max<qreal>(0.0, box.width() - 2.0 * pad);
    const QColor ink = colorFromPdf(field.daColorComponents, field.daColor,
                                    QColor(Qt::black));

    // The layout cache: reused when the composed text, the font, the width
    // and the mode are all unchanged; rebuilt (and so invalidated) otherwise.
    const bool reuse = cache.valid && cache.multiline == multiline &&
                       cache.width == contentW && cache.font == font &&
                       cache.text == text;
    if (!reuse) {
        cache.valid = false;
        cache.lines.clear();
        cache.prefix.clear();
        cache.multiline = multiline;
        cache.width = contentW;
        cache.font = font;
        cache.text = text;
    }
    stats.layoutReused = reuse;

    painter.save();
    painter.setClipRect(box);
    painter.setFont(font);
    painter.setPen(ink);

    auto drawUnderline = [&](qreal x0, qreal x1, qreal baseline) {
        if (x1 <= x0) return;
        QPen pen(ink);
        pen.setWidthF(std::max<qreal>(1.0, 0.75 * z));
        painter.save();
        painter.setPen(pen);
        const qreal y = baseline + std::max<qreal>(1.5, 0.5 * fm.descent());
        painter.drawLine(QPointF(x0, y), QPointF(x1, y));
        painter.restore();
    };

    // Draw text[lineStart, lineEnd) from `x0`, split where the composition
    // starts and ends: the buffer runs and the preedit run are separate draws,
    // and the preedit run is underlined. At most kMaxRunUnits units are drawn.
    auto drawRun = [&](qsizetype lineStart, qsizetype lineEnd, qreal x0,
                       qreal baseline) {
        if (lineEnd - lineStart > kMaxRunUnits) {
            lineEnd = lineStart + kMaxRunUnits;
            if (text[lineEnd].isLowSurrogate() && text[lineEnd - 1].isHighSurrogate())
                --lineEnd;
        }
        const qsizetype cuts[4] = {lineStart, std::clamp(preStart, lineStart, lineEnd),
                                   std::clamp(caretIdx, lineStart, lineEnd), lineEnd};
        for (int k = 0; k < 3; ++k) {
            const qsizetype a = cuts[k];
            const qsizetype b = cuts[k + 1];
            if (b <= a) continue;
            const qreal xa = x0 + fm.horizontalAdvance(text.mid(lineStart, a - lineStart));
            const QString run = text.mid(a, b - a);
            ++stats.runsDrawn;
            stats.unitsDrawn += b - a;
            if (k != 1) {
                painter.drawText(QPointF(xa, baseline), run);   // editBuffer_ run
                continue;
            }
            painter.drawText(QPointF(xa, baseline), run);       // preedit run
            const qreal xb = x0 + fm.horizontalAdvance(text.mid(lineStart, b - lineStart));
            drawUnderline(xa, xb, baseline);
        }
    };

    qreal caretX = contentLeft;
    qreal caretBaseline = box.top();

    // /Q quadding (0 left, 1 centred, 2 right): the shift of a run whose
    // advance leaves `freeWidth` of the content box unused. Comb fields
    // ignore it, as MuPDF's comb writer does.
    const int quad = field.quadding;
    auto quadOffset = [quad](qreal freeWidth) -> qreal {
        if (freeWidth <= 0.0) return 0.0;
        return quad == 1 ? freeWidth / 2.0 : quad == 2 ? freeWidth : 0.0;
    };

    // Glyph overhang allowance when deciding what can reach the field box.
    const qreal reach = 2.0 * fm.height();

    if (comb) {
        // PDF 32000-1 12.7.4.3: /MaxLen equal cells, one character centred
        // in each (MuPDF's write_comb_string). Phase-3 security audit: a
        // codepoint past /MaxLen has no cell and is not drawn (nor counted),
        // and when cells are narrower than a pixel (a hostile /MaxLen such
        // as 1e9) only the first cell starting in each pixel column is drawn
        // -- for any comb with cells a pixel or wider that is every cell.
        const qreal cellW = box.width() / field.maxLen;
        const qreal baseline = box.top() + (contentHPt / 2.0 + 0.3 * sizePt) * z;
        qsizetype cell = 0;
        qsizetype caretCell = 0;
        qsizetype preCell = 0;
        long long lastColumn = -1;
        for (qsizetype i = 0; i < text.size() && cell < field.maxLen; ++cell) {
            if (i < preStart) preCell = cell + 1;
            if (i < caretIdx) caretCell = cell + 1;
            const qsizetype next = nextCodepoint(text, i);
            const long long column = static_cast<long long>(std::floor(cell * cellW));
            if (column != lastColumn) {
                lastColumn = column;
                const QString ch = text.mid(i, next - i);
                const qreal adv = fm.horizontalAdvance(ch);
                const qreal x = box.left() + cell * cellW + (cellW - adv) / 2.0;
                painter.drawText(QPointF(x, baseline), ch);
                ++stats.runsDrawn;
                stats.unitsDrawn += next - i;
            }
            i = next;
        }
        if (caretCell > preCell)
            drawUnderline(box.left() + preCell * cellW,
                          box.left() + caretCell * cellW, baseline);
        caretX = box.left() + std::min<qsizetype>(caretCell, field.maxLen) * cellW;
        caretBaseline = baseline;
    } else if (multiline) {
        if (!reuse) {
            cache.lines = wrapLines(text, fm, contentW);
            cache.valid = true;
        }
        const auto& lines = cache.lines;
        const qreal lineH = 1.116 * sizePt * z;
        qreal firstBaseline = box.top() + (padPt + 1.116 * sizePt) * z;
        // The caret's line: the last line starting at or before it (line
        // starts strictly increase).
        const auto after = std::upper_bound(
            lines.begin(), lines.end(), caretIdx,
            [](qsizetype v, const std::pair<qsizetype, qsizetype>& l) { return v < l.first; });
        const std::size_t caretLine =
            after == lines.begin() ? 0 : static_cast<std::size_t>(after - lines.begin() - 1);
        // Scroll up so the caret's line stays inside the field.
        const qreal caretBottom = firstBaseline + caretLine * lineH + fm.descent();
        if (caretBottom > box.bottom() - pad)
            firstBaseline -= caretBottom - (box.bottom() - pad);
        // Each line's /Q origin, as write_string_with_quadding: the width it
        // aligns by excludes the space a soft wrap broke at (break_string
        // returns the advance before that space), but not a typed trailing
        // space at a hard end.
        auto lineX = [&](std::size_t k) {
            const auto [a, b] = lines[k];
            const bool softBreak = k + 1 < lines.size() && lines[k + 1].first == b &&
                                   b > a && text[b - 1] == QChar(' ');
            const qreal w = fm.horizontalAdvance(text.mid(a, (softBreak ? b - 1 : b) - a));
            return contentLeft + quadOffset(contentW - w);
        };
        // Only the lines that can put ink inside the box are drawn; the rest
        // were clipped away anyway.
        const double n = static_cast<double>(lines.size());
        double kFirst = 0.0;
        double kLast = n - 1.0;
        if (lineH > 0.0) {
            kFirst = std::max(0.0, std::floor((box.top() - reach - firstBaseline) / lineH));
            kLast = std::min(n - 1.0, std::ceil((box.bottom() + reach - firstBaseline) / lineH));
        }
        // Hard cap (phase-3 security audit, LOW): however the range above
        // came out, never draw more lines than the box, widened by the glyph
        // reach on both sides, can show at the smallest line height, plus 2.
        // A backstop only: with a sane range the cap is never reached.
        const qreal span = box.height() + 2.0 * reach;
        const double maxLinesDrawn =
            (std::isfinite(span) && std::isfinite(lineH))
                ? std::ceil(span / std::max(lineH, kMinLineHeightPx)) + 2.0
                : 2.0;
        kLast = std::min(kLast, kFirst + maxLinesDrawn - 1.0);
        stats.linesTotal = static_cast<long long>(lines.size());
        for (double kd = kFirst; kd <= kLast; kd += 1.0) {
            const auto k = static_cast<std::size_t>(kd);
            drawRun(lines[k].first, lines[k].second, lineX(k), firstBaseline + k * lineH);
            ++stats.linesDrawn;
        }
        const auto& cl = lines[caretLine];
        caretX = lineX(caretLine) +
                 fm.horizontalAdvance(text.mid(cl.first, caretIdx - cl.first));
        caretBaseline = firstBaseline + caretLine * lineH;
    } else if (text.size() <= kExactSingleLineLimit) {
        // Single line, vertically centred as MuPDF centres it, and placed by
        // /Q as write_variable_text places it: tx = (w - tw) / 2 or w - tw
        // inside the padded content box. Text wider than the box is not
        // aligned but scrolled left once the caret passes the right edge, so
        // the caret stays visible while typing.
        const qreal baseline = box.top() + (padPt + contentHPt / 2.0 + 0.3 * sizePt) * z;
        const qreal caretAdv = fm.horizontalAdvance(text.left(caretIdx));
        const qreal scroll = std::max<qreal>(0.0, caretAdv - contentW);
        const qreal x0 =
            contentLeft - scroll + quadOffset(contentW - fm.horizontalAdvance(text));
        drawRun(0, text.size(), x0, baseline);
        caretX = x0 + caretAdv;
        caretBaseline = baseline;
        stats.linesTotal = 1;
        stats.linesDrawn = 1;
    } else {
        // A single line too long to shape whole on every repaint (over
        // kExactSingleLineLimit units). Same placement as above, measured
        // with cached per-codepoint advance sums, and only the stretch of
        // text that can reach the field box is drawn.
        if (!reuse) {
            AdvanceTable table(fm);
            cache.prefix.assign(static_cast<std::size_t>(text.size()) + 1, 0.0);
            qreal sum = 0.0;
            for (qsizetype i = 0; i < text.size();) {
                const qsizetype next = nextCodepoint(text, i);
                for (qsizetype j = i + 1; j < next; ++j)
                    cache.prefix[static_cast<std::size_t>(j)] = sum;
                sum += table.at(text, i, next);
                cache.prefix[static_cast<std::size_t>(next)] = sum;
                i = next;
            }
            cache.valid = true;
        }
        const auto& P = cache.prefix;
        const qreal baseline = box.top() + (padPt + contentHPt / 2.0 + 0.3 * sizePt) * z;
        const qreal caretAdv = P[static_cast<std::size_t>(caretIdx)];
        const qreal scroll = std::max<qreal>(0.0, caretAdv - contentW);
        const qreal x0 = contentLeft - scroll + quadOffset(contentW - P.back());
        qsizetype i0 = static_cast<qsizetype>(
            std::upper_bound(P.begin(), P.end(), box.left() - reach - x0) - P.begin()) - 1;
        i0 = std::clamp<qsizetype>(i0, 0, text.size());
        if (i0 > 0 && i0 < text.size() && text[i0].isLowSurrogate() &&
            text[i0 - 1].isHighSurrogate())
            --i0;
        qsizetype i1 = static_cast<qsizetype>(
            std::lower_bound(P.begin(), P.end(), box.right() + reach - x0) - P.begin());
        i1 = std::clamp<qsizetype>(i1, i0, text.size());
        if (i1 > 0 && i1 < text.size() && text[i1].isLowSurrogate() &&
            text[i1 - 1].isHighSurrogate())
            ++i1;
        if (i1 > i0) drawRun(i0, i1, x0 + P[static_cast<std::size_t>(i0)], baseline);
        caretX = x0 + caretAdv;
        caretBaseline = baseline;
        stats.linesTotal = 1;
        stats.linesDrawn = i1 > i0 ? 1 : 0;
    }

    painter.restore();

    qreal top = std::max(box.top() + 1.0, caretBaseline - fm.ascent());
    qreal bottom = std::min(box.bottom() - 1.0, caretBaseline + fm.descent());
    if (bottom <= top) bottom = top + 1.0;
    stats.caret = QRectF(QPointF(caretX, top), QPointF(caretX + 1.0, bottom));
    return stats.caret;
}

/// Pipeline form-reconcile, decision 2: whether the at-rest overlay strokes an
/// outline around this widget. MuPDF draws a widget's border from /MK /BC at
/// the /BS /W width (pdf_annot_border_width: /BS /W, else 1), so a widget
/// with a border colour and a non-zero width already shows a border, and an
/// overlay outline on top would double it. Signature is the exception: an
/// unsigned signature's appearance (pdf_signature_appearance_unsigned)
/// ignores /MK, so it always gets the outline. The width comes through
/// FormManager's read sanitiser, and is sanitised again here because a
/// FormFieldData can be hand-built.
bool overlayOutlineNeeded(const FormFieldData& field) {
    if (field.type == FormFieldType::Signature) return true;
    const FormFieldBorderAppearance& a = field.borderAppearance;
    if (sanitizeColorComponentCount(a.borderColorComponents) == 0) return true;
    const float width = a.hasBorderStyle ? sanitizeBorderWidth(a.borderWidth) : 1.0f;
    return !(width > 0.0f);
}

/// Read QClipboard::text() with retries to absorb transient empty reads under
/// Windows RDP / remote sessions, where the OS briefly transfers clipboard
/// ownership to the session manager (rdpclip) during a read. A single ->text()
/// can then legitimately return "" and a genuine paste would silently no-op.
/// Mirrors TextEditTool::readClipboardTextWithRetry — the reference clipboard
/// path for this project (this runner is frequently RDP). Returns the first
/// non-empty value, or the empty string if every attempt failed.
QString readClipboardTextWithRetry(int maxAttempts = 5, int delayMs = 15) {
    QClipboard* cb = QGuiApplication::clipboard();
    if (!cb) return {};
    for (int i = 0; i < maxAttempts; ++i) {
        QString s = cb->text();
        if (!s.isEmpty()) return s;
        if (i + 1 < maxAttempts) {
            QCoreApplication::processEvents(
                QEventLoop::ExcludeUserInputEvents, delayMs);
            QThread::msleep(static_cast<unsigned long>(delayMs));
        }
    }
    return {};
}

} // namespace

// GitHub #243 — live-instance census. See FormTool::liveInstanceCountForTesting().
int FormTool::liveInstanceCount_ = 0;
int FormTool::setCreationFieldTypeCalls_ = 0;

FormTool::FormTool(FormManager* manager,
                   JavaScriptEngine* jsEngine,
                   pdfe::commands::UndoManager* undoManager)
    : manager_(manager)
    , jsEngine_(jsEngine)
    , undoManager_(undoManager)
{
    ++liveInstanceCount_;
}

FormTool::~FormTool()
{
    --liveInstanceCount_;
}

pdfe::annotations::AnnotationType FormTool::toolType() const {
    return pdfe::annotations::AnnotationType::Unknown;
}

void FormTool::activate() {
    selectedWidget_ = -1;
    isEditing_ = false;
    editBuffer_.clear();
    cursorPosition_ = 0;
    // Don't clear tabOrder_ here — setPage() populates it before activate()
    // is called by PdfCanvas::setCurrentTool().
    tabOrderPosition_ = -1;
    isDrawingRect_ = false;
    drawPageIndex_ = -1;
}

void FormTool::deactivate() {
    if (isEditing_) {
        commitEdit();
    }
    deselectField();

    // form-field-bugs phase-9, bug 10 — sticky placement is left on an EXPLICIT
    // action. Switching tools is one such action: tearing the FormTool down (or
    // swapping it out) ends placement mode, so drop the armed type and notify the
    // UI (uncheck the form field-type toolbar action) the same way Esc does. With
    // sticky placement this no longer fires after each placement, so it will not
    // spuriously uncheck the action while the user is still placing fields.
    if (isCreationMode()) {
        creationFieldType_ = FormFieldType::Unknown;
        if (creationModeExited) {
            creationModeExited();
        }
    }
    isDrawingRect_ = false;
    drawPageIndex_ = -1;
}

// GitHub #269. Was FormTool::setPage; split across the base class's two hooks
// so the assignment to page_ itself lives in AnnotationTool::setPage and cannot
// be skipped. The split is exactly where it was: commitEdit()/deselectField()
// concern the page being LEFT and must run before the pointer moves; tab order
// is derived FROM the new page and must run after.
void FormTool::onPageChangingImpl() {
    if (isEditing_) {
        commitEdit();
    }
    deselectField();
}

void FormTool::onPageChangedImpl() {
    if (page_) {
        tabOrder_ = manager_->getTabOrder(page_);
    } else {
        tabOrder_.clear();
    }
    tabOrderPosition_ = -1;
}

void FormTool::onDocumentClosingImpl() {
    // GitHub #235. Release the cached raw Page* BEFORE its document frees the
    // pages, so a repaint queued after the close cannot run page_->index() on
    // freed memory.
    //
    // DELIBERATELY NOT setPage(nullptr), and deliberately not commitEdit() or
    // cancelEdit(). All three touch page_ on the way past -- setPage() commits
    // an in-flight edit, and cancelEdit() fires a JS blur event through
    // `jsEngine_->fireBlurEvent(page_, ...)`. The page is being destroyed; the
    // one thing we must not do on the way out is dereference it. An edit in
    // flight when the document closes is discarded, which is the same thing
    // that happens to every other unsaved change at that moment.
    selectedWidget_ = -1;
    isEditing_ = false;
    editBuffer_.clear();
    preeditText_.clear();
    cursorPosition_ = 0;
    caretScreenRectValid_ = false;
    tabOrder_.clear();
    tabOrderPosition_ = -1;
}

void FormTool::onAnnotationsChanged() {
    // The document is still open and page_ is still valid -- only the field set
    // moved, so a cached widget index may now name a different field or none.
    // Keep page_; drop the index and any edit bound to it.
    selectedWidget_ = -1;
    isEditing_ = false;
    editBuffer_.clear();
    preeditText_.clear();
    cursorPosition_ = 0;
    caretScreenRectValid_ = false;
    if (page_ && manager_) {
        // Guarded: this runs from PdfCanvas::onPageContentChanged, i.e. inside
        // Qt event delivery, where an escaping exception is as fatal as one
        // from paintEvent. An empty tab order is a degraded but survivable
        // state; a terminated process is not.
        try {
            tabOrder_ = manager_->getTabOrder(page_);
        } catch (const std::exception&) {
            tabOrder_.clear();
        }
    }
    tabOrderPosition_ = -1;
}

void FormTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) {
    (void)event;

    if (!page_) return;

    auto hitResult = manager_->getFormFieldAt(page_, pdfPoint);

    // Creation mode: a press on EMPTY page space starts the rubber band for a
    // new field. A press on an EXISTING field falls through to the
    // interaction path below so the field can be selected and filled.
    //
    // Sticky placement (form-field-bugs phase-9, bug 10) keeps the tool armed
    // after a field is placed, and activating the Form Tool always arms Text
    // creation. Before this check the creation branch ran first and
    // unconditionally, so clicking the field the user had just placed only
    // started another rubber band (a short click creates nothing): a placed
    // field could not be typed into until the user found Esc or switched
    // tools. Placing a new field ON TOP of an existing one is the one gesture
    // this gives up; drag from empty space next to it instead.
    if (isCreationMode() && !hitResult.has_value()) {
        if (isEditing_) {
            commitEdit();
        }
        deselectField();
        drawStart_ = pdfPoint;
        drawEnd_ = pdfPoint;
        drawPageIndex_ = pageIndex;
        isDrawingRect_ = true;
        return;
    }

    // Interaction (or creation-mode press on a field): act on the field hit.

    if (hitResult.has_value()) {
        int widgetIdx = hitResult.value();

        if (widgetIdx != selectedWidget_) {
            // Commit any in-progress edit before switching
            if (isEditing_) {
                commitEdit();
            }

            auto fieldData = manager_->getFormField(page_, widgetIdx);

            if (fieldData.isReadOnly) {
                // Just select, don't edit
                selectField(widgetIdx);
                return;
            }

            switch (fieldData.type) {
                case FormFieldType::Checkbox:
                case FormFieldType::RadioButton: {
                    // Toggle immediately via command
                    auto cmd = std::make_unique<ToggleFieldCommand>(
                        manager_, page_, widgetIdx);
                    undoManager_->execute(std::move(cmd));
                    selectField(widgetIdx);
                    if (fieldValueChanged) {
                        auto updated = manager_->getFormField(page_, widgetIdx);
                        fieldValueChanged(page_->index(), updated);
                    }
                    break;
                }

                case FormFieldType::Text:
                    selectField(widgetIdx);
                    startEditing(widgetIdx);
                    break;

                case FormFieldType::ComboBox:
                case FormFieldType::ListBox:
                    selectField(widgetIdx);
                    // ComboBox/ListBox popup handled by MainWindow or FormFieldPropertiesPanel
                    break;

                case FormFieldType::PushButton:
                    selectField(widgetIdx);
                    // Fire JS button action if JS is enabled
                    if (jsEngine_ && jsEngine_->isEnabled()) {
                        jsEngine_->fireFocusEvent(page_, widgetIdx);
                        jsEngine_->fireBlurEvent(page_, widgetIdx);
                    }
                    break;

                case FormFieldType::Signature:
                    // GitHub #328 — placement set /FT /Sig, but there was no
                    // arm here, so a click on a signature field fell through
                    // to `default:` and only selected it: nothing on the
                    // canvas ever reached the signing flow. Select it (so the
                    // highlight/properties follow the click), then report the
                    // activation. MainWindow owns the SignatureManager and the
                    // document and runs the dialog/save/sign sequence against
                    // THIS field.
                    selectField(widgetIdx);
                    if (signatureFieldActivated) {
                        signatureFieldActivated(page_->index(), widgetIdx);
                    }
                    break;

                default:
                    selectField(widgetIdx);
                    break;
            }
        } else {
            // GitHub #369 — re-click on the ALREADY-SELECTED widget. The
            // `widgetIdx != selectedWidget_` gate above made a second click on
            // a selected Signature field completely inert, so a user who
            // selected the field (or had just placed it) could never reach the
            // signing flow by clicking it again. Re-fire the activation for
            // Signature fields only; other field types keep their established
            // re-click behavior (text edit stays in progress, checkbox
            // toggling already runs through the first-click path, etc.).
            try {
                auto fieldData = manager_->getFormField(page_, widgetIdx);
                if (fieldData.type == FormFieldType::Signature &&
                    !fieldData.isReadOnly && signatureFieldActivated) {
                    signatureFieldActivated(page_->index(), widgetIdx);
                }
            } catch (const std::exception&) {
                // Stale index — treat as a no-op, like the paint guard does.
            }
        }
    } else {
        // Clicked outside any field
        if (isEditing_) {
            commitEdit();
        }
        deselectField();
    }
}

void FormTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) {
    (void)event;

    // Update rubber-band rectangle during drawing
    if (isDrawingRect_ && pageIndex == drawPageIndex_) {
        drawEnd_ = pdfPoint;
    }
}

void FormTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) {
    (void)event;
    (void)pdfPoint;

    // Finalize field creation
    if (isDrawingRect_) {
        isDrawingRect_ = false;

        // Build normalized rectangle
        qreal left = std::min(drawStart_.x(), drawEnd_.x());
        qreal top = std::min(drawStart_.y(), drawEnd_.y());
        qreal right = std::max(drawStart_.x(), drawEnd_.x());
        qreal bottom = std::max(drawStart_.y(), drawEnd_.y());
        QRectF rect(left, top, right - left, bottom - top);

        // Only create if rectangle is large enough (minimum size)
        if (rect.width() >= 10.0 && rect.height() >= 10.0) {
            if (fieldCreated) {
                fieldCreated(drawPageIndex_, creationFieldType_, rect);

                // form-field-bugs phase-9, bug 10 — STICKY placement. The tool
                // STAYS ARMED after a successful placement (creationFieldType_
                // is left unchanged, creationModeExited does NOT fire) so the
                // next drag places ANOTHER field without re-clicking the toolbar
                // button — the Acrobat-like behaviour the user asked for, and
                // the same behaviour every annotation tool (ShapeTool, InkTool,
                // StampTool, …) already has (they reset only per-gesture drag
                // state, never their armed type).
                //
                // This REPLACES the GitHub #369 auto-exit that used to run here.
                // #369's real requirement — that a click can still REACH a
                // placed field (select / activate it) rather than forever
                // re-entering the rubber-band — is preserved, but via the
                // EXPLICIT exit path: Esc (phase-8 cancelInProgress) or switching
                // tools leaves creation mode (fires creationModeExited → the
                // toolbar action unchecks), and THEN interaction-mode clicks
                // select/activate as #369 intended. Leaving placement mode is now
                // a deliberate user action, never an automatic side effect of
                // placing one field.
            }
        }
    }
}

bool FormTool::cancelInProgress() {
    // form-field-bugs phase-8, bug 6: abort an in-progress field placement.
    // Two overlapping states can be live: a started rubber-band drag
    // (isDrawingRect_, between onMousePress and onMouseRelease) and armed
    // creation mode (isCreationMode(), the field-type toolbar action is down).
    // Cancel both: drop the rubber-band so a pending onMouseRelease finalizes
    // nothing, and leave creation mode — reusing the existing #369
    // creationModeExited callback so the toolbar action unchecks. No partial
    // field is ever created (fieldCreated only fires from onMouseRelease, which
    // we have now disarmed).
    if (!isDrawingRect_ && !isCreationMode()) {
        return false;
    }
    isDrawingRect_ = false;
    drawPageIndex_ = -1;
    if (isCreationMode()) {
        creationFieldType_ = FormFieldType::Unknown;
        if (creationModeExited) {
            creationModeExited();
        }
    }
    return true;
}

void FormTool::onKeyPress(QKeyEvent* event) {
    if (!page_) return;

    // form-field-bugs phase-6, bug-4 (delete half): a field SELECTED but NOT
    // being edited is removed by Delete/Backspace. Before this, onKeyPress
    // handled Delete/Backspace ONLY inside the isEditing_ text path below (to
    // erase a character), so there was no keyboard route to remove a placed
    // field once it was selected — DeleteFormFieldCommand's only trigger was the
    // toolbar's Delete Field button (#253). A non-text field reaches
    // "selected, not editing" with a single click; a text field reaches it by
    // Escape (which exits the edit but keeps the selection), so Escape-then-
    // Delete removes a placed text field. This runs BEFORE the isEditing_ text
    // path, and only when !isEditing_, so an in-progress edit still gets
    // per-character Delete/Backspace.
    if (!isEditing_ && selectedWidget_ >= 0 &&
        (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        const int widgetIndex = selectedWidget_;
        bool handled = false;
        if (deleteSelectedField) {
            // Host path: parks the Page in commandPages_ so the delete command
            // joins the same drain+rebind lifetime machinery every other command
            // site uses, and an undo of the delete cannot dangle (bug-5 class).
            handled = deleteSelectedField(page_->index(), widgetIndex);
        }
        if (!handled) {
            // Standalone / unit context with no host registry: execute directly
            // against page_. (page_ is the caller-owned Page; in production the
            // host path above is what keeps it alive across a save.)
            auto cmd = std::make_unique<DeleteFormFieldCommand>(
                manager_, page_, widgetIndex);
            undoManager_->execute(std::move(cmd));
        }
        // The field is gone; drop the now-stale selection/edit state so the next
        // repaint or hit-test cannot dereference a removed widget index.
        deselectField();
        return;
    }

    if (event->key() == Qt::Key_Tab) {
        if (isEditing_) {
            commitEdit();
        }
        if (event->modifiers() & Qt::ShiftModifier) {
            tabToPreviousField();
        } else {
            tabToNextField();
        }
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        // form-field-bugs phase-8, bug 6: FIRST abort any in-progress placement
        // (rubber-band drag or armed creation mode). Only when nothing is being
        // placed does Escape fall through to the edit/selection handling (exit an
        // edit, else deselect) it had before.
        if (cancelInProgress()) {
            return;
        }
        if (isEditing_) {
            cancelEdit();
        } else {
            deselectField();
        }
        return;
    }

    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (isEditing_) {
            auto fieldData = manager_->getFormField(page_, selectedWidget_);
            if (fieldData.isMultiline) {
                handleTextInput("\n");
            } else {
                commitEdit();
            }
        }
        return;
    }

    // Clipboard shortcuts (form-field-bugs phase-4, bug-11: cannot copy/paste
    // into a form text field). onKeyPress had NO Qt::ControlModifier branch, so
    // Ctrl+V/C/X/A fell through to the printable-text handler below — and the
    // control characters they emit (event->text() == "\x16" for Ctrl+V, etc.)
    // are not isPrint(), so they were silently dropped: paste did nothing.
    //
    // Only PASTE is wired. FormTool has no selection model (there is only a
    // single caret, cursorPosition_ — no selectionStart/End as in TextEditTool),
    // so Copy / Cut / Select-All have nothing to act on. Rather than ship a
    // half-built selection model, they are deliberately scoped out here as
    // explicit no-ops (and still consumed, so no stray control glyph reaches the
    // buffer). Paste is the reported bug and the required fix; adding a form-
    // field selection model is a separate, larger piece of work.
    if (isEditing_ && (event->modifiers() & Qt::ControlModifier)) {
        switch (event->key()) {
        case Qt::Key_V: {
            // Resolve the field first: a read-only field must refuse the paste
            // (the reachable permission gate in FormTool — a read-only field is
            // also never put into edit mode by onMousePress, so this is
            // defence-in-depth). A stale widget index is treated as a no-op,
            // like the paint/hit guards elsewhere in this file.
            FormFieldData fieldData;
            try {
                fieldData = manager_->getFormField(page_, selectedWidget_);
            } catch (const std::exception&) {
                return;
            }
            if (fieldData.isReadOnly) return;

            QString pasted = readClipboardTextWithRetry();
            if (pasted.isEmpty()) return;

            if (fieldData.isMultiline) {
                // Normalize CR / CRLF to LF so the field value never holds a
                // stray carriage return; line breaks are legal here.
                pasted.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
                pasted.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            } else {
                // A single-line field cannot represent a line break; collapse
                // any run of newline characters to a single space so a
                // multi-line clipboard payload lands as one line rather than
                // being truncated at the first newline (or corrupting the /V).
                pasted.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                               QStringLiteral(" "));
            }
            if (pasted.isEmpty()) return;

            // Insert at the caret through the same handleTextInput() the typed
            // path uses: it preserves the UTF-8 codepoint-boundary invariant
            // (WIN-49) and advances cursorPosition_.
            handleTextInput(pasted);
            return;
        }
        case Qt::Key_C:
        case Qt::Key_X:
        case Qt::Key_A:
            // Scoped out — no selection model (see block comment above).
            return;
        default:
            // Other Ctrl combos are not ours; fall through so nothing inserts
            // a control character (event->text() for them is not isPrint()).
            return;
        }
    }

    // Text input
    if (isEditing_) {
        // CODEPOINT BOUNDARIES, NOT BYTES (WIN-49 prerequisite).
        //
        // cursorPosition_ remains a BYTE offset -- see src/core/Utf8.h for why
        // -- but every movement and erase below steps a whole codepoint. The
        // previous code erased and stepped exactly ONE BYTE, so a single
        // Backspace over any non-ASCII character (every CJK glyph, every
        // accented Latin letter) split its multi-byte sequence and left
        // INVALID UTF-8 in editBuffer_, which commitEdit() then writes into
        // the PDF field value.
        if (event->key() == Qt::Key_Backspace) {
            cursorPosition_ = static_cast<int>(pdfe::core::utf8::erasePrev(
                editBuffer_, static_cast<std::size_t>(cursorPosition_)));
        } else if (event->key() == Qt::Key_Delete) {
            pdfe::core::utf8::eraseAt(editBuffer_,
                                      static_cast<std::size_t>(cursorPosition_));
        } else if (event->key() == Qt::Key_Left) {
            cursorPosition_ = static_cast<int>(pdfe::core::utf8::prevBoundary(
                editBuffer_, static_cast<std::size_t>(cursorPosition_)));
        } else if (event->key() == Qt::Key_Right) {
            cursorPosition_ = static_cast<int>(pdfe::core::utf8::nextBoundary(
                editBuffer_, static_cast<std::size_t>(cursorPosition_)));
        } else if (event->key() == Qt::Key_Home) {
            cursorPosition_ = 0;
        } else if (event->key() == Qt::Key_End) {
            cursorPosition_ = static_cast<int>(editBuffer_.size());
        } else {
            QString text = event->text();
            if (!text.isEmpty() && text[0].isPrint()) {
                handleTextInput(text);
            }
        }
    }
}

void FormTool::onPaintOverlay(QPainter& painter, int pageIndex,
                               float zoom, QPointF pageOrigin, float pageHeightPt) {
    (void)pageHeightPt;

    // The live edit layout lives only while an edit is painted: drop it (and
    // a megabyte-sized value's text with it) as soon as no edit is active.
    if (!isEditing_) editLayoutCache_.reset();

    // Draw rubber-band rectangle during field creation
    if (isDrawingRect_ && pageIndex == drawPageIndex_) {
        painter.save();

        // Set pen for rubber-band effect
        QPen pen(QColor(0, 120, 215));
        pen.setWidthF(2.0);
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(QColor(0, 120, 215, 30));

        // Build normalized rectangle
        qreal left = std::min(drawStart_.x(), drawEnd_.x());
        qreal top = std::min(drawStart_.y(), drawEnd_.y());
        qreal right = std::max(drawStart_.x(), drawEnd_.x());
        qreal bottom = std::max(drawStart_.y(), drawEnd_.y());

        // Convert to screen coordinates
        float screenLeft = static_cast<float>(pageOrigin.x()) + static_cast<float>(left) * zoom;
        float screenTop = static_cast<float>(pageOrigin.y()) + static_cast<float>(top) * zoom;
        float screenWidth = static_cast<float>(right - left) * zoom;
        float screenHeight = static_cast<float>(bottom - top) * zoom;

        painter.drawRect(QRectF(screenLeft, screenTop, screenWidth, screenHeight));
        painter.restore();
        return;
    }

    // form-field-bugs phase-3, bug-1 (form fields invisible at rest). Before
    // this phase, onPaintOverlay drew an overlay ONLY for the single SELECTED
    // widget — every unselected field got nothing, so a freshly placed / empty
    // field whose MuPDF /AP is blank or transparent was completely invisible
    // until it happened to be selected. Acrobat shows every form field with a
    // tinted fill + outline at rest; draw that cue for EVERY widget on the
    // page, independent of selection. The selected field's stronger highlight
    // (and any live edit text) is drawn ON TOP, afterwards, so it still stands
    // out.
    //
    // Type-agnostic by construction: the loop draws the outline+tint from each
    // widget's rect regardless of field type, so it covers Text, Checkbox,
    // RadioButton, ComboBox, ListBox and — most importantly — Signature, which
    // is created with NO /AP at all (FormManager.cpp:961-963) and therefore has
    // the overlay outline as its ONLY at-rest cue.
    //
    // Note on sibling tools: the free-text-box / FreeText annotation tool does
    // NOT share this defect and needs no change here — FreeTextTool creates its
    // annotation with an opaque white /C background + a visible "Text" /Contents
    // and bakes an /AP immediately (FreeTextTool.cpp:37-43), so it renders at
    // rest through MuPDF's appearance stream, a different render path than this
    // overlay. Only FormTool's widgets relied on the selected-only overlay.
    if (!page_) return;
    if (page_->index() != pageIndex) return;

    // GitHub #234's sibling, in this tool. getFormFields()/getFormField() go
    // through MuPdfContext::execute(), which converts ANY fz error into a C++
    // MuPdfException -- and a widget index/page that no longer resolves is
    // exactly such an error. We are inside QWidget::paintEvent, where an
    // escaping exception terminates the process.
    //
    // Note this is a DIFFERENT failure from the access violation that #235 is
    // about. That one was `page_->index()` above dereferencing a freed page and
    // is fixed by onDocumentClosing() releasing the pointer; no try/catch can
    // help there, because an access violation is not a C++ exception. This
    // guard covers the throwing half of the same paint path.

    // At-rest pass: a subtle tint for every widget rect, plus a light outline
    // for every widget that does not draw a border of its own. The only
    // per-repaint work added is the widget-rect enumeration (the same
    // pdf_first_widget/next walk the hit-test uses).
    //
    // Reconciled with GitHub #458 (pipeline form-reconcile, decision 2): a
    // widget with an /MK /BC border colour and a non-zero border width already
    // has its border drawn by MuPDF from /MK and /BS -- every field the app
    // creates, except a Signature, gets one. Stroking the overlay outline over
    // it would draw a second, differently coloured rectangle on the same edge.
    // So such a widget gets the tint only (the "this is a fillable field" cue,
    // like Acrobat's field highlight), and the outline is kept for the widgets
    // that have no border of their own -- a loaded field without /MK /BC, or
    // with /BS /W 0 -- and for every Signature, whose unsigned appearance
    // ignores /MK. See overlayOutlineNeeded().
    {
        std::vector<FormFieldData> allFields;
        try {
            allFields = manager_->getFormFields(page_);
        } catch (const std::exception&) {
            allFields.clear();
        }
        if (!allFields.empty()) {
            painter.save();
            QPen restPen(QColor(90, 110, 140, 200));
            restPen.setWidthF(1.0);
            painter.setBrush(QColor(0, 120, 215, 20));
            for (int i = 0; i < static_cast<int>(allFields.size()); ++i) {
                // The selected field gets its stronger highlight below; don't
                // double-paint it here.
                if (i == selectedWidget_) continue;
                const QRectF& r = allFields[i].rect;
                float rx = static_cast<float>(pageOrigin.x()) +
                           static_cast<float>(r.x()) * zoom;
                float ry = static_cast<float>(pageOrigin.y()) +
                           static_cast<float>(r.y()) * zoom;
                float rw = static_cast<float>(r.width()) * zoom;
                float rh = static_cast<float>(r.height()) * zoom;
                if (overlayOutlineNeeded(allFields[i])) {
                    painter.setPen(restPen);
                } else {
                    painter.setPen(Qt::NoPen);
                }
                painter.drawRect(QRectF(rx, ry, rw, rh));
            }
            painter.restore();
        }
    }

    // Draw selection highlight for the selected field (drawn on top of the
    // at-rest pass so it remains visually distinct).
    if (selectedWidget_ < 0) return;

    FormFieldData fieldData;
    try {
        fieldData = manager_->getFormField(page_, selectedWidget_);
    } catch (const std::exception&) {
        // Stale widget index: drop it rather than re-throwing on every repaint.
        selectedWidget_ = -1;
        isEditing_ = false;
        caretScreenRectValid_ = false;
        return;
    }

    // Convert fitz-space rect to screen coordinates (both Y-down, no flip needed)
    QRectF pdfRect = fieldData.rect;
    float x = static_cast<float>(pageOrigin.x()) + static_cast<float>(pdfRect.x()) * zoom;
    float y = static_cast<float>(pageOrigin.y()) + static_cast<float>(pdfRect.y()) * zoom;
    float w = static_cast<float>(pdfRect.width()) * zoom;
    float h = static_cast<float>(pdfRect.height()) * zoom;

    QRectF screenRect(x, y, w, h);

    painter.save();

    if (isEditing_ && fieldData.type == FormFieldType::Text) {
        // GitHub #459 (closes IME-FORM-PREEDIT-PAINT-01). Paint the edit
        // itself -- opaque background, editBuffer_ and the underlined preedit
        // in the field's /DA font, size and colour -- because the page raster
        // underneath is the appearance stream, stale until commitEdit(). The
        // document is NOT written here: the value still reaches the PDF only
        // through commitEdit()'s SetFieldValueCommand, so undo and the JS
        // keystroke/blur/calculate sequence are unchanged.
        if (!editLayoutCache_) editLayoutCache_ = std::make_unique<detail::EditLayoutCache>();
        const QRectF caret = paintFieldEdit(painter, fieldData, screenRect, zoom,
                                            editBuffer_, cursorPosition_,
                                            preeditText_, *editLayoutCache_,
                                            lastEditPaintStats_);

        // Selection outline, without the translucent fill, so the field shows
        // its own background while it is being typed into.
        painter.setPen(QPen(QColor(0, 120, 215), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(screenRect);

        // The caret, at the text advance of everything before it (measured
        // with QFontMetricsF in the field's font, so it is exact for every
        // script rather than a uniform-width estimate).
        // A null caret means the paint refused hostile geometry (non-finite
        // or absurd zoom / field box): no caret is drawn or reported.
        if (!caret.isNull()) {
            painter.setPen(QPen(colorFromPdf(fieldData.daColorComponents,
                                             fieldData.daColor, QColor(Qt::black)),
                                1));
            painter.drawLine(QPointF(caret.left(), caret.top()),
                             QPointF(caret.left(), caret.bottom()));
        }

        // Record the caret we just DREW, for Qt::ImCursorRectangle (phase-76,
        // criterion 1). Recording the drawn position rather than deriving a
        // second one keeps the IME candidate window exactly where the caret
        // appears. Width 1 so a platform reading an empty rect as "no
        // position" does not discard it.
        caretScreenRect_ = caret;
        caretScreenRectValid_ = !caret.isNull();
    } else {
        // Draw selection highlight
        painter.setPen(QPen(QColor(0, 120, 215), 2));
        painter.setBrush(QColor(0, 120, 215, 30));
        painter.drawRect(screenRect);
    }

    painter.restore();
}

void FormTool::selectField(int widgetIndex) {
    if (selectedWidget_ != widgetIndex) {
        selectedWidget_ = widgetIndex;

        // Update tab order position
        auto it = std::find(tabOrder_.begin(), tabOrder_.end(), widgetIndex);
        if (it != tabOrder_.end()) {
            tabOrderPosition_ = static_cast<int>(std::distance(tabOrder_.begin(), it));
        }

        if (fieldSelected) {
            fieldSelected(widgetIndex);
        }
    }
}

void FormTool::deselectField() {
    if (selectedWidget_ >= 0) {
        selectedWidget_ = -1;
        isEditing_ = false;
        editBuffer_.clear();
        cursorPosition_ = 0;
        // NO preedit clear here, deliberately. The first pass at phase-76 added
        // one, reasoning that this is a fourth exit from editing. Mutation M10
        // disabled it and SURVIVED: every caller that can reach deselectField()
        // with an edit in progress -- deactivate(), setPage(), onMousePress
        // outside any field, Escape while editing, and setCreationFieldType()
        // -- calls commitEdit() or cancelEdit() first, and both of those clear
        // it. (The phase-23 karen gate caught this list omitting the fifth
        // caller; a comment that enumerates call sites is exactly the kind that
        // rots, so it is worth re-deriving rather than trusting.) A composition can
        // therefore never be live here, and a clear that no input can exercise
        // is decoration (the DCA 0039 lesson, applied to this phase's own fix).
        // What actually holds the invariant is those commit/cancel calls, and
        // that is what EveryExitFromEditingClearsTheComposition pins.
        tabOrderPosition_ = -1;

        if (fieldDeselected) {
            fieldDeselected();
        }
    }
}

void FormTool::startEditing(int widgetIndex) {
    auto fieldData = manager_->getFormField(page_, widgetIndex);
    isEditing_ = true;
    editBuffer_ = fieldData.value;
    cursorPosition_ = static_cast<int>(editBuffer_.size());
    preeditText_.clear();
    // A new edit starts from a fresh layout (phase-3 security audit: the
    // cache is keyed on its inputs anyway; this just frees the old one).
    editLayoutCache_.reset();
    // Drop the previous field's recorded caret HERE, on entry, rather
    // than at each exit. Entry is one site and always runs; the exits
    // are several and the set grows. Until this field paints, the
    // accessor reports nothing, so the IME never sees the old field's
    // position. (Invalidating at the exits AS WELL made the property
    // doubly-enforced, and mutation M12 then survived removing either
    // one -- the phase-75 M3 shape.)
    caretScreenRectValid_ = false;

    // Fire focus JS event
    if (jsEngine_ && jsEngine_->isEnabled()) {
        jsEngine_->fireFocusEvent(page_, widgetIndex);
    }
}

void FormTool::commitEdit() {
    if (!isEditing_ || selectedWidget_ < 0 || !page_) return;

    auto fieldData = manager_->getFormField(page_, selectedWidget_);
    if (editBuffer_ != fieldData.value) {
        // form-field-bugs phase-2, bug-5 (undo-after-edit crash). Do NOT
        // execute an unparked command here. A SetFieldValueCommand built with
        // page_ caches the single-slot canvas/MainWindow toolPage_, which is
        // freed on a page/tool switch or the full-rewrite save that field
        // creation forces — leaving this stacked command's page_ dangling so a
        // later undo() dereferences freed memory (page_->fzPage() on a freed
        // Page → access violation, the #234/#235/#360 use-after-free class).
        // Delegate to the host, which parks the Page in its command-page
        // registry so the command joins the SAME drain+rebind lifetime
        // machinery every other command site uses (and the panel-edit path
        // MainWindow::onFormFieldValueChanged already uses). Fall back to a
        // direct execute only when no host is wired (standalone/unit contexts).
        bool handled = false;
        if (commitFieldValue) {
            handled = commitFieldValue(page_->index(), selectedWidget_, editBuffer_);
        }
        if (!handled) {
            auto cmd = std::make_unique<SetFieldValueCommand>(
                manager_, page_, selectedWidget_, editBuffer_);
            undoManager_->execute(std::move(cmd));
        }

        if (fieldValueChanged) {
            auto updated = manager_->getFormField(page_, selectedWidget_);
            fieldValueChanged(page_->index(), updated);
        }
    }

    // Fire blur JS event then run all calculate handlers.
    // pdf_annot_event_blur fires /AA/Bl (Blur) and /AA/V (Validate via
    // pdf_annot_event_blur's internal chain on some MuPDF builds).
    // pdf_calculate_form propagates /AA/C (Calculate) on all dependent
    // fields (those listed in /CO on the AcroForm dictionary) so that
    // calculated fields like "Total" or "RESULT-<Source>" are updated
    // whenever a source field is committed.
    if (jsEngine_ && jsEngine_->isEnabled()) {
        jsEngine_->fireBlurEvent(page_, selectedWidget_);
        jsEngine_->calculateAll(page_);
    }

    isEditing_ = false;
    editBuffer_.clear();
    cursorPosition_ = 0;
    // An uncommitted composition is abandoned with the edit; it was never in
    // editBuffer_, so the committed value above cannot contain it.
    preeditText_.clear();
}

void FormTool::commitValueEditForTesting(int widgetIndex, const std::string& newValue) {
    // Mirror the state the real onMousePress→startEditing→onKeyPress sequence
    // leaves before a commit fires (selected widget, editing, buffer loaded),
    // then run the SAME commitEdit() the live edit loop calls. This exercises
    // the production commit/park wiring (the bug-5 commitFieldValue delegation)
    // deterministically, with no synthesized Qt events or page geometry.
    selectedWidget_ = widgetIndex;
    isEditing_ = true;
    editBuffer_ = newValue;
    cursorPosition_ = static_cast<int>(newValue.size());
    preeditText_.clear();
    commitEdit();
}

void FormTool::cancelEdit() {
    isEditing_ = false;
    editBuffer_.clear();
    cursorPosition_ = 0;
    preeditText_.clear();

    // Fire blur JS event
    if (jsEngine_ && jsEngine_->isEnabled() && selectedWidget_ >= 0) {
        jsEngine_->fireBlurEvent(page_, selectedWidget_);
    }
}

void FormTool::handleTextInput(const QString& text) {
    const std::string textStr = text.toStdString();
    // Insert at a codepoint boundary. cursorPosition_ should already be on one
    // -- every movement above keeps it there -- but setFieldValue() and
    // beginEdit() also assign it, and a value arriving from elsewhere could
    // leave it mid-sequence. Aligning here means a stale cursor degrades to
    // "inserted one character earlier" instead of "produced invalid UTF-8".
    const std::size_t at = pdfe::core::utf8::alignLeft(
        editBuffer_, static_cast<std::size_t>(cursorPosition_));
    editBuffer_.insert(at, textStr);
    cursorPosition_ = static_cast<int>(at + textStr.size());
}

// ─── Input-method (IME) support — WIN-49, follow-up phase-76 ───────────────

void FormTool::setPreeditText(const QString& text) {
    if (!isEditing_) {
        preeditText_.clear();
        return;
    }
    // NOTE: editBuffer_ is deliberately NOT touched here. See FormTool.h.
    preeditText_ = text.toStdString();
}

void FormTool::clearPreedit() {
    preeditText_.clear();
}

void FormTool::commitPreedit(const QString& text,
                             int replaceFrom, int replaceLength) {
    if (!isEditing_) return;

    preeditText_.clear();

    if (replaceLength > 0) {
        // replaceFrom/replaceLength are in CODEPOINTS relative to the caret,
        // while cursorPosition_ is a BYTE offset (src/core/Utf8.h explains
        // why), so walk boundaries rather than doing byte arithmetic — the
        // exact class of bug phase-74 fixed for Backspace.
        std::size_t start = pdfe::core::utf8::alignLeft(
            editBuffer_, static_cast<std::size_t>(cursorPosition_));
        for (int i = 0; i < replaceFrom; ++i)
            start = pdfe::core::utf8::nextBoundary(editBuffer_, start);
        for (int i = 0; i > replaceFrom; --i)
            start = pdfe::core::utf8::prevBoundary(editBuffer_, start);

        cursorPosition_ = static_cast<int>(start);
        for (int i = 0; i < replaceLength; ++i) {
            if (start >= editBuffer_.size()) break;
            pdfe::core::utf8::eraseAt(editBuffer_, start);
        }
    }

    if (!text.isEmpty()) {
        // handleTextInput keeps the codepoint-boundary invariant on insert.
        handleTextInput(text);
    }
}

FormTool::EditPaintStats FormTool::paintFieldEditForTesting(
    QPainter& painter, const FormFieldData& field, const QRectF& box, float zoom,
    const std::string& buffer, int cursorByte, const std::string& preedit) {
    detail::EditLayoutCache cache;
    EditPaintStats stats;
    paintFieldEdit(painter, field, box, zoom, buffer, cursorByte, preedit, cache, stats);
    return stats;
}

bool FormTool::caretScreenRect(QRectF& out) const {
    if (!caretScreenRectValid_ || !isEditing_)
        return false;
    out = caretScreenRect_;
    return true;
}

int FormTool::cursorPositionForIme() const {
    const std::size_t at = pdfe::core::utf8::alignLeft(
        editBuffer_, static_cast<std::size_t>(std::max(0, cursorPosition_)));
    return static_cast<int>(
        pdfe::core::utf8::codepointCount(editBuffer_.substr(0, at)));
}

void FormTool::tabToNextField() {
    if (tabOrder_.empty()) return;

    tabOrderPosition_++;
    if (tabOrderPosition_ >= static_cast<int>(tabOrder_.size())) {
        tabOrderPosition_ = 0;
    }

    int nextWidget = tabOrder_[tabOrderPosition_];
    selectField(nextWidget);

    auto fieldData = manager_->getFormField(page_, nextWidget);
    if (fieldData.type == FormFieldType::Text && !fieldData.isReadOnly) {
        startEditing(nextWidget);
    }
}

void FormTool::tabToPreviousField() {
    if (tabOrder_.empty()) return;

    tabOrderPosition_--;
    if (tabOrderPosition_ < 0) {
        tabOrderPosition_ = static_cast<int>(tabOrder_.size()) - 1;
    }

    int prevWidget = tabOrder_[tabOrderPosition_];
    selectField(prevWidget);

    auto fieldData = manager_->getFormField(page_, prevWidget);
    if (fieldData.type == FormFieldType::Text && !fieldData.isReadOnly) {
        startEditing(prevWidget);
    }
}

FormFieldData FormTool::selectedFieldData() const {
    if (selectedWidget_ >= 0 && page_) {
        return manager_->getFormField(page_, selectedWidget_);
    }
    return FormFieldData();
}

void FormTool::syncEditBuffer(const std::string& value)
{
    // Only update when a field is actively selected and being edited.
    // This prevents a stale editBuffer_ from overwriting a value that was
    // committed via the panel pipe (setTextValueForTesting path).
    if (isEditing_) {
        editBuffer_ = value;
        cursorPosition_ = static_cast<int>(editBuffer_.size());
    }
}

void FormTool::cancelCurrentEdit()
{
    cancelEdit();
}

void FormTool::setCreationFieldType(FormFieldType type) {
    // GitHub #243 — counted on ENTRY, before anything touches `this`, so a call
    // arriving through a stale connection is still counted. That is the whole
    // point: the defect was extra invocations, not a wrong final value.
    ++setCreationFieldTypeCalls_;

    // Exit editing mode if switching to/from creation mode
    if (isEditing_) {
        commitEdit();
    }
    deselectField();

    creationFieldType_ = type;
    isDrawingRect_ = false;
}

} // namespace pdfe::forms
