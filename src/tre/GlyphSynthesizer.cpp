#include "tre/GlyphSynthesizer.h"
#include "tre/GlyphAtlas.h"

#include <QPainter>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

namespace pdfe::tre {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GlyphSynthesizer::GlyphSynthesizer() = default;

// ---------------------------------------------------------------------------
// Phase A: Profile Extraction
// ---------------------------------------------------------------------------

void GlyphSynthesizer::buildProfile(const GlyphAtlas& atlas)
{
    profile_ = extractProfile(atlas);
    profileBuilt_ = true;
}

const FontStyleProfile& GlyphSynthesizer::profile() const
{
    return profile_;
}

float GlyphSynthesizer::measureStemWidth(const QImage& glyph)
{
    if (glyph.isNull() || glyph.width() < 2 || glyph.height() < 4)
        return 0.0f;

    const int h = glyph.height();
    const int w = glyph.width();
    float totalMinWidth = 0.0f;
    int validScans = 0;

    // Scan at 25%, 50%, 75% of glyph height
    for (float frac : {0.25f, 0.50f, 0.75f}) {
        int y = static_cast<int>(h * frac);
        if (y < 0 || y >= h)
            continue;

        // Find contiguous runs of foreground (alpha > 128) pixels
        int minRunWidth = w + 1;  // sentinel
        int runStart = -1;

        for (int x = 0; x <= w; ++x) {
            bool fg = (x < w) && (qAlpha(glyph.pixel(x, y)) > 128);
            if (fg && runStart < 0) {
                runStart = x;
            } else if (!fg && runStart >= 0) {
                int runW = x - runStart;
                if (runW > 0 && runW < minRunWidth)
                    minRunWidth = runW;
                runStart = -1;
            }
        }

        if (minRunWidth <= w) {
            totalMinWidth += static_cast<float>(minRunWidth);
            ++validScans;
        }
    }

    return validScans > 0 ? totalMinWidth / validScans : 0.0f;
}

float GlyphSynthesizer::measureInkDensity(const QImage& glyph)
{
    if (glyph.isNull())
        return 0.0f;

    int fgCount = 0;
    const int w = glyph.width();
    const int h = glyph.height();

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (qAlpha(glyph.pixel(x, y)) > 128)
                ++fgCount;
        }
    }

    int total = w * h;
    return total > 0 ? static_cast<float>(fgCount) / total : 0.0f;
}

float GlyphSynthesizer::measureSlant(const QImage& glyph)
{
    if (glyph.isNull() || glyph.height() < 8)
        return 0.0f;

    const int w = glyph.width();
    const int h = glyph.height();

    // Horizontal centroid of alpha pixels in top 25% and bottom 25%
    auto centroid = [&](int yStart, int yEnd) -> float {
        float sumX = 0.0f;
        int count = 0;
        for (int y = yStart; y < yEnd; ++y) {
            for (int x = 0; x < w; ++x) {
                if (qAlpha(glyph.pixel(x, y)) > 128) {
                    sumX += static_cast<float>(x);
                    ++count;
                }
            }
        }
        return count > 0 ? sumX / count : w * 0.5f;
    };

    int quarter = h / 4;
    float topCentroid = centroid(0, quarter);
    float botCentroid = centroid(h - quarter, h);
    float dy = static_cast<float>(h - quarter);  // vertical distance between region midpoints

    if (dy < 1.0f)
        return 0.0f;

    float dx = topCentroid - botCentroid;
    return std::atan2(std::abs(dx), dy) * 180.0f / 3.14159265f;
}

