#include "BitmapCompositor.h"
#include "fonts/TextShaper.h"
#include "fonts/FontMetricProfile.h"  // FontScript
#include "tre/FontMatcher.h"          // detectScript / scriptForRun (R2-20/R2-21)

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include FT_LCD_FILTER_H

#include <QImage>
#include <QPainter>
#include <QDebug>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace pdfe::tre {

class BitmapCompositor::Impl {
public:
    FT_Library ftLibrary = nullptr;
    pdfe::fonts::TextShaper textShaper;

    // C5: when true, subsequent renderText() calls set
    // FT_LOAD_TARGET_LCD for subpixel RGB rendering instead of
    // the default grayscale coverage. Default false: matches the
    // historical WYSIWYG behavior where overlay and save produce
    // visually identical output.
    bool lcdSubpixelEnabled_ = false;

    // R2-21: vertical (top-to-bottom) writing mode. Mirrors MuPDF wmode /
    // core::TextLine::writingMode (0 = horizontal, 1 = vertical). Default
    // false so Latin and all existing horizontal callers are byte-identical.
    // When true, shapeLine() builds vertical ShapeParams so HarfBuzz emits
    // TTB advances and the caret model steps along Y.
    bool verticalWritingMode_ = false;

    // R2-20/R2-21: shape a whole logical line script- and direction-aware.
    //
    // This is the single product-side integration of the i18n shaping API.
    // It (1) splits the logical text into bidi directional runs
    // (TextShaper::bidiRuns), (2) resolves each run's script via
    // FontMatcher::scriptForRun (-> FontMatcher::detectScript), (3) builds
    // per-run HarfBuzz params with ShapeParams::forScript(script, vertical),
    // and (4) shapes each run with the 4-arg TextShaper::shape. HarfBuzz
    // returns each RTL run's glyphs already in visual order and applies
    // Arabic GSUB joining (positional forms). For an RTL paragraph base the
    // runs themselves are emitted right-to-left (UAX#9 L2 at run
    // granularity).
    //
    // Backward compatibility: a pure Latin/LTR line yields a single
    // non-RTL run whose script resolves to Latin, so this calls
    // shape(face, text, size, forScript(Latin)) which is byte-identical to
    // the legacy 3-arg LTR/Latin/en path. Existing horizontal render tests
    // therefore do not regress.
    std::vector<pdfe::fonts::ShapedGlyph>
    shapeLine(FT_Face face, const std::u32string& text, float fontSizePt)
    {
        using pdfe::fonts::FontScript;
        using pdfe::fonts::ShapeParams;
        using pdfe::fonts::ShapeDirection;
        using pdfe::fonts::DirectionalRun;
        using pdfe::fonts::TextShaper;

        std::vector<pdfe::fonts::ShapedGlyph> out;
        if (!face || text.empty())
            return out;

        std::vector<DirectionalRun> dirRuns = TextShaper::bidiRuns(text);
        if (dirRuns.empty()) {
            // No content classified into runs: shape as one Latin run so the
            // writing-mode selection still applies. (Empty text already
            // returned above; this guards against a degenerate partition.)
            ShapeParams p = ShapeParams::forScript(FontScript::Latin, verticalWritingMode_);
            return textShaper.shape(static_cast<void*>(face), text, fontSizePt, p);
        }

        // Paragraph base direction: the first directional run carries it
        // (bidiRuns resolves leading neutrals to the base level). For an RTL
        // base, lay the runs out right-to-left.
        const bool baseRtl = dirRuns.front().rtl;
        if (baseRtl)
            std::reverse(dirRuns.begin(), dirRuns.end());

        for (const auto& r : dirRuns) {
            if (r.end <= r.start)
                continue;
            const std::u32string runText = text.substr(r.start, r.end - r.start);

            // Per-run script (defect #7 machinery): the run's OWN script, not
            // the region-dominant one, so a minority-script run shapes with the
            // correct HarfBuzz script tag / language.
            const FontScript script =
                pdfe::tre::FontMatcher::scriptForRun(text, r.start, r.end);

            ShapeParams params = ShapeParams::forScript(script, verticalWritingMode_);
            // Honour the bidi-resolved direction for runs whose script alone
            // (e.g. Common/neutral) would not imply RTL. Vertical overrides.
            if (!params.vertical)
                params.direction = r.rtl ? ShapeDirection::RTL : ShapeDirection::LTR;

            std::vector<pdfe::fonts::ShapedGlyph> runGlyphs =
                textShaper.shape(static_cast<void*>(face), runText, fontSizePt, params);

            // Re-base clusters onto the full-line logical index space so callers
            // that map glyphs back to characters stay correct across runs.
            for (auto& g : runGlyphs)
                g.cluster += static_cast<uint32_t>(r.start);

            out.insert(out.end(), runGlyphs.begin(), runGlyphs.end());
        }
        return out;
    }

