#include "fonts/FontSubstitutionEngine.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/GlyphScaleAdjuster.h"
#include "fonts/TextShaper.h"
#include "fonts/PdfFontEmbedder.h"
#include "textedit/TextEditModel.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/Document.h"

#include <algorithm>
#include <unordered_map>

namespace pdfe::fonts {

FontSubstitutionEngine::FontSubstitutionEngine(
    pdfe::core::MuPdfContext* context,
    const std::filesystem::path& cachePath)
    : context_(context)
    , analyzer_(std::make_unique<EmbeddedFontAnalyzer>(context))
    , cache_(std::make_unique<FontAnalysisCache>(cachePath))
    , shaper_(std::make_unique<TextShaper>())
    , embedder_(std::make_unique<PdfFontEmbedder>(context))
{
    selector_ = std::make_unique<SubstituteFontSelector>(analyzer_.get(), cache_.get());
    scaleAdjuster_ = std::make_unique<GlyphScaleAdjuster>(shaper_.get());
}

FontSubstitutionEngine::~FontSubstitutionEngine() = default;

std::vector<SubstitutionInfo> FontSubstitutionEngine::processEdits(
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    const std::vector<pdfe::textedit::TextSpan>& allSpans,
    const std::vector<std::pair<int, std::u32string>>& modifications)
{
    std::vector<SubstitutionInfo> results;

    if (!page || !document || modifications.empty())
        return results;

    // Step 1: Analyze page fonts to get embedded font profiles
    auto pageProfiles = analyzer_->analyzePageFonts(page, document);

    // Build map from font name prefix to profile for quick lookup
    // Font names in spans are like "F1", page profiles have postScriptName
    // We match by iterating page profiles since they're in font dict order
    std::unordered_map<std::string, const FontMetricProfile*> profileByName;
    for (const auto& profile : pageProfiles) {
        profileByName[profile.postScriptName] = &profile;
        profileByName[profile.familyName] = &profile;
    }

    // Track which font files we've already embedded (avoid duplicates)
    std::unordered_map<std::string, std::string> embeddedFontCache; // filePath -> resourceName

    // Step 2: Process each modification
    for (const auto& [spanIndex, newText] : modifications) {
        if (spanIndex < 0 || spanIndex >= static_cast<int>(allSpans.size()))
            continue;

        const auto& span = allSpans[spanIndex];

        // Find the embedded font profile for this span
        // Try matching by font name - spans use resource names like "F1"
        // while profiles have PostScript names. Try substring match.
        auto profileIt = std::find_if(pageProfiles.begin(), pageProfiles.end(),
            [&span](const auto& profile) {
                return profile.postScriptName.find(span.fontName) != std::string::npos ||
                       span.fontName.find(profile.postScriptName) != std::string::npos;
            });
        const FontMetricProfile* embeddedProfile =
            (profileIt != pageProfiles.end()) ? &(*profileIt) : nullptr;

        // If no profile found by name, use first profile as fallback
        // (common for PDFs with a single font)
        if (!embeddedProfile && !pageProfiles.empty()) {
            embeddedProfile = &pageProfiles[0];
        }

        if (!embeddedProfile)
            continue;

        // Step 2a: Check if original font covers all characters
        // For embedded fonts with filePath, use findMissingGlyphs
        // For fonts without filePath (non-embedded), check profile coverage flags
        bool needsSubstitution = false;

        if (!embeddedProfile->filePath.empty()) {
            auto missing = analyzer_->findMissingGlyphs(embeddedProfile->filePath, newText);
            needsSubstitution = !missing.empty();
        } else {
            // Heuristic: check if text has chars outside known coverage
            for (char32_t cp : newText) {
                if (cp <= 0x20) continue;  // Space and control chars
                if (cp <= 0x7E && embeddedProfile->hasLatinBasic) continue;
                if (cp >= 0x0100 && cp <= 0x017F && embeddedProfile->hasLatinExtA) continue;
                if (cp >= 0x0400 && cp <= 0x04FF && embeddedProfile->hasCyrillic) continue;
                if (cp >= 0x4E00 && cp <= 0x9FFF && embeddedProfile->hasCJK) continue;
                // Character outside known coverage ranges
                if (cp > 0x7E && !embeddedProfile->hasLatinExtA) {
                    needsSubstitution = true;
                    break;
                }
            }
        }

        if (!needsSubstitution)
            continue;

        // Step 2b: Find substitute font
        auto substituteResult = selector_->findSubstitute(*embeddedProfile, newText);
        if (!substituteResult.has_value())
            continue;  // No substitute found, will render with .notdef

        // Step 2c: Embed substitute font (or reuse if already embedded)
        std::string resourceName;
        auto cacheIt = embeddedFontCache.find(substituteResult->filePath);
        if (cacheIt != embeddedFontCache.end()) {
            resourceName = cacheIt->second;
        } else {
            auto embeddedInfo = embedder_->embedFont(page, document, substituteResult->filePath);
            if (!embeddedInfo.has_value())
                continue;  // Embedding failed
            resourceName = embeddedInfo->resourceName;
            embeddedFontCache[substituteResult->filePath] = resourceName;
        }

        // Step 2d: Compute scaling (skip if we don't have file paths for both)
        float horizontalScale = 1.0f;
        // Scale adjustment requires FT_Face for both fonts - we'd need to load them.
        // For now, use profile-based estimation from avgCharWidth.
        if (embeddedProfile->avgCharWidth > 0.0f &&
            substituteResult->profile.avgCharWidth > 0.0f) {
            horizontalScale = embeddedProfile->avgCharWidth /
                              substituteResult->profile.avgCharWidth;
        }

        // Step 2e: Build SubstitutionInfo
        SubstitutionInfo info;
        info.spanIndex = spanIndex;
        info.substituteResName = resourceName;
        info.horizontalScale = horizontalScale;
        info.originalFontSize = span.fontSize;
        info.originalFontName = span.fontName;
        results.push_back(std::move(info));
    }

    return results;
}

} // namespace pdfe::fonts