float GlyphSynthesizer::measureSerifExtent(const QImage& glyph)
{
    if (glyph.isNull() || glyph.height() < 6)
        return 0.0f;

    const int w = glyph.width();
    const int h = glyph.height();

    // Count foreground pixels in bottom 2 rows (baseline region)
    int baselineCount = 0;
    for (int y = h - 2; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (qAlpha(glyph.pixel(x, y)) > 128)
                ++baselineCount;
        }
    }

    // Count foreground pixels in middle 2 rows (stem region)
    int midY = h / 2;
    int stemCount = 0;
    for (int y = midY - 1; y <= midY; ++y) {
        if (y < 0 || y >= h) continue;
        for (int x = 0; x < w; ++x) {
            if (qAlpha(glyph.pixel(x, y)) > 128)
                ++stemCount;
        }
    }

    if (stemCount < 1)
        return 0.0f;

    float extent = static_cast<float>(baselineCount - stemCount) / static_cast<float>(stemCount);
    return std::max(0.0f, extent);  // Clamp to non-negative (0=sans-serif, higher=more serif)
}

FontStyleProfile GlyphSynthesizer::extractProfile(const GlyphAtlas& atlas)
{
    FontStyleProfile p;

    auto cps = atlas.codepoints();
    if (cps.empty())
        return p;

    std::vector<float> stemWidths;
    std::vector<float> densities;
    std::vector<float> slants;
    std::vector<float> serifExtents;
    float totalW = 0.0f, totalH = 0.0f;
    float totalAspect = 0.0f;

    for (char32_t cp : cps) {
        QImage g = atlas.glyph(cp);
        if (g.isNull() || g.width() < 2 || g.height() < 4)
            continue;

        float sw = measureStemWidth(g);
        if (sw > 0.0f)
            stemWidths.push_back(sw);

        densities.push_back(measureInkDensity(g));
        slants.push_back(measureSlant(g));
        serifExtents.push_back(measureSerifExtent(g));

        totalW += static_cast<float>(g.width());
        totalH += static_cast<float>(g.height());
        totalAspect += static_cast<float>(g.width()) / static_cast<float>(g.height());

        ++p.sampleCount;
    }

    if (p.sampleCount == 0)
        return p;

    // Median helper (averages two middle elements for even-sized vectors)
    auto median = [](std::vector<float>& v) -> float {
        if (v.empty()) return 0.0f;
        std::sort(v.begin(), v.end());
        size_t mid = v.size() / 2;
        if (v.size() % 2 == 0)
            return (v[mid - 1] + v[mid]) * 0.5f;
        return v[mid];
    };

    p.avgStemWidth = median(stemWidths);
    p.avgStrokeWeight = (totalH > 0.0f && !stemWidths.empty())
        ? p.avgStemWidth / (totalH / p.sampleCount) : 0.0f;
    p.avgInkDensity = std::accumulate(densities.begin(), densities.end(), 0.0f) / p.sampleCount;
    p.avgCharWidth = totalW / p.sampleCount;
    p.avgCharHeight = totalH / p.sampleCount;
    p.avgAspectRatio = totalAspect / p.sampleCount;
    p.serifExtent = median(serifExtents);
    p.slantAngle = median(slants);

    return p;
}

// ---------------------------------------------------------------------------
// Phase B: Letter Decomposition Knowledge Base
// ---------------------------------------------------------------------------

using Transform = GlyphRecipe::Strategy::Transform;

// Helper to build a simple recipe with multiple strategies
static GlyphRecipe makeRecipe(char32_t target,
    std::initializer_list<std::tuple<char32_t, Transform, float>> strats)
{
    GlyphRecipe r;
    r.target = target;
    for (auto& [donor, xform, quality] : strats) {
        GlyphRecipe::Strategy s;
        s.donor = donor;
        s.transform = xform;
        s.expectedQuality = quality;
        r.strategies.push_back(std::move(s));
    }
    return r;
}

// Helper to build a composite recipe with optional non-composite fallbacks
static GlyphRecipe makeCompositeRecipe(char32_t target, float quality,
    std::initializer_list<GlyphRecipe::Strategy::Part> compositeParts,
    std::initializer_list<std::tuple<char32_t, Transform, float>> fallbacks = {})
{
    GlyphRecipe r;
    r.target = target;
    GlyphRecipe::Strategy s;
    s.transform = Transform::Composite;
    s.expectedQuality = quality;
    s.parts = compositeParts;
    r.strategies.push_back(std::move(s));
    for (auto& [donor, xform, q] : fallbacks) {
        GlyphRecipe::Strategy fb;
        fb.donor = donor;
        fb.transform = xform;
        fb.expectedQuality = q;
        r.strategies.push_back(std::move(fb));
    }
    return r;
}

