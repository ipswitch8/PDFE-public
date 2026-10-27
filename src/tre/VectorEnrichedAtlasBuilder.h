#pragma once

#include "tre/EnrichedAtlas.h"
#include "tre/TreTypes.h"

#include <memory>
#include <string>
#include <vector>

struct FT_FaceRec_;
typedef struct FT_FaceRec_* FT_Face;

namespace pdfe::tre {

/// Result of `VectorEnrichedAtlasBuilder::build`. Owns the constructed
/// `EnrichedAtlas` (heap-allocated because `EnrichedAtlas` is move-only
/// and the synth/FreeType caches contain `mutable` members the move
/// constructor would have to forward — wrapping in `unique_ptr` keeps
/// the result trivially movable for callers).
struct VectorEnrichedAtlasResult {
    std::unique_ptr<EnrichedAtlas> atlas;

    /// Codepoints requested via `requestedText` that are absent from the
    /// FT_Face cmap. After `EnrichedAtlas::build`, some of these may have
    /// been pre-synthesized via `GlyphSynthesizer` recipes; the caller can
    /// query `atlas->contains(cp)` to find out which.
    std::vector<char32_t> missingCodepoints;

    /// Echo of input target height (em pixel size).
    int targetHeightPx = 0;

    /// Number of glyphs harvested directly from the FT_Face cmap (i.e.
    /// the non-synthesized base atlas size). Synthesis adds entries on
    /// top of this.
    int rasterizedCount = 0;
};

/// One-call helper that turns an `FT_Face` into a fully-built
/// `EnrichedAtlas` — the form downstream overlay + save paths consume.
///
/// Internally:
///   1. Calls `VectorFontAtlasBuilder::build(face, ...)` to harvest
///      every glyph in the cmap into a `GlyphAtlas`.
///   2. Calls `EnrichedAtlas::build(atlas, requestedText)` so the
///      synthesizer profiles the harvested glyphs and pre-synthesizes
///      any missing requested codepoints via recipe + transform.
///   3. Calls `EnrichedAtlas::calibrateStyleFrom(harvestedAtlas)` so
///      a later `setFreeTypeFallback` invocation produces glyphs whose
///      stroke weight, blur, and ink density match the embedded font's
///      visual style instead of the raw FreeType outline.
///
/// The caller is responsible for the `FT_Face` lifetime (typically owned
/// by `EmbeddedFontAnalyzer`'s extracted-font-data buffer or a system
/// font path).
class VectorEnrichedAtlasBuilder {
public:
    static VectorEnrichedAtlasResult build(
        FT_Face face,
        int targetHeightPx,
        const std::u32string& requestedText = {},
        const RgbColor& textColor = RgbColor{0, 0, 0});
};

} // namespace pdfe::tre
