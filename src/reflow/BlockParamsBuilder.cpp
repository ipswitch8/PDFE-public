#include "BlockParamsBuilder.h"

namespace pdfe::reflow {

AlignmentWriter::BlockParams BlockParamsBuilder::build(
    const std::string& utf8Text,
    const QRectF& pdfBbox,
    float fontSizePt,
    Alignment alignment,
    TextReflowEngine::Measurer measurer,
    float lineHeightMultiplier,
    const std::string& fontResourceName)
{
    AlignmentWriter::BlockParams p;
    p.alignment = alignment;
    p.blockLeftPt  = static_cast<float>(pdfBbox.left());
    p.blockTopPt   = static_cast<float>(pdfBbox.top());
    p.blockWidthPt = static_cast<float>(pdfBbox.width());
    p.fontSizePt   = (fontSizePt > 0.0f) ? fontSizePt : 12.0f;
    p.lineHeightPt = p.fontSizePt * lineHeightMultiplier;
    p.fontResourceName = fontResourceName;

    const float approxCharW = p.fontSizePt * 0.5f;
    const float approxSpaceW = p.fontSizePt * 0.25f;

    TextReflowEngine::Measurer useMeasurer = measurer
        ? measurer
        : [approxCharW](const std::string& w) {
              return static_cast<float>(w.size()) * approxCharW;
          };

    auto words = TextReflowEngine::tokenize(utf8Text, useMeasurer);
    p.lines = TextReflowEngine::reflow(words, p.blockWidthPt, approxSpaceW);
    return p;
}

} // namespace pdfe::reflow