    Impl() {
        FT_Error err = FT_Init_FreeType(&ftLibrary);
        if (err) {
            throw std::runtime_error("BitmapCompositor: FT_Init_FreeType failed");
        }
        // Install the default LCD filter so that when LCD mode is
        // enabled per-call the color fringing is softened. This is
        // the same filter FreeType's native LCD pipeline uses.
        FT_Library_SetLcdFilter(ftLibrary, FT_LCD_FILTER_DEFAULT);
    }

    ~Impl() {
        if (ftLibrary) {
            FT_Done_FreeType(ftLibrary);
            ftLibrary = nullptr;
        }
    }

    QImage renderText(const std::u32string& text,
                      const std::string& fontFilePath,
                      float fontSizePt,
                      const RgbColor& textColor,
                      int targetWidth,
                      int targetHeight,
                      int dpi)
    {
        qDebug() << "[BitmapCompositor] renderText:"
                 << "fontSizePt=" << fontSizePt
                 << "targetW=" << targetWidth << "targetH=" << targetHeight
                 << "dpi=" << dpi
                 << "fontPath=" << QString::fromStdString(fontFilePath);

        // Create transparent image for compositing on page backgrounds.
        // NOTE: For CNN font comparison, the caller (CNNFontRanker) composites
        // the result onto a white background before embedding extraction.
        QImage image(targetWidth, targetHeight, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);

        // Empty text - return transparent image
        if (text.empty() || fontFilePath.empty()) {
            return image;
        }

        // Load font
        FT_Face face = nullptr;
        FT_Error err = FT_New_Face(ftLibrary, fontFilePath.c_str(), 0, &face);
        if (err || !face) {
            throw std::runtime_error("BitmapCompositor: Failed to load font file: " + fontFilePath);
        }

        // Cleanup helper using RAII
        struct FaceGuard {
            FT_Face face;
            ~FaceGuard() { if (face) FT_Done_Face(face); }
        } faceGuard{face};

        // Set font size
        err = FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(fontSizePt * 64.0f), dpi, dpi);
        if (err) {
            throw std::runtime_error("BitmapCompositor: Failed to set font size");
        }

        // Shape text using TextShaper (shapes at 72 DPI internally).
        // R2-20/R2-21: script/direction/writing-mode aware (see shapeLine).
        std::vector<pdfe::fonts::ShapedGlyph> glyphs;
        try {
            glyphs = shapeLine(face, text, fontSizePt);
        } catch (...) {
            throw std::runtime_error("BitmapCompositor: Text shaping failed");
        }

        if (glyphs.empty()) {
            return image; // No glyphs to render
        }

