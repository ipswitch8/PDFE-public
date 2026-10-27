#pragma once

#include "TextLine.h"
#include <vector>
#include <string>
#include <QRectF>

namespace pdfe::core {

enum class TextBlockType {
    Paragraph,
    List,
    Table,
    Image,      ///< Image block (no text content)
    Unknown
};

/// A block of text (typically a paragraph) extracted from a PDF page.
struct TextBlock {
    std::vector<TextLine> lines;
    QRectF boundingBox;            ///< Block bounding box in PDF coordinates
    TextBlockType type = TextBlockType::Unknown;

    /// Concatenate all lines into a UTF-8 string, separated by newlines.
    std::string text() const;
};

} // namespace pdfe::core
