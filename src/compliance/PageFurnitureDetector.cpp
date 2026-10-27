#include "PageFurnitureDetector.h"
#include "AutoTagger.h"

#include "../core/MuPdfContext.h"
#include "../core/StextOptions.h"
#include "../core/Document.h"
#include "core/FormXObjectDepthGuard.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

namespace {

// One candidate block: a text block that sits inside the top or bottom band of
// its page. Kept separate from FurnitureRegion because a candidate only becomes
// furniture once the repeat test over the whole document has run.
struct Candidate {
    int pageIndex = -1;
    ArtifactSubtype subtype = ArtifactSubtype::Header;
    float fzBbox[4] = {0.f, 0.f, 0.f, 0.f};
    float bbox[4] = {0.f, 0.f, 0.f, 0.f};
    std::string normalizedText;
};

std::string blockText(const fz_stext_block* block) {
    std::string out;
    for (const fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
        for (const fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
            char buf[8] = {0};
            const int n = fz_runetochar(buf, ch->c);
            if (n > 0) out.append(buf, static_cast<size_t>(n));
        }
        out.push_back(' ');
    }
    return out;
}

// Median glyph size over every text char on the page. Used as the "what counts
// as normal body text here" yardstick for the watermark size test, so the rule
// travels to a page set in 6pt as well as one set in 12pt.
float medianCharSize(const fz_stext_page* page) {
    std::vector<float> sizes;
    for (const fz_stext_block* b = page->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (const fz_stext_line* l = b->u.t.first_line; l; l = l->next) {
            for (const fz_stext_char* c = l->first_char; c; c = c->next) {
                if (c->size > 0.f) sizes.push_back(c->size);
            }
        }
    }
    if (sizes.empty()) return 0.f;
    const size_t mid = sizes.size() / 2;
    std::nth_element(sizes.begin(), sizes.begin() + mid, sizes.end());
    return sizes[mid];
}

// True when every baseline in `block` is turned away from horizontal by at
// least `minSin`. An empty block, or one containing a vertical-writing-mode
// line, is NOT rotated.
bool blockIsRotated(const fz_stext_block* block, float minSin) {
    int lines = 0;
    for (const fz_stext_line* l = block->u.t.first_line; l; l = l->next) {
        if (l->wmode != 0) return false;             // tategaki is content
        const float dx = l->dir.x, dy = l->dir.y;
        if (dx == 0.f && dy == 0.f) return false;    // degenerate direction
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.f) return false;
        if (std::fabs(dy) / len < minSin) return false;
        ++lines;
    }
    return lines > 0;
}

float blockMaxCharSize(const fz_stext_block* block) {
    float maxSize = 0.f;
    for (const fz_stext_line* l = block->u.t.first_line; l; l = l->next) {
        for (const fz_stext_char* c = l->first_char; c; c = c->next) {
            if (c->size > maxSize) maxSize = c->size;
        }
    }
    return maxSize;
}

} // namespace

std::string normalizeForRepeatMatching(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    bool inDigits = false;
    bool pendingSpace = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            inDigits = false;
            if (!out.empty()) pendingSpace = true;
            continue;
        }
        if (std::isdigit(c)) {
            // A DIGIT RUN collapses to a single '#'. This is the whole reason a
            // folio can be detected at all: "1", "2", "3" on successive pages
            // are three different strings and would never repeat, but "#", "#",
            // "#" do. Roman-numeral folios are deliberately NOT folded — "i" /
            // "ii" / "iii" are indistinguishable from ordinary words here, and a
            // fold that swallowed them would start artifacting body text.
            if (inDigits) continue;
            inDigits = true;
            if (pendingSpace) { out.push_back(' '); pendingSpace = false; }
            out.push_back('#');
            continue;
        }
        inDigits = false;
        if (pendingSpace) { out.push_back(' '); pendingSpace = false; }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

