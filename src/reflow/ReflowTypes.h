#pragma once

#include <QRectF>
#include <string>
#include <vector>

namespace pdfe::reflow {

enum class Alignment {
    Left,
    Center,
    Right,
    Justify,
};

/// A paragraph-level abstraction that sits above the FormattingRun/TextSpan
/// layer in pdfe_textedit. Aggregates one or more existing TextSpans under
/// a single alignment setting.
///
/// We deliberately don't modify TextSpan's layout to add alignment —
/// adding members to that struct has repeatedly caused stale-.obj heap
/// corruption in this project. Instead, FormattingBlock references spans
/// by index so existing call-sites keep working unchanged.
struct FormattingBlock {
    /// Indices into the owning TextEditModel's span list.
    std::vector<int> spanIndices;

    Alignment alignment = Alignment::Left;

    /// Block bounds in PDF coords. Empty = use the union of the
    /// referenced spans' bounding boxes.
    QRectF bounds;
};

/// Single word produced by tokenization. Carries the word's measured
/// advance width so reflow doesn't re-measure.
struct ReflowWord {
    std::string text;      // UTF-8
    float width = 0.0f;    // advance width in PDF points
};

/// A single output line from reflow.
struct ReflowLine {
    std::string text;
    float       width = 0.0f;   // measured width of this line's glyph run
    int         wordCount = 0;
    bool        isLastLine = false;  // last line of the block
};

} // namespace pdfe::reflow
