#include "core/TempFileSafety.h"
#include "tre/FontMatcher.h"
#include "tre/CoordMapper.h"
#include "tre/BitmapCompositor.h"
#include "tre/ImageSimilarity.h"
#include "tre/VisualFontRanker.h"
#include "tre/CNNFontClassifier.h"
#include "tre/CNNFontRanker.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontMetricProfile.h"
#include "fonts/FontResolver.h"

#include <QColor>
#include <QDebug>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <numeric>
#include <vector>

namespace pdfe::tre {

FontMatcher::FontMatcher(pdfe::fonts::SubstituteFontSelector* selector,
                         pdfe::fonts::EmbeddedFontAnalyzer* analyzer)
    : selector_(selector), analyzer_(analyzer) {
}

void FontMatcher::setBitmapCompositor(BitmapCompositor* compositor) {
    compositor_ = compositor;
}

void FontMatcher::setCNNFontClassifier(CNNFontClassifier* classifier) {
    cnnClassifier_ = classifier;
}

FontMatcher::~FontMatcher() = default;

// ---------------------------------------------------------------------------
// detectProperties
// ---------------------------------------------------------------------------

ScannedTextProperties FontMatcher::detectProperties(
    const RegionTextData& regionData,
    const QImage& pageImage,
    int scanDpi) const {

    ScannedTextProperties props;

    // Collect metrics from all lines
    std::vector<float> xSizes;
    std::vector<float> descenders;
    std::vector<float> ascenders;
    std::vector<float> baselineSlopes;
    std::vector<HocrWord> allWords;

    for (const auto& line : regionData.lines) {
        if (line.xSizePx > 0.0f) {
            xSizes.push_back(line.xSizePx);
        }
        if (line.descendersPx > 0.0f) {
            descenders.push_back(line.descendersPx);
        }
        if (line.ascendersPx > 0.0f) {
            ascenders.push_back(line.ascendersPx);
        }
        baselineSlopes.push_back(line.baselineSlope);

        std::copy(line.words.begin(), line.words.end(),
            std::back_inserter(allWords));
    }

    // Compute averages
    const float avgXSizePx = xSizes.empty() ? 0.0f :
        std::accumulate(xSizes.begin(), xSizes.end(), 0.0f) / xSizes.size();

    const float avgDescendersPx = descenders.empty() ? 0.0f :
        std::accumulate(descenders.begin(), descenders.end(), 0.0f) / descenders.size();

    const float avgAscendersPx = ascenders.empty() ? 0.0f :
        std::accumulate(ascenders.begin(), ascenders.end(), 0.0f) / ascenders.size();

    const float avgSlope = baselineSlopes.empty() ? 0.0f :
        std::accumulate(baselineSlopes.begin(), baselineSlopes.end(), 0.0f) / baselineSlopes.size();

    // 1. Font size estimation — combine two independent measurements:
    //
    //  A) x_size estimate: Tesseract's x_size is (x_height + ascenders) in
    //     pixels — the distance from baseline to ascender top (see Tesseract
    //     hocrrenderer.cpp: x_size = row->x_height() + row->ascenders()).
    //     Convert to PDF points: fontSizePt = x_size * 72 / dpi.
    //     This underestimates the em-square by the descender amount (~10-15%).
    //
    //  B) Baseline-to-ascender estimate (Option C): The HOCR baseline equation
    //     gives the baseline y-position. The distance from line bbox top to
    //     baseline is the ascender height. Dividing by the typical ascender-
    //     to-em ratio (~0.78) gives the em-square estimate.
    //
    //  When both estimates are available, averaging reduces variance.
    //  For CJK text (no meaningful baseline), only the x_size estimate is used.
    //
    //  When x_size is unavailable, fall back to average line bbox height
    //  divided by ~1.2 (line bbox ≈ em + inter-line padding).

    float avgLineBboxHeight = 0.0f;
    if (!regionData.lines.empty()) {
        const float sumH = std::accumulate(
            regionData.lines.begin(), regionData.lines.end(), 0.0f,
            [](float acc, const auto& line) {
                return acc + static_cast<float>(line.bbox.height());
            });
        avgLineBboxHeight = sumH / static_cast<float>(regionData.lines.size());
    }

    // Estimate A: from x_size
    float xSizeEstimatePt = 0.0f;
    if (avgXSizePx > 0.0f) {
        xSizeEstimatePt = static_cast<float>(
            CoordMapper::pixelSizeToPdfPt(avgXSizePx, scanDpi));
    } else if (avgLineBboxHeight > 0.0f) {
        float emPx = avgLineBboxHeight / 1.2f;
        xSizeEstimatePt = static_cast<float>(
            CoordMapper::pixelSizeToPdfPt(emPx, scanDpi));
    } else {
        xSizeEstimatePt = 12.0f; // Default fallback
    }

    // Estimate B: baseline-to-ascender (Option C).
    //
    // A2: per-script ascender ratio. In typical Latin fonts the ascender
    // height (baseline to top of 'b'/'d'/'h') is ~78% of the em-square.
    // Other scripts with meaningful baselines have different ratios:
    //
    //   - Latin:      0.78  (default)
    //   - Cyrillic:   0.78  (similar letterform envelope)
    //   - Arabic:     0.72  (tall ascenders / short descenders pull
    //                        the em-square farther beyond the ascender)
    //   - Hebrew:     0.75  (between Latin and Arabic)
    //   - Thai:       0.80  (above/below marks extend the em-square
    //                        beyond the ascender cluster)
    //   - Devanagari: 0.80  (shirorekha + combining marks above and
    //                        below make the nominal ascender smaller
    //                        relative to the em-square)
    //   - CJK:        skip  (no meaningful baseline — return 0 and
    //                        fall through to x_size only)
    //   - Unknown:    skip  (no confident ratio to apply)
    //
    // See docs/image-pdf-risks.md (A2).
    float baselineEstimatePt = 0.0f;
    bool hasBaselineEstimate = false;

    pdfe::fonts::FontScript textScript = pdfe::fonts::FontScript::Unknown;
    if (!regionData.plainText.empty()) {
        const std::u32string u32 = utf8ToU32(regionData.plainText);
        textScript = detectScript(u32);
    }

    const float kAscenderRatio = ascenderRatioForScript(textScript);
    if (kAscenderRatio > 0.0f) {

        std::vector<float> baselineEstimates;
        for (const auto& line : regionData.lines) {
            const float lineHeight = static_cast<float>(line.bbox.height());
            if (lineHeight < 5.0f)
                continue;

            // Correct baseline formula: the HOCR baseline offset is relative
            // to the BOTTOM of the line bbox (y1), not the top (y0).
            const float lineCenterX =
                static_cast<float>((line.bbox.x0 + line.bbox.x1) / 2.0);
            const float baselineY =
                static_cast<float>(line.bbox.y1)
                + line.baselineSlope * (lineCenterX - static_cast<float>(line.bbox.x0))
                + line.baselineOffset;

            // Ascender height = distance from top of line bbox to baseline
            const float ascenderPx =
                baselineY - static_cast<float>(line.bbox.y0);

            // Validity: ascender should be 30-95% of line height.
            // Outside this range the baseline data is degenerate (e.g. offset=0).
            if (ascenderPx > lineHeight * 0.30f &&
                ascenderPx < lineHeight * 0.95f) {
                const float emPx = ascenderPx / kAscenderRatio;
                baselineEstimates.push_back(static_cast<float>(
                    CoordMapper::pixelSizeToPdfPt(emPx, scanDpi)));
            }
        }

        if (!baselineEstimates.empty()) {
            baselineEstimatePt =
                std::accumulate(baselineEstimates.begin(),
                                baselineEstimates.end(), 0.0f)
                / static_cast<float>(baselineEstimates.size());
            hasBaselineEstimate = true;
        }
    }

    // Combine estimates
    if (hasBaselineEstimate) {
        props.fontSizePt = (xSizeEstimatePt + baselineEstimatePt) / 2.0f;
    } else {
        props.fontSizePt = xSizeEstimatePt;
    }

    // 1b. Per-line font size (pipeline 3f8a1c72 Phase 3).
    //
    //  The region-average fontSizePt above is wrong for any region that mixes
    //  sizes (a heading line over body text — measured per-line CV up to 0.24).
    //  Compute each line's OWN size from the SAME two estimates used for the
    //  region (A: x_size; B: baseline-to-ascender), so a caller that knows which
    //  line it is rendering (e.g. the clicked-line overlay) can use the line's
    //  true size instead of the region average. The region value is reused only
    //  as a fallback for a line that yields no valid measurement — it is never
    //  broadcast over a line that has its own. The per-line formula lives in the
    //  shared static lineFontSizePt() so the text-edit overlay can apply the exact
    //  same computation to the (separately-OCR'd) atlas lines a click resolves
    //  against — see MainWindow's atlas-build path.
    props.perLineFontSizePt.clear();
    props.perLineFontSizePt.reserve(regionData.lines.size());
    for (const auto& line : regionData.lines) {
        const auto [linePt, valid] = lineFontSizePt(line, textScript, scanDpi);
        props.perLineFontSizePt.push_back(valid ? linePt : props.fontSizePt);
    }

    // 2. Metric ratios
    const float totalHeight = avgXSizePx + avgDescendersPx + avgAscendersPx;
    if (totalHeight > 0.0f) {
        props.xHeightRatio = avgXSizePx / totalHeight;
        props.ascenderRatio = avgAscendersPx / totalHeight;
        props.descenderRatio = avgDescendersPx / totalHeight;
    } else {
        props.xHeightRatio = 0.48f;
        props.ascenderRatio = 0.35f;
        props.descenderRatio = 0.17f;
    }

    // 3. Weight: Start with x-height heuristic (may be upgraded by per-word bold detection)
    props.weight = estimateWeight(props.xHeightRatio);

    // 4. Italic: Check baseline slope. Scanned pages often have slight rotations
    // (slope 0.01-0.04) that are NOT italic — just imperfect scanning. True italic
    // text has a visible slant of 10-15 degrees (slope ~0.10-0.15). Use a higher
    // threshold to avoid false positives on scanned PDFs.
    props.isItalic = std::abs(avgSlope) > 0.08f;

    // 5. Serif: Detect from image pixel analysis at baseline vs mid-stem.
    // Kept REGION-WIDE deliberately (pipeline 3f8a1c72 Phase 3): on real fixtures
    // serif/sans is uniform within a region (a selection is normally one text
    // block of one family), and detectSerif() already does per-character median
    // analysis across all lines — making it MORE robust than a per-line call,
    // which would have fewer diagnostic glyphs per measurement. Unlike font size
    // (which genuinely varies line-to-line), per-line serif is not warranted.
    bool serifDetermined = false;
    props.isSerif = detectSerif(pageImage, regionData.lines, &serifDetermined);
    props.serifDetermined = serifDetermined;

    // 8. Per-word style detection (bold, italic, underline, strikethrough)
    auto mutableLines = regionData.lines;
    detectWordStyles(pageImage, mutableLines);
    int boldWordCount = 0;
    int totalWordCount = 0;
    for (const auto& line : mutableLines) {
        for (const auto& word : line.words) {
            ++totalWordCount;
            if (word.style.isBold) {
                props.hasAnyBold = true;
                ++boldWordCount;
            }
            if (word.style.isItalic) props.hasAnyItalic = true;
            if (word.style.isUnderlined) props.hasAnyUnderline = true;
            if (word.style.isStrikethrough) props.hasAnyStrikethrough = true;
        }
    }

    // Override xHeightRatio-based weight when pixel analysis disagrees.
    // The xHeightRatio heuristic is unreliable because Tesseract's height
    // measurements can inflate the ratio (e.g., short descenders → high
    // x-height ratio → false Bold). Per-word stem-width analysis is more
    // direct: it measures actual stroke thickness vs word height.
    // Only downgrade (never upgrade): the stem-width approach has false
    // positives on synthetic/compressed images.
    if (totalWordCount > 0 && props.weight >= FontWeight::SemiBold) {
        const float boldFraction = static_cast<float>(boldWordCount) / totalWordCount;
        if (boldFraction < 0.2f) {
            // xHeightRatio says SemiBold/Bold but pixel analysis disagrees —
            // trust the pixel analysis and downgrade to Regular
            props.weight = FontWeight::Regular;
            qDebug() << "[FontMatcher] Weight downgraded to Regular:"
                     << "xHeightRatio=" << props.xHeightRatio
                     << "boldWords=" << boldWordCount << "/" << totalWordCount;
        }
    }

    // Note: per-word italic detection (centroid-based slant) is NOT used to
    // override the region-level italic flag. The centroid approach produces
    // false positives on serif fonts because asymmetric serif shapes shift
    // the horizontal centroid, mimicking italic slant. Baseline slope (above)
    // is a much more reliable indicator of true italic text.
    // The per-word hasAnyItalic flag is still populated for per-word style
    // rendering in the overlay/save paths.

    // 6. Text color: Sample foreground pixels from word bounding boxes.
    // Kept as the REGION-WIDE median deliberately (pipeline 3f8a1c72 Phase 3):
    // text within a region is a single colour in real documents, and where a
    // genuine per-word colour exists it already flows through the per-word
    // word.style path used by the overlay/save code. The region median is the
    // right representative value here; per-word colour is not re-derived.
    props.textColor = detectTextColor(pageImage, allWords);

    // 7. Line spacing: Compute from line positions
    if (regionData.lines.size() >= 2) {
        float totalSpacing = 0.0f;
        int spacingCount = 0;

        for (size_t i = 1; i < regionData.lines.size(); ++i) {
            const float gap = regionData.lines[i].bbox.y0 - regionData.lines[i - 1].bbox.y0;
            if (gap > 0.0f && avgXSizePx > 0.0f) {
                totalSpacing += gap / avgXSizePx;
                ++spacingCount;
            }
        }

        if (spacingCount > 0) {
            props.lineSpacingRatio = totalSpacing / spacingCount;
        } else {
            props.lineSpacingRatio = 1.2f;
        }
    } else {
        props.lineSpacingRatio = 1.2f; // Default for single line
    }

    qDebug() << "[FontMatcher::detectProperties] RESULT:"
             << "fontSizePt=" << props.fontSizePt
             << "isSerif=" << props.isSerif
             << "isItalic=" << props.isItalic
             << "weight=" << static_cast<int>(props.weight)
             << "xHeightRatio=" << props.xHeightRatio
             << "textColor=(" << props.textColor.r << "," << props.textColor.g << "," << props.textColor.b << ")"
             << "lineCount=" << regionData.lines.size()
             << "plainText=" << QString::fromStdString(regionData.plainText).left(50);

    return props;
}

// ---------------------------------------------------------------------------
// detectTextColor (static)
// ---------------------------------------------------------------------------

RgbColor FontMatcher::detectTextColor(
    const QImage& pageImage,
    const std::vector<HocrWord>& words) {

    if (pageImage.isNull() || words.empty()) {
        return RgbColor{0, 0, 0}; // Default to black
    }

    std::vector<uint8_t> rValues;
    std::vector<uint8_t> gValues;
    std::vector<uint8_t> bValues;

    // Sample foreground pixels from each word bounding box
    for (const auto& word : words) {
        // Clamp bbox to image dimensions
        const int x0 = std::max(0, static_cast<int>(word.bbox.x0));
        const int y0 = std::max(0, static_cast<int>(word.bbox.y0));
        const int x1 = std::min(pageImage.width(), static_cast<int>(word.bbox.x1));
        const int y1 = std::min(pageImage.height(), static_cast<int>(word.bbox.y1));

        if (x1 <= x0 || y1 <= y0) {
            continue; // Invalid bbox
        }

        // Sample pixels in word bbox
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const QColor pixel = pageImage.pixelColor(x, y);
                const int r = pixel.red();
                const int g = pixel.green();
                const int b = pixel.blue();

                // Compute grayscale value: 0.299*R + 0.587*G + 0.114*B
                const float gray = 0.299f * r + 0.587f * g + 0.114f * b;

                // Classify as foreground if dark (grayscale < 128)
                if (gray < 128.0f) {
                    rValues.push_back(static_cast<uint8_t>(r));
                    gValues.push_back(static_cast<uint8_t>(g));
                    bValues.push_back(static_cast<uint8_t>(b));
                }
            }
        }
    }

    // If no foreground pixels found, return black
    if (rValues.empty()) {
        return RgbColor{0, 0, 0};
    }

    // Compute median for each channel
    auto computeMedian = [](std::vector<uint8_t>& values) -> uint8_t {
        if (values.empty()) return 0;
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };

    return RgbColor{
        computeMedian(rValues),
        computeMedian(gValues),
        computeMedian(bValues)
    };
}