ArtifactRegion furnitureToArtifactRegion(const FurnitureRegion& region) {
    ArtifactRegion out;
    // Running heads, running feet and folios are PAGINATION artifacts
    // (PDF 32000-1 14.8.2.2.2), not Layout and not Background. A watermark is
    // Pagination too: /Watermark is one of the three /Subtype values Table 331
    // defines for a Pagination artifact, alongside /Header and /Footer.
    out.type = ArtifactType::Pagination;
    out.subtype = region.subtype;
    out.hasBBox = true;
    for (int i = 0; i < 4; ++i) out.bbox[i] = region.bbox[i];
    // No contentOperators: the furniture is already drawn by the page's own
    // content. This marking DECLARES the region, it does not redraw it —
    // re-emitting the operators here would paint the running head a second time
    // and duplicate it for every text extractor.
    return out;
}

std::vector<ArtifactRegion> artifactRegionsForPage(
    const std::vector<FurnitureRegion>& regions, int pageIndex)
{
    std::vector<ArtifactRegion> out;
    for (const auto& r : regions) {
        if (r.pageIndex == pageIndex) out.push_back(furnitureToArtifactRegion(r));
    }
    return out;
}

std::vector<FurnitureRegion> AutoTagger::detectPageFurniture(
    pdfe::core::Document* doc, const FurnitureOptions& options)
{
    std::vector<FurnitureRegion> furniture;
    if (!doc) return furniture;

    const int pageCount = doc->pageCount();
    if (pageCount < options.minPages) return furniture;

    std::vector<Candidate> candidates;

    // SCRATCH IN THIS FRAME, NOT INSIDE THE LAMBDA (GitHub #118's hazard class).
    //
    // `norm` used to be declared inside the inner block loop, which sits inside
    // both an explicit fz_try and the execute lambda's implicit one. MuPDF
    // unwinds by longjmp, so an fz_throw from a later safeLoadPage or
    // fz_new_stext_page_from_page skips the destructors of everything declared
    // in between and leaks the string's buffer — and running-header text is
    // routinely past the small-string optimisation.
    //
    // Reused across iterations rather than redeclared: assignment replaces the
    // contents, and the single destruction happens here on the way out, under
    // ordinary C++ unwinding. `candidates` above is already hoisted for exactly
    // this reason, so this follows the shape the file already had.
    std::string norm;

    context()->execute([&](fz_context* ctx) {
        fz_document* fzDoc = doc->fzDocument();

        for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
            fz_page* page = nullptr;
            fz_stext_page* stextPage = nullptr;
            fz_var(page);
            fz_var(stextPage);

            fz_try(ctx) {
                page = pdfe::core::safeLoadPage(ctx, fzDoc, pageIndex);

                fz_stext_options opts = pdfe::core::pdfeStextOptions(
                    FZ_STEXT_PRESERVE_WHITESPACE);
                stextPage = fz_new_stext_page_from_page(ctx, page, &opts);

                const fz_rect bounds = stextPage->mediabox;
                const float height = bounds.y1 - bounds.y0;

                // The inverse page transform takes an stext (device-space,
                // y-down) rect back to DEFAULT USER SPACE (y-up), which is the
                // space an artifact's /BBox is expressed in. Guessing a y-flip
                // from the bounds would be wrong on any page with a rotation or
                // a non-zero MediaBox origin.
                fz_matrix toUser = fz_identity;
                pdf_page* pp = pdf_page_from_fz_page(ctx, page);
                if (pp) {
                    fz_rect mediabox;
                    fz_matrix ctm;
                    pdf_page_transform(ctx, pp, &mediabox, &ctm);
                    toUser = fz_invert_matrix(ctm);
                }

                if (height > 0.f) {
                    const float band = height * options.bandFraction;
                    const float topEdge = bounds.y0 + band;
                    const float bottomEdge = bounds.y1 - band;

                    for (fz_stext_block* block = stextPage->first_block; block;
                         block = block->next) {
                        if (block->type != FZ_STEXT_BLOCK_TEXT) continue;

                        const bool inTop = block->bbox.y1 <= topEdge;
                        const bool inBottom = block->bbox.y0 >= bottomEdge;
                        if (!inTop && !inBottom) continue;

                        norm = normalizeForRepeatMatching(blockText(block));
                        if (norm.empty()) continue;

                        Candidate c;
                        c.pageIndex = pageIndex;
                        c.subtype = inTop ? ArtifactSubtype::Header
                                          : ArtifactSubtype::Footer;
                        c.fzBbox[0] = block->bbox.x0;
                        c.fzBbox[1] = block->bbox.y0;
                        c.fzBbox[2] = block->bbox.x1;
                        c.fzBbox[3] = block->bbox.y1;

                        const fz_rect user = fz_transform_rect(block->bbox, toUser);
                        c.bbox[0] = user.x0;
                        c.bbox[1] = user.y0;
                        c.bbox[2] = user.x1;
                        c.bbox[3] = user.y1;

                        c.normalizedText = norm;
                        candidates.push_back(c);
                    }
                }
            }
            fz_always(ctx) {
                fz_drop_stext_page(ctx, stextPage);
                fz_drop_page(ctx, page);
            }
            fz_catch(ctx) {
                // Skip an unreadable page; the repeat test simply sees one
                // fewer occurrence.
            }
        }
    });

    // A candidate is furniture only if the SAME normalized string occupies the
    // SAME band on at least `minPages` DISTINCT pages. Counting occurrences
    // instead of pages would let a single page that repeats a word twice in its
    // footer promote itself.
    std::map<std::pair<int, std::string>, std::set<int>> pagesByKey;
    for (const auto& c : candidates) {
        pagesByKey[{static_cast<int>(c.subtype), c.normalizedText}].insert(c.pageIndex);
    }

    for (const auto& c : candidates) {
        const auto& pages =
            pagesByKey[{static_cast<int>(c.subtype), c.normalizedText}];
        if (static_cast<int>(pages.size()) < options.minPages) continue;

        FurnitureRegion r;
        r.pageIndex = c.pageIndex;
        r.subtype = c.subtype;
        for (int i = 0; i < 4; ++i) {
            r.fzBbox[i] = c.fzBbox[i];
            r.bbox[i] = c.bbox[i];
        }
        r.normalizedText = c.normalizedText;
        furniture.push_back(std::move(r));
    }

    std::stable_sort(furniture.begin(), furniture.end(),
                     [](const FurnitureRegion& a, const FurnitureRegion& b) {
                         return a.pageIndex < b.pageIndex;
                     });
    return furniture;
}

