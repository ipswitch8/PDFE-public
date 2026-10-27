#include "tre/GlyphAtlasCompositor.h"
#include "tre/GlyphAtlas.h"
#include "tre/EnrichedAtlas.h"
#include "tre/GlyphSynthesizer.h"
#include "tre/BitmapCompositor.h"

#include <QDebug>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace pdfe::tre {

namespace {

/// Crop a transparent ARGB32 image to its visible (alpha > 0) bounding box.
/// Returns null QImage if no visible pixels exist.
QImage cropToContent(const QImage& img) {
    if (img.isNull())
        return {};

    int top = img.height(), bottom = -1, left = img.width(), right = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(line[x]) > 0) {
                if (y < top) top = y;
                if (y > bottom) bottom = y;
                if (x < left) left = x;
                if (x > right) right = x;
            }
        }
    }
    if (top > bottom || left > right)
        return {};
    return img.copy(left, top, right - left + 1, bottom - top + 1);
}

} // anonymous namespace

QImage GlyphAtlasCompositor::compose(
    const std::u32string& text,
    const GlyphAtlas& atlas,
    const FontMatch& fontMatch,
    const RgbColor& bgColor,
    int targetWidth,
    int targetHeight,
    int dpi,
    BitmapCompositor* bitmapCompositor) const
{
    if (text.empty() || targetWidth <= 0 || targetHeight <= 0)
        return {};

    // Create transparent output bitmap
    QImage output(targetWidth, targetHeight, QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);

    // Calculate average glyph height from atlas for baseline positioning
    int totalGlyphH = 0;
    int glyphCount = 0;
    int maxGlyphH = 0;
    int totalGlyphW = 0;

    for (char32_t c : text) {
        if (c == U' ' || c == U'\n' || c == U'\r' || c == U'\t')
            continue;
        QImage g = atlas.glyph(c);
        if (!g.isNull()) {
            totalGlyphH += g.height();
            totalGlyphW += g.width();
            maxGlyphH = std::max(maxGlyphH, g.height());
            ++glyphCount;
        }
    }

    // Average glyph dimensions for spacing calculations
    float avgGlyphH = glyphCount > 0
        ? static_cast<float>(totalGlyphH) / glyphCount
        : static_cast<float>(targetHeight) * 0.7f;
    float avgGlyphW = glyphCount > 0
        ? static_cast<float>(totalGlyphW) / glyphCount
        : avgGlyphH * 0.5f;

    // Baseline: position so glyphs are vertically centered with slight descent
    // Approximately 75% from top (ascender line) for Latin text
    int baselineY = static_cast<int>(targetHeight * 0.75f);

    // Space width: use average character width or fraction of font size
    float spaceWidth = avgGlyphW * 0.6f;
    if (spaceWidth < 2.0f)
        spaceWidth = static_cast<float>(dpi) * fontMatch.sizePt / 72.0f * 0.25f;

    // Pre-synthesize missing characters from atlas glyphs
    std::unordered_map<char32_t, QImage> synthesized;
    if (atlas.size() > 0) {
        GlyphSynthesizer synth;
        synth.buildProfile(atlas);
        synthesized = synth.synthesizeAll(text, atlas);
    }

    QPainter painter(&output);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    float penX = 0.0f;

    for (size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];

        // Handle whitespace
        if (c == U' ') {
            penX += spaceWidth;
            continue;
        }
        if (c == U'\n' || c == U'\r' || c == U'\t') {
            if (c == U'\t')
                penX += spaceWidth * 4.0f;
            continue;
        }

        if (atlas.contains(c)) {
            // Atlas path: blit the pre-extracted character image
            QImage glyphImg = atlas.glyph(c);
            if (!glyphImg.isNull()) {
                // Align glyph bottom edge at baseline
                int drawX = static_cast<int>(penX);
                int drawY = baselineY - glyphImg.height();

                // Clamp to output bounds
                if (drawX < targetWidth && drawY < targetHeight) {
                    painter.drawImage(drawX, drawY, glyphImg);
                }

                penX += static_cast<float>(glyphImg.width());
            }
        } else if (auto it = synthesized.find(c); it != synthesized.end()) {
            // Synthesized glyph: atlas-derived substitute
            const QImage& synthImg = it->second;
            int drawX = static_cast<int>(penX);
            int drawY = baselineY - synthImg.height();

            if (drawX < targetWidth && drawY < targetHeight) {
                painter.drawImage(drawX, drawY, synthImg);
            }

            penX += static_cast<float>(synthImg.width());
        } else if (bitmapCompositor) {
            // FreeType fallback: render single character in a full-height canvas
            // (FreeType needs room for ascender positioning), then crop to visible
            // pixels so the cropped glyph matches atlas glyph dimensions and can
            // be aligned at the baseline consistently.
            std::u32string singleChar(1, c);
            int charW = std::max(1, static_cast<int>(avgGlyphW * 2.0f));
            int charH = targetHeight;

            QImage rendered = bitmapCompositor->renderText(
                singleChar,
                fontMatch.fontFilePath,
                fontMatch.sizePt,
                fontMatch.color,
                charW,
                charH,
                dpi);

            if (!rendered.isNull()) {
                // Crop to visible content — removes FreeType's positioning
                // whitespace so the cropped glyph is similar in size to atlas
                // glyphs and can be aligned at baselineY the same way.
                QImage cropped = cropToContent(rendered);
                if (!cropped.isNull()) {
                    int drawX = static_cast<int>(penX);
                    int drawY = baselineY - cropped.height();

                    if (drawX < targetWidth && drawY < targetHeight) {
                        painter.drawImage(drawX, drawY, cropped);
                    }

                    penX += static_cast<float>(cropped.width());
                } else {
                    penX += avgGlyphW;
                }
            } else {
                // Last resort: skip with average width advance
                penX += avgGlyphW;
            }
        } else {
            // No fallback available, skip
            penX += avgGlyphW;
        }

        // Stop if we've run past the right edge
        if (penX >= static_cast<float>(targetWidth))
            break;
    }

    painter.end();
    return output;
}

