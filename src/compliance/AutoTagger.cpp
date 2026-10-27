#include "AutoTagger.h"
#include "../core/MuPdfContext.h"
#include "../core/StextOptions.h"
#include "../core/Document.h"
#include "core/FormXObjectDepthGuard.h"
#include "../core/FontNameUtil.h"

#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

class AutoTagger::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;

    // Compute average font size across all text blocks on a page
    float computeAverageFontSize(const fz_context* ctx, const fz_stext_page* stextPage) {
        float totalSize = 0;
        int count = 0;

        for (fz_stext_block* block = stextPage->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                    if (ch->size > 0) {
                        totalSize += ch->size;
                        count++;
                    }
                }
            }
        }

        return count > 0 ? totalSize / count : 12.0f;
    }

    // Check if text appears bold (heuristic based on font name). Resolves the
    // untruncated /BaseFont first: MuPDF's fz_font.name is a char[32] buffer, so
    // a weight keyword ("Bold"/"Black"/"Heavy") past char 31 of a long name
    // would otherwise be clipped off and the heading misclassified as regular.
    bool isBoldFont(fz_context* ctx, fz_font* font,
                    const pdfe::core::BaseFontMap& baseFontMap) {
        if (!font) return false;
        const char* fzName = fz_font_name(ctx, font);
        if (!fzName) return false;
        const std::string name =
            pdfe::core::resolveUntruncatedFontName(baseFontMap,
                                                   std::string(fzName));
        // Check for common bold indicators in font name
        return name.find("Bold") != std::string::npos ||
               name.find("bold") != std::string::npos ||
               name.find("BOLD") != std::string::npos ||
               name.find("Black") != std::string::npos ||
               name.find("Heavy") != std::string::npos;
    }

    // Determine the dominant font characteristics of a text block
    struct BlockInfo {
        float fontSize = 0;
        bool isBold = false;
        bool isImage = false;
    };

    BlockInfo analyzeTextBlock(fz_context* ctx, const fz_stext_block* block,
                               const pdfe::core::BaseFontMap& baseFontMap) {
        BlockInfo info;

        if (block->type == FZ_STEXT_BLOCK_IMAGE) {
            info.isImage = true;
            return info;
        }

        if (block->type != FZ_STEXT_BLOCK_TEXT) return info;

        float totalSize = 0;
        int count = 0;
        int boldCount = 0;

        for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
            for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                totalSize += ch->size;
                count++;
                if (isBoldFont(ctx, ch->font, baseFontMap)) boldCount++;
            }
        }

        if (count > 0) {
            info.fontSize = totalSize / count;
            info.isBold = (boldCount > count / 2); // More than half chars are bold
        }

        return info;
    }

    StructureTag classifyBlock(const BlockInfo& info, float avgSize) {
        if (info.isImage) return StructureTag::Figure;

        float ratio = info.fontSize / avgSize;

        if (info.isBold || ratio > 1.2f) {
            if (ratio > 1.8f) return StructureTag::H1;
            if (ratio > 1.4f) return StructureTag::H2;
            if (ratio > 1.2f || info.isBold) return StructureTag::H3;
        }

        return StructureTag::P;
    }
};

