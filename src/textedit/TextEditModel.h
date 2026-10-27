#pragma once

#include "core/TextBlock.h"
#include <QPointF>
#include <QRectF>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace pdfe::textedit {

/// Recognize the glyphless placeholder fonts that OCR tools embed for an
/// invisible text layer drawn over a scanned bitmap (OCRmyPDF/Tesseract
/// "GlyphLessFont", Adobe "Glyphless", etc.). Case-insensitive, and tolerant
/// of a leading 6-letter subset prefix ("ABCDEF+GlyphLessFont").
///
/// This is the canonical per-span provenance signal: a span drawn with such a
/// font carries no real vector glyphs — the visible mark on the page is the
/// underlying image — so editing it must keep the text invisible (Tr=3) and
/// paint the visible result into the bitmap layer. A span drawn with any
/// genuine font originates from real text-show operators and must render as
/// visible vector text. Conservative by design: anything not clearly glyphless
/// is treated as real text (the safe failure mode is "visible", never "blank").
[[nodiscard]] inline bool isGlyphlessOcrFontName(std::string name) {
    // Strip a canonical subset prefix "ABCDEF+" so "ABCDEF+GlyphLessFont"
    // matches the same as a bare "GlyphLessFont".
    if (name.size() >= 8 && name[6] == '+') {
        bool allUpper = true;
        for (int i = 0; i < 6; ++i) {
            if (name[i] < 'A' || name[i] > 'Z') { allUpper = false; break; }
        }
        if (allUpper)
            name = name.substr(7);
    }
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name.find("glyphless") != std::string::npos;
}

/// Per-character-range formatting override within a TextSpan.
/// Gaps between runs inherit the span's base fontName/fontSize with no style flags.
struct FormattingRun {
    int charStart = 0;           ///< UTF-32 character index (inclusive)
    int charEnd = 0;             ///< UTF-32 character index (exclusive)
    std::string fontFamily;      ///< Qt font family name (empty = span default)
    float fontSize = 0.0f;       ///< Font size in points (0 = span default)
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    std::string fontFilePath;    ///< Resolved .ttf/.otf path (for embedding/FreeType)
    uint32_t textColor = 0x000000; ///< RGB text color (0x000000 = black, default)

    /// Pre-installed PDF font resource name. When set, takes precedence
    /// over `fontFilePath` and `fontFamily` in the save path — the run
    /// uses an existing `/Font` resource that the caller already added
    /// to the page (e.g. a Type 3 bundle produced by
    /// `SynthesizedType3FontBundler`). No font embedding happens for
    /// this run during save; the resource is referenced as-is.
    std::string preInstalledResName;

    /// Check if two runs have identical formatting (ignoring char range).
    [[nodiscard]] bool sameFormatAs(const FormattingRun& other) const {
        return fontFamily == other.fontFamily
            && std::abs(fontSize - other.fontSize) < 0.01f
            && bold == other.bold
            && italic == other.italic
            && underline == other.underline
            && strikethrough == other.strikethrough
            && fontFilePath == other.fontFilePath
            && preInstalledResName == other.preInstalledResName
            && textColor == other.textColor;
    }
};

/// Represents a contiguous run of text with uniform font, extracted from a TextLine.
struct TextSpan {
    int blockIndex = 0;          ///< Source TextBlock index
    int lineIndex = 0;           ///< Source TextLine index within block
    int charStartIndex = 0;      ///< First char index in the source TextLine
    int charEndIndex = 0;        ///< Past-end char index in the source TextLine
    std::u32string text;         ///< Current text (may differ from original after editing)
    std::u32string originalText; ///< Original text at construction time
    QRectF boundingBox;          ///< Bounding box in PDF coordinates
    std::string fontName;        ///< PDF font name from the first char
    float fontSize = 0.0f;       ///< Font size from the first char
    bool isItalic = false;       ///< Italic per the source chars' /FontDescriptor (name-independent)
    bool isBold = false;         ///< Bold per the source chars' /FontDescriptor (name-independent)
    uint32_t textColor = 0x000000; ///< Original sRGB fill color (0xRRGGBB) from the source chars

    /// R2-21a writing-mode CARRIER on the text-edit path. 0 = horizontal
    /// (default), 1 = vertical (top-to-bottom). Copied in the TextEditModel
    /// ctor from the source core::TextLine::writingMode (populated in
    /// TextExtractor from MuPDF fz_stext_line::wmode). Consumed by
    /// TextEditTool::selectSpan, which drives the overlay's vertical/RTL caret
    /// geometry (R2-21b) automatically — no manual setOverlayVerticalMode()
    /// call. Default 0 keeps every horizontal caller byte-identical.
    int writingMode = 0;

    /// True when textColor was captured from the source glyphs (vs the default).
    /// Distinguishes "genuinely black text" from "no color captured".
    bool textColorKnown = false;

    /// Per-character bounding boxes in PDF coordinates (from source TextChars).
    std::vector<QRectF> charQuads;

    /// Per-character formatting overrides. Empty = uniform span formatting.
    std::vector<FormattingRun> formattingRuns;

    [[nodiscard]] bool isModified() const {
        return text != originalText || !formattingRuns.empty();
    }

    /// True when THIS span's glyphs come from an OCR invisible-text layer
    /// (a glyphless placeholder font over a scanned bitmap) rather than from
    /// real text-show operators. Per-span provenance: it inspects only this
    /// span's own font identity — never the page's image area — so a real
    /// vector heading on an image-heavy page is correctly classified as
    /// visible text. The text-edit pipeline uses it to gate the invisible
    /// (Tr=3) path (#1); the visible-vector path is used for every other span.
    [[nodiscard]] bool isOcrInvisibleLayer() const {
        return isGlyphlessOcrFontName(fontName);
    }
};

/// In-memory model of editable text spans on a single page.
///
/// Built from TextExtractor output. Each TextLine (or sub-run with uniform font)
/// becomes a TextSpan. Provides hit testing and in-place editing support.
class TextEditModel {
public:
    /// Build model from extracted text blocks.
    explicit TextEditModel(const std::vector<pdfe::core::TextBlock>& blocks);

    // Span access
    [[nodiscard]] int spanCount() const;
    [[nodiscard]] const TextSpan& span(int index) const;
    /// Mutable span access (for TextEditTool to read formattingRuns).
    TextSpan& mutableSpan(int index);

    // Hit testing (PDF coordinates)
    /// Returns span index containing the point, or -1 if none.
    [[nodiscard]] int spanAtPoint(const QPointF& pdfPoint) const;
    /// Returns character index within a span closest to the point.
    [[nodiscard]] int charIndexAtPoint(int spanIndex, const QPointF& pdfPoint) const;

    // Editing
    void replaceSpanText(int spanIndex, const std::u32string& newText);
    void replaceCharRange(int spanIndex, int startChar, int endChar, const std::u32string& newText);

    // Formatting
    void applyFormatting(int spanIndex, int startChar, int endChar,
                         const FormattingRun& formatting);
    void clearFormatting(int spanIndex, int startChar, int endChar);
    [[nodiscard]] FormattingRun formattingAtChar(int spanIndex, int charIndex) const;

    // Query
    [[nodiscard]] bool hasModifications() const;
    [[nodiscard]] std::vector<int> modifiedSpanIndices() const;

private:
    static void adjustFormattingRuns(std::vector<FormattingRun>& runs,
                                      int editPos, int removedLen, int insertedLen);
    std::vector<TextSpan> spans_;
};

} // namespace pdfe::textedit