QImage GlyphAtlasCompositor::compose(
    const std::u32string& text,
    const EnrichedAtlas& enrichedAtlas,
    const FontMatch& fontMatch,
    const RgbColor& bgColor,
    int targetWidth,
    int targetHeight,
    int dpi,
    BitmapCompositor* bitmapCompositor) const
{
    if (text.empty() || targetWidth <= 0 || targetHeight <= 0)
        return {};

    // Create transparent output bitmap
    QImage output(targetWidth, targetHeight, QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);

    // EnrichedAtlas glyphs are already normalized to refLineH_ with baseline
    // alignment baked in (via normalizeHeight: descender space at bottom,
    // ascender space at top). Draw at native pixel size, bottom-aligned to
    // the canvas — matching AtlasComparisonDialog::renderAtlasPreview().
    // No separate baseline offset needed; it's already in the glyph image.
    int refLineH = enrichedAtlas.referenceLineHeight();

    // GitHub #266 — rescale when the atlas was harvested at a different DPI
    // than we are compositing at.
    //
    // Every length in EnrichedAtlas is in the pixels of the page render its
    // glyphs were cropped from, and that render is page-wide at the OCR DPI.
    // The composite, since defect #2 was fixed, happens at the NATIVE DPI of
    // the image under the edited region. On a page where those differ — a
    // 150-DPI thumbnail drawn over a 300-DPI photo — atlas glyphs arrive at
    // twice their correct size and render as a block. Synthesised and FreeType
    // glyphs are normalised independently and were unaffected, which is why a
    // single atlas-sourced character was wrong while its neighbours were right.
    //
    // The ratio is exactly 1.0 whenever the two DPIs agree, which is every
    // single-DPI page, so this changes nothing outside the mixed case. A
    // sourceDpi of 0 means an atlas built before this was recorded: assume they
    // match, i.e. the historical behaviour.
    const int atlasDpi = enrichedAtlas.sourceDpi();
    const double glyphScale =
        (atlasDpi > 0 && dpi > 0 && atlasDpi != dpi)
            ? static_cast<double>(dpi) / static_cast<double>(atlasDpi)
            : 1.0;
    if (glyphScale != 1.0) {
        refLineH = std::max(1, static_cast<int>(std::lround(refLineH * glyphScale)));
        qDebug() << "[GlyphAtlasCompositor] atlas harvested at" << atlasDpi
                 << "DPI, compositing at" << dpi
                 << "DPI -> scaling glyphs by" << glyphScale;
    }

    // GitHub #268. Ask the FreeType fallback to normalise straight to composite
    // scale, so a synthesised glyph is RASTERISED at the dpi it will be shown
    // at instead of being rasterised at the atlas dpi and resampled below.
    // Hinting and anti-aliasing are tuned for the output grid, so a 2x
    // downsample of an anti-aliased glyph is measurably softer than rendering
    // at the target size. Atlas glyphs are genuine crops from the scan and must
    // still be rescaled — only the synthesised ones can move.
    enrichedAtlas.setFreeTypeOutputScale(glyphScale);

    // Scale a glyph from atlas pixels into composite pixels. Identity when the
    // DPIs agree, so the common path allocates nothing.
    //
    // A FreeType-sourced glyph arrives ALREADY at composite scale (see above),
    // so rescaling it here would apply glyphScale twice. That is why glyph()
    // reports its source: the QImage carries no trace of where it came from,
    // and this is exactly the distinction #266 could not make.
    auto toCompositeScale = [glyphScale](const QImage& g,
                                         EnrichedAtlas::GlyphSource src)
                            -> QImage {
        if (glyphScale == 1.0 || g.isNull()) return g;
        if (src == EnrichedAtlas::GlyphSource::FreeType) return g;  // pre-scaled
        const int w = std::max(1, static_cast<int>(std::lround(g.width() * glyphScale)));
        const int h = std::max(1, static_cast<int>(std::lround(g.height() * glyphScale)));
        return g.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    };

    // Compute average glyph width at composite scale for space/fallback width
    int totalGlyphW = 0;
    int glyphCount = 0;
    for (char32_t c : text) {
        if (c == U' ' || c == U'\n' || c == U'\r' || c == U'\t')
            continue;
        // GitHub #302/#266. SEQUENCED ON PURPOSE -- do not re-inline.
        // Function arguments are UNSEQUENCED in C++, so
        //     toCompositeScale(enrichedAtlas.glyph(c, &gsrc), gsrc)
        // may read `gsrc` BEFORE glyph() writes it. On this build it
        // does: toCompositeScale saw a stale GlyphSource::None, its
        // "a FreeType glyph is already pre-scaled, do not rescale it"
        // guard never fired, and glyphScale was applied TWICE to
        // fallback glyphs on a mixed-DPI page. That is exactly the
        // #266 defect the GlyphSource out-param was added to prevent,
        // which made #268's fix inert in production.
        EnrichedAtlas::GlyphSource gsrc{};
        const QImage raw = enrichedAtlas.glyph(c, &gsrc);
        QImage g = toCompositeScale(raw, gsrc);
        if (!g.isNull()) {
            totalGlyphW += g.width();
            ++glyphCount;
        }
    }
    float avgGlyphW = glyphCount > 0
        ? static_cast<float>(totalGlyphW) / glyphCount
        : static_cast<float>(refLineH > 0 ? refLineH : targetHeight) * 0.5f;

    float spaceWidth = avgGlyphW * 0.6f;
    if (spaceWidth < 2.0f)
        spaceWidth = static_cast<float>(dpi) * fontMatch.sizePt / 72.0f * 0.25f;

    QPainter painter(&output);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    float penX = 0.0f;

    for (size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];

        if (c == U' ') {
            penX += spaceWidth;
            continue;
        }
        if (c == U'\n' || c == U'\r' || c == U'\t') {
            if (c == U'\t')
                penX += spaceWidth * 4.0f;
            continue;
        }

        // EnrichedAtlas::glyph() handles the full fallback chain:
        // atlas -> pre-synth -> on-demand synthesis -> FreeType+ScanStyleTransfer -> null
        // SEQUENCED ON PURPOSE -- see the identical note at the
        // measuring pass above. Both call sites had the bug; fixing
        // only the one that showed a symptom would have left the
        // other to reintroduce it.
        EnrichedAtlas::GlyphSource gsrc{};
        const QImage rawGlyph = enrichedAtlas.glyph(c, &gsrc);
        QImage glyphImg = toCompositeScale(rawGlyph, gsrc);
        if (!glyphImg.isNull()) {
            // Bottom-align: baseline is already encoded in the normalized image.
            int drawX = static_cast<int>(penX);
            int drawY = targetHeight - glyphImg.height();

            if (drawX < targetWidth && drawY < targetHeight) {
                painter.drawImage(drawX, drawY, glyphImg);
            }

            penX += static_cast<float>(glyphImg.width());
        } else {
            // EnrichedAtlas exhausted all fallbacks (atlas, synthesis, FreeType)
            penX += avgGlyphW;
        }

        if (penX >= static_cast<float>(targetWidth))
            break;
    }

    painter.end();
    return output;
}

} // namespace pdfe::tre