AutoTagger::AutoTagger(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

AutoTagger::~AutoTagger() = default;

std::shared_ptr<pdfe::core::MuPdfContext> AutoTagger::context() const {
    return impl_->context_;
}

namespace {

// True when `bbox` (MuPDF stext space) matches a detected furniture region for
// this page, under whichever rule that region recorded.
//
// BlockCentreInside — the header/footer rule. Containment is tested on the
// block CENTRE rather than on any overlap, so a body paragraph whose first line
// merely brushes the footer band is not swallowed whole.
//
// BlockBoxEquals — the watermark rule. A rotated watermark's axis-aligned box
// covers most of the page, so centre-containment would artifact every body
// paragraph sitting under it and the "watermark excluded" property would be
// indistinguishable from "everything excluded". Only the block that IS the
// watermark matches.
const FurnitureRegion* furnitureForBlock(
    const std::vector<FurnitureRegion>& furniture,
    int pageIndex,
    const fz_rect& bbox)
{
    const float cx = (bbox.x0 + bbox.x1) * 0.5f;
    const float cy = (bbox.y0 + bbox.y1) * 0.5f;
    for (const auto& r : furniture) {
        if (r.pageIndex != pageIndex) continue;

        if (r.match == FurnitureMatch::BlockBoxEquals) {
            const float t = kFurnitureBoxTolerance;
            if (std::fabs(bbox.x0 - r.fzBbox[0]) <= t &&
                std::fabs(bbox.y0 - r.fzBbox[1]) <= t &&
                std::fabs(bbox.x1 - r.fzBbox[2]) <= t &&
                std::fabs(bbox.y1 - r.fzBbox[3]) <= t) {
                return &r;
            }
            continue;
        }

        if (cx >= r.fzBbox[0] && cx <= r.fzBbox[2] &&
            cy >= r.fzBbox[1] && cy <= r.fzBbox[3]) {
            return &r;
        }
    }
    return nullptr;
}

} // namespace

std::vector<StructureNode> AutoTagger::tagPage(
    pdfe::core::Document* doc, int pageIndex, int& nextMcid,
    const std::vector<FurnitureRegion>& furniture)
{
    std::vector<StructureNode> nodes;
    if (!doc) return nodes;

    impl_->context_->execute([&](fz_context* ctx) {
        fz_document* fzDoc = doc->fzDocument();
        fz_page* page = nullptr;
        fz_stext_page* stextPage = nullptr;

        fz_try(ctx) {
            // Guarded: auto-tagging walks the page like any renderer.
            page = pdfe::core::safeLoadPage(ctx, fzDoc, pageIndex);

            // FZ_STEXT_PRESERVE_IMAGES is REQUIRED for MuPDF to emit
            // FZ_STEXT_BLOCK_IMAGE blocks (same reason Page.cpp:393 passes it).
            // Without it `analyzeTextBlock` never sees an image block,
            // `classifyBlock` never returns StructureTag::Figure, and the whole
            // figure/alt-text branch below is unreachable — an image-only page
            // auto-tagged to a structure tree containing no /Figure at all.
            fz_stext_options opts = pdfe::core::pdfeStextOptions(
                FZ_STEXT_PRESERVE_WHITESPACE | FZ_STEXT_PRESERVE_IMAGES);
            stextPage = fz_new_stext_page_from_page(ctx, page, &opts);

            float avgSize = impl_->computeAverageFontSize(ctx, stextPage);

            // Untruncated /BaseFont map for this page so bold detection survives
            // MuPDF's 31-char fz_font_name truncation on long font names.
            const pdfe::core::BaseFontMap baseFontMap =
                pdfe::core::buildBaseFontMap(ctx, page);

            for (fz_stext_block* block = stextPage->first_block; block; block = block->next) {
                auto info = impl_->analyzeTextBlock(ctx, block, baseFontMap);
                StructureTag tag = impl_->classifyBlock(info, avgSize);

                StructureNode node;
                node.pageIndex = pageIndex;

                // GitHub #173: page furniture, decided BEFORE any MCID is
                // handed out. An artifact gets mcid -1, so
                // StructureTreeBuilder writes no /StructElem for it and records
                // no ParentTree entry — and the MCID counter does not advance,
                // keeping the remaining ids contiguous.
                if (furnitureForBlock(furniture, pageIndex, block->bbox)) {
                    node.tag = StructureTag::Artifact;
                    node.mcid = -1;
                    nodes.push_back(std::move(node));
                    continue;
                }

                node.tag = tag;
                node.mcid = nextMcid++;

                if (tag == StructureTag::Figure) {
                    // NO PLACEHOLDER. This used to write the literal string
                    // "Image" for every figure. That is worse than writing
                    // nothing: /Alt (Image) satisfies every automated
                    // "figure has alternative text" check while telling a
                    // screen-reader user precisely nothing, so the document
                    // passes validation and remains inaccessible. Leaving /Alt
                    // absent and flagging the node keeps the omission visible
                    // and actionable to whoever can actually describe the
                    // image.
                    node.altText.clear();
                    node.needsAltText = true;
                }

                nodes.push_back(std::move(node));
            }
        }
        fz_always(ctx) {
            fz_drop_stext_page(ctx, stextPage);
            fz_drop_page(ctx, page);
        }
        fz_catch(ctx) {
            // Skip page on error
        }
    });

    return nodes;
}

StructureNode AutoTagger::tagDocument(
    pdfe::core::Document* doc,
    const std::vector<FurnitureRegion>& furniture)
{
    StructureNode root;
    root.tag = StructureTag::Document;

    if (!doc) return root;

    int pageCount = doc->pageCount();
    int nextMcid = 0;

    for (int i = 0; i < pageCount; ++i) {
        auto pageNodes = tagPage(doc, i, nextMcid, furniture);

        for (auto& node : pageNodes) {
            root.children.push_back(std::make_shared<StructureNode>(std::move(node)));
        }
    }

    return root;
}

} // namespace pdfe::compliance
