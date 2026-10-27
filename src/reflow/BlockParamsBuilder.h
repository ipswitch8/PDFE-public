#pragma once

#include "AlignmentWriter.h"
#include "ReflowTypes.h"
#include "TextReflowEngine.h"

#include <QRectF>
#include <string>

namespace pdfe::reflow {

/// Build AlignmentWriter::BlockParams from a plain-text paragraph and a
/// PDF-space bounding box. Intended to bridge UI-layer selection state
/// (TextEditTool's active span) to the Phase-9 reflow pipeline without
/// making AlignmentPipeline depend on the textedit module.
///
/// The caller supplies the text + bbox + font size + alignment; this
/// helper tokenizes, reflows within the bbox width, and populates all
/// the geometric fields of BlockParams.
class BlockParamsBuilder {
public:
    /// `measurer` may be null, in which case a simple char-count
    /// approximation (0.5 * fontSizePt per char) is used. Real callers
    /// should pass a QFontMetricsF-backed measurer.
    /// `lineHeightMultiplier` defaults to 1.2 (typographic convention).
    /// `fontResourceName` defaults to "F1" matching AlignmentWriter's
    /// default — real callers should pass the actual PDF resource name.
    static AlignmentWriter::BlockParams build(
        const std::string& utf8Text,
        const QRectF& pdfBbox,
        float fontSizePt,
        Alignment alignment,
        TextReflowEngine::Measurer measurer = nullptr,
        float lineHeightMultiplier = 1.2f,
        const std::string& fontResourceName = "F1");
};

} // namespace pdfe::reflow
