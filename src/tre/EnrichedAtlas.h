#pragma once

#include "tre/GlyphAtlas.h"
#include "tre/GlyphSynthesizer.h"
#include "tre/ScanStyleTransfer.h"
#include "tre/TreTypes.h"
#include <QImage>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdfe::tre {

class BitmapCompositor;

/// A GlyphAtlas enriched with pre-synthesized glyphs and an on-demand
/// synthesis capability. Provides a unified glyph lookup for both the
/// overlay and save paths, ensuring WYSIWYG consistency.
///
/// Lookup order:
///   1. Original atlas glyph (highest fidelity -- cropped from scan)
///   2. Pre-synthesized glyph (computed at build time)
///   3. On-demand synthesis (for characters typed after initial build)
///   4. Null image (caller falls back to QFont/FreeType)
class EnrichedAtlas {
public:
    /// Build from a GlyphAtlas. Profiles the synthesizer and optionally
    /// pre-synthesizes missing characters for the given text.
    /// @param atlas         The font-verified glyph atlas (moved in).
    /// @param presynthText  Text whose missing chars should be pre-synthesized.
    void build(GlyphAtlas atlas, const std::u32string& presynthText = {});

    /// Which level supplied a glyph. GitHub #268: a caller that rescales atlas
    /// glyphs from scan space into composite space must NOT also rescale a
    /// FreeType-fallback glyph that was already produced at composite scale,
    /// and it cannot tell them apart from the QImage alone.
    enum class GlyphSource { None, Atlas, Synthesized, FreeType };

    /// Look up a glyph: atlas -> pre-synth -> on-demand synth -> FreeType -> null.
    /// @param source  Optional out-param reporting which level answered.
    [[nodiscard]] QImage glyph(char32_t codepoint,
                               GlyphSource* source = nullptr) const;

    /// GitHub #268. Scale at which FreeType-fallback glyphs are NORMALISED.
    ///
    /// The fallback renders a glyph, crops it to ink, scales it by
    /// atlasCapH/ftCapH to match the atlas, and places it on a refLineH canvas
    /// — so its output lands in ATLAS (scan-DPI) space. A compositor drawing a
    /// region at a different DPI then rescales everything by
    /// compositeDpi/atlasDpi, which for these glyphs means rendering at one
    /// size and resampling to another. On a 300-DPI scan composited at 150 that
    /// is a 2x downsample of an anti-aliased glyph, and hinting and AA are
    /// tuned for the OUTPUT grid, so the result is measurably softer than
    /// rendering at the target size would have been.
    ///
    /// Setting this to the compositor's own ratio makes the fallback normalise
    /// straight to composite space, so no resample happens. Glyphs produced
    /// that way report GlyphSource::FreeType and must NOT be rescaled again.
    ///
    /// 1.0 (the default) reproduces the previous behaviour exactly.
    /// Changing it invalidates the FreeType METRICS but NOT the glyph cache,
    /// which is keyed by (codepoint, scale) — a mixed-DPI page alternates
    /// between two scales and wholesale clearing would make each switch evict
    /// the other scale's glyphs.
    ///
    /// `const` deliberately, matching this class's existing idiom: the whole
    /// FreeType fallback is a lazy render cache behind a const interface, and
    /// ftCache_, ftCanvasH_, ftBaseline_, ftCapH_, atlasCapH_ and
    /// ftMetricsValid_ are all already `mutable` for that reason. The scale is
    /// render configuration of the same kind, and the compositor that needs to
    /// set it holds a `const EnrichedAtlas&`.
    void setFreeTypeOutputScale(double scale) const;

    /// Current FreeType output scale. 1.0 means atlas-space normalisation.
    [[nodiscard]] double freeTypeOutputScale() const { return ftOutputScale_; }

    /// The dpi the FreeType fallback actually rasterises at: ftDpi_ scaled by
    /// the output scale. Exposed so a test can assert the rasterisation grid
    /// moved, rather than inferring it from the look of the output.
    [[nodiscard]] int effectiveFtDpi() const;