// ---------------------------------------------------------------------------
// estimateWeight (static)
// ---------------------------------------------------------------------------

FontWeight FontMatcher::estimateWeight(float xHeightRatio) {
    if (xHeightRatio >= 0.55f) {
        return FontWeight::Bold;
    } else if (xHeightRatio >= 0.50f) {
        return FontWeight::SemiBold;
    } else if (xHeightRatio >= 0.42f) {
        return FontWeight::Regular;
    } else if (xHeightRatio >= 0.38f) {
        return FontWeight::Light;
    } else {
        return FontWeight::Thin;
    }
}

// ---------------------------------------------------------------------------
// buildQueryProfile (shared by findMatch and matchRegion)
// ---------------------------------------------------------------------------

pdfe::fonts::FontMetricProfile FontMatcher::buildQueryProfile(
    const ScannedTextProperties& properties,
    const std::u32string& textNeeded,
    pdfe::fonts::FontScript scriptOverride) {

    pdfe::fonts::FontMetricProfile profile;
    profile.weightClass = static_cast<int>(properties.weight);
    profile.isItalic = properties.isItalic;
    profile.isSerif = properties.isSerif;

    // Enrich with detected metric ratios to improve distanceTo() scoring.
    // ascenderRatio ≈ capHeight ratio for Latin fonts, so we use it directly.
    if (properties.xHeightRatio > 0.0f && properties.ascenderRatio > 0.0f) {
        profile.xToCapRatio = properties.xHeightRatio / properties.ascenderRatio;
    }
    profile.italicAngle = properties.isItalic ? -12.0f : 0.0f;

    // Script for the query (defect #7): when a per-run override is supplied use
    // it verbatim — the run's OWN script drives matching. Otherwise fall back to
    // the region-dominant detectScript(), now only a coarse hint for whole-region
    // matches, not an authority that overrides a known minority-script run.
    profile.primaryScript = (scriptOverride != pdfe::fonts::FontScript::Unknown)
                                ? scriptOverride
                                : detectScript(textNeeded);

    qDebug() << "[FontMatcher::buildQueryProfile]"
             << "isSerif=" << profile.isSerif
             << "isItalic=" << profile.isItalic
             << "weightClass=" << profile.weightClass
             << "xToCapRatio=" << profile.xToCapRatio
             << "italicAngle=" << profile.italicAngle
             << "perRunOverride=" << (scriptOverride != pdfe::fonts::FontScript::Unknown)
             << "script=" << static_cast<int>(profile.primaryScript);

    return profile;
}

