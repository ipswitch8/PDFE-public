#pragma once

#include "ContentStreamTagger.h"   // ArtifactType / ArtifactSubtype / ArtifactRegion

#include <string>
#include <vector>

namespace pdfe::compliance {

// ── Page furniture (GitHub #173) ──────────────────────────────────────────
//
// A running head, a running foot and a folio are PAGE FURNITURE, not document
// content: PDF 32000-1 14.8.2.2 says they must be emitted as artifacts so an
// assistive-technology consumer skips them. `ContentStreamTagger::markArtifacts`
// has been able to emit that marking since GitHub #10 — but nothing ever
// DECIDED that a given block was furniture, so the capability had zero callers
// and every tagged document still handed its running head to a screen reader as
// body text.
//
// `AutoTagger::detectPageFurniture` is that decision. The signal is the
// conventional one: text that repeats, in the same vertical band, across
// several pages.
//
// ORDERING (the trap #173 names explicitly): detection MUST run before MCIDs are
// assigned. An artifact carries no /MCID and occupies no ParentTree slot, so a
// post-pass would have to unpick ids it should never have handed out. Detection
// therefore lives on the tagger itself and its output is fed INTO
// `AutoTagger::tagPage`, which tags a furniture block `StructureTag::Artifact`
// and skips the MCID counter for it.
// How `AutoTagger::tagPage` decides that one of a page's stext blocks belongs to
// a detected region. The two detectors record geometrically different things, so
// they cannot share one rule.
enum class FurnitureMatch {
    // The block's CENTRE lies inside the region's box. Correct for a running
    // head or foot: the recorded box is the furniture's own tight box, and
    // testing the centre keeps a body paragraph whose first line merely brushes
    // the band from being swallowed whole.
    BlockCentreInside,

    // The block's box IS the region's box, to within a fraction of a point.
    // Required for a watermark, because a rotated watermark's AXIS-ALIGNED
    // bounding box covers most of the page: under centre-containment every body
    // paragraph sitting underneath the watermark would be artifacted too, which
    // is precisely the "the heuristic excluded everything" failure. The
    // watermark detector runs stext with the same option flags `tagPage` uses,
    // so the two see identical block geometry.
    BlockBoxEquals,
};

// Edge tolerance, in points, for FurnitureMatch::BlockBoxEquals.
inline constexpr float kFurnitureBoxTolerance = 1.0f;

struct FurnitureRegion {
    int pageIndex = -1;
    ArtifactSubtype subtype = ArtifactSubtype::Header;   // Header, Footer or Watermark
    FurnitureMatch match = FurnitureMatch::BlockCentreInside;

    // MuPDF stext / device space for this page (y grows DOWNWARD, page-relative).
    // Used to decide which stext block is furniture during tagging.
    float fzBbox[4] = {0.f, 0.f, 0.f, 0.f};   // x0 y0 x1 y1

    // Default user space for this page (y grows UPWARD), which is what an
    // artifact's /BBox is expressed in.
    float bbox[4] = {0.f, 0.f, 0.f, 0.f};     // x0 y0 x1 y1

    // Whitespace-collapsed, case-folded text with every digit run folded to '#'
    // so "Page 1" and "Page 7" are recognised as the SAME running foot. Without
    // that fold a folio — the single most common piece of page furniture —
    // never repeats and is never detected.
    std::string normalizedText;
};

struct FurnitureOptions {
    // Fraction of page height at the top and at the bottom that counts as the
    // header / footer band.
    float bandFraction = 0.15f;
    // How many distinct pages a normalized string must occupy, in the same
    // band, before it is called furniture. 2 is the minimum that can mean
    // "repeats"; a one-page document therefore yields nothing, which is correct
    // — a single occurrence is indistinguishable from a title.
    int minPages = 2;
};

// ── Rotated centred watermarks (GitHub #173, second heuristic) ─────────────
//
// A watermark is page furniture too — PDF 32000-1 14.8.2.2.2 gives it its own
// artifact subtype — but NOTHING about the band-plus-repeat signal above can
// find one. A watermark sits in the MIDDLE of the page, not in a header or
// footer band, and it is perfectly normal for it to appear on a ONE-PAGE
// document, where the repeat test cannot fire at all (`pageCount < minPages`
// returns immediately). Widening the band until it reached the page centre
// would simply artifact the body; the two signals have to stay separate.
//
// The watermark signal is its own three-part conjunction, and all three parts
// are required:
//
//   ROTATED   every baseline in the block is turned away from horizontal by at
//             least `minRotationSin`. Body text is set on the horizontal;
//             a ~45 degree "DRAFT" is not. Vertical WRITING MODE (wmode == 1,
//             i.e. CJK tategaki) is explicitly NOT rotation — it is a language
//             feature and its text is content — so those lines disqualify a
//             block rather than qualifying it.
//   CENTRED   the block's centre is within `maxCentreOffsetFraction` of the
//             page centre on both axes.
//   LARGE     the glyphs are display-sized, by either of two arms: an absolute
//             fraction of the page's short side (which still fires on a page
//             whose ONLY text is the watermark), or a multiple of the page's
//             median glyph size (which fires on a small-format page where the
//             absolute arm would be too generous).
//
// Semi-transparency is the other thing a human notices about a watermark, and
// it is deliberately NOT part of the test: a constant-alpha /ca in an ExtGState
// does not reach `fz_stext_char::argb`, so keying on it would make the rule
// silently miss the most common way watermarks are actually drawn.
struct WatermarkOptions {
    // |sin| of the baseline direction's departure from horizontal.
    // 0.25 is about 14.5 degrees.
    float minRotationSin = 0.25f;

    // Permitted distance from the page centre to the block centre, as a
    // fraction of page width / page height respectively.
    float maxCentreOffsetFraction = 0.20f;

    // LARGE, arm 1: glyph size as a fraction of the page's shorter side.
    float minSizeFractionOfShortSide = 0.05f;

    // LARGE, arm 2: glyph size as a multiple of the page's median glyph size.
    float minSizeRatioToMedian = 2.0f;
};

/// The fold used for the repeat test. Exposed so a caller (and a test) can ask
/// the same question the detector asked. Empty result means "nothing worth
/// comparing".
std::string normalizeForRepeatMatching(const std::string& text);

/// Convert one detected region into the artifact marking that describes it.
ArtifactRegion furnitureToArtifactRegion(const FurnitureRegion& region);

/// Subset of `regions` belonging to `pageIndex`, converted to artifact regions.
std::vector<ArtifactRegion> artifactRegionsForPage(
    const std::vector<FurnitureRegion>& regions, int pageIndex);

} // namespace pdfe::compliance