    /// Number of ENTRIES in the FreeType glyph cache, i.e. distinct
    /// (codepoint, output scale) pairs — NOT distinct codepoints, which is what
    /// size() reports.
    ///
    /// A test seam, and it exists because phase-9's claim-check gate was right
    /// to object: the original cache test asserted only that two scales produce
    /// different image heights and that switching back restores the first
    /// height. Both hold equally if the glyph is silently RE-RENDERED on every
    /// switch, so the test proved output correctness while the claim being made
    /// was about cache residency. Those are different properties and a
    /// mixed-DPI page cares about the second one. This is the observable that
    /// tells them apart.
    [[nodiscard]] std::size_t freeTypeCacheEntryCount() const { return ftCache_.size(); }

    /// Look up a glyph from scan sources only (atlas + synthesis).
    /// Does NOT fall back to FreeType. Returns null for chars that require FreeType.
    /// Used by comparison dialog to show actual scan quality vs FreeType.
    [[nodiscard]] QImage glyphFromScan(char32_t codepoint) const;

    /// Check if any source (atlas or synthesis) can provide this glyph.
    [[nodiscard]] bool contains(char32_t codepoint) const;

    /// Number of glyphs available (atlas + synthesized + FreeType cache).
    [[nodiscard]] int size() const;

    /// Number of scan-sourced glyphs (atlas + synthesized, no FreeType).
    [[nodiscard]] int scanSize() const;

    /// Coverage for a text string, including synthesizable characters.
    [[nodiscard]] float coverage(const std::u32string& text) const;

    /// Configure FreeType fallback rendering with scan style transfer.
    /// Call after build(). When set, glyph() renders missing chars via
    /// BitmapCompositor, applies ScanStyleTransfer, and caches the result.
    /// @param compositor  Non-owning pointer; caller must keep alive.
    /// @param fontMatch   Font/color/size for FreeType rendering.
    /// @param dpi         Rendering DPI (usually scan DPI).
    void setFreeTypeFallback(BitmapCompositor* compositor,
                             const FontMatch& fontMatch, int dpi);

    /// Calibrate ScanStyleTransfer from an external atlas's glyphs.
    /// Used to calibrate a FreeType-only atlas from the scan-cropped atlas.
    void calibrateStyleFrom(const GlyphAtlas& referenceAtlas);

    /// Set reference line metrics from an external source.
    /// Used when building a FreeType-only atlas (empty GlyphAtlas) that needs
    /// reference dimensions from the scan-cropped atlas for proper glyph sizing.
    /// Clears the FreeType cache since cached glyphs were rendered at old dimensions.
    void setReferenceMetrics(int lineHeight, int baselineFromBottom);

    /// Get the raw atlas (for backward compatibility).
    [[nodiscard]] const GlyphAtlas& atlas() const { return atlas_; }

    /// All codepoints available (atlas + pre-synthesized).
    [[nodiscard]] std::vector<char32_t> codepoints() const;

    /// Reference line height in OCR pixels. This is the canonical height for
    /// all reference line glyphs and should be used as the scale denominator
    /// in overlay rendering (instead of computing maxH from glyph images,
    /// which can be wrong when donor glyphs have different heights).
    [[nodiscard]] int referenceLineHeight() const { return refLineH_; }

    /// Baseline offset measured from bottom of the reference line height.
    /// Used for accurate vertical glyph positioning in compositor output.
    [[nodiscard]] int referenceBaselineFromBottom() const { return refBaselineFromBottom_; }

    /// The DPI of the page render these glyphs were cropped from.
    ///
    /// GitHub #266. Every length in this class — `refLineH_`, the glyph images
    /// themselves — is in the pixels of that render, and the atlas is normally
    /// built from a page-wide render at the OCR DPI. A consumer compositing at
    /// a DIFFERENT DPI must rescale, and until this was recorded it had no way
    /// to know it needed to: on `mixed_dpi_page.pdf` the atlas is harvested at
    /// the page-wide 300 DPI while the 150-DPI thumbnail region is composited
    /// at its own native DPI, so atlas glyphs arrived at exactly twice the
    /// right size and rendered as a block. Synthesised and FreeType glyphs were
    /// unaffected, which is why only the atlas-sourced character was wrong.
    ///
    /// 0 means "not recorded" — consumers must then assume the DPIs match,
    /// which is the historical behaviour and correct for every single-DPI page.
    [[nodiscard]] int sourceDpi() const { return sourceDpi_; }