// ---------------------------------------------------------------------------
// findMatch
// ---------------------------------------------------------------------------

FontMatch FontMatcher::findMatch(
    const ScannedTextProperties& properties,
    const std::u32string& textNeeded) const {

    // Region-level match: no per-run override (uses region-dominant script hint).
    return findMatchWithScript(properties, textNeeded,
                               pdfe::fonts::FontScript::Unknown);
}

FontMatch FontMatcher::findMatchWithScript(
    const ScannedTextProperties& properties,
    const std::u32string& textNeeded,
    pdfe::fonts::FontScript scriptOverride) const {

    FontMatch fontMatch;
    fontMatch.detectedProperties = properties;
    fontMatch.sizePt = properties.fontSizePt;
    fontMatch.color = properties.textColor;
    fontMatch.lineSpacingRatio = properties.lineSpacingRatio;

    // Build FontMetricProfile from ScannedTextProperties. When scriptOverride is
    // set (defect #7), the query carries the RUN's script so the selector's
    // distanceTo() penalizes only candidates that lack coverage for THIS run's
    // script — not for the surrounding region's majority script.
    auto profile = buildQueryProfile(properties, textNeeded, scriptOverride);

    // Try to find a matching font via SubstituteFontSelector
    qDebug() << "[FontMatcher::findMatch] Calling selector->findSubstitute with"
             << "profile.isSerif=" << profile.isSerif
             << "profile.postScriptName=" << QString::fromStdString(profile.postScriptName)
             << "profile.familyName=" << QString::fromStdString(profile.familyName)
             << "textLen=" << textNeeded.size();
    auto result = selector_->findSubstitute(profile, textNeeded);

    // Note: no italic hard-filter here. When CNN is primary (matchRegion path),
    // visual ranking handles italic discrimination. When findMatch() is the
    // metric-only fallback, italic match still gets a scoring bonus via
    // SubstituteFontSelector, but mismatches are not rejected outright.

    if (result.has_value()) {
        // Match found
        fontMatch.fontFilePath = result->filePath;
        fontMatch.fontFamily = result->profile.familyName;
        fontMatch.fontStyle = result->profile.styleName;

        // Convert match score to confidence (lower score = better match)
        // Clamp confidence to [0.0, 1.0]
        fontMatch.confidence = 1.0f - std::min(1.0f, result->matchScore / 100.0f);
        qDebug() << "[FontMatcher::findMatch] Selector returned:"
                 << "family=" << QString::fromStdString(fontMatch.fontFamily)
                 << "style=" << QString::fromStdString(fontMatch.fontStyle)
                 << "filePath=" << QString::fromStdString(fontMatch.fontFilePath)
                 << "matchScore=" << result->matchScore
                 << "confidence=" << fontMatch.confidence
                 << "isKnownPair=" << result->isKnownPair
                 << "result.profile.isSerif=" << result->profile.isSerif;
    } else {
        // Fallback to default system fonts, respecting italic detection.
        // FontResolver queries the OS (fontconfig on Linux/macOS,
        // DirectWrite on Windows) and walks a metric-compatible
        // substitution chain — no hardcoded paths. Bundled fonts
        // checked first for security.
        if (properties.isSerif) {
            fontMatch.fontFamily = "Times New Roman";
            std::string path = pdfe::fonts::FontResolver::resolveFontPath(
                "Times New Roman", false, properties.isItalic);
            if (path.empty()) {
                // Try upright as a last resort
                path = pdfe::fonts::FontResolver::resolveFontPath(
                    "Times New Roman", false, false);
            }
            fontMatch.fontFilePath = path;
            fontMatch.fontStyle = properties.isItalic && !path.empty() ? "Italic" : "Regular";
        } else {
            fontMatch.fontFamily = "Arial";
            std::string path = pdfe::fonts::FontResolver::resolveFontPath(
                "Arial", false, properties.isItalic);
            if (path.empty()) {
                path = pdfe::fonts::FontResolver::resolveFontPath(
                    "Arial", false, false);
            }
            fontMatch.fontFilePath = path;
            fontMatch.fontStyle = properties.isItalic && !path.empty() ? "Italic" : "Regular";
        }
        qDebug() << "[FontMatcher::findMatch] FALLBACK:"
                 << "isSerif=" << properties.isSerif
                 << "isItalic=" << properties.isItalic
                 << "→" << QString::fromStdString(fontMatch.fontFamily)
                 << QString::fromStdString(fontMatch.fontStyle)
                 << "path=" << QString::fromStdString(fontMatch.fontFilePath);
        fontMatch.confidence = 0.3f; // Low confidence for fallback
    }

    return fontMatch;
}

// ---------------------------------------------------------------------------
// matchRegion
// ---------------------------------------------------------------------------

FontMatch FontMatcher::matchRegion(
    const RegionTextData& regionData,
    const QImage& pageImage,
    int scanDpi) const {

    // Step 1: Detect properties from OCR data and image
    const ScannedTextProperties properties = detectProperties(regionData, pageImage, scanDpi);

    // Step 2: Convert plain text to u32string (full UTF-8 support)
    std::u32string u32text = utf8ToU32(regionData.plainText);
    // buildQueryProfile is called here for its qDebug-based
    // diagnostic side-effect; the metric-only path below uses
    // properties directly, not the profile object.
    (void)buildQueryProfile(properties, u32text);

    // Defect #7: segment the region into per-script runs instead of collapsing
    // it to a single dominant script. For a single-script region this is exactly
    // the old behavior. For a genuinely mixed region we match the font from the
    // DOMINANT RUN (longest contiguous same-script run) using that run's own
    // script — so a minority-script run can no longer skew the region match, and
    // the chosen font is selected against (not penalized for) the script it must
    // actually render. CNN visual ranking is still deferred to
    // visualRankForLine() which runs when the user clicks a specific span.
    const std::vector<ScriptRun> runs = segmentScriptRuns(u32text);
    FontMatch result;
    bool multiScript = false;
    for (size_t i = 1; i < runs.size(); ++i) {
        if (runs[i].script != runs[0].script &&
            runs[i].script != pdfe::fonts::FontScript::Unknown &&
            runs[0].script != pdfe::fonts::FontScript::Unknown) {
            multiScript = true;
            break;
        }
    }

    if (multiScript) {
        // Pick the longest scripted run as the region-representative run.
        const ScriptRun* dominant = nullptr;
        size_t bestLen = 0;
        for (const ScriptRun& r : runs) {
            if (r.script == pdfe::fonts::FontScript::Unknown)
                continue;
            const size_t len = r.end - r.start;
            if (len > bestLen) {
                bestLen = len;
                dominant = &r;
            }
        }
        if (dominant) {
            qDebug() << "[FontMatcher::matchRegion] multi-script region; matching"
                     << "dominant run script=" << static_cast<int>(dominant->script)
                     << "len=" << bestLen << "of" << runs.size() << "runs";
            result = findMatchForRun(properties, u32text,
                                     dominant->start, dominant->end);
        } else {
            result = findMatch(properties, u32text);
        }
    } else {
        // matchRegion returns a metric-only result quickly at tool activation time.
        result = findMatch(properties, u32text);
    }
    result.detectedProperties = properties;

    qDebug() << "[FontMatcher::matchRegion] metric-only result:"
             << "family=" << QString::fromStdString(result.fontFamily)
             << "sizePt=" << result.sizePt
             << "confidence=" << result.confidence;
    return result;
}

