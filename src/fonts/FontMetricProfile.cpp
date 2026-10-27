#include "fonts/FontMetricProfile.h"
#include <cmath>

namespace pdfe::fonts {

bool FontMetricProfile::isValid() const
{
    return !postScriptName.empty() && glyphCount > 0;
}

bool FontMetricProfile::coversScript(FontScript s) const
{
    // Unknown is the "couldn't determine" sentinel — always allow fallback.
    if (s == FontScript::Unknown)
        return true;

    // Multi-script aware path (#8): when coverage was analyzed, membership in
    // the supported-script set is authoritative. A font that genuinely carries
    // Arabic glyph ranges covers Arabic even if Latin is its glyph-count plurality.
    if (!supportedScripts.empty())
        return supportedScripts.count(s) > 0;

    // Legacy fallback: no coverage set populated — reproduce the historical
    // single-script comparison exactly. Unknown primaryScript matched anything.
    return primaryScript == FontScript::Unknown || primaryScript == s;
}

float FontMetricProfile::distanceTo(const FontMetricProfile& other) const
{
    return distanceTo(other, FontScript::Unknown);
}

float FontMetricProfile::distanceTo(const FontMetricProfile& other,
                                    FontScript queryScriptOverride) const
{
    // Weighted Euclidean distance for font matching.
    // Higher weights on properties that most affect visual appearance.
    float distance = 0.0f;

    // Vertical stem weight: weight 25.0
    {
        float diff = stemWeightV - other.stemWeightV;
        distance += 25.0f * diff * diff;
    }

    // x-height to cap-height ratio: weight 20.0
    {
        float diff = xToCapRatio - other.xToCapRatio;
        distance += 20.0f * diff * diff;
    }

    // Weight class difference: weight 10.0 (normalized to 0-1 scale: diff/500)
    {
        float diff = static_cast<float>(weightClass - other.weightClass) / 500.0f;
        distance += 10.0f * diff * diff;
    }

    // Average character width: weight 10.0
    {
        float diff = avgCharWidth - other.avgCharWidth;
        distance += 10.0f * diff * diff;
    }

    // Italic angle mismatch: penalty 30.0
    // Heavy penalty ensures italic fonts never beat upright fonts on metrics alone.
    if ((italicAngle != 0.0f) != (other.italicAngle != 0.0f)) {
        distance += 30.0f;
    }

    // Monospace mismatch: penalty 5.0
    if (isMonospace != other.isMonospace) {
        distance += 5.0f;
    }

    // PANOSE classification: weight 12.0
    // Only compare when both have same familyKind (byte 0) and it's non-zero
    if (panose[0] == other.panose[0] && panose[0] != 0) {
        int mismatchCount = 0;
        int comparedCount = 0;
        for (int i = 1; i < 10; ++i) {
            if (panose[i] != 0 && other.panose[i] != 0) {
                ++comparedCount;
                if (panose[i] != other.panose[i])
                    ++mismatchCount;
            }
        }
        if (comparedCount > 0) {
            distance += 12.0f * static_cast<float>(mismatchCount) /
                        static_cast<float>(comparedCount);
        }
    }

    // Take the sqrt of continuous metrics, then add categorical penalties.
    // Previously serif/script penalties were inside the sqrt, which dampened
    // a 50-point script penalty to ~1.5 distance points — not enough to
    // separate Korean/Myanmar fonts from Latin candidates.
    float result = std::sqrt(distance);

    // Serif mismatch: additive penalty 4.0
    if (isSerif != other.isSerif) {
        result += 4.0f;
    }

    // Script mismatch: additive penalty 8.0.
    // The "needed" script is the per-run override (defect #7) when supplied,
    // else this profile's region-wide primaryScript (fallback hint). The penalty
    // fires only when the candidate font (`other`) does NOT cover that script —
    // a multi-script font covering the needed script incurs no penalty (#8).
    // Unknown needed-script gets no penalty (allows fallback matching).
    //
    // Backward compatibility: with empty supportedScripts on both sides and no
    // override, coversScript() reduces to the old `primaryScript != other.primaryScript`
    // comparison, so legacy behavior (and its tests) are preserved exactly.
    const FontScript needed = (queryScriptOverride != FontScript::Unknown)
                                  ? queryScriptOverride
                                  : primaryScript;
    if (needed != FontScript::Unknown && !other.coversScript(needed)) {
        result += 8.0f;
    }

    return result;
}

} // namespace pdfe::fonts
