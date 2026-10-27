#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/FontResolver.h"
#include "core/SystemFontDiscovery.h"

#include <QDebug>
#include <algorithm>
#include <cmath>

namespace pdfe::fonts {

// Exclude decorative, script, handwriting, and symbol fonts from matching.
// These are never appropriate substitutes for document body text.
static bool isDecorativeFont(const std::string& familyName)
{
    std::string lower = familyName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Script/handwriting fonts ("hand " with trailing space catches "Bradley Hand ITC" etc.)
    static const char* const patterns[] = {
        "script", "handwrit", "hand ", "cursive", "calligr", "brush",
        // Decorative/display
        "ornament", "decorat", "display", "poster", "stencil",
        "grunge", "distress", "vintage", "retro",
        // Symbol/dingbat
        "symbol", "dingbat", "wingding", "webding", "emoji",
        "icon", "marlett", "zapf",
        // Casual/informal — not suitable for body text substitution
        "comic", "ink free", "mv boli", "segoe print", "gabriola",
        // Specific well-known decorative fonts on Windows
        "french script", "edwardian", "lucida handwriting",
        "mistral", "palace script", "pristina", "rage",
        "viner hand", "vivaldi", "vladimir",
        "harrington", "jokerman", "juice", "kristen",
        "magneto", "matura", "niagara", "papyrus",
        "parchment", "playbill", "poor richard", "ravie",
        "showcard", "snap", "tempus", "blackadder",
        "curlz", "gigi", "kunstler",
        // Monotype/Microsoft decorative
        "algerian", "bauhaus", "broadway", "chiller",
        "colonna", "cooper black", "footlight", "forte",
        "goudy stout", "haettenschweiler", "imprint",
        "informal roman", "wide latin",
        // Math/technical/specialized — not body text fonts
        "math", "tex ", "gyre", "stix", "xits", "asana math",
        "cambria math", "fira math",
    };

    return std::any_of(std::begin(patterns), std::end(patterns),
        [&lower](const char* pat) {
            return lower.find(pat) != std::string::npos;
        });
}

// Catch known multi-script / non-Latin-primary system font families by name.
// These pass `isDecorativeFont` because they're legitimate body-text fonts —
// they just have the wrong script primary intent for Latin source text. The
// per-profile primaryScript flag should catch them via `distanceTo`'s
// 8.0 script-mismatch penalty, but the FontAnalysisCache may hold older
// profile records that pre-date the script detection in
// EmbeddedFontAnalyzer (lines 715-734). Filtering by family name at
// scoreSystemFonts time is a name-based safety net that works regardless
// of cache state.
//
// Also includes Microsoft variable fonts (Bahnschrift, etc.) that
// MuPDF's pdf_add_cid_font does not embed correctly — the resulting
// PDF renders glyphs at ~2x intended size at the same Tfs. Until that
// embedding issue is fixed, exclude the affected families from
// substitution candidacy. See memory/textedit-tm-sign-bug.md.
//
// When the query text is Latin (the common case for editing English-
// language PDFs), these families should never win against Arial / Times /
// Calibri / Cambria etc. on a 0.06-point margin like the Lean Startup
// repro produced (Nirmala UI 3.555 vs Arial 3.615).
static bool isNonLatinPrimaryFamily(const std::string& familyName)
{
    std::string lower = familyName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    static const char* const patterns[] = {
        // Indic / South Asian (also covers Tamil, Bengali, Gujarati, etc.
        // via the Nirmala family which bundles all Indic scripts)
        "nirmala", "mangal", "kokila", "aparajita", "utsaah",
        "kartika", "vrinda", "shruti", "raavi", "tunga",
        "iskoola", "vijaya", "shonar", "gautami", "latha",
        // Southeast Asian
        "leelawadee", "myanmar", "padauk", "lao", "khmer", "javanese",
        // Central Asian / other Asian
        "mongolian", "tibetan", "himalaya", "baiti", "phagspa",
        "tai lue", "yi baiti",
        // African / Cherokee
        "ebrima", "gadugi", "plantagenet",
        // CJK
        "simsun", "simhei", "simfang", "simli", "simyou",
        "msgothic", "msmincho", "yu gothic", "yu mincho",
        "meiryo", "ms ui gothic",
        "malgun", "gulim", "batang", "dotum", "gungsuh",
        "dfkai", "fangsong",
        // Arabic / Hebrew
        "arabic typesetting", "scheherazade", "amiri", "naskh",
        "sakkal", "aldhabi", "sakkal majalla",
        "david", "frank ruehl", "miriam", "narkisim", "rod",
        // Microsoft variable fonts that MuPDF's pdf_add_cid_font does
        // not embed at correct scale (renders ~2x oversized in saved PDF)
        "bahnschrift", "cascadia code", "cascadia mono",
    };
    return std::any_of(std::begin(patterns), std::end(patterns),
        [&lower](const char* pat) {
            return lower.find(pat) != std::string::npos;
        });
}

// Source text is "Latin-only" if every codepoint is in Latin/punctuation/
// space ranges. This is the common case for English/European-language
// editing and the case where multi-script substitutes are clearly wrong.
static bool textIsLatinOnly(const std::u32string& text)
{
    for (char32_t cp : text) {
        if (cp <= 0x007F) continue;                  // ASCII
        if (cp >= 0x00A0 && cp <= 0x00FF) continue;  // Latin-1 Supplement
        if (cp >= 0x0100 && cp <= 0x017F) continue;  // Latin Extended-A
        if (cp >= 0x0180 && cp <= 0x024F) continue;  // Latin Extended-B
        if (cp >= 0x2000 && cp <= 0x206F) continue;  // General Punctuation
                                                       // (smart quotes etc.)
        if (cp >= 0x2070 && cp <= 0x209F) continue;  // Super/subscripts
        if (cp >= 0x20A0 && cp <= 0x20CF) continue;  // Currency Symbols
        if (cp >= 0x2100 && cp <= 0x214F) continue;  // Letterlike Symbols
        return false;
    }
    return true;
}

SubstituteFontSelector::SubstituteFontSelector(
    EmbeddedFontAnalyzer* analyzer,
    FontAnalysisCache* cache)
    : analyzer_(analyzer)
    , cache_(cache)
    , systemFonts_(std::make_unique<pdfe::core::SystemFontDiscovery>())
{
}

SubstituteFontSelector::~SubstituteFontSelector() = default;

// Build the known-pair table by querying FontResolver for each
// PDF font name. FontResolver checks bundled fonts first (security),
// then queries the OS via fontconfig/DirectWrite (no hardcoded
// paths), then walks the metric-compatible substitution chain.
//
// The table is built lazily on first access and cached for the
// process lifetime. If a particular family isn't installed AND
// no metric-compatible substitute is available, that entry will
// have an empty path — the scorer treats those as "no known pair"
// and falls back to system-font scoring.
const std::vector<SubstituteFontSelector::KnownPair>& SubstituteFontSelector::knownPairs()
{
    static const std::vector<KnownPair> pairs = []() {
        // List of (PDF family name lookup key, canonical name to
        // resolve). The canonical name is what FontResolver expects
        // (a "real" family with weight/style info, e.g. "Helvetica");
        // the lookup key is what the scorer matches against.
        struct Entry { const char* lookupKey; const char* canonical; };
        static const std::array<Entry, 25> kEntries = {{
            // Standard 14 PDF fonts
            {"helvetica",     "Helvetica"},
            {"arialmt",       "Arial"},
            {"arial",         "Arial"},
            {"times",         "Times New Roman"},
            {"timesnewroman", "Times New Roman"},
            {"courier",       "Courier New"},
            {"couriernew",    "Courier New"},
            // Common modern document fonts
            {"georgia",       "Georgia"},
            {"verdana",       "Verdana"},
            {"tahoma",        "Tahoma"},
            {"trebuchet",     "Trebuchet MS"},
            {"calibri",       "Calibri"},
            {"cambria",       "Cambria"},
            {"consolas",      "Consolas"},
            {"palatino",      "Palatino Linotype"},
            {"garamond",      "Garamond"},
            // OCR invisible / placeholder fonts → sans-serif fallback
            {"glyphlessfont", "Helvetica"},
            {"invisiblefont", "Helvetica"},
            {"hiddenocrfont", "Helvetica"},
            {"ocrfont",       "Helvetica"},
            // OCR-A / OCR-B (ISO 1073) → monospace fallback
            {"ocra",          "Consolas"},
            {"ocrb",          "Consolas"},
            // Other OCR engines
            {"finereaderfont","Helvetica"},
            {"adobeblank",    "Helvetica"},
            // Symbol fonts
            {"symbol",        "Symbol"},
        }};

        std::vector<KnownPair> out;
        out.reserve(kEntries.size());
        for (const auto& e : kEntries) {
            std::string path = FontResolver::resolveFontPath(e.canonical);
            if (!path.empty()) {
                out.push_back({e.lookupKey, std::move(path)});
            }
            // If the OS can't supply the font, omit the entry — the
            // SubstituteFontSelector scorer will fall through to its
            // system-font enumeration path.
        }
        return out;
    }();
    return pairs;
}

std::optional<SubstituteResult> SubstituteFontSelector::findSubstitute(
    const FontMetricProfile& embeddedProfile,
    const std::u32string& textNeeded)
{
    if (!analyzer_ || textNeeded.empty())
        return std::nullopt;

    // Step 1: Try known pair lookup
    auto knownResult = tryKnownPair(embeddedProfile, textNeeded);
    if (knownResult.has_value()) {
        qDebug() << "[SubstituteFontSelector::findSubstitute] Known pair found:"
                 << "family=" << QString::fromStdString(knownResult->profile.familyName)
                 << "filePath=" << QString::fromStdString(knownResult->filePath);
        return knownResult;
    }

    qDebug() << "[SubstituteFontSelector::findSubstitute] No known pair, scoring system fonts"
             << "query.isSerif=" << embeddedProfile.isSerif
             << "query.weightClass=" << embeddedProfile.weightClass;
    // Step 2: Score system fonts
    auto systemResult = scoreSystemFonts(embeddedProfile, textNeeded);
    if (systemResult.has_value()) {
        qDebug() << "[SubstituteFontSelector::findSubstitute] System font winner:"
                 << "family=" << QString::fromStdString(systemResult->profile.familyName)
                 << "score=" << systemResult->matchScore
                 << "isSerif=" << systemResult->profile.isSerif;
    } else {
        qDebug() << "[SubstituteFontSelector::findSubstitute] No system font found!";
    }
    return systemResult;
}

std::optional<SubstituteResult> SubstituteFontSelector::tryKnownPair(
    const FontMetricProfile& embeddedProfile,
    const std::u32string& textNeeded)
{
    // Build lowercase search key from postScriptName and familyName
    std::string lowerPS = embeddedProfile.postScriptName;
    std::transform(lowerPS.begin(), lowerPS.end(), lowerPS.begin(), ::tolower);
    // Remove hyphens and spaces for fuzzy matching
    lowerPS.erase(std::remove_if(lowerPS.begin(), lowerPS.end(),
        [](char c) { return c == '-' || c == ' '; }), lowerPS.end());

    std::string lowerFamily = embeddedProfile.familyName;
    std::transform(lowerFamily.begin(), lowerFamily.end(), lowerFamily.begin(), ::tolower);
    lowerFamily.erase(std::remove_if(lowerFamily.begin(), lowerFamily.end(),
        [](char c) { return c == '-' || c == ' '; }), lowerFamily.end());

    qDebug() << "[SubstituteFontSelector::tryKnownPair]"
             << "lowerPS=" << QString::fromStdString(lowerPS)
             << "lowerFamily=" << QString::fromStdString(lowerFamily)
             << "isSerif=" << embeddedProfile.isSerif;

    for (const auto& pair : knownPairs()) {
        bool matches = false;
        if (!lowerPS.empty() && lowerPS.find(pair.embeddedPattern) != std::string::npos) {
            matches = true;
        } else if (!lowerFamily.empty() && lowerFamily.find(pair.embeddedPattern) != std::string::npos) {
            matches = true;
        }

        if (!matches)
            continue;

        // Check coverage
        auto missing = analyzer_->findMissingGlyphs(pair.systemFilePath, textNeeded);
        if (!missing.empty())
            continue;  // Known pair doesn't cover the needed text

        // Analyze the known pair for profile
        SubstituteResult result;
        result.filePath = pair.systemFilePath;
        result.isKnownPair = true;
        result.matchScore = 0.0f;  // Perfect match for known pairs

        // Try cache first, then analyze
        if (cache_) {
            auto cached = cache_->get(pair.embeddedPattern);
            if (cached.has_value()) {
                result.profile = std::move(*cached);
                return result;
            }
        }

        auto profile = analyzer_->analyzeFromFile(pair.systemFilePath);
        if (profile.has_value()) {
            result.profile = *profile;
            result.matchScore = embeddedProfile.distanceTo(*profile);
            if (cache_) {
                cache_->put(*profile);
            }
            return result;
        }
    }

    return std::nullopt;
}

std::string SubstituteFontSelector::lookupKnownPairPath(const std::string& fontName)
{
    std::string lower = fontName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    lower.erase(std::remove_if(lower.begin(), lower.end(),
        [](char c) { return c == '-' || c == ' '; }), lower.end());

    const auto& pairs = knownPairs();
    auto it = std::find_if(pairs.begin(), pairs.end(),
        [&lower](const auto& pair) {
            return lower.find(pair.embeddedPattern) != std::string::npos ||
                   std::string(pair.embeddedPattern).find(lower) != std::string::npos;
        });
    return (it != pairs.end()) ? it->systemFilePath : std::string{};
}

std::vector<SubstituteResult> SubstituteFontSelector::findTopCandidates(
    const FontMetricProfile& embeddedProfile,
    const std::u32string& textNeeded,
    int maxCandidates)
{
    std::vector<SubstituteResult> results;

    if (!analyzer_ || textNeeded.empty() || maxCandidates <= 0)
        return results;

    // Try known pair first -- if found, insert at position 0
    auto knownResult = tryKnownPair(embeddedProfile, textNeeded);

    // Score system fonts -- reuse the same quick filter + deep scoring logic
    const auto& allFonts = systemFonts_->enumerateFonts();
    if (allFonts.empty()) {
        if (knownResult.has_value())
            results.push_back(std::move(*knownResult));
        return results;
    }

    // Quick filter: match structural properties
    struct Candidate {
        const pdfe::core::FontMetrics* metrics;
        int quickScore;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(allFonts.size());

    const bool latinSource = textIsLatinOnly(textNeeded);

    for (const auto& fm : allFonts) {
        if (fm.filePath.empty())
            continue;
        if (isDecorativeFont(fm.familyName))
            continue;  // Hard filter: exclude decorative/script/symbol fonts
        if (latinSource && isNonLatinPrimaryFamily(fm.familyName))
            continue;  // Hard filter: keep multi-script families away from
                       // Latin-only source text. Cached profiles may have
                       // stale primaryScript=Unknown.

        int score = 0;
        if (fm.isSerif == embeddedProfile.isSerif)
            score += 3;
        if (fm.isMonospace == embeddedProfile.isMonospace)
            score += 1;
        int weightDiff = std::abs(fm.weightValue() - embeddedProfile.weightClass);
        if (weightDiff <= 100)
            score += 2;
        else if (weightDiff <= 300)
            score += 1;
        // Italic: soft scoring (CNN handles visual italic detection)
        bool fmItalic = (fm.style == pdfe::core::FontStyle::Italic ||
                         fm.style == pdfe::core::FontStyle::Oblique);
        if (fmItalic == embeddedProfile.isItalic)
            score += 2;

        // Low threshold: CNN visual ranking is the real discriminator,
        // so include a broad mix (serif+sans, various weights).
        if (score >= 1) {
            Candidate c;
            c.metrics = &fm;
            c.quickScore = score;
            candidates.push_back(c);
        }
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            return a.quickScore > b.quickScore;
        });

    size_t limit = std::min(candidates.size(), static_cast<size_t>(maxCandidates));

    // Deep scoring -- keep all scored results
    std::vector<SubstituteResult> scored;
    for (size_t i = 0; i < limit; ++i) {
        const auto& candidate = candidates[i];
        const std::string& filePath = candidate.metrics->filePath;

        auto missing = analyzer_->findMissingGlyphs(filePath, textNeeded);
        if (!missing.empty())
            continue;

        FontMetricProfile candidateProfile;
        bool haveProfile = false;

        if (cache_) {
            auto cached = cache_->get(candidate.metrics->postScriptName);
            if (cached.has_value()) {
                candidateProfile = std::move(*cached);
                haveProfile = true;
            }
        }

        if (!haveProfile) {
            auto profile = analyzer_->analyzeFromFile(filePath);
            if (!profile.has_value())
                continue;
            candidateProfile = std::move(*profile);
            if (cache_) {
                cache_->put(candidateProfile);
            }
        }

        float score = embeddedProfile.distanceTo(candidateProfile);

        SubstituteResult sr;
        sr.profile = std::move(candidateProfile);
        sr.filePath = filePath;
        sr.matchScore = score;
        sr.isKnownPair = false;
        scored.push_back(std::move(sr));
    }

    // Include known-pair in scored list so sort is consistent.
    // Replace any duplicate (same filePath, case-insensitive on Windows) with the known-pair flag.
    if (knownResult.has_value()) {
        std::string knownLower = knownResult->filePath;
        std::transform(knownLower.begin(), knownLower.end(), knownLower.begin(), ::tolower);

        bool replaced = false;
        for (auto& sr : scored) {
            std::string srLower = sr.filePath;
            std::transform(srLower.begin(), srLower.end(), srLower.begin(), ::tolower);
            if (srLower == knownLower) {
                sr.isKnownPair = true;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            scored.push_back(std::move(*knownResult));
        }
    }

    // Sort by match score ascending (best first)
    std::sort(scored.begin(), scored.end(),
        [](const SubstituteResult& a, const SubstituteResult& b) {
            return a.matchScore < b.matchScore;
        });

    // Take top N
    for (auto& sr : scored) {
        if (static_cast<int>(results.size()) >= maxCandidates)
            break;
        results.push_back(std::move(sr));
    }

    return results;
}

std::vector<SubstituteResult> SubstituteFontSelector::findAllNonDecorativeCandidates(
    const std::u32string& textNeeded)
{
    std::vector<SubstituteResult> results;
    const auto& allFonts = systemFonts_->enumerateFonts();
    if (allFonts.empty())
        return results;

    // Deduplicate by file path (TTC collections enumerate same file multiple times)
    std::set<std::string> seenPaths;

    const bool latinSource = textIsLatinOnly(textNeeded);

    for (const auto& fm : allFonts) {
        if (fm.filePath.empty())
            continue;
        if (isDecorativeFont(fm.familyName))
            continue;
        if (latinSource && isNonLatinPrimaryFamily(fm.familyName))
            continue;

        // Deduplicate by file path (TTC collections enumerate same file many times)
        if (seenPaths.count(fm.filePath))
            continue;
        seenPaths.insert(fm.filePath);

        // Check glyph coverage
        auto missing = analyzer_->findMissingGlyphs(fm.filePath, textNeeded);
        if (!missing.empty())
            continue;

        // Build a minimal profile for the result
        FontMetricProfile candidateProfile;
        bool haveProfile = false;
        if (cache_) {
            auto cached = cache_->get(fm.postScriptName);
            if (cached.has_value()) {
                candidateProfile = std::move(*cached);
                haveProfile = true;
            }
        }
        if (!haveProfile) {
            auto profile = analyzer_->analyzeFromFile(fm.filePath);
            if (!profile.has_value())
                continue;
            candidateProfile = std::move(*profile);
            if (cache_)
                cache_->put(candidateProfile);
        }

        SubstituteResult sr;
        sr.profile = std::move(candidateProfile);
        sr.filePath = fm.filePath;
        sr.matchScore = 0.0f;  // No metric ranking — CNN will score
        sr.isKnownPair = false;
        results.push_back(std::move(sr));
    }

    return results;
}

std::optional<SubstituteResult> SubstituteFontSelector::scoreSystemFonts(
    const FontMetricProfile& embeddedProfile,
    const std::u32string& textNeeded)
{
    const auto& allFonts = systemFonts_->enumerateFonts();
    if (allFonts.empty())
        return std::nullopt;

    // Quick filter: match structural properties
    struct Candidate {
        const pdfe::core::FontMetrics* metrics;
        int quickScore;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(allFonts.size());

    const bool latinSource = textIsLatinOnly(textNeeded);

    for (const auto& fm : allFonts) {
        if (fm.filePath.empty())
            continue;
        if (isDecorativeFont(fm.familyName))
            continue;  // Hard filter: exclude decorative/script/symbol fonts
        if (latinSource && isNonLatinPrimaryFamily(fm.familyName))
            continue;  // Hard filter: keep multi-script families away from
                       // Latin-only source text (Lean Startup p2 repro:
                       // Nirmala UI Ascent=1079 vs CharisSIL Ascent=731
                       // produces visibly oversized text at the same Tfs).

        int score = 0;

        // Structural class match
        if (fm.isSerif == embeddedProfile.isSerif)
            score += 3;
        if (fm.isMonospace == embeddedProfile.isMonospace)
            score += 2;

        // Weight within ±200
        int weightDiff = std::abs(fm.weightValue() - embeddedProfile.weightClass);
        if (weightDiff <= 200)
            score += 2;

        // Italic: soft scoring (CNN handles visual italic detection)
        bool fmItalic = (fm.style == pdfe::core::FontStyle::Italic ||
                         fm.style == pdfe::core::FontStyle::Oblique);
        if (fmItalic == embeddedProfile.isItalic)
            score += 2;

        // Only keep candidates with minimum structural match
        if (score >= 3) {
            Candidate c;
            c.metrics = &fm;
            c.quickScore = score;
            candidates.push_back(c);
        }
    }

    // Sort by quick score, take top 20
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            return a.quickScore > b.quickScore;
        });

    size_t limit = std::min(candidates.size(), static_cast<size_t>(20));

    // Deep scoring
    SubstituteResult bestResult;
    bool foundAny = false;

    for (size_t i = 0; i < limit; ++i) {
        const auto& candidate = candidates[i];
        const std::string& filePath = candidate.metrics->filePath;

        // Check coverage first (cheaper than full analysis)
        auto missing = analyzer_->findMissingGlyphs(filePath, textNeeded);
        if (!missing.empty())
            continue;  // Doesn't cover needed text

        // Get or compute full profile
        FontMetricProfile candidateProfile;
        bool haveProfile = false;

        if (cache_) {
            auto cached = cache_->get(candidate.metrics->postScriptName);
            if (cached.has_value()) {
                candidateProfile = std::move(*cached);
                haveProfile = true;
            }
        }

        if (!haveProfile) {
            auto profile = analyzer_->analyzeFromFile(filePath);
            if (!profile.has_value())
                continue;
            candidateProfile = std::move(*profile);
            if (cache_) {
                cache_->put(candidateProfile);
            }
        }

        float score = embeddedProfile.distanceTo(candidateProfile);

        qDebug() << "[SubstituteFontSelector::scoreSystemFonts] candidate:"
                 << "family=" << QString::fromStdString(candidateProfile.familyName)
                 << "score=" << score
                 << "isSerif=" << candidateProfile.isSerif
                 << "quickScore=" << candidate.quickScore
                 << "filePath=" << QString::fromStdString(filePath);

        if (!foundAny || score < bestResult.matchScore) {
            bestResult.profile = std::move(candidateProfile);
            bestResult.filePath = filePath;
            bestResult.matchScore = score;
            bestResult.isKnownPair = false;
            foundAny = true;
        }
    }

    if (foundAny)
        return bestResult;

    return std::nullopt;
}

} // namespace pdfe::fonts