static const std::vector<GlyphRecipe>& knowledgeBase()
{
    static const std::vector<GlyphRecipe> kb = {
        // Tier 1: Mirror/Rotation (0.80-0.90)
        makeRecipe(U'b', {{U'd', Transform::HFlip, 0.85f},
                          {U'p', Transform::VFlip, 0.70f}}),
        makeRecipe(U'd', {{U'b', Transform::HFlip, 0.85f},
                          {U'q', Transform::VFlip, 0.70f}}),
        makeRecipe(U'p', {{U'q', Transform::HFlip, 0.80f},
                          {U'b', Transform::VFlip, 0.75f},
                          {U'P', Transform::ScaleToXHeight, 0.55f}}),
        makeRecipe(U'q', {{U'p', Transform::HFlip, 0.80f},
                          {U'd', Transform::VFlip, 0.75f},
                          {U'Q', Transform::ScaleToXHeight, 0.50f}}),
        makeRecipe(U'u', {{U'n', Transform::VFlip, 0.80f},
                          {U'U', Transform::ScaleToXHeight, 0.70f}}),
        makeRecipe(U'n', {{U'u', Transform::VFlip, 0.80f}}),
        makeRecipe(U'6', {{U'9', Transform::Rotate180, 0.85f}}),
        makeRecipe(U'9', {{U'6', Transform::Rotate180, 0.85f}}),

        // Tier 2: Case Scaling -- lowercase from uppercase (visually similar only)
        // Only include pairs where upper and lowercase forms are geometrically
        // similar (just size difference). Structurally different pairs like
        // a↔A, e↔E, r↔R, g↔G are omitted — FreeType fallback renders the
        // correct shape rather than producing a wrong-shaped scaled glyph.
        makeRecipe(U'o', {{U'O', Transform::ScaleToXHeight, 0.80f},
                          {U'0', Transform::ScaleToXHeight, 0.70f}}),
        makeRecipe(U'c', {{U'C', Transform::ScaleToXHeight, 0.80f}}),
        makeRecipe(U's', {{U'S', Transform::ScaleToXHeight, 0.75f}}),
        makeRecipe(U'v', {{U'V', Transform::ScaleToXHeight, 0.75f}}),
        makeRecipe(U'w', {{U'W', Transform::ScaleToXHeight, 0.70f},
                          {U'm', Transform::VFlip, 0.45f}}),
        makeRecipe(U'x', {{U'X', Transform::ScaleToXHeight, 0.75f}}),
        makeRecipe(U'z', {{U'Z', Transform::ScaleToXHeight, 0.75f}}),
        makeRecipe(U'k', {{U'K', Transform::ScaleToXHeight, 0.70f}}),
        makeRecipe(U'i', {{U'l', Transform::Identity, 0.45f},
                          {U'1', Transform::Identity, 0.40f}}),
        makeRecipe(U'g', {{U'q', Transform::Identity, 0.45f}}),
        makeRecipe(U'h', {{U'n', Transform::Identity, 0.50f}}),
        makeRecipe(U'j', {{U'i', Transform::Identity, 0.40f}}),
        makeRecipe(U'm', {{U'n', Transform::Identity, 0.40f}}),

        // Tier 2: Case Scaling -- uppercase from lowercase (visually similar only)
        makeRecipe(U'O', {{U'o', Transform::ScaleToCapHeight, 0.65f},
                          {U'0', Transform::Identity, 0.60f}}),
        makeRecipe(U'C', {{U'c', Transform::ScaleToCapHeight, 0.65f}}),
        makeRecipe(U'S', {{U's', Transform::ScaleToCapHeight, 0.60f}}),
        makeRecipe(U'V', {{U'v', Transform::ScaleToCapHeight, 0.60f}}),
        makeRecipe(U'W', {{U'w', Transform::ScaleToCapHeight, 0.55f},
                          {U'M', Transform::VFlip, 0.50f}}),
        makeRecipe(U'M', {{U'W', Transform::VFlip, 0.55f}}),
        makeRecipe(U'U', {{U'u', Transform::ScaleToCapHeight, 0.60f}}),
        makeRecipe(U'X', {{U'x', Transform::ScaleToCapHeight, 0.60f}}),
        // 'A' from VFlip V + crossbar from '-' at ~40% height.
        // Fallback: just VFlip V (no crossbar) at lower quality.
        // Part fields: donor, srcY0, srcY1, dstY0, dstY1, srcX0, srcX1, dstX0, dstX1, partTransform
        makeCompositeRecipe(U'A', 0.55f, {
            {U'V', 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, Transform::VFlip},
            {U'-', 0.0f, 1.0f, 0.35f, 0.50f, 0.0f, 1.0f, 0.15f, 0.85f, Transform::Identity}
        }, {{U'V', Transform::VFlip, 0.40f}}),
        // 'B' from stem ('l'/'I') on left + '8' body on right.
        // Fallback: just '8' alone (no stem) at lower quality.
        makeCompositeRecipe(U'B', 0.50f, {
            {U'l', 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.15f, Transform::Identity},
            {U'8', 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.10f, 1.0f, Transform::Identity}
        }, {{U'8', Transform::Identity, 0.40f}}),
        // H←h, J←j, P←p removed: structurally different (arch vs crossbar,
        // descender+dot vs hook, descender vs stem+bowl). FreeType fallback
        // renders the correct shape. G←C removed: G has horizontal tongue
        // that C lacks.
        // K←k removed: lowercase 'k' has a different arm structure (angled
        // meeting point vs. diagonal arms). FreeType fallback renders correctly.
        // Y←y removed: lowercase 'y' has a descender tail that uppercase 'Y'
        // lacks; scaling up produces wrong shape. V is a better donor (same
        // upper half, just missing the stem junction).
        makeRecipe(U'Y', {{U'V', Transform::Identity, 0.45f}}),
        makeRecipe(U'Q', {{U'O', Transform::Identity, 0.50f},
                          {U'0', Transform::Identity, 0.45f}}),
        makeRecipe(U'Z', {{U'z', Transform::ScaleToCapHeight, 0.60f}}),

        // Tier 3: Shape Similarity
        makeRecipe(U'0', {{U'O', Transform::Identity, 0.65f},
                          {U'o', Transform::ScaleToCapHeight, 0.55f}}),
        makeRecipe(U'l', {{U'I', Transform::Identity, 0.60f},
                          {U'1', Transform::Identity, 0.55f}}),
        makeRecipe(U'I', {{U'l', Transform::Identity, 0.60f},
                          {U'1', Transform::Identity, 0.55f}}),
        makeRecipe(U'1', {{U'l', Transform::Identity, 0.55f},
                          {U'I', Transform::Identity, 0.55f}}),
        makeRecipe(U'!', {{U'l', Transform::Identity, 0.40f},
                          {U'I', Transform::Identity, 0.40f}}),
        makeRecipe(U'.', {{U',', Transform::Identity, 0.50f}}),
        makeRecipe(U',', {{U'.', Transform::Identity, 0.50f},
                          {U';', Transform::Identity, 0.40f}}),
        makeRecipe(U':', {{U';', Transform::Identity, 0.50f},
                          {U'.', Transform::Identity, 0.40f}}),
        makeRecipe(U';', {{U':', Transform::Identity, 0.50f},
                          {U',', Transform::Identity, 0.40f}}),
        // '?' has no good synthesis donor — FreeType fallback preferred
        makeRecipe(U'-', {{U'_', Transform::Identity, 0.50f}}),
        makeRecipe(U'_', {{U'-', Transform::Identity, 0.50f}}),

        // Tier 3: Digit shape similarity
        // '2' has no good synthesis donor — FreeType fallback preferred
        // '3' has no good synthesis donor — FreeType fallback preferred
        // '4' has no good synthesis donor — FreeType fallback preferred
        makeRecipe(U'5', {{U'S', Transform::Identity, 0.45f},
                          {U's', Transform::ScaleToCapHeight, 0.40f}}),
        // '7' has no good synthesis donor — FreeType fallback preferred
        // '8' has no good synthesis donor — FreeType fallback preferred

        // Tier 4: Bracket/paren pairs
        makeRecipe(U'(', {{U')', Transform::HFlip, 0.80f}}),
        makeRecipe(U')', {{U'(', Transform::HFlip, 0.80f}}),
        makeRecipe(U'[', {{U']', Transform::HFlip, 0.85f}}),
        makeRecipe(U']', {{U'[', Transform::HFlip, 0.85f}}),
        makeRecipe(U'{', {{U'}', Transform::HFlip, 0.80f}}),
        makeRecipe(U'}', {{U'{', Transform::HFlip, 0.80f}}),
        makeRecipe(U'<', {{U'>', Transform::HFlip, 0.85f}}),
        makeRecipe(U'>', {{U'<', Transform::HFlip, 0.85f}}),
        makeRecipe(U'/', {{U'\\', Transform::HFlip, 0.80f}}),
        makeRecipe(U'\\', {{U'/', Transform::HFlip, 0.80f}}),
        makeRecipe(U'\'', {{U',', Transform::VFlip, 0.45f}}),
        // '"' has no good synthesis donor — FreeType fallback preferred
    };
    return kb;
}