    /// Record the DPI of the page render this atlas was cropped from.
    /// See sourceDpi(). Call after build().
    void setSourceDpi(int dpi) { sourceDpi_ = dpi > 0 ? dpi : 0; }

private:
    /// Shared implementation for levels 1-3 (atlas + synthesis).
    /// Called by both glyph() and glyphFromScan() to avoid duplication.
    [[nodiscard]] QImage glyphFromScanImpl(char32_t codepoint) const;

    /// Normalize a glyph to refLineH_ using baseline-aligned padding.
    /// Glyphs taller than refLineH_ are scaled down; shorter ones get
    /// transparent padding with baseline alignment.
    [[nodiscard]] QImage normalizeHeight(const QImage& glyph) const;

    /// Compute and cache FreeType font metrics (baseline, cap-top, vertical
    /// window) by rendering a reference character ('H'). Called lazily on
    /// first FreeType fallback request.
    void ensureFreeTypeMetrics() const;

    GlyphAtlas atlas_;
    GlyphSynthesizer synthesizer_;
    mutable std::unordered_map<char32_t, QImage> synthesized_;  ///< mutable for lazy on-demand
    int refLineH_ = 0;              ///< Reference line height in OCR pixels
    int refBaselineFromBottom_ = 0;  ///< Baseline offset from image bottom (OCR px)
    int refInkHeight_ = 0;          ///< Average ink height of non-descender atlas glyphs
    int sourceDpi_ = 0;             ///< DPI of the render glyphs were cropped from; 0 = unrecorded

    // FreeType fallback (level 4) with scan style transfer
    BitmapCompositor* ftCompositor_ = nullptr;
    FontMatch ftFontMatch_;
    int ftDpi_ = 300;
    ScanStyleTransfer scanStyle_;

    /// GitHub #268. Output normalisation scale; 1.0 = atlas space, the historic
    /// behaviour. See setFreeTypeOutputScale(). `mutable` for the same reason
    /// as ftCache_ and the ft* metrics below: lazy render state behind a const
    /// interface.
    mutable double ftOutputScale_ = 1.0;

    /// Cache key for ftCache_. Keyed by (codepoint, output scale) rather than
    /// codepoint alone: #266 stopped short of fixing #268 precisely because one
    /// normalised image per codepoint cannot hold two target scales, and a
    /// mixed-DPI page asks for both. The scale is quantised to 1e-4 so that
    /// floating-point noise in an otherwise identical ratio does not multiply
    /// the cache.
    struct FtKey {
        char32_t cp;
        int scaleQ;
        bool operator==(const FtKey& o) const noexcept {
            return cp == o.cp && scaleQ == o.scaleQ;
        }
    };
    struct FtKeyHash {
        std::size_t operator()(const FtKey& k) const noexcept {
            return std::hash<std::uint64_t>{}(
                (static_cast<std::uint64_t>(k.cp) << 32)
                ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.scaleQ)));
        }
    };
    [[nodiscard]] static int quantiseScale(double s) noexcept {
        return static_cast<int>(std::lround(s * 10000.0));
    }

    mutable std::unordered_map<FtKey, QImage, FtKeyHash> ftCache_;  ///< mutable for lazy caching

    // Cached FreeType font metrics for baseline-anchored rendering.
    // Computed lazily by ensureFreeTypeMetrics() on first FreeType request.
    mutable int ftCanvasH_ = 0;       ///< Canvas height used for FreeType rendering
    mutable int ftBaseline_ = -1;     ///< Baseline row in FreeType canvas (bottom of 'H' ink)
    mutable int ftCapH_ = 0;          ///< FreeType cap-height in pixels (ink height of 'H')
    mutable int atlasCapH_ = 0;       ///< Atlas cap-height in pixels (ink height of atlas 'H')
    mutable bool ftMetricsValid_ = false;  ///< True once metrics have been computed
};

} // namespace pdfe::tre
