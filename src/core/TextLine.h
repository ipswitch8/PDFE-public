#pragma once

#include "TextChar.h"
#include <vector>
#include <string>
#include <QRectF>

namespace pdfe::core {

/// A line of text extracted from a PDF page.
struct TextLine {
    std::vector<TextChar> chars;
    QRectF boundingBox;           ///< Line bounding box in PDF coordinates
    int writingMode = 0;          ///< 0 = horizontal (LTR/RTL), 1 = vertical

    /// Concatenate characters into a UTF-8 string.
    std::string text() const;
};

} // namespace pdfe::core