// ---------------------------------------------------------------------------
// collectCandidates
// ---------------------------------------------------------------------------

std::vector<pdfe::fonts::SubstituteResult> FontMatcher::collectCandidates(
    const ScannedTextProperties& properties,
    const std::u32string& textNeeded) const {

    bool hasCNN = cnnClassifier_ && cnnClassifier_->isAvailable();
    auto profile = buildQueryProfile(properties, textNeeded);

    if (hasCNN) {
        // Use metric-filtered top candidates with a larger pool for CNN.
        // The metric scorer already weights serif/italic/weight matching, so
        // structurally wrong fonts (serif on sans-serif page) rank low.
        // Without metric pre-filtering, the CNN ranks 300+ fonts and the weak
        // visual scores let serif fonts beat sans-serif on sans-serif pages.
        constexpr int kCnnCandidateCount = 40;
        auto candidates = selector_->findTopCandidates(profile, textNeeded, kCnnCandidateCount);
        qDebug() << "[FontMatcher::collectCandidates] CNN mode: metric top"
                 << kCnnCandidateCount << "returned" << candidates.size()
                 << "serif=" << properties.isSerif
                 << "italic=" << properties.isItalic;
        return candidates;
    } else {
        return selector_->findTopCandidates(profile, textNeeded, 20);
    }
}

// ---------------------------------------------------------------------------
// visualRankForLine / visualRankForLineWithCandidates
// ---------------------------------------------------------------------------

FontMatcher::VisualRankResult FontMatcher::visualRankForLineWithCandidates(
    const HocrLine& clickedLine,
    const QImage& pageImage,
    int scanDpi,
    const ScannedTextProperties& properties,
    const std::vector<pdfe::fonts::SubstituteResult>& candidates) const
{
    VisualRankResult result;

    if (clickedLine.words.empty() || candidates.empty())
        return result;

    bool hasCNN = cnnClassifier_ && cnnClassifier_->isAvailable();
    bool hasNCC = compositor_ != nullptr;
    if (!hasCNN && !hasNCC)
        return result;

    // Crop 2-3 words from the center of the clicked line
    const auto& words = clickedLine.words;
    int mid = static_cast<int>(words.size()) / 2;
    int lo = std::max(0, mid - 1);
    int hi = std::min(static_cast<int>(words.size()) - 1, mid + 1);

    PixelRect wordsBbox = words[lo].bbox;
    std::string sampleText;
    for (int wi = lo; wi <= hi; ++wi) {
        const auto& wb = words[wi].bbox;
        wordsBbox.x0 = std::min(wordsBbox.x0, wb.x0);
        wordsBbox.y0 = std::min(wordsBbox.y0, wb.y0);
        wordsBbox.x1 = std::max(wordsBbox.x1, wb.x1);
        wordsBbox.y1 = std::max(wordsBbox.y1, wb.y1);
        if (!sampleText.empty()) sampleText += ' ';
        sampleText += words[wi].text;
    }

    // Use full line height (ascenders/descenders)
    wordsBbox.y0 = clickedLine.bbox.y0;
    wordsBbox.y1 = clickedLine.bbox.y1;

    QRect cropRect(
        static_cast<int>(wordsBbox.x0), static_cast<int>(wordsBbox.y0),
        static_cast<int>(wordsBbox.width()), static_cast<int>(wordsBbox.height()));
    QImage scannedCrop = ImageSimilarity::extractAndScale(
        pageImage, cropRect, cropRect.size());

    if (scannedCrop.isNull() || sampleText.empty())
        return result;

    // Diagnostic logging
    {
        // WIN-87 wider class (phase-60): under the per-process unpredictable
        // diagnostics root rather than a fixed %TEMP% name. Empty means
        // unavailable, and the open below then simply fails -- a diagnostic
        // dump is never worth a fallback to a guessable path.
        const QString diagDir = QString::fromStdWString(
            pdfe::core::diagnosticsSubdir("pdfe_font_match").wstring());
        QFile diagFile(diagDir.isEmpty() ? QString()
                                         : diagDir + "/pdfe_match_diag.txt");
        if (diagFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QTextStream ts(&diagFile);
            ts << "visualRankForLine diagnostic\n";
            ts << "hasCNN=" << hasCNN << " hasNCC=" << hasNCC << "\n";
            ts << "sampleText: " << QString::fromStdString(sampleText) << "\n";
            ts << "scannedCrop: " << scannedCrop.width() << "x" << scannedCrop.height() << "\n";
            ts << "candidates: " << candidates.size() << "\n";
            ts << "fontSizePt=" << properties.fontSizePt << "\n";
        }
    }

    // Run visual ranking
    std::vector<VisualFontRanker::RankedCandidate> ranked;
    if (hasCNN) {
        CNNFontRanker cnnRanker(cnnClassifier_);
        ranked = cnnRanker.rankByVisualSimilarity(
            scannedCrop, candidates,
            sampleText, properties.fontSizePt,
            properties.textColor, compositor_, scanDpi);
        qDebug() << "[FontMatcher::visualRankForLineWithCandidates] CNN results=" << ranked.size();
    }
    // NCC fallback
    if (ranked.empty() && hasNCC) {
        VisualFontRanker ranker;
        ranked = ranker.rankByVisualSimilarity(
            scannedCrop, candidates,
            sampleText, properties.fontSizePt,
            properties.textColor, compositor_, scanDpi);
        qDebug() << "[FontMatcher::visualRankForLineWithCandidates] NCC results=" << ranked.size();
    }

    // GitHub #249 -- serif/sans is categorical, so enforce it AFTER scoring.
    //
    // Deliberately here rather than inside CNNFontRanker: the NCC fallback
    // above produces the same user-visible ranking and would otherwise keep
    // the defect. One call site covers both paths, and both rankers stay
    // purely about similarity.
    //
    // Guarded on serifDetermined, NOT on isSerif alone. detectSerif() returns
    // false for "could not measure" as well as for "measured as sans", so
    // partitioning on an undetermined value would force sans-only results on
    // any region whose serif-ness could not be read. Unknown must mean "do not
    // reorder", not "assume sans".
    if (!ranked.empty() && properties.serifDetermined) {
        const std::string before =
            ranked.front().candidate.profile.familyName;
        demoteSerifMismatches(ranked, properties.isSerif);
        if (ranked.front().candidate.profile.familyName != before) {
            qDebug() << "[FontMatcher] serif partition changed the top match from"
                     << QString::fromStdString(before) << "to"
                     << QString::fromStdString(
                            ranked.front().candidate.profile.familyName)
                     << "(query isSerif=" << properties.isSerif << ")";
        }
    } else if (!ranked.empty()) {
        qDebug() << "[FontMatcher] serif-ness undetermined -> no serif "
                    "partition applied (GitHub #249)";
    }

    // Diagnostic: results
    {
        // WIN-87 wider class (phase-60): under the per-process unpredictable
        // diagnostics root rather than a fixed %TEMP% name. Empty means
        // unavailable, and the open below then simply fails -- a diagnostic
        // dump is never worth a fallback to a guessable path.
        const QString diagDir = QString::fromStdWString(
            pdfe::core::diagnosticsSubdir("pdfe_font_match").wstring());
        QFile diagFile(diagDir.isEmpty() ? QString()
                                         : diagDir + "/pdfe_match_diag.txt");
        if (diagFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream ts(&diagFile);
            ts << "\n--- ranking results ---\n";
            for (size_t i = 0; i < std::min(ranked.size(), size_t(10)); ++i) {
                ts << "  [" << i << "] " << QString::fromStdString(ranked[i].candidate.profile.familyName)
                   << " visual=" << ranked[i].visualScore
                   << " combined=" << ranked[i].combinedScore << "\n";
            }
        }
    }

    // Store the full ranked list regardless of best-match outcome
    result.rankedCandidates = ranked;

    if (ranked.empty() || ranked[0].visualScore <= 0.05f) {
        qDebug() << "[FontMatcher::visualRankForLineWithCandidates] No meaningful visual scores";
        return result;
    }

    const auto& best = ranked[0];
    FontMatch fm;
    fm.fontFilePath = best.candidate.filePath;
    fm.fontFamily = best.candidate.profile.familyName;
    fm.fontStyle = best.candidate.profile.styleName;
    fm.sizePt = properties.fontSizePt;
    fm.color = properties.textColor;
    fm.lineSpacingRatio = properties.lineSpacingRatio;
    fm.confidence = std::max(0.3f, best.visualScore);
    fm.detectedProperties = properties;

    qDebug() << "[FontMatcher::visualRankForLineWithCandidates] Winner:"
             << QString::fromStdString(fm.fontFamily)
             << "visual=" << best.visualScore
             << "confidence=" << fm.confidence;
    result.bestMatch = fm;
    return result;
}