        // Re-set char size with target DPI for glyph rendering
        // (TextShaper internally uses 72 DPI for shaping)
        err = FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(fontSizePt * 64.0f), dpi, dpi);
        if (err) {
            throw std::runtime_error("BitmapCompositor: Failed to set font size for rendering");
        }

        // Scale glyph positions from 72 DPI (shaping) to target DPI (rendering)
        const float dpiScale = static_cast<float>(dpi) / 72.0f;
        for (auto& g : glyphs) {
            g.xAdvance *= dpiScale;
            g.yAdvance *= dpiScale;
            g.xOffset *= dpiScale;
            g.yOffset *= dpiScale;
        }

        // Get font metrics (now at target DPI)
        float ascender = static_cast<float>(face->size->metrics.ascender) / 64.0f;
        float lineHeight = static_cast<float>(face->size->metrics.height) / 64.0f;

        // Initialize pen position
        float penX = 0.0f;
        float penY = ascender;

        // Render each glyph
        for (size_t i = 0; i < glyphs.size(); ++i) {
            const auto& glyph = glyphs[i];

            // Word wrapping: wrap before placing glyph if it would exceed width
            if (penX + glyph.xAdvance > static_cast<float>(targetWidth) && penX > 0.0f) {
                penX = 0.0f;
                penY += lineHeight;

                // Stop if we exceed target height
                if (penY + lineHeight > static_cast<float>(targetHeight)) {
                    break;
                }
            }

            // Load and render glyph. C5: in LCD mode, request
            // subpixel RGB rendering by combining FT_LOAD_RENDER
            // with FT_LOAD_TARGET_LCD. FreeType returns a bitmap
            // at 3x horizontal resolution with pixel_mode =
            // FT_PIXEL_MODE_LCD.
            FT_Int32 loadFlags = FT_LOAD_RENDER;
            if (lcdSubpixelEnabled_) {
                loadFlags |= FT_LOAD_TARGET_LCD;
            }
            err = FT_Load_Glyph(face, glyph.glyphId, loadFlags);
            if (err) {
                // Skip glyphs that fail to load (advance along the writing axis)
                const auto caFail =
                    pdfe::fonts::TextShaper::caretAdvance(glyph, verticalWritingMode_);
                penX += caFail.dx;
                penY += caFail.dy;
                continue;
            }

            const FT_Bitmap& bitmap = face->glyph->bitmap;

            // Calculate glyph position
            int x = static_cast<int>(penX + glyph.xOffset) + face->glyph->bitmap_left;
            int y = static_cast<int>(penY - glyph.yOffset) - face->glyph->bitmap_top;

            // Blit glyph bitmap to QImage
            if (bitmap.buffer && bitmap.width > 0 && bitmap.rows > 0) {
                if (bitmap.pixel_mode == FT_PIXEL_MODE_LCD) {
                    blitGlyphLcd(image, bitmap, x, y, textColor);
                } else {
                    blitGlyph(image, bitmap, x, y, textColor);
                }
            }

            // Advance pen position along the writing axis (R2-21). For
            // horizontal text caretAdvance yields {dx=xAdvance, dy=0} — i.e.
            // byte-identical to the previous `penX += glyph.xAdvance`. For
            // vertical (TTB) text it advances penY so CJK glyphs stack.
            const auto ca =
                pdfe::fonts::TextShaper::caretAdvance(glyph, verticalWritingMode_);
            penX += ca.dx;
            penY += ca.dy;
        }

        return image;
    }

    void blitGlyph(QImage& image, const FT_Bitmap& bitmap, int x, int y, const RgbColor& color)
    {
        // Blit with bounds checking and premultiplied alpha
        for (unsigned int row = 0; row < bitmap.rows; ++row) {
            int imgY = y + static_cast<int>(row);
            if (imgY < 0 || imgY >= image.height()) {
                continue;
            }

            for (unsigned int col = 0; col < bitmap.width; ++col) {
                int imgX = x + static_cast<int>(col);
                if (imgX < 0 || imgX >= image.width()) {
                    continue;
                }

                // Get coverage value from glyph bitmap
                uint8_t coverage = bitmap.buffer[row * bitmap.pitch + col];

                if (coverage > 0) {
                    // Premultiplied alpha blending
                    uint8_t r = (color.r * coverage) / 255;
                    uint8_t g = (color.g * coverage) / 255;
                    uint8_t b = (color.b * coverage) / 255;
                    uint8_t a = coverage;

                    // Blend with existing pixel (if any)
                    QRgb existingPixel = image.pixel(imgX, imgY);
                    uint8_t existingA = qAlpha(existingPixel);

                    if (existingA > 0) {
                        // Alpha compositing (over operation with premultiplied alpha)
                        uint8_t existingR = qRed(existingPixel);
                        uint8_t existingG = qGreen(existingPixel);
                        uint8_t existingB = qBlue(existingPixel);

                        // out.rgb = src.rgb + dst.rgb * (1 - src.a)
                        uint8_t outA = a + (existingA * (255 - a)) / 255;
                        uint8_t outR = r + (existingR * (255 - a)) / 255;
                        uint8_t outG = g + (existingG * (255 - a)) / 255;
                        uint8_t outB = b + (existingB * (255 - a)) / 255;

                        image.setPixel(imgX, imgY, qRgba(outR, outG, outB, outA));
                    } else {
                        // No existing pixel, just set new one
                        image.setPixel(imgX, imgY, qRgba(r, g, b, a));
                    }
                }
            }
        }
    }

    /// C5: blit an FT LCD (subpixel RGB) bitmap. The source
    /// bitmap.width is 3x the logical pixel count — every group
    /// of 3 bytes is one subpixel R/G/B coverage triple. The
    /// destination is a normal RGBA image where we encode the
    /// subpixel variation as weighted channel contributions from
    /// the text color.
    ///
    /// Per-pixel algorithm:
    ///   logicalCol = srcCol / 3
    ///   (covR, covG, covB) = bitmap[row, srcCol..srcCol+2]
    ///   alpha = max(covR, covG, covB)
    ///   out.r = color.r * covR / 255   (premultiplied)
    ///   out.g = color.g * covG / 255
    ///   out.b = color.b * covB / 255
    ///
    /// This preserves the per-channel sub-pixel positioning
    /// information in the output RGB, so when the final bitmap is
    /// displayed at 1:1 on an RGB-striped LCD the sub-pixel detail
    /// is visible. Viewed at any scaling it degrades gracefully
    /// back to weighted grayscale.
    void blitGlyphLcd(QImage& image, const FT_Bitmap& bitmap,
                      int x, int y, const RgbColor& color)
    {
        // bitmap.width is the BYTE width (3x logical pixel count).
        const int logicalW = static_cast<int>(bitmap.width / 3);
        for (unsigned int row = 0; row < bitmap.rows; ++row) {
            int imgY = y + static_cast<int>(row);
            if (imgY < 0 || imgY >= image.height()) continue;

            const uint8_t* srcRow = bitmap.buffer + row * bitmap.pitch;
            for (int col = 0; col < logicalW; ++col) {
                int imgX = x + col;
                if (imgX < 0 || imgX >= image.width()) continue;

                uint8_t covR = srcRow[col * 3 + 0];
                uint8_t covG = srcRow[col * 3 + 1];
                uint8_t covB = srcRow[col * 3 + 2];
                if ((covR | covG | covB) == 0) continue;

                uint8_t a = std::max({covR, covG, covB});
                uint8_t r = (color.r * covR) / 255;
                uint8_t g = (color.g * covG) / 255;
                uint8_t b = (color.b * covB) / 255;

                QRgb existingPixel = image.pixel(imgX, imgY);
                uint8_t existingA = qAlpha(existingPixel);
                if (existingA > 0) {
                    uint8_t existingR = qRed(existingPixel);
                    uint8_t existingG = qGreen(existingPixel);
                    uint8_t existingB = qBlue(existingPixel);

                    uint8_t outA = a + (existingA * (255 - a)) / 255;
                    uint8_t outR = r + (existingR * (255 - a)) / 255;
                    uint8_t outG = g + (existingG * (255 - a)) / 255;
                    uint8_t outB = b + (existingB * (255 - a)) / 255;
                    image.setPixel(imgX, imgY, qRgba(outR, outG, outB, outA));
                } else {
                    image.setPixel(imgX, imgY, qRgba(r, g, b, a));
                }
            }
        }
    }

    // Find the style for a given character index
    static WordStyleFlags styleForChar(int charIndex,
                                        const std::vector<StyledRun>& runs)
    {
        const auto it = std::find_if(runs.begin(), runs.end(),
            [charIndex](const StyledRun& run) {
                return charIndex >= run.charStart && charIndex < run.charEnd;
            });
        return (it != runs.end()) ? it->style : WordStyleFlags{};
    }

    // Find the FontRun covering a given character index.
    // Returns nullptr if no run covers it (use default font).
    static const FontRun* fontRunForChar(int charIndex,
                                          const std::vector<FontRun>& runs)
    {
        const auto it = std::find_if(runs.begin(), runs.end(),
            [charIndex](const FontRun& run) {
                return charIndex >= run.charStart && charIndex < run.charEnd;
            });
        return (it != runs.end()) ? &(*it) : nullptr;
    }

    QImage renderMultiFontText(const std::u32string& text,
                                const std::string& defaultFontPath,
                                float defaultFontSizePt,
                                const RgbColor& textColor,
                                const std::vector<FontRun>& fontRuns,
                                int targetWidth,
                                int targetHeight,
                                int dpi)
    {
        // If no font runs, fall back to regular styled rendering
        if (fontRuns.empty()) {
            return renderText(text, defaultFontPath, defaultFontSizePt, textColor,
                              targetWidth, targetHeight, dpi);
        }

        QImage image(targetWidth, targetHeight, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);

        if (text.empty())
            return image;

        // Split text into contiguous segments sharing the same FontRun (or default).
        // Each segment gets its own FT_Face, shaping, and rendering pass.
        struct Segment {
            int charStart;
            int charEnd;
            std::string fontPath;
            float fontSizePt;
            WordStyleFlags style;
        };

        qDebug() << "[BitmapCompositor] renderMultiFontText:"
                 << "textLen=" << static_cast<int>(text.size())
                 << "defaultFontSizePt=" << defaultFontSizePt
                 << "fontRuns=" << fontRuns.size()
                 << "targetW=" << targetWidth << "targetH=" << targetHeight
                 << "dpi=" << dpi;

        std::vector<Segment> segments;
        int pos = 0;
        int textLen = static_cast<int>(text.size());
        while (pos < textLen) {
            const FontRun* run = fontRunForChar(pos, fontRuns);
            Segment seg;
            seg.charStart = pos;
            if (run) {
                seg.charEnd = std::min(run->charEnd, textLen);
                seg.fontPath = run->fontFilePath.empty() ? defaultFontPath : run->fontFilePath;
                seg.fontSizePt = (run->fontSizePt > 0.0f) ? run->fontSizePt : defaultFontSizePt;
                seg.style = run->style;
            } else {
                // Default font — find next run boundary
                int nextStart = textLen;
                for (const auto& r : fontRuns) {
                    if (r.charStart > pos && r.charStart < nextStart)
                        nextStart = r.charStart;
                }
                seg.charEnd = nextStart;
                seg.fontPath = defaultFontPath;
                seg.fontSizePt = defaultFontSizePt;
                seg.style = {};
            }
            segments.push_back(std::move(seg));
            pos = segments.back().charEnd;
        }

        for (size_t i = 0; i < segments.size(); ++i) {
            qDebug() << "  [BitmapCompositor] segment[" << i << "]:"
                     << "chars=" << segments[i].charStart << "-" << segments[i].charEnd
                     << "fontSizePt=" << segments[i].fontSizePt
                     << "path=" << QString::fromStdString(segments[i].fontPath);
        }

        // Render each segment sequentially, advancing penX across segments
        float penX = 0.0f;
        float globalAscender = 0.0f;
        float globalLineHeight = 0.0f;
        float globalDescender = 0.0f;
        const float dpiScale = static_cast<float>(dpi) / 72.0f;

        // First pass: compute global metrics from the largest font size
        {
            float maxSizePt = defaultFontSizePt;
            std::string maxPath = defaultFontPath;
            for (const auto& seg : segments) {
                if (seg.fontSizePt > maxSizePt) {
                    maxSizePt = seg.fontSizePt;
                    maxPath = seg.fontPath;
                }
            }
            if (!maxPath.empty()) {
                FT_Face tmpFace = nullptr;
                FT_Error err = FT_New_Face(ftLibrary, maxPath.c_str(), 0, &tmpFace);
                if (!err && tmpFace) {
                    FT_Set_Char_Size(tmpFace, 0,
                                      static_cast<FT_F26Dot6>(maxSizePt * 64.0f), dpi, dpi);
                    globalAscender = static_cast<float>(tmpFace->size->metrics.ascender) / 64.0f;
                    globalDescender = static_cast<float>(tmpFace->size->metrics.descender) / 64.0f;
                    globalLineHeight = static_cast<float>(tmpFace->size->metrics.height) / 64.0f;
                    FT_Done_Face(tmpFace);
                }
            }
        }

        float penY = globalAscender;

        // Decoration ranges for underline/strikethrough
        struct DecoRange {
            float x0, x1, y;
            bool underline;
            bool strikethrough;
            float fontSizePt;
        };
        std::vector<DecoRange> decoRanges;

        for (const auto& seg : segments) {
            std::u32string segText = text.substr(seg.charStart, seg.charEnd - seg.charStart);
            if (segText.empty()) continue;

            if (seg.fontPath.empty()) {
                // No font path — skip this segment but estimate advance
                penX += static_cast<float>(segText.size()) * seg.fontSizePt * dpiScale * 0.5f;
                continue;
            }

            // Load face for this segment
            FT_Face face = nullptr;
            FT_Error err = FT_New_Face(ftLibrary, seg.fontPath.c_str(), 0, &face);
            if (err || !face) {
                // Skip segment if font can't load
                penX += static_cast<float>(segText.size()) * seg.fontSizePt * dpiScale * 0.5f;
                continue;
            }
            struct FaceGuard {
                FT_Face face;
                ~FaceGuard() { if (face) FT_Done_Face(face); }
            } faceGuard{face};

            err = FT_Set_Char_Size(face, 0,
                                    static_cast<FT_F26Dot6>(seg.fontSizePt * 64.0f), dpi, dpi);
            if (err) continue;

            // Shape segment text (R2-20/R2-21: script/direction/wmode aware).
            std::vector<pdfe::fonts::ShapedGlyph> glyphs;
            try {
                glyphs = shapeLine(face, segText, seg.fontSizePt);
            } catch (...) {
                continue;
            }
            if (glyphs.empty()) continue;

            // Re-set char size for rendering DPI
            FT_Set_Char_Size(face, 0,
                              static_cast<FT_F26Dot6>(seg.fontSizePt * 64.0f), dpi, dpi);

            // Scale glyph positions from 72 DPI to target DPI
            for (auto& g : glyphs) {
                g.xAdvance *= dpiScale;
                g.yAdvance *= dpiScale;
                g.xOffset *= dpiScale;
                g.yOffset *= dpiScale;
            }

            // Track decoration start for this segment
            float segStartX = penX;
            bool needsStyleTransform = seg.style.isBold || seg.style.isItalic;

            for (size_t i = 0; i < glyphs.size(); ++i) {
                const auto& glyph = glyphs[i];

                // Word wrapping
                if (penX + glyph.xAdvance > static_cast<float>(targetWidth) && penX > 0.0f) {
                    // Flush decoration before wrap
                    if (seg.style.isUnderlined || seg.style.isStrikethrough) {
                        decoRanges.push_back({segStartX, penX, penY,
                                               seg.style.isUnderlined,
                                               seg.style.isStrikethrough,
                                               seg.fontSizePt});
                    }
                    penX = 0.0f;
                    penY += globalLineHeight;
                    segStartX = 0.0f;
                    if (penY + globalLineHeight > static_cast<float>(targetHeight))
                        break;
                }

                // Load glyph
                err = FT_Load_Glyph(face, glyph.glyphId,
                                     needsStyleTransform ? FT_LOAD_DEFAULT : FT_LOAD_RENDER);
                if (err) {
                    penX += glyph.xAdvance;
                    continue;
                }

                if (seg.style.isBold)
                    FT_GlyphSlot_Embolden(face->glyph);
                if (seg.style.isItalic)
                    FT_GlyphSlot_Oblique(face->glyph);

                if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP)
                    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);

                const FT_Bitmap& bitmap = face->glyph->bitmap;
                int x = static_cast<int>(penX + glyph.xOffset) + face->glyph->bitmap_left;
                int y = static_cast<int>(penY - glyph.yOffset) - face->glyph->bitmap_top;

                if (bitmap.buffer && bitmap.width > 0 && bitmap.rows > 0) {
                    blitGlyph(image, bitmap, x, y, textColor);
                }

                penX += glyph.xAdvance;
            }

            // Flush decoration range for this segment
            if (seg.style.isUnderlined || seg.style.isStrikethrough) {
                decoRanges.push_back({segStartX, penX, penY,
                                       seg.style.isUnderlined,
                                       seg.style.isStrikethrough,
                                       seg.fontSizePt});
            }
        }

        // Draw decorations
        if (!decoRanges.empty()) {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing, true);
            QColor penColor(textColor.r, textColor.g, textColor.b);

            for (const auto& deco : decoRanges) {
                float lineThickness = std::max(1.0f, deco.fontSizePt * dpiScale / 20.0f);
                QPen pen(penColor, lineThickness);
                painter.setPen(pen);

                if (deco.underline) {
                    float ulY = deco.y - globalDescender * 0.3f;
                    painter.drawLine(QPointF(deco.x0, ulY), QPointF(deco.x1, ulY));
                }
                if (deco.strikethrough) {
                    float xHeight = globalAscender * 0.5f;
                    float stY = deco.y - xHeight;
                    painter.drawLine(QPointF(deco.x0, stY), QPointF(deco.x1, stY));
                }
            }
        }

        return image;
    }

    QImage renderStyledText(const std::u32string& text,
                             const std::string& fontFilePath,
                             float fontSizePt,
                             const RgbColor& textColor,
                             const std::vector<StyledRun>& styledRuns,
                             int targetWidth,
                             int targetHeight,
                             int dpi)
    {
        // If no styled runs, fall back to regular rendering
        if (styledRuns.empty()) {
            return renderText(text, fontFilePath, fontSizePt, textColor,
                              targetWidth, targetHeight, dpi);
        }

        QImage image(targetWidth, targetHeight, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);

        if (text.empty() || fontFilePath.empty())
            return image;

        // Load font
        FT_Face face = nullptr;
        FT_Error err = FT_New_Face(ftLibrary, fontFilePath.c_str(), 0, &face);
        if (err || !face)
            throw std::runtime_error("BitmapCompositor: Failed to load font: " + fontFilePath);

        struct FaceGuard {
            FT_Face face;
            ~FaceGuard() { if (face) FT_Done_Face(face); }
        } faceGuard{face};

        err = FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(fontSizePt * 64.0f), dpi, dpi);
        if (err)
            throw std::runtime_error("BitmapCompositor: Failed to set font size");

        // Shape text (R2-20/R2-21: script/direction/wmode aware).
        std::vector<pdfe::fonts::ShapedGlyph> glyphs;
        try {
            glyphs = shapeLine(face, text, fontSizePt);
        } catch (...) {
            throw std::runtime_error("BitmapCompositor: Text shaping failed");
        }

        if (glyphs.empty())
            return image;

        // Re-set char size for rendering DPI
        FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(fontSizePt * 64.0f), dpi, dpi);

        // Scale glyph positions
        const float dpiScale = static_cast<float>(dpi) / 72.0f;
        for (auto& g : glyphs) {
            g.xAdvance *= dpiScale;
            g.yAdvance *= dpiScale;
            g.xOffset *= dpiScale;
            g.yOffset *= dpiScale;
        }

        float ascender = static_cast<float>(face->size->metrics.ascender) / 64.0f;
        float descender = static_cast<float>(face->size->metrics.descender) / 64.0f;
        float lineHeight = static_cast<float>(face->size->metrics.height) / 64.0f;

        float penX = 0.0f;
        float penY = ascender;

        // Track underline/strikethrough ranges (pen positions)
        struct DecoRange {
            float x0, x1, y;
            bool underline;
            bool strikethrough;
        };
        std::vector<DecoRange> decoRanges;
        float currentRunX0 = 0.0f;
        int prevCharIndex = -1;
        WordStyleFlags prevFlags;

        // Map glyph index back to character index (approximate: 1:1 for non-ligature text)
        for (size_t i = 0; i < glyphs.size(); ++i) {
            const auto& glyph = glyphs[i];
            int charIndex = static_cast<int>(i);
            if (charIndex >= static_cast<int>(text.size()))
                charIndex = static_cast<int>(text.size()) - 1;

            WordStyleFlags flags = styleForChar(charIndex, styledRuns);

            // Track decoration transitions
            if (prevCharIndex >= 0 &&
                (flags.isUnderlined != prevFlags.isUnderlined ||
                 flags.isStrikethrough != prevFlags.isStrikethrough)) {
                if (prevFlags.isUnderlined || prevFlags.isStrikethrough) {
                    decoRanges.push_back({currentRunX0, penX, penY,
                                           prevFlags.isUnderlined,
                                           prevFlags.isStrikethrough});
                }
                currentRunX0 = penX;
            }
            if (prevCharIndex < 0)
                currentRunX0 = penX;
            prevFlags = flags;
            prevCharIndex = charIndex;

            // Word wrapping
            if (penX + glyph.xAdvance > static_cast<float>(targetWidth) && penX > 0.0f) {
                // Flush decoration range before wrapping
                if (prevFlags.isUnderlined || prevFlags.isStrikethrough) {
                    decoRanges.push_back({currentRunX0, penX, penY,
                                           prevFlags.isUnderlined,
                                           prevFlags.isStrikethrough});
                }
                penX = 0.0f;
                penY += lineHeight;
                currentRunX0 = 0.0f;
                if (penY + lineHeight > static_cast<float>(targetHeight))
                    break;
            }

            // Load glyph: use FT_LOAD_DEFAULT for styled glyphs so we can
            // apply Embolden/Oblique to the outline before rasterizing.
            bool needsStyleTransform = flags.isBold || flags.isItalic;
            err = FT_Load_Glyph(face, glyph.glyphId,
                                needsStyleTransform ? FT_LOAD_DEFAULT : FT_LOAD_RENDER);
            if (err) {
                penX += glyph.xAdvance;
                continue;
            }

            // Apply synthetic bold/italic per-glyph on the outline
            if (flags.isBold) {
                FT_GlyphSlot_Embolden(face->glyph);
            }
            if (flags.isItalic) {
                FT_GlyphSlot_Oblique(face->glyph);
            }

            // Render outline to bitmap after synthetic transforms
            if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP) {
                FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
            }

            const FT_Bitmap& bitmap = face->glyph->bitmap;
            int x = static_cast<int>(penX + glyph.xOffset) + face->glyph->bitmap_left;
            int y = static_cast<int>(penY - glyph.yOffset) - face->glyph->bitmap_top;

            if (bitmap.buffer && bitmap.width > 0 && bitmap.rows > 0) {
                blitGlyph(image, bitmap, x, y, textColor);
            }

            penX += glyph.xAdvance;
        }

        // Flush final decoration range
        if (prevFlags.isUnderlined || prevFlags.isStrikethrough) {
            decoRanges.push_back({currentRunX0, penX, penY,
                                   prevFlags.isUnderlined,
                                   prevFlags.isStrikethrough});
        }

        // Draw underline and strikethrough decorations using QPainter
        if (!decoRanges.empty()) {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing, true);
            QColor penColor(textColor.r, textColor.g, textColor.b);
            float lineThickness = std::max(1.0f, fontSizePt * dpiScale / 20.0f);

            for (const auto& deco : decoRanges) {
                QPen pen(penColor, lineThickness);
                painter.setPen(pen);

                if (deco.underline) {
                    // Draw underline just below baseline (ascender + small offset)
                    float ulY = deco.y - descender * 0.3f;
                    painter.drawLine(QPointF(deco.x0, ulY), QPointF(deco.x1, ulY));
                }
                if (deco.strikethrough) {
                    // Draw strikethrough at mid x-height
                    float xHeight = ascender * 0.5f; // Approximate x-height as half ascender
                    float stY = deco.y - xHeight;
                    painter.drawLine(QPointF(deco.x0, stY), QPointF(deco.x1, stY));
                }
            }
        }

        return image;
    }
};

