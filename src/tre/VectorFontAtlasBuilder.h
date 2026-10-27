#pragma once

#include "tre/GlyphAtlas.h"
#include "tre/TreTypes.h"

#include <string>
#include <vector>

struct FT_FaceRec_;
typedef struct FT_FaceRec_* FT_Face;

namespace pdfe::tre {

/// Result of rasterizing every glyph reachable through an `FT_Face`'s
/// character map. `atlas` holds one premultiplied-ARGB image per covered
/// codepoint (excluding glyph index 0 / `.notdef`). `missingCodepoints`
/// lists every codepoint from the caller's requested text that the face
/// cannot render — these are the characters the caller will need to
/// synthesize, substitute, or extend the embedded subset with.
struct VectorAtlasResult {
    GlyphAtlas atlas;
    std::vector<char32_t> missingCodepoints;

    /// Echo of the input target height (em pixel size). Tests verify the
    /// face's `size->metrics.x_ppem` matches.
    int targetHeightPx = 0;

    /// Number of distinct codepoints rasterized into `atlas`.
    int rasterizedCount = 0;
};

/// Builds a per-font glyph atlas by rasterizing each glyph in the face's
/// cmap via FreeType. This is the vector-PDF counterpart to
/// `GlyphAtlas::build` (which crops glyphs from a scanned page bitmap).
///
/// Used by phase-3+ of the vector-subset extension pipeline:
///   1. Phase 2 (this class): expose every glyph the embedded subset
///      contains as a queryable image cache.
///   2. Phase 3: feed missing-codepoint requests through `GlyphSynthesizer`
///      backed by these images.
///   3. Phase 4-5: use the same images as the basis for extending the
///      embedded subset's glyf / CharStrings tables.
///
/// Stateless static API — callers manage their own `FT_Face` lifetime
/// (typically obtained via `EmbeddedFontAnalyzer`'s extracted font data
/// or `FT_New_Memory_Face` over a font stream from the PDF).
class VectorFontAtlasBuilder {
public:
    /// Rasterize every glyph in @p face at @p targetHeightPx em size.
    /// @param face                 FreeType face. Must be valid; caller owns lifetime.
    ///                             The face's pixel size is mutated by this call.
    /// @param targetHeightPx       Em-square pixel size to rasterize at.
    ///                             Must be >= 1.
    /// @param requestedCodepoints  Optional text query. Codepoints in this
    ///                             string that the face cannot encode are
    ///                             reported in `missingCodepoints`. Pass an
    ///                             empty string to skip the missing check.
    /// @param textColor            Color used for premultiplied-alpha output.
    ///                             Defaults to opaque black.
    /// @return                     `VectorAtlasResult` with the populated atlas
    ///                             and missing-codepoint list. On failure
    ///                             (null face, invalid size, no cmap), returns
    ///                             an empty result.
    static VectorAtlasResult build(
        FT_Face face,
        int targetHeightPx,
        const std::u32string& requestedCodepoints = {},
        const RgbColor& textColor = RgbColor{0, 0, 0});
};

} // namespace pdfe::tre