std::optional<FontMatch> FontMatcher::visualRankForLine(
    const HocrLine& clickedLine,
    const QImage& pageImage,
    int scanDpi,
    const ScannedTextProperties& properties,
    const std::vector<pdfe::fonts::SubstituteResult>& candidates) const
{
    auto rankResult = visualRankForLineWithCandidates(
        clickedLine, pageImage, scanDpi, properties, candidates);
    return rankResult.bestMatch;
}

// ---------------------------------------------------------------------------
// detectScript
// ---------------------------------------------------------------------------

float FontMatcher::ascenderRatioForScript(pdfe::fonts::FontScript s)
{
    switch (s) {
        case pdfe::fonts::FontScript::Latin:      return 0.78f;
        case pdfe::fonts::FontScript::Cyrillic:   return 0.78f;
        case pdfe::fonts::FontScript::Arabic:     return 0.72f;
        case pdfe::fonts::FontScript::Hebrew:     return 0.75f;
        case pdfe::fonts::FontScript::Thai:       return 0.80f;
        case pdfe::fonts::FontScript::Devanagari: return 0.80f;
        case pdfe::fonts::FontScript::CJK:        return 0.0f;  // no meaningful baseline
        case pdfe::fonts::FontScript::Other:      return 0.0f;  // skip
        case pdfe::fonts::FontScript::Unknown:    return 0.0f;  // skip
    }
    return 0.0f;
}

std::pair<float, bool> FontMatcher::lineFontSizePt(
    const HocrLine& line, pdfe::fonts::FontScript script, int scanDpi)
{
    // Estimate A: x_size (or line-bbox-height fallback).
    float aPt = 0.0f;
    bool hasA = false;
    if (line.xSizePx > 0.0f) {
        aPt = static_cast<float>(CoordMapper::pixelSizeToPdfPt(line.xSizePx, scanDpi));
        hasA = true;
    } else {
        const float lineHeight = static_cast<float>(line.bbox.height());
        if (lineHeight > 0.0f) {
            aPt = static_cast<float>(
                CoordMapper::pixelSizeToPdfPt(lineHeight / 1.2f, scanDpi));
            hasA = true;
        }
    }
    // Estimate B: baseline-to-ascender, gated to ascender 30-95% of line height.
    float bPt = 0.0f;
    bool hasB = false;
    const float kAscenderRatio = ascenderRatioForScript(script);
    if (kAscenderRatio > 0.0f) {
        const float lineHeight = static_cast<float>(line.bbox.height());
        if (lineHeight >= 5.0f) {
            const float lineCenterX =
                static_cast<float>((line.bbox.x0 + line.bbox.x1) / 2.0);
            const float baselineY =
                static_cast<float>(line.bbox.y1)
                + line.baselineSlope * (lineCenterX - static_cast<float>(line.bbox.x0))
                + line.baselineOffset;
            const float ascenderPx = baselineY - static_cast<float>(line.bbox.y0);
            if (ascenderPx > lineHeight * 0.30f && ascenderPx < lineHeight * 0.95f) {
                bPt = static_cast<float>(
                    CoordMapper::pixelSizeToPdfPt(ascenderPx / kAscenderRatio, scanDpi));
                hasB = true;
            }
        }
    }
    if (hasA && hasB) return {(aPt + bPt) / 2.0f, true};
    if (hasA) return {aPt, true};
    if (hasB) return {bPt, true};
    return {0.0f, false};
}

float FontMatcher::clickedLineSizePt(
    const std::vector<float>& perLineSizePt, int lineIndex, float fallbackPt)
{
    if (lineIndex >= 0 && lineIndex < static_cast<int>(perLineSizePt.size())) {
        const float v = perLineSizePt[lineIndex];
        if (v > 0.0f)
            return v;
    }
    return fallbackPt;
}

pdfe::fonts::FontScript FontMatcher::detectScript(const std::u32string& text)
{
    int latinCount = 0;
    int arabicCount = 0;
    int cyrillicCount = 0;
    int cjkCount = 0;
    int devanagariCount = 0;
    int hebrewCount = 0;
    int thaiCount = 0;

    for (char32_t ch : text) {
        // Only count actual Latin letters, NOT spaces/digits/punctuation
        if ((ch >= 0x0041 && ch <= 0x005A) ||   // A-Z
            (ch >= 0x0061 && ch <= 0x007A) ||   // a-z
            (ch >= 0x00C0 && ch <= 0x024F)) {   // Latin Extended (accented chars)
            ++latinCount;
        } else if ((ch >= 0x0600 && ch <= 0x06FF) ||  // Arabic
                   (ch >= 0x0750 && ch <= 0x077F) ||  // Arabic Supplement
                   (ch >= 0xFB50 && ch <= 0xFDFF) ||  // Arabic Presentation-A
                   (ch >= 0xFE70 && ch <= 0xFEFF)) {  // Arabic Presentation-B
            ++arabicCount;
        } else if (ch >= 0x0400 && ch <= 0x052F) {    // Cyrillic
            ++cyrillicCount;
        } else if ((ch >= 0x4E00 && ch <= 0x9FFF) ||  // CJK Unified Ideographs
                   (ch >= 0x3400 && ch <= 0x4DBF) ||  // CJK Ext A
                   (ch >= 0x3000 && ch <= 0x303F) ||  // CJK Symbols/Punctuation
                   (ch >= 0x3040 && ch <= 0x309F) ||  // Hiragana
                   (ch >= 0x30A0 && ch <= 0x30FF) ||  // Katakana
                   (ch >= 0xAC00 && ch <= 0xD7AF)) {  // Hangul Syllables
            ++cjkCount;
        } else if (ch >= 0x0900 && ch <= 0x097F) {    // Devanagari
            ++devanagariCount;
        } else if ((ch >= 0x0590 && ch <= 0x05FF) ||  // Hebrew
                   (ch >= 0xFB1D && ch <= 0xFB4F)) {  // Hebrew Presentation
            ++hebrewCount;
        } else if (ch >= 0x0E00 && ch <= 0x0E7F) {    // Thai
            ++thaiCount;
        }
    }

    int maxCount = latinCount;
    auto script = pdfe::fonts::FontScript::Latin;

    auto bump = [&](int count, pdfe::fonts::FontScript s) {
        if (count > maxCount) {
            maxCount = count;
            script = s;
        }
    };
    bump(arabicCount,     pdfe::fonts::FontScript::Arabic);
    bump(cyrillicCount,   pdfe::fonts::FontScript::Cyrillic);
    bump(cjkCount,        pdfe::fonts::FontScript::CJK);
    bump(devanagariCount, pdfe::fonts::FontScript::Devanagari);
    bump(hebrewCount,     pdfe::fonts::FontScript::Hebrew);
    bump(thaiCount,       pdfe::fonts::FontScript::Thai);

    return maxCount > 0 ? script : pdfe::fonts::FontScript::Unknown;
}

// ---------------------------------------------------------------------------
// Per-run script segmentation (defect #7)
// ---------------------------------------------------------------------------

// Classify a single codepoint into a script, or Unknown for script-neutral
// characters (spaces, ASCII digits, punctuation, symbols) which should attach
// to the surrounding run rather than break it.
static pdfe::fonts::FontScript scriptOfCodepoint(char32_t ch)
{
    if ((ch >= 0x0041 && ch <= 0x005A) ||   // A-Z
        (ch >= 0x0061 && ch <= 0x007A) ||   // a-z
        (ch >= 0x00C0 && ch <= 0x024F)) {   // Latin Extended
        return pdfe::fonts::FontScript::Latin;
    }
    if ((ch >= 0x0600 && ch <= 0x06FF) ||
        (ch >= 0x0750 && ch <= 0x077F) ||
        (ch >= 0xFB50 && ch <= 0xFDFF) ||
        (ch >= 0xFE70 && ch <= 0xFEFF)) {
        return pdfe::fonts::FontScript::Arabic;
    }
    if (ch >= 0x0400 && ch <= 0x052F) {
        return pdfe::fonts::FontScript::Cyrillic;
    }
    if ((ch >= 0x4E00 && ch <= 0x9FFF) ||
        (ch >= 0x3400 && ch <= 0x4DBF) ||
        (ch >= 0x3000 && ch <= 0x303F) ||
        (ch >= 0x3040 && ch <= 0x309F) ||
        (ch >= 0x30A0 && ch <= 0x30FF) ||
        (ch >= 0xAC00 && ch <= 0xD7AF)) {
        return pdfe::fonts::FontScript::CJK;
    }
    if (ch >= 0x0900 && ch <= 0x097F) {
        return pdfe::fonts::FontScript::Devanagari;
    }
    if ((ch >= 0x0590 && ch <= 0x05FF) ||
        (ch >= 0xFB1D && ch <= 0xFB4F)) {
        return pdfe::fonts::FontScript::Hebrew;
    }
    if (ch >= 0x0E00 && ch <= 0x0E7F) {
        return pdfe::fonts::FontScript::Thai;
    }
    return pdfe::fonts::FontScript::Unknown;  // script-neutral
}