BitmapCompositor::BitmapCompositor()
    : pImpl_(std::make_unique<Impl>())
{
}

BitmapCompositor::~BitmapCompositor() = default;

BitmapCompositor::BitmapCompositor(BitmapCompositor&& other) noexcept = default;
BitmapCompositor& BitmapCompositor::operator=(BitmapCompositor&& other) noexcept = default;

QImage BitmapCompositor::renderText(const std::u32string& text,
                                     const std::string& fontFilePath,
                                     float fontSizePt,
                                     const RgbColor& textColor,
                                     int targetWidth,
                                     int targetHeight,
                                     int dpi)
{
    return pImpl_->renderText(text, fontFilePath, fontSizePt, textColor,
                              targetWidth, targetHeight, dpi);
}

QImage BitmapCompositor::renderText(const std::u32string& text,
                                     const FontMatch& fontMatch,
                                     int targetWidth,
                                     int targetHeight,
                                     int dpi)
{
    return pImpl_->renderText(text, fontMatch.fontFilePath, fontMatch.sizePt,
                              fontMatch.color, targetWidth, targetHeight, dpi);
}

QImage BitmapCompositor::renderStyledText(const std::u32string& text,
                                           const FontMatch& fontMatch,
                                           const std::vector<StyledRun>& styledRuns,
                                           int targetWidth,
                                           int targetHeight,
                                           int dpi)
{
    return pImpl_->renderStyledText(text, fontMatch.fontFilePath, fontMatch.sizePt,
                                     fontMatch.color, styledRuns,
                                     targetWidth, targetHeight, dpi);
}

