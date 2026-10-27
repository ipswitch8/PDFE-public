#pragma once

#include "ReflowTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace pdfe::reflow {

/// Pure engine that converts a raw UTF-8 paragraph + a width budget +
/// a word-measuring callable into a wrapped list of ReflowLine. No Qt
/// or MuPDF dependencies — callers inject the width measurement.
///
/// Also provides `lineOrigin()` to compute the per-line x offset for a
/// given alignment (the number that would go into the content stream's
/// Tm / Td operators).
class TextReflowEngine {
public:
    /// Measures the width of a single word in PDF points. Callers
    /// inject this from HarfBuzz or QFontMetrics as appropriate.
    using Measurer = std::function<float(const std::string& word)>;

    /// Break UTF-8 `text` into words on whitespace and measure each via
    /// `measurer`. Consecutive whitespace is collapsed; leading and
    /// trailing whitespace is dropped.
    static std::vector<ReflowWord> tokenize(const std::string& text,
                                            const Measurer& measurer);

    /// Greedy line breaker. Fills each line up to `maxWidthPt`. Words
    /// larger than maxWidthPt land on their own line and overflow.
    /// The returned vector always has at least one element when the
    /// input has any words.
    static std::vector<ReflowLine> reflow(const std::vector<ReflowWord>& words,
                                          float maxWidthPt,
                                          float spaceWidthPt);

    /// Compute the x-origin offset (relative to the block's left edge)
    /// for a line with the given measured width and alignment. For
    /// Justify, only the last line is treated as Left (so words don't
    /// space out unnaturally at paragraph end).
    static float lineXOffset(float blockWidth, float lineWidth,
                             Alignment alignment, bool isLastLine);

    /// Compute the extra inter-word spacing for Justify: returns the
    /// additional advance (in PDF points) to add to each space so the
    /// line fills the block. Returns 0 for non-Justify or when a line
    /// has fewer than 2 words (or is the last line in the block).
    static float justifyExtraSpace(float blockWidth, float lineWidth,
                                   int wordCount, Alignment alignment,
                                   bool isLastLine);
};

} // namespace pdfe::reflow
