#pragma once

#include "ReflowTypes.h"
#include "TextReflowEngine.h"

#include <string>
#include <vector>

namespace pdfe::reflow {

/// Emits the PDF content-stream fragment that places a reflowed,
/// alignment-adjusted paragraph. Produces a self-contained BT…ET
/// block with per-line Tm matrices so the renderer sees exactly the
/// alignment the user selected regardless of what the original content
/// stream had for the same region.
///
/// The caller wraps this fragment into its page content stream; the
/// filter-based editor in pdfe_textedit is the likely consumer.
class AlignmentWriter {
public:
    struct BlockParams {
        std::vector<ReflowLine> lines;    // output of TextReflowEngine::reflow
        Alignment alignment = Alignment::Left;
        float blockLeftPt   = 0.0f;       // block's absolute left edge (PDF coords)
        float blockTopPt    = 0.0f;       // block's absolute top edge (PDF coords, Y-down)
        float blockWidthPt  = 0.0f;       // block width
        float lineHeightPt  = 12.0f;      // baseline-to-baseline spacing
        float fontSizePt    = 12.0f;      // active font size, used for Tc/Tw
        std::string fontResourceName = "F1"; // /F1 etc.
    };

    /// Build the PDF content-stream fragment for the block. The fragment
    /// is a single BT…ET region; the caller must ensure the named font
    /// resource is registered in the page's /Resources /Font dict
    /// before this is emitted.
    ///
    /// Marker comment: a leading "%%PDFE-ALIGN: <mode>\n" line records
    /// the chosen alignment so a subsequent reopen+edit pass can
    /// recover the paragraph's alignment intent without re-inferring
    /// it from visual positioning.
    static std::string emitFragment(const BlockParams& params);

    /// Just the marker comment, for callers that emit the body
    /// themselves. Static for testing.
    static std::string alignmentMarker(Alignment alignment);

    /// Parse the alignment marker from a content-stream fragment;
    /// returns Alignment::Left if not found. Static for testing.
    static Alignment parseAlignmentMarker(const std::string& content);

    /// Escape a UTF-8 string as a PDF literal-string body (handles
    /// `(`, `)`, `\\`). Exposed for testing.
    static std::string escapePdfLiteral(const std::string& utf8);
};

} // namespace pdfe::reflow