std::vector<FurnitureRegion> AutoTagger::detectWatermarks(
    pdfe::core::Document* doc, const WatermarkOptions& options)
{
    std::vector<FurnitureRegion> watermarks;
    if (!doc) return watermarks;

    const int pageCount = doc->pageCount();
    // NO minimum page count. That is the whole point of this being a second
    // signal: a watermark on a one-page document is still a watermark, whereas
    // the repeat test needs at least two pages before it can say anything.
    if (pageCount <= 0) return watermarks;

    // SCRATCH IN THIS FRAME, NOT INSIDE THE LAMBDA (GitHub #118's hazard class).
    //
    // `r` used to be declared inside the inner block loop. FurnitureRegion owns
    // a std::string (normalizedText), and MuPDF unwinds by longjmp, so an
    // fz_throw from a later page's safeLoadPage would skip its destructor and
    // leak that buffer. Declared here it is destroyed by ordinary C++ unwinding
    // however the region is left.
    //
    // RESET AT THE TOP OF EACH USE, which is load-bearing now that it outlives
    // one iteration: `watermarks.push_back(std::move(r))` leaves it in a moved-
    // from state, and every scalar field is assigned below but `normalizedText`
    // is assigned LAST — so without the reset a `continue` between the move and
    // that assignment could carry a previous block's text into the next region.
    FurnitureRegion r;

    context()->execute([&](fz_context* ctx) {
        fz_document* fzDoc = doc->fzDocument();

        for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
            fz_page* page = nullptr;
            fz_stext_page* stextPage = nullptr;
            fz_var(page);
            fz_var(stextPage);

            fz_try(ctx) {
                page = pdfe::core::safeLoadPage(ctx, fzDoc, pageIndex);

                // These flags MUST match the ones AutoTagger::tagPage uses.
                // FurnitureMatch::BlockBoxEquals compares this detector's block
                // box against the box tagPage sees, and differing stext options
                // can segment blocks differently.
                fz_stext_options opts = pdfe::core::pdfeStextOptions(
                    FZ_STEXT_PRESERVE_WHITESPACE | FZ_STEXT_PRESERVE_IMAGES);
                stextPage = fz_new_stext_page_from_page(ctx, page, &opts);

                const fz_rect bounds = stextPage->mediabox;
                const float width = bounds.x1 - bounds.x0;
                const float height = bounds.y1 - bounds.y0;
                // Guarded rather than `continue`d: a `continue` here would bind
                // to the page loop OUTSIDE the fz_try and jump out of the
                // setjmp block, which is undefined for MuPDF's exception macros.
                if (width > 0.f && height > 0.f) {

                const float pageCx = (bounds.x0 + bounds.x1) * 0.5f;
                const float pageCy = (bounds.y0 + bounds.y1) * 0.5f;
                const float shortSide = std::min(width, height);
                const float median = medianCharSize(stextPage);

                fz_matrix toUser = fz_identity;
                pdf_page* pp = pdf_page_from_fz_page(ctx, page);
                if (pp) {
                    fz_rect mediabox;
                    fz_matrix ctm;
                    pdf_page_transform(ctx, pp, &mediabox, &ctm);
                    toUser = fz_invert_matrix(ctm);
                }

                for (fz_stext_block* block = stextPage->first_block; block;
                     block = block->next) {
                    if (block->type != FZ_STEXT_BLOCK_TEXT) continue;

                    // ROTATED
                    if (!blockIsRotated(block, options.minRotationSin)) continue;

                    // CENTRED
                    const float cx = (block->bbox.x0 + block->bbox.x1) * 0.5f;
                    const float cy = (block->bbox.y0 + block->bbox.y1) * 0.5f;
                    if (std::fabs(cx - pageCx) >
                            options.maxCentreOffsetFraction * width) continue;
                    if (std::fabs(cy - pageCy) >
                            options.maxCentreOffsetFraction * height) continue;

                    // LARGE (either arm)
                    const float size = blockMaxCharSize(block);
                    const bool largeAbsolute =
                        size >= options.minSizeFractionOfShortSide * shortSide;
                    const bool largeRelative =
                        median > 0.f &&
                        size >= options.minSizeRatioToMedian * median;
                    if (!largeAbsolute && !largeRelative) continue;

                    r = FurnitureRegion{};
                    r.pageIndex = pageIndex;
                    r.subtype = ArtifactSubtype::Watermark;
                    r.match = FurnitureMatch::BlockBoxEquals;
                    r.fzBbox[0] = block->bbox.x0;
                    r.fzBbox[1] = block->bbox.y0;
                    r.fzBbox[2] = block->bbox.x1;
                    r.fzBbox[3] = block->bbox.y1;

                    const fz_rect user = fz_transform_rect(block->bbox, toUser);
                    r.bbox[0] = user.x0;
                    r.bbox[1] = user.y0;
                    r.bbox[2] = user.x1;
                    r.bbox[3] = user.y1;

                    r.normalizedText =
                        normalizeForRepeatMatching(blockText(block));
                    watermarks.push_back(std::move(r));
                }

                }   // width/height guard
            }
            fz_always(ctx) {
                fz_drop_stext_page(ctx, stextPage);
                fz_drop_page(ctx, page);
            }
            fz_catch(ctx) {
                // Skip an unreadable page.
            }
        }
    });

    return watermarks;
}

} // namespace pdfe::compliance
