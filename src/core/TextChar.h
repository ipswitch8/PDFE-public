#pragma once

#include <QPointF>
#include <QRectF>
#include <string>

namespace pdfe::core {

/// A single character with position and font information extracted from a PDF page.
struct TextChar {
    char32_t codepoint = 0;      ///< Unicode codepoint
    QPointF origin;               ///< Baseline origin in PDF coordinates (bottom-left, Y-up)
    QRectF quad;                  ///< Character bounding box in PDF coordinates
    std::string fontName;         ///< Font name from the PDF
    float fontSize = 0.0f;        ///< Font size in points
    bool isItalic = false;        ///< Italic per the font's /FontDescriptor (name-independent)
    bool isBold = false;          ///< Bold per the font's /FontDescriptor (name-independent)
    uint32_t color = 0x000000;    ///< sRGB fill color of the glyph (0xRRGGBB; from stext argb)
};

} // namespace pdfe::core