std::vector<FontMatcher::ScriptRun> FontMatcher::segmentScriptRuns(
    const std::u32string& text)
{
    std::vector<ScriptRun> runs;
    if (text.empty())
        return runs;

    // Walk the text, opening a new run each time the (non-neutral) script
    // changes. Script-neutral codepoints extend whatever run is currently open
    // (or, before any scripted char, a leading Unknown run that the next
    // scripted char will claim). This keeps "ABC ١٢٣" from fragmenting on the
    // space while still splitting Latin from Arabic.
    ScriptRun current{0, 0, pdfe::fonts::FontScript::Unknown};
    bool haveRun = false;

    for (size_t i = 0; i < text.size(); ++i) {
        const pdfe::fonts::FontScript s = scriptOfCodepoint(text[i]);

        if (!haveRun) {
            current = ScriptRun{i, i + 1, s};
            haveRun = true;
            continue;
        }

        if (current.script == pdfe::fonts::FontScript::Unknown) {
            // The open run is still script-neutral (only leading spaces/digits/
            // punctuation so far) — adopt this codepoint's script (or stay
            // Unknown if it too is neutral). This keeps leading neutrals attached
            // to the first scripted run instead of fragmenting off.
            current.script = s;
            current.end = i + 1;
        } else if (s == pdfe::fonts::FontScript::Unknown || s == current.script) {
            // Neutral char or same script — extend the open run.
            current.end = i + 1;
        } else {
            // Genuine script change — close the open run and start a new one.
            runs.push_back(current);
            current = ScriptRun{i, i + 1, s};
        }
    }
    if (haveRun)
        runs.push_back(current);

    return runs;
}

pdfe::fonts::FontScript FontMatcher::scriptForRun(
    const std::u32string& text, size_t start, size_t end)
{
    if (start >= end || start >= text.size())
        return pdfe::fonts::FontScript::Unknown;
    end = std::min(end, text.size());
    return detectScript(text.substr(start, end - start));
}

FontMatch FontMatcher::findMatchForRun(
    const ScannedTextProperties& properties,
    const std::u32string& fullText,
    size_t runStart,
    size_t runEnd) const
{
    // Clamp the run window to the text bounds.
    runEnd = std::min(runEnd, fullText.size());
    const std::u32string runText =
        (runStart < runEnd) ? fullText.substr(runStart, runEnd - runStart)
                            : std::u32string();

    // The run's OWN script drives both the coverage check (runText) and the
    // mismatch penalty (scriptOverride) — never the region-dominant script.
    const pdfe::fonts::FontScript runScript =
        scriptForRun(fullText, runStart, runEnd);

    qDebug() << "[FontMatcher::findMatchForRun]"
             << "run=[" << runStart << "," << runEnd << ")"
             << "runScript=" << static_cast<int>(runScript)
             << "runLen=" << runText.size();

    return findMatchWithScript(properties, runText, runScript);
}

// ---------------------------------------------------------------------------
// detectSerif
// ---------------------------------------------------------------------------

// Pre-filter: returns true if the word text contains any tall character.
// Broader than isSerifDiagnosticChar() — includes round-based chars like
// 'h', 'd', 'b' because this is just a pre-filter for word-level fallback,
// not the measurement itself. Per-char mode uses isSerifDiagnosticChar() instead.
static bool hasTallCharacter(const std::string& text)
{
    return std::any_of(text.begin(), text.end(), [](char c) {
        switch (c) {
            case 'I': case 'l': case 'i': case 'T': case 'H':
            case 'h': case 'd': case 'b': case 'k': case 'f':
            case 'L': case 'F': case 'E': case 't':
                return true;
            default:
                return false;
        }
    });
}

// Characters whose shape is diagnostic for serif detection.
// These have narrow vertical strokes where serifs produce a clear baseline
// widening. Round characters (a, b, d, e, o) are excluded because their
// curved bases are naturally wider than mid-height, causing false positives.
static bool isSerifDiagnosticChar(char32_t cp)
{
    switch (cp) {
        case U'I': case U'l': case U'T': case U'H':
        case U'L': case U'F': case U'E': case U'f':
        case U'i': case U't': case U'k':
            return true;
        default:
            return false;
    }
}

// Check if a pixel is foreground (dark) using luminance threshold.
static bool isForeground(const QImage& image, int x, int y)
{
    if (x < 0 || x >= image.width() || y < 0 || y >= image.height())
        return false;
    const QColor pixel = image.pixelColor(x, y);
    const float gray = 0.299f * pixel.red() + 0.587f * pixel.green() + 0.114f * pixel.blue();
    return gray < 128.0f;
}

// Count foreground (dark) pixels in a horizontal row within [x0, x1).
static int countForegroundPixelsInRow(const QImage& image, int y, int x0, int x1)
{
    if (y < 0 || y >= image.height())
        return 0;
    x0 = std::max(0, x0);
    x1 = std::min(image.width(), x1);
    int count = 0;
    for (int x = x0; x < x1; ++x) {
        if (isForeground(image, x, y))
            ++count;
    }
    return count;
}

// A horizontal connected run of foreground pixels (one stroke cross-section).
struct FgRun {
    int start;  // inclusive
    int end;    // exclusive
    int width() const { return end - start; }
};

// Find connected runs of foreground pixels in a horizontal row within [x0, x1).
static std::vector<FgRun> findForegroundRuns(const QImage& image, int y, int x0, int x1)
{
    std::vector<FgRun> runs;
    if (y < 0 || y >= image.height())
        return runs;
    x0 = std::max(0, x0);
    x1 = std::min(image.width(), x1);

    bool inRun = false;
    int runStart = 0;
    for (int x = x0; x < x1; ++x) {
        if (isForeground(image, x, y)) {
            if (!inRun) {
                runStart = x;
                inRun = true;
            }
        } else {
            if (inRun) {
                runs.push_back({runStart, x});
                inRun = false;
            }
        }
    }
    if (inRun) {
        runs.push_back({runStart, x1});
    }
    return runs;
}

// Per-character serif detection: measures baseline vs mid-stem pixel width
// on individual character bounding boxes from HOCR char-level data.
// Sets hasCharData=true if any word had char-level boxes (even if no
// diagnostic ratios were collected). Returns the number of ratios collected.
static int detectSerifPerChar(const QImage& pageImage,
                               const std::vector<HocrLine>& lines,
                               std::vector<float>& serifRatios,
                               bool& hasCharData)
{
    int collected = 0;
    hasCharData = false;

    for (const auto& line : lines) {
        for (const auto& word : line.words) {
            if (word.confidence < 50)
                continue;
            if (word.chars.empty())
                continue;

            hasCharData = true;

            for (const auto& ch : word.chars) {
                if (!isSerifDiagnosticChar(ch.codepoint))
                    continue;
                if (ch.confidence < 40)
                    continue;

                // Clamp char bbox to image
                const int cx0 = std::max(0, static_cast<int>(ch.bbox.x0));
                const int cy0 = std::max(0, static_cast<int>(ch.bbox.y0));
                const int cx1 = std::min(pageImage.width(), static_cast<int>(ch.bbox.x1));
                const int cy1 = std::min(pageImage.height(), static_cast<int>(ch.bbox.y1));

                if (cx1 <= cx0 || cy1 <= cy0)
                    continue;

                const int charH = cy1 - cy0;
                const int charW = cx1 - cx0;
                if (charH < 8 || charW < 3)
                    continue;  // Too small to measure reliably

                // Compute baseline Y from HOCR line equation within this char's bbox
                const float xCenter = (ch.bbox.x0 + ch.bbox.x1) / 2.0f;
                int baselineY = static_cast<int>(
                    line.bbox.y0 + line.baselineSlope * (xCenter - line.bbox.x0)
                    + line.baselineOffset);

                // Clamp baseline to char bbox (near bottom of character)
                baselineY = std::clamp(baselineY, cy0 + 2, cy1 - 1);

                // Mid-stem Y: halfway between char top and baseline
                const int midStemY = (cy0 + baselineY) / 2;

                if (midStemY <= cy0 || midStemY >= baselineY)
                    continue;

                // Measure foreground pixels on this single character's bbox
                const int baselineWidth = countForegroundPixelsInRow(
                    pageImage, baselineY, cx0, cx1);
                const int stemWidth = countForegroundPixelsInRow(
                    pageImage, midStemY, cx0, cx1);

                if (stemWidth < 2)
                    continue;

                const float ratio = static_cast<float>(baselineWidth - stemWidth)
                                  / static_cast<float>(stemWidth);

                serifRatios.push_back(ratio);
                ++collected;
            }
        }
    }
    return collected;
}