const GlyphRecipe* GlyphSynthesizer::findRecipe(char32_t target)
{
    // Build lookup map on first call
    static const auto& kb = knowledgeBase();
    static const auto lookupMap = []() {
        std::unordered_map<char32_t, const GlyphRecipe*> m;
        for (const auto& r : kb)
            m[r.target] = &r;
        return m;
    }();

    auto it = lookupMap.find(target);
    return (it != lookupMap.end()) ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Phase C: Glyph Synthesis Engine
// ---------------------------------------------------------------------------

QImage GlyphSynthesizer::applyTransform(const QImage& donor,
                                          GlyphRecipe::Strategy::Transform transform)
{
    if (donor.isNull())
        return {};

    switch (transform) {
    case Transform::Identity:
        return donor.copy();

    case Transform::HFlip:
        return donor.mirrored(true, false);

    case Transform::VFlip:
        return donor.mirrored(false, true);

    case Transform::Rotate180:
        return donor.mirrored(true, true);

    case Transform::ScaleToXHeight: {
        // Scale down to ~70% of original height (typical x-height/cap-height ratio)
        int newH = std::max(1, static_cast<int>(donor.height() * 0.70f));
        int newW = std::max(1, static_cast<int>(donor.width() * 0.70f));
        return donor.scaled(newW, newH, Qt::KeepAspectRatio,
                            Qt::SmoothTransformation);
    }

    case Transform::ScaleToCapHeight: {
        // Scale up to ~143% of original height (inverse of x-height/cap-height ratio)
        int newH = std::max(1, static_cast<int>(donor.height() / 0.70f));
        int newW = std::max(1, static_cast<int>(donor.width() / 0.70f));
        return donor.scaled(newW, newH, Qt::KeepAspectRatio,
                            Qt::SmoothTransformation);
    }

    case Transform::Composite:
        // Composite is handled by applyComposite(), not here
        return donor.copy();

    default:
        return donor.copy();
    }
}

QImage GlyphSynthesizer::applyComposite(
    const std::vector<GlyphRecipe::Strategy::Part>& parts,
    const GlyphAtlas& atlas,
    int targetW, int targetH)
{
    if (parts.empty() || targetW <= 0 || targetH <= 0)
        return {};

    QImage result(targetW, targetH, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter painter(&result);

    for (const auto& part : parts) {
        QImage donorImg = atlas.glyph(part.donor);
        if (donorImg.isNull())
            continue;

        // Apply part transform
        QImage partImg = applyTransform(donorImg, part.partTransform);
        if (partImg.isNull())
            continue;

        // Crop source region (normalized [0,1] X and Y ranges)
        int srcX0 = static_cast<int>(part.srcX0 * partImg.width());
        int srcX1 = static_cast<int>(part.srcX1 * partImg.width());
        int srcW = std::max(1, srcX1 - srcX0);
        int srcY0 = static_cast<int>(part.srcY0 * partImg.height());
        int srcY1 = static_cast<int>(part.srcY1 * partImg.height());
        int srcH = std::max(1, srcY1 - srcY0);
        QImage cropped = partImg.copy(srcX0, srcY0, srcW, srcH);

        // Place at destination region (normalized [0,1] X and Y ranges)
        int dstX0 = static_cast<int>(part.dstX0 * targetW);
        int dstW = std::max(1, static_cast<int>((part.dstX1 - part.dstX0) * targetW));
        int dstY0 = static_cast<int>(part.dstY0 * targetH);
        int dstH = std::max(1, static_cast<int>((part.dstY1 - part.dstY0) * targetH));
        QImage scaled = cropped.scaled(dstW, dstH,
                                       Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
        painter.drawImage(dstX0, dstY0, scaled);
    }

    painter.end();
    return result;
}

QImage GlyphSynthesizer::matchStrokeWeight(const QImage& glyph,
                                             const FontStyleProfile& profile)
{
    if (glyph.isNull() || profile.avgStemWidth < 1.0f)
        return glyph;

    float currentStem = measureStemWidth(glyph);
    if (currentStem < 1.0f)
        return glyph;

    float ratio = profile.avgStemWidth / currentStem;

    // Only adjust if difference is significant (>15%)
    if (std::abs(ratio - 1.0f) < 0.15f)
        return glyph;

    // Convert to non-premultiplied for safe alpha manipulation
    QImage src = glyph.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();

    int iterations = std::clamp(static_cast<int>(std::round(std::abs(ratio - 1.0f))), 1, 2);

    for (int iter = 0; iter < iterations; ++iter) {
        QImage result(src.size(), QImage::Format_ARGB32);
        result.fill(Qt::transparent);

        if (ratio > 1.0f) {
            // Dilation: spread alpha to 4-connected neighbors
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    int maxAlpha = 0;
                    QRgb bestPixel = qRgba(0, 0, 0, 0);
                    constexpr int dx[] = {0, -1, 1, 0, 0};
                    constexpr int dy[] = {0, 0, 0, -1, 1};
                    for (int d = 0; d < 5; ++d) {
                        int nx = x + dx[d], ny = y + dy[d];
                        if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                            QRgb p = src.pixel(nx, ny);
                            if (qAlpha(p) > maxAlpha) {
                                maxAlpha = qAlpha(p);
                                bestPixel = p;
                            }
                        }
                    }
                    if (maxAlpha > 0)
                        result.setPixel(x, y, bestPixel);
                }
            }
        } else {
            // Erosion: keep pixel only if all 4 neighbors have alpha > 0
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (qAlpha(src.pixel(x, y)) == 0)
                        continue;
                    bool allFg = true;
                    constexpr int dx[] = {-1, 1, 0, 0};
                    constexpr int dy[] = {0, 0, -1, 1};
                    for (int d = 0; d < 4; ++d) {
                        int nx = x + dx[d], ny = y + dy[d];
                        if (nx < 0 || nx >= w || ny < 0 || ny >= h ||
                            qAlpha(src.pixel(nx, ny)) == 0) {
                            allFg = false;
                            break;
                        }
                    }
                    if (allFg)
                        result.setPixel(x, y, src.pixel(x, y));
                }
            }
        }

        src = result;
    }

    // Convert back to premultiplied
    return src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