QImage BitmapCompositor::renderMultiFontText(const std::u32string& text,
                                              const FontMatch& fontMatch,
                                              const std::vector<FontRun>& fontRuns,
                                              int targetWidth,
                                              int targetHeight,
                                              int dpi)
{
    return pImpl_->renderMultiFontText(text, fontMatch.fontFilePath, fontMatch.sizePt,
                                        fontMatch.color, fontRuns,
                                        targetWidth, targetHeight, dpi);
}

void BitmapCompositor::setVerticalWritingMode(bool vertical) {
    pImpl_->verticalWritingMode_ = vertical;
}

bool BitmapCompositor::verticalWritingMode() const {
    return pImpl_->verticalWritingMode_;
}

std::u32string BitmapCompositor::visualOrder(const std::u32string& logical) const {
    // R2-21: logical -> visual reordering for caret / selection hit-testing on
    // mixed-direction lines. Shares the single bidi implementation
    // (TextShaper::reorderForDisplay -> bidiRuns) with the render path so the
    // on-screen glyph order and the caret index mapping never diverge.
    return pdfe::fonts::TextShaper::reorderForDisplay(logical);
}

void BitmapCompositor::setLcdSubpixelEnabled(bool enabled) {
    pImpl_->lcdSubpixelEnabled_ = enabled;
}

