#include "tre/AtlasBuilder.h"
#include "tre/GlyphTemplateCache.h"
#include "tre/OcrPipeline.h"
#include "tre/HocrParser.h"

#include <QDebug>
#include <algorithm>
#include <numeric>
#include <set>

namespace pdfe::tre {

int AtlasBuilder::selectReferenceLine(const std::vector<HocrLine>& lines)
{
    int bestIdx = 0;
    int bestCharCount = 0;

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const int charCount = std::accumulate(
            lines[i].words.begin(), lines[i].words.end(), 0,
            [](int acc, const auto& w) {
                return acc + static_cast<int>(w.chars.size());
            });
        if (charCount > bestCharCount) {
            bestCharCount = charCount;
            bestIdx = i;
        }
    }

    return bestIdx;
}


AtlasBuilder::Options AtlasBuilder::defaultOptions() {
    return {};
}

AtlasBuilder::BuildResult AtlasBuilder::build(
    const QImage& pageImage,
    const std::vector<HocrLine>& allLines,
    const RgbColor& textColor,
    const RgbColor& bgColor,
    OcrPipeline* ocrPipeline,
    std::function<QImage(int pageIndex)> renderPage,
    int pageIndex,
    int totalPages,
    const Options& options)
{
    BuildResult result;

    if (allLines.empty() || pageImage.isNull()) {
        result.enrichedAtlas = std::make_shared<EnrichedAtlas>();
        return result;
    }

    // Step 1: Select reference line (clicked line if specified, else longest)
    int refIdx = (options.clickedLineIdx >= 0
                  && options.clickedLineIdx < static_cast<int>(allLines.size()))
        ? options.clickedLineIdx
        : selectReferenceLine(allLines);
    const HocrLine& refLine = allLines[refIdx];
    qDebug() << "[AtlasBuilder] refIdx=" << refIdx
             << "clickedLineIdx=" << options.clickedLineIdx
             << "totalLines=" << allLines.size();

    // Step 1b: Render reference templates if font path is provided.
    // These are FreeType-rendered single-character images used to score
    // candidate crops from the scan via NCC (normalized cross-correlation).
    // This picks the cleanest instance of each character, avoiding
    // contamination from adjacent characters in overlapping HOCR bboxes.
    TemplateMap templates;
    const TemplateMap* templatesPtr = nullptr;
    if (!options.fontFilePath.empty()) {
        // Compute reference line height for template rendering
        int refLineH = std::max(1, static_cast<int>(refLine.bbox.y1 - refLine.bbox.y0));

        // Collect unique codepoints across all lines
        std::set<char32_t> uniqueCps;
        for (const auto& ln : allLines) {
            for (const auto& w : ln.words) {
                for (const auto& c : w.chars) {
                    if (c.codepoint != 0)
                        uniqueCps.insert(c.codepoint);
                }
            }
        }
        std::vector<char32_t> cpVec(uniqueCps.begin(), uniqueCps.end());

        templates = GlyphTemplateCache::renderTemplates(
            options.fontFilePath, options.fontFaceIndex, refLineH, cpVec);

        if (!templates.empty()) {
            templatesPtr = &templates;
            qDebug() << "[AtlasBuilder] Rendered" << templates.size()
                     << "templates at lineH=" << refLineH
                     << "from" << QString::fromStdString(options.fontFilePath);
        }
    }

    // Step 2: Build verified atlas from same page
    std::vector<HocrLine> otherLines;
    otherLines.reserve(allLines.size() - 1);
    for (int i = 0; i < static_cast<int>(allLines.size()); ++i) {
        if (i != refIdx)
            otherLines.push_back(allLines[i]);
    }

    GlyphAtlas atlas;
    result.samePageDonorLines = atlas.buildVerified(
        pageImage, refLine, otherLines, textColor, bgColor, templatesPtr);
    result.samePageRejectedLines =
        static_cast<int>(otherLines.size()) - result.samePageDonorLines;

    // Step 3: Cross-page harvesting if coverage is low
    if (!options.presynthText.empty()) {
        float cov = atlas.coverage(options.presynthText);
        if (cov < options.minCoverageForCrossPage && ocrPipeline && renderPage) {
            // Try pages near the current one (interleaved: -1, +1, -2, +2, ...)
            std::vector<int> candidatePages;
            for (int delta = 1; delta <= options.maxCrossPages; ++delta) {
                if (pageIndex - delta >= 0)
                    candidatePages.push_back(pageIndex - delta);
                if (pageIndex + delta < totalPages)
                    candidatePages.push_back(pageIndex + delta);
            }

            for (int cpIdx : candidatePages) {
                ++result.crossPagesScanned;
                try {
                    std::string charHocr = ocrPipeline->ocrPageWithCharBoxes(cpIdx);
                    if (charHocr.empty())
                        continue;

                    auto charDoc = HocrParser::parse(charHocr);

                    // Collect all lines from donor page
                    std::vector<HocrLine> donorLines;
                    for (const auto& blk : charDoc.blocks) {
                        for (const auto& par : blk.paragraphs) {
                            std::copy(par.lines.begin(), par.lines.end(),
                                      std::back_inserter(donorLines));
                        }
                    }

                    if (donorLines.empty())
                        continue;

                    QImage donorImage = renderPage(cpIdx);
                    if (donorImage.isNull())
                        continue;

                    // Build a temporary atlas from verified donor lines.
                    // Cross-page verification compares the reference line's glyphs
                    // (from the current page image) against donor line glyphs
                    // (from the donor page image). Since LineFontVerifier crops
                    // from a single pageImage, we compare donor lines against
                    // each other using the donor image, picking the donor's
                    // longest line as a proxy reference.
                    int donorRefIdx = selectReferenceLine(donorLines);

                    // But first verify the donor's ref line matches our ref line
                    // by comparing their HOCR metrics (can't do pixel NCC across
                    // different page images with the current API).
                    auto crossVerify = LineFontVerifier::verify(
                        pageImage, refLine, donorLines[donorRefIdx],
                        options.verifyThresholds);

                    // For cross-page, metric-only comparison is acceptable
                    // since we can't do pixel NCC across different images.
                    // Use a relaxed threshold: just check metric score.
                    if (crossVerify.metricScore < options.verifyThresholds.minMetricScore)
                        continue;

                    // Build donor atlas from all lines on donor page
                    // (they're already verified against each other via buildVerified)
                    std::vector<HocrLine> donorOther;
                    for (int i = 0; i < static_cast<int>(donorLines.size()); ++i) {
                        if (i != donorRefIdx)
                            donorOther.push_back(donorLines[i]);
                    }

                    GlyphAtlas donorAtlas;
                    int donorAccepted = donorAtlas.buildVerified(
                        donorImage, donorLines[donorRefIdx], donorOther,
                        textColor, bgColor, templatesPtr);

                    int beforeMerge = atlas.size();
                    atlas.mergeFrom(donorAtlas);
                    int harvested = atlas.size() - beforeMerge;
                    result.crossPageDonorLines += donorAccepted + 1;  // +1 for donor ref line

                    qDebug() << "[AtlasBuilder] Cross-page" << cpIdx
                             << "harvested" << harvested << "new glyphs"
                             << "from" << (donorAccepted + 1) << "verified lines";

                    // Re-check coverage
                    if (atlas.coverage(options.presynthText) >= options.minCoverageForCrossPage)
                        break;

                } catch (const std::exception& e) {
                    qDebug() << "[AtlasBuilder] Cross-page harvest failed for page"
                             << cpIdx << ":" << e.what();
                }
            }
        }
    }

    result.atlasGlyphCount = atlas.size();

    // Step 4: Build EnrichedAtlas with synthesis
    auto enriched = std::make_shared<EnrichedAtlas>();
    enriched->build(std::move(atlas), options.presynthText);
    result.synthesizedGlyphCount = enriched->size() - result.atlasGlyphCount;
    result.enrichedAtlas = std::move(enriched);

    // Compute unique chars in presynthText for coverage reporting
    int uniqueTextChars = 0;
    if (!options.presynthText.empty()) {
        std::set<char32_t> unique;
        for (char32_t c : options.presynthText)
            if (c != U' ' && c != U'\t' && c != U'\n' && c != U'\r')
                unique.insert(c);
        uniqueTextChars = static_cast<int>(unique.size());
    }

    qDebug() << "[AtlasBuilder::build] SUMMARY:"
             << "scanCropped=" << result.atlasGlyphCount
             << "synthesized=" << result.synthesizedGlyphCount
             << "total=" << result.enrichedAtlas->size()
             << "of" << uniqueTextChars << "unique chars"
             << "| samePageDonors=" << result.samePageDonorLines
             << "rejected=" << result.samePageRejectedLines
             << "crossPageLines=" << result.crossPageDonorLines
             << "crossPagesScanned=" << result.crossPagesScanned;

    return result;
}

} // namespace pdfe::tre