// Per-stroke word-level serif detection for when char-level boxes are unavailable.
// Finds individual vertical strokes (connected foreground runs) at mid-stem height,
// then measures each stroke's pixel extent at the baseline. This isolates individual
// character measurements even without explicit char bounding boxes, avoiding the
// false positives caused by summing all pixels across a word row.
static int detectSerifPerWord(const QImage& pageImage,
                               const std::vector<HocrLine>& lines,
                               std::vector<float>& serifRatios)
{
    int collected = 0;

    for (const auto& line : lines) {
        for (const auto& word : line.words) {
            // Pre-filter: skip words without tall chars. hasTallCharacter is broader
            // than isSerifDiagnosticChar — round-based chars (h,d,b) are filtered out
            // later by the stroke width check (run.width() > wordHeight/3).
            if (word.confidence < 50 || !hasTallCharacter(word.text))
                continue;

            const int wx0 = std::max(0, static_cast<int>(word.bbox.x0));
            const int wy0 = std::max(0, static_cast<int>(word.bbox.y0));
            const int wx1 = std::min(pageImage.width(), static_cast<int>(word.bbox.x1));
            const int wy1 = std::min(pageImage.height(), static_cast<int>(word.bbox.y1));

            if (wx1 <= wx0 || wy1 <= wy0)
                continue;

            if (wy1 - wy0 < 8)
                continue;

            const float xCenter = (word.bbox.x0 + word.bbox.x1) / 2.0f;
            int baselineY = static_cast<int>(
                line.bbox.y0 + line.baselineSlope * (xCenter - line.bbox.x0)
                + line.baselineOffset);
            baselineY = std::clamp(baselineY, wy0 + 2, wy1 - 1);

            const int midStemY = (wy0 + baselineY) / 2;
            if (midStemY <= wy0 || midStemY >= baselineY)
                continue;

            // Find individual strokes at mid-stem height.
            // Each run is one character's vertical stroke cross-section.
            auto stemRuns = findForegroundRuns(pageImage, midStemY, wx0, wx1);

            for (const auto& run : stemRuns) {
                if (run.width() < 2)
                    continue;  // Too thin to measure

                // Skip wide runs — these are likely round characters (a, b, d, e, o)
                // whose broad cross-section would cause false positives.
                // Narrow stems (I, l, T strokes) are 2-8px at 300 DPI.
                const int wordHeight = wy1 - wy0;
                if (run.width() > wordHeight / 3)
                    continue;

                // Measure baseline foreground extent in the same horizontal range
                // as this stroke, plus a small margin for serif overhang.
                const int margin = run.width();  // Serifs extend ~1 stem width
                const int bx0 = std::max(wx0, run.start - margin);
                const int bx1 = std::min(wx1, run.end + margin);

                const int baselineExtent = countForegroundPixelsInRow(
                    pageImage, baselineY, bx0, bx1);

                if (baselineExtent < 2)
                    continue;

                const float ratio = static_cast<float>(baselineExtent - run.width())
                                  / static_cast<float>(run.width());

                serifRatios.push_back(ratio);
                ++collected;
            }
        }
    }
    return collected;
}

bool FontMatcher::detectSerif(const QImage& pageImage,
                               const std::vector<HocrLine>& lines,
                               bool* determined)
{
    // Default to NOT determined and set it true only on the one path that
    // actually measures. Every early return below is an "unknown", and they
    // are all currently reported as false -- indistinguishable from a real
    // sans-serif measurement without this flag (GitHub #249).
    if (determined)
        *determined = false;

    if (pageImage.isNull() || lines.empty())
        return false;

    std::vector<float> serifRatios;

    // Prefer per-character analysis when char-level HOCR boxes are available.
    // This isolates measurements to individual diagnostic characters (I, l, T, etc.)
    // and eliminates false positives from round letter shapes (a, b, d, e, o)
    // that naturally widen at the baseline due to their curved geometry.
    bool hasCharData = false;
    int charCount = detectSerifPerChar(pageImage, lines, serifRatios, hasCharData);

    // Fallback: word-level analysis only if no char-level boxes existed at all.
    // When char-level data is present but no diagnostic ratios were collected
    // (e.g., all chars were non-diagnostic or low confidence), we trust the
    // char-level data and do NOT fall back to the less accurate word-level path.
    // Note: Tesseract typically provides char boxes for all words or none;
    // mixed pages (some words with chars, some without) are not expected.
    if (!hasCharData) {
        (void)detectSerifPerWord(pageImage, lines, serifRatios);
    }

    if (serifRatios.empty()) {
        qDebug() << "[FontMatcher::detectSerif] No valid ratios collected -> default sans-serif";
        return false;
    }

    // Compute median ratio (true median: average of two middle values for even count)
    std::sort(serifRatios.begin(), serifRatios.end());
    const size_t n = serifRatios.size();
    const float medianRatio = (n % 2 == 1)
        ? serifRatios[n / 2]
        : (serifRatios[n / 2 - 1] + serifRatios[n / 2]) / 2.0f;

    // Per-character measurement is more precise — true serifs show 50-100%
    // wider baseline on characters like 'I' and 'l'. Sans-serif characters
    // show 0-20% variance from antialiasing/noise. Use a lower threshold
    // for per-char (0.30) since there's no round-letter contamination.
    // Word-level fallback keeps the higher threshold (0.35) to compensate
    // for adjacent character contamination.
    const bool usedPerChar = hasCharData && charCount > 0;
    const float threshold = usedPerChar ? 0.30f : 0.35f;
    bool isSerif = medianRatio > threshold;

    // The ONLY path that reaches here has at least one valid stroke-width
    // ratio, so this is the only result that is a measurement.
    if (determined)
        *determined = true;

    qDebug() << "[FontMatcher::detectSerif] RESULT: medianRatio=" << medianRatio
             << "threshold=" << threshold
             << "isSerif=" << isSerif
             << "ratioCount=" << serifRatios.size()
             << "mode=" << (usedPerChar ? "per-char" : "per-word")
             << "allRatios=[" << serifRatios.front() << ".." << serifRatios.back() << "]";
    return isSerif;
}

// ---------------------------------------------------------------------------
// measureStemWidth — scan horizontally at a given Y to measure stroke width
// ---------------------------------------------------------------------------

static int measureStemWidth(const QImage& image, int midX, int y, int xMin, int xMax)
{
    if (y < 0 || y >= image.height())
        return 0;

    if (!isForeground(image, midX, y))
        return 0;

    // Expand left from midX
    int left = midX;
    while (left > xMin && isForeground(image, left - 1, y))
        --left;

    // Expand right from midX
    int right = midX;
    while (right < xMax - 1 && isForeground(image, right + 1, y))
        ++right;

    return right - left + 1;
}

// ---------------------------------------------------------------------------
// detectWordBold — vertical stem thickness analysis
// ---------------------------------------------------------------------------

bool FontMatcher::detectWordBold(const QImage& pageImage,
                                  const HocrWord& word,
                                  const HocrLine& line)
{
    if (pageImage.isNull() || !hasTallCharacter(word.text))
        return false;

    const int wx0 = std::max(0, static_cast<int>(word.bbox.x0));
    const int wy0 = std::max(0, static_cast<int>(word.bbox.y0));
    const int wx1 = std::min(pageImage.width(), static_cast<int>(word.bbox.x1));
    const int wy1 = std::min(pageImage.height(), static_cast<int>(word.bbox.y1));

    if (wx1 <= wx0 || wy1 <= wy0)
        return false;

    const int wordHeight = wy1 - wy0;
    if (wordHeight < 8)
        return false;

    // Compute baseline Y from HOCR line equation
    const float xCenter = (word.bbox.x0 + word.bbox.x1) / 2.0f;
    int baselineY = static_cast<int>(
        line.bbox.y0 + line.baselineSlope * (xCenter - line.bbox.x0)
        + line.baselineOffset);
    baselineY = std::clamp(baselineY, wy0 + 2, wy1 - 1);

    // Mid-stem Y: halfway between word top and baseline
    const int midStemY = (wy0 + baselineY) / 2;
    if (midStemY <= wy0 || midStemY >= baselineY)
        return false;

    // Measure stem width by scanning horizontally at mid-stem
    const int midX = (wx0 + wx1) / 2;
    const int stemWidth = measureStemWidth(pageImage, midX, midStemY, wx0, wx1);

    if (stemWidth < 1)
        return false;

    // Normalize by word height
    const float ratio = static_cast<float>(stemWidth) / static_cast<float>(wordHeight);

    // Bold fonts have thicker stems. At 300 DPI, 12pt regular ≈ 3-4px stem,
    // bold ≈ 5-7px stem for ~50px word height. Ratio: regular ~0.07, bold ~0.12+
    return ratio > 0.11f;
}