bool BitmapCompositor::lcdSubpixelEnabled() const {
    return pImpl_->lcdSubpixelEnabled_;
}

bool BitmapCompositor::shouldUseLcdSubpixel(DisplayQuality quality,
                                            bool willDegrade)
{
    // C5 decision logic. See docs/image-pdf-risks.md (C5).
    //
    //   quality=WYSIWYG, willDegrade=true  → false
    //       Match the save path's output; don't waste compute on
    //       subpixel detail that ScanStyleTransfer will wipe out.
    //
    //   quality=WYSIWYG, willDegrade=false → true
    //       The calibration is mild enough that LCD survives
    //       the pipeline — use it for free quality on both
    //       overlay and save.
    //
    //   quality=HighQuality, * → true
    //       User has explicitly opted into crisp editing even if
    //       it means the overlay looks cleaner than the save.
    if (quality == DisplayQuality::HighQuality) return true;
    return !willDegrade;
}

int BitmapCompositor::measureTextWidth(const std::u32string& text,
                                       const std::string& fontFilePath,
                                       float fontSizePt,
                                       int dpi)
{
    // B2: measure actual advance width via HarfBuzz shaping instead of
    // character-count ratios. Returns 0 for empty input or on load
    // failure — callers should fall back to computeExpandedWidth.
    if (text.empty() || fontFilePath.empty() || fontSizePt <= 0.0f) {
        return 0;
    }

    FT_Face face = nullptr;
    FT_Error err = FT_New_Face(pImpl_->ftLibrary, fontFilePath.c_str(), 0, &face);
    if (err || !face) {
        qWarning() << "[BitmapCompositor] measureTextWidth: failed to load"
                   << QString::fromStdString(fontFilePath);
        return 0;
    }
    struct FaceGuard {
        FT_Face face;
        ~FaceGuard() { if (face) FT_Done_Face(face); }
    } guard{face};

    // Set size at target DPI so glyph metrics match the render path.
    err = FT_Set_Char_Size(
        face, 0, static_cast<FT_F26Dot6>(fontSizePt * 64.0f), dpi, dpi);
    if (err) {
        return 0;
    }

    // Shape the text. TextShaper returns advances in 72-DPI points;
    // multiply by dpi/72 to convert to target-DPI pixels. This matches
    // the scaling the render path applies at BitmapCompositor.cpp:100-106.
    // R2-20: script/direction-aware shaping so Arabic joining (and any
    // per-run script) is reflected in the measured advance width, matching
    // the render path's shapeLine().
    std::vector<pdfe::fonts::ShapedGlyph> glyphs;
    try {
        glyphs = pImpl_->shapeLine(face, text, fontSizePt);
    } catch (...) {
        return 0;
    }
    if (glyphs.empty()) {
        return 0;
    }

    const float totalAdvancePt = std::accumulate(
        glyphs.begin(), glyphs.end(), 0.0f,
        [](float acc, const auto& g) { return acc + g.xAdvance; });
    const float dpiScale = static_cast<float>(dpi) / 72.0f;
    const float totalAdvancePx = totalAdvancePt * dpiScale;
    return std::max(0, static_cast<int>(std::ceil(totalAdvancePx)));
}

} // namespace pdfe::tre