float GlyphSynthesizer::scoreQuality(const QImage& synth,
                                       const FontStyleProfile& profile)
{
    if (synth.isNull() || profile.sampleCount == 0)
        return 0.0f;

    float synthStem = measureStemWidth(synth);
    float synthDensity = measureInkDensity(synth);
    float synthAspect = synth.width() > 0 && synth.height() > 0
        ? static_cast<float>(synth.width()) / synth.height() : 0.0f;

    auto matchScore = [](float actual, float expected) -> float {
        if (expected < 0.001f)
            return actual < 0.001f ? 1.0f : 0.0f;
        return std::clamp(1.0f - std::abs(actual - expected) / expected, 0.0f, 1.0f);
    };

    float stemMatch = matchScore(synthStem, profile.avgStemWidth);
    float densityMatch = matchScore(synthDensity, profile.avgInkDensity);
    float aspectMatch = matchScore(synthAspect, profile.avgAspectRatio);

    return 0.4f * stemMatch + 0.3f * densityMatch + 0.3f * aspectMatch;
}

// ---------------------------------------------------------------------------
// Public synthesis API
// ---------------------------------------------------------------------------

SynthesisResult GlyphSynthesizer::synthesize(char32_t target,
                                               const GlyphAtlas& atlas) const
{
    SynthesisResult result;

    const GlyphRecipe* recipe = findRecipe(target);
    if (!recipe)
        return result;

    for (const auto& strategy : recipe->strategies) {
        // For composite, check all parts
        if (strategy.transform == Transform::Composite) {
            const bool anyMissing = std::any_of(strategy.parts.begin(),
                strategy.parts.end(),
                [&atlas](const auto& part) { return !atlas.contains(part.donor); });
            if (anyMissing)
                continue;

            // Determine target dimensions from profile
            int targetW = static_cast<int>(profile_.avgCharWidth);
            int targetH = static_cast<int>(profile_.avgCharHeight);
            if (targetW < 1) targetW = 15;
            if (targetH < 1) targetH = 20;

            QImage composited = applyComposite(strategy.parts, atlas, targetW, targetH);
            if (composited.isNull())
                continue;

            // Apply stroke weight matching
            QImage matched = profileBuilt_
                ? matchStrokeWeight(composited, profile_) : composited;

            float quality = profileBuilt_
                ? scoreQuality(matched, profile_) : strategy.expectedQuality;

            if (quality >= kMinQualityThreshold) {
                result.success = true;
                result.image = std::move(matched);
                result.quality = quality;
                result.donorUsed = strategy.parts.empty() ? 0 : strategy.parts[0].donor;
                return result;
            }
            continue;
        }

        // Non-composite: check if donor is in atlas
        if (!atlas.contains(strategy.donor))
            continue;

        QImage donor = atlas.glyph(strategy.donor);
        if (donor.isNull())
            continue;

        // Apply geometric transform
        QImage transformed = applyTransform(donor, strategy.transform);
        if (transformed.isNull())
            continue;

        // Apply stroke weight matching
        QImage matched = profileBuilt_
            ? matchStrokeWeight(transformed, profile_) : transformed;

        // Score quality
        float quality = profileBuilt_
            ? scoreQuality(matched, profile_) : strategy.expectedQuality;

        if (quality >= kMinQualityThreshold) {
            result.success = true;
            result.image = std::move(matched);
            result.quality = quality;
            result.donorUsed = strategy.donor;
            return result;
        }
    }

    return result;
}

std::unordered_map<char32_t, QImage> GlyphSynthesizer::synthesizeAll(
    const std::u32string& text,
    const GlyphAtlas& atlas) const
{
    std::unordered_map<char32_t, QImage> results;

    // Collect unique missing characters
    std::set<char32_t> missing;
    for (char32_t c : text) {
        if (c == U' ' || c == U'\t' || c == U'\n' || c == U'\r')
            continue;
        if (!atlas.contains(c) && missing.find(c) == missing.end())
            missing.insert(c);
    }

    for (char32_t c : missing) {
        auto result = synthesize(c, atlas);
        if (result.success && !result.image.isNull())
            results[c] = std::move(result.image);
    }

    return results;
}

} // namespace pdfe::tre