// ---------------------------------------------------------------------------
// detectWordItalic — character slant angle measurement
// ---------------------------------------------------------------------------

// Compute horizontal centroid of foreground pixels in a band [y0, y1) within [x0, x1)
static float horizontalCentroid(const QImage& image, int x0, int y0, int x1, int y1)
{
    x0 = std::max(0, x0);
    x1 = std::min(image.width(), x1);
    y0 = std::max(0, y0);
    y1 = std::min(image.height(), y1);

    float sumX = 0.0f;
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const QColor pixel = image.pixelColor(x, y);
            const float gray = 0.299f * pixel.red() + 0.587f * pixel.green() + 0.114f * pixel.blue();
            if (gray < 128.0f) {
                sumX += static_cast<float>(x);
                ++count;
            }
        }
    }
    return count > 0 ? sumX / static_cast<float>(count) : -1.0f;
}

bool FontMatcher::detectWordItalic(const QImage& pageImage,
                                    const HocrWord& word,
                                    const HocrLine& line)
{
    if (pageImage.isNull() || !hasTallCharacter(word.text))
        return false;

    const int wx0 = std::max(0, static_cast<int>(word.bbox.x0));
    const int wy0 = std::max(0, static_cast<int>(word.bbox.y0));
    const int wx1 = std::min(pageImage.width(), static_cast<int>(word.bbox.x1));
    const int wy1 = std::min(pageImage.height(), static_cast<int>(word.bbox.y1));

    if (wx1 <= wx0 || wy1 <= wy0)
        return false;

    const int wordHeight = wy1 - wy0;
    if (wordHeight < 10)
        return false;

    // Compute centroids at top quarter and bottom quarter
    const int topBandY0 = wy0;
    const int topBandY1 = wy0 + wordHeight / 4;
    const int bottomBandY0 = wy1 - wordHeight / 4;
    const int bottomBandY1 = wy1;

    const float topCentroid = horizontalCentroid(pageImage, wx0, topBandY0, wx1, topBandY1);
    const float bottomCentroid = horizontalCentroid(pageImage, wx0, bottomBandY0, wx1, bottomBandY1);

    if (topCentroid < 0.0f || bottomCentroid < 0.0f)
        return false;

    // Horizontal displacement: italic characters lean right (bottom centroid > top)
    const float dx = bottomCentroid - topCentroid;
    const float dy = static_cast<float>(wordHeight);

    // Angle in degrees
    const float angle = std::atan2(std::abs(dx), dy) * 180.0f / 3.14159265f;

    // Italic fonts typically have 10-15° slant. Threshold at 10° to avoid false
    // positives from scan rotation, kerning, and character shape asymmetry on
    // upright text. Previously 5° — too sensitive for scanned PDFs.
    return angle > 10.0f;
}

// ---------------------------------------------------------------------------
// detectUnderlinedWords — horizontal line below baseline
// ---------------------------------------------------------------------------

void FontMatcher::detectUnderlinedWords(const QImage& pageImage, HocrLine& line)
{
    if (pageImage.isNull() || line.words.empty())
        return;

    const float lineHeight = static_cast<float>(line.bbox.y1 - line.bbox.y0);
    if (lineHeight < 10.0f)
        return;

    for (auto& word : line.words) {
        if (word.confidence < 50)
            continue;

        const int wx0 = std::max(0, static_cast<int>(word.bbox.x0));
        const int wx1 = std::min(pageImage.width(), static_cast<int>(word.bbox.x1));
        const int wordWidth = wx1 - wx0;

        if (wordWidth < 4)
            continue;

        // Compute baseline Y from HOCR equation
        const float xCenter = (word.bbox.x0 + word.bbox.x1) / 2.0f;
        int baselineY = static_cast<int>(
            line.bbox.y0 + line.baselineSlope * (xCenter - line.bbox.x0)
            + line.baselineOffset);
        baselineY = std::clamp(baselineY,
            static_cast<int>(line.bbox.y0) + 2,
            static_cast<int>(line.bbox.y1) - 1);

        // Scan a 3-row band just below baseline (15% of line height below baseline)
        const int underlineY = baselineY + static_cast<int>(lineHeight * 0.15f);
        if (underlineY < 0 || underlineY + 2 >= pageImage.height())
            continue;

        // Count foreground pixels in a 3-row band at the underline position
        int underlinePixels = 0;
        for (int dy = 0; dy < 3; ++dy) {
            underlinePixels += countForegroundPixelsInRow(
                pageImage, underlineY + dy, wx0, wx1);
        }

        // Also count at a control position (deep descender area, ~35% below baseline)
        const int controlY = baselineY + static_cast<int>(lineHeight * 0.35f);
        int controlPixels = 0;
        if (controlY >= 0 && controlY + 2 < pageImage.height()) {
            for (int dy = 0; dy < 3; ++dy) {
                controlPixels += countForegroundPixelsInRow(
                    pageImage, controlY + dy, wx0, wx1);
            }
        }

        // Underline creates dense pixel coverage below baseline.
        // Require: underline band has > 60% coverage, and significantly more
        // than the control zone.
        const float coverage = static_cast<float>(underlinePixels)
            / static_cast<float>(wordWidth * 3);  // 3 rows

        const bool denseAtUnderline = coverage > 0.50f;
        const bool moreThanControl = underlinePixels > controlPixels * 2;

        word.style.isUnderlined = denseAtUnderline && moreThanControl;
    }
}

// ---------------------------------------------------------------------------
// detectStrikethroughWords — horizontal line through text center
// ---------------------------------------------------------------------------

void FontMatcher::detectStrikethroughWords(const QImage& pageImage, HocrLine& line)
{
    if (pageImage.isNull() || line.words.empty())
        return;

    const float lineHeight = static_cast<float>(line.bbox.y1 - line.bbox.y0);
    if (lineHeight < 10.0f)
        return;

    for (auto& word : line.words) {
        if (word.confidence < 50)
            continue;

        const int wx0 = std::max(0, static_cast<int>(word.bbox.x0));
        const int wy0 = std::max(0, static_cast<int>(word.bbox.y0));
        const int wx1 = std::min(pageImage.width(), static_cast<int>(word.bbox.x1));
        const int wordWidth = wx1 - wx0;

        if (wordWidth < 4)
            continue;

        // Compute baseline Y
        const float xCenter = (word.bbox.x0 + word.bbox.x1) / 2.0f;
        int baselineY = static_cast<int>(
            line.bbox.y0 + line.baselineSlope * (xCenter - line.bbox.x0)
            + line.baselineOffset);
        baselineY = std::clamp(baselineY,
            static_cast<int>(line.bbox.y0) + 2,
            static_cast<int>(line.bbox.y1) - 1);

        // Mid-text Y: between word top and baseline (x-height zone)
        const int midTextY = (wy0 + baselineY) / 2;
        if (midTextY <= wy0 || midTextY >= baselineY)
            continue;

        // Count foreground pixels in a 3-row band at mid-text
        int midPixels = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            midPixels += countForegroundPixelsInRow(
                pageImage, midTextY + dy, wx0, wx1);
        }

        // Control: top 10% of line (only ascenders — sparse for most text)
        const int controlY = wy0 + static_cast<int>(lineHeight * 0.10f);
        int controlPixels = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            controlPixels += countForegroundPixelsInRow(
                pageImage, controlY + dy, wx0, wx1);
        }

        // Strikethrough fills inter-character gaps at mid-height, creating
        // significantly more pixels than normal text at the same height.
        // Require mid-text coverage > 70% of word width AND at least 40% more
        // pixels than the control zone.
        const float midCoverage = static_cast<float>(midPixels)
            / static_cast<float>(wordWidth * 3);

        const bool denseAtMid = midCoverage > 0.65f;
        // controlPixels can be 0 for short words; guard against division by zero
        const bool excessVsControl = (controlPixels < 1)
            ? (midPixels > wordWidth)
            : (midPixels > static_cast<int>(controlPixels * 1.4f));

        word.style.isStrikethrough = denseAtMid && excessVsControl;
    }
}

// ---------------------------------------------------------------------------
// detectWordStyles — orchestrator for all per-word detections
// ---------------------------------------------------------------------------

void FontMatcher::detectWordStyles(const QImage& pageImage,
                                    std::vector<HocrLine>& lines)
{
    if (pageImage.isNull())
        return;

    for (auto& line : lines) {
        // Per-word bold and italic detection
        for (auto& word : line.words) {
            if (word.confidence < 50)
                continue;

            word.style.isBold = detectWordBold(pageImage, word, line);
            word.style.isItalic = detectWordItalic(pageImage, word, line);
        }

        // Line-level decoration detection (underline/strikethrough)
        detectUnderlinedWords(pageImage, line);
        detectStrikethroughWords(pageImage, line);
    }
}

} // namespace pdfe::tre
