#include <gtest/gtest.h>
#include "fonts/FontMetricProfile.h"
#include <cmath>
#include <cstring>

using namespace pdfe::fonts;

class FontMetricProfileTest : public ::testing::Test {
protected:
    FontMetricProfile makeProfile(const std::string& name, int glyphs = 100)
    {
        FontMetricProfile p;
        p.postScriptName = name;
        p.familyName = name;
        p.glyphCount = glyphs;
        p.weightClass = 400;
        p.widthClass = 5;
        p.stemWeightV = 0.08f;
        p.stemWeightH = 0.06f;
        p.xHeight = 0.5f;
        p.capHeight = 0.7f;
        p.xToCapRatio = 0.714f;
        p.ascToCapRatio = 1.1f;
        p.avgCharWidth = 0.5f;
        p.isSerif = false;
        p.isMonospace = false;
        p.isItalic = false;
        p.italicAngle = 0.0f;
        p.hasLatinBasic = true;
        return p;
    }
};

TEST_F(FontMetricProfileTest, DefaultConstructionIsInvalid) {
    FontMetricProfile p;
    EXPECT_FALSE(p.isValid());
    EXPECT_TRUE(p.postScriptName.empty());
    EXPECT_EQ(p.glyphCount, 0);
}

TEST_F(FontMetricProfileTest, IsValidRequiresNameAndGlyphs) {
    FontMetricProfile p;

    // Name only -- invalid
    p.postScriptName = "TestFont";
    EXPECT_FALSE(p.isValid());

    // Glyphs only -- invalid
    p.postScriptName.clear();
    p.glyphCount = 100;
    EXPECT_FALSE(p.isValid());

    // Both -- valid
    p.postScriptName = "TestFont";
    EXPECT_TRUE(p.isValid());
}

TEST_F(FontMetricProfileTest, DistanceToIdenticalIsZero) {
    auto p = makeProfile("Arial");
    float dist = p.distanceTo(p);
    EXPECT_FLOAT_EQ(dist, 0.0f);
}

TEST_F(FontMetricProfileTest, DistanceToSerifVsSansIncludesPenalty) {
    auto sans = makeProfile("Arial");
    sans.isSerif = false;

    auto serif = makeProfile("Times");
    serif.isSerif = true;

    float dist = sans.distanceTo(serif);
    // Should include the 15.0 serif penalty (sqrt includes this)
    EXPECT_GT(dist, 0.0f);

    // Without serif difference, distance should be smaller
    serif.isSerif = false;
    float distSame = sans.distanceTo(serif);
    EXPECT_LT(distSame, dist);
}

TEST_F(FontMetricProfileTest, DistanceToDifferentStemWeights) {
    auto thin = makeProfile("Thin");
    thin.stemWeightV = 0.04f;

    auto bold = makeProfile("Bold");
    bold.stemWeightV = 0.12f;

    float dist = thin.distanceTo(bold);
    EXPECT_GT(dist, 0.0f);

    // Closer stem weights should yield smaller distance
    auto medium = makeProfile("Medium");
    medium.stemWeightV = 0.06f;

    float distCloser = thin.distanceTo(medium);
    EXPECT_LT(distCloser, dist);
}

TEST_F(FontMetricProfileTest, DistanceToDifferentWeightClasses) {
    auto normal = makeProfile("Normal");
    normal.weightClass = 400;

    auto bold = makeProfile("Bold");
    bold.weightClass = 700;

    float dist = normal.distanceTo(bold);
    EXPECT_GT(dist, 0.0f);

    // Closer weight class should yield smaller distance
    auto medium = makeProfile("Medium");
    medium.weightClass = 500;

    float distCloser = normal.distanceTo(medium);
    EXPECT_LT(distCloser, dist);
}

TEST_F(FontMetricProfileTest, DistanceToItalicMismatch) {
    auto regular = makeProfile("Regular");
    regular.italicAngle = 0.0f;

    auto italic = makeProfile("Italic");
    italic.italicAngle = -12.0f;

    float dist = regular.distanceTo(italic);
    EXPECT_GT(dist, 0.0f);
}

// Italic mismatch penalty should be heavy enough to outweigh good metric scores.
// An italic font with perfect stem/ratio should still lose to an upright font
// with moderate metric differences. Penalty = 30.0 > serif penalty (15.0).
TEST_F(FontMetricProfileTest, ItalicPenaltyOutweighsGoodMetrics) {
    auto query = makeProfile("Query");
    query.italicAngle = 0.0f;
    query.stemWeightV = 0.10f;
    query.xToCapRatio = 0.50f;

    // Italic font with perfect metrics
    auto italicPerfect = makeProfile("ItalicPerfect");
    italicPerfect.italicAngle = -12.0f;
    italicPerfect.stemWeightV = 0.10f;  // Perfect match
    italicPerfect.xToCapRatio = 0.50f;  // Perfect match

    // Upright font with moderate metric differences
    auto uprightModerate = makeProfile("UprightModerate");
    uprightModerate.italicAngle = 0.0f;
    uprightModerate.stemWeightV = 0.14f;  // Off by 0.04
    uprightModerate.xToCapRatio = 0.55f;  // Off by 0.05

    float italicDist = query.distanceTo(italicPerfect);
    float uprightDist = query.distanceTo(uprightModerate);

    EXPECT_LT(uprightDist, italicDist)
        << "Upright font with moderate metrics should beat italic font with perfect metrics";
}

// Italic penalty should be heavier than serif penalty
TEST_F(FontMetricProfileTest, ItalicPenaltyHeavierThanSerif) {
    auto base = makeProfile("Base");
    base.italicAngle = 0.0f;
    base.isSerif = false;

    auto italicMismatch = makeProfile("ItalicOnly");
    italicMismatch.italicAngle = -12.0f;
    italicMismatch.isSerif = false;

    auto serifMismatch = makeProfile("SerifOnly");
    serifMismatch.italicAngle = 0.0f;
    serifMismatch.isSerif = true;

    float italicDist = base.distanceTo(italicMismatch);
    float serifDist = base.distanceTo(serifMismatch);

    EXPECT_GT(italicDist, serifDist)
        << "Italic mismatch (30.0) should produce larger distance than serif mismatch (15.0)";
}

TEST_F(FontMetricProfileTest, DistanceToMonospaceMismatch) {
    auto proportional = makeProfile("Proportional");
    proportional.isMonospace = false;

    auto mono = makeProfile("Mono");
    mono.isMonospace = true;

    float dist = proportional.distanceTo(mono);
    EXPECT_GT(dist, 0.0f);
}

// ---------------------------------------------------------------------------
// PANOSE distance tests
// ---------------------------------------------------------------------------

TEST_F(FontMetricProfileTest, PanoseIdenticalContributesZero) {
    auto a = makeProfile("Arial");
    auto b = makeProfile("Arial");
    // Set identical PANOSE
    uint8_t panose[10] = {2, 11, 6, 3, 5, 4, 2, 2, 2, 4};
    std::memcpy(a.panose, panose, 10);
    std::memcpy(b.panose, panose, 10);
    float dist = a.distanceTo(b);
    EXPECT_FLOAT_EQ(dist, 0.0f);
}

TEST_F(FontMetricProfileTest, PanoseDifferentFamilyKindNoPenalty) {
    auto a = makeProfile("FontA");
    auto b = makeProfile("FontB");
    // familyKind differs: 2 (Latin Text) vs 3 (Latin Hand)
    uint8_t panoseA[10] = {2, 11, 6, 3, 5, 4, 2, 2, 2, 4};
    uint8_t panoseB[10] = {3, 11, 6, 3, 5, 4, 2, 2, 2, 4};
    std::memcpy(a.panose, panoseA, 10);
    std::memcpy(b.panose, panoseB, 10);
    // Identical other fields, so distance should be 0 (PANOSE skipped due to familyKind mismatch)
    float dist = a.distanceTo(b);
    EXPECT_FLOAT_EQ(dist, 0.0f);
}

TEST_F(FontMetricProfileTest, PanoseThreeDiffer) {
    auto a = makeProfile("FontA");
    auto b = makeProfile("FontB");
    // Same familyKind, 3 of 9 bytes differ
    uint8_t panoseA[10] = {2, 11, 6, 3, 5, 4, 2, 2, 2, 4};
    uint8_t panoseB[10] = {2, 11, 8, 3, 7, 4, 2, 2, 9, 4};  // bytes 2, 4, 8 differ
    std::memcpy(a.panose, panoseA, 10);
    std::memcpy(b.panose, panoseB, 10);
    float dist = a.distanceTo(b);
    // Expected PANOSE contribution: 12.0 * 3/9 = 4.0, then sqrt(4.0) = 2.0
    EXPECT_NEAR(dist, 2.0f, 0.01f);
}

TEST_F(FontMetricProfileTest, PanoseZeroBytesSkipped) {
    auto a = makeProfile("FontA");
    auto b = makeProfile("FontB");
    // Same familyKind, but some bytes are 0 (missing data)
    uint8_t panoseA[10] = {2, 11, 0, 3, 0, 4, 0, 0, 2, 4};
    uint8_t panoseB[10] = {2, 11, 8, 3, 7, 4, 9, 1, 2, 4};
    std::memcpy(a.panose, panoseA, 10);
    std::memcpy(b.panose, panoseB, 10);
    // Bytes 2,4,6,7 in A are 0 → skipped. Only compare bytes 1,3,5,8,9
    // Bytes 1: 11==11, 3: 3==3, 5: 4==4, 8: 2==2, 9: 4==4 → 0 mismatches
    float dist = a.distanceTo(b);
    EXPECT_FLOAT_EQ(dist, 0.0f);
}

TEST_F(FontMetricProfileTest, PanoseAllNineDifferMaxPenalty) {
    auto a = makeProfile("FontA");
    auto b = makeProfile("FontB");
    // Same familyKind, all 9 remaining bytes differ
    uint8_t panoseA[10] = {2, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    uint8_t panoseB[10] = {2, 9, 8, 7, 6, 5, 4, 3, 2, 1};  // bytes 1-4,6-9 differ, 5 same
    // Actually make ALL 9 differ:
    uint8_t panoseB2[10] = {2, 10, 10, 10, 10, 10, 10, 10, 10, 10};
    std::memcpy(a.panose, panoseA, 10);
    std::memcpy(b.panose, panoseB2, 10);
    float dist = a.distanceTo(b);
    // Expected: sqrt(12.0 * 9/9) = sqrt(12.0) ≈ 3.464
    EXPECT_NEAR(dist, std::sqrt(12.0f), 0.01f);
}

TEST_F(FontMetricProfileTest, PanoseAllZeroFamilyKindNoPenalty) {
    auto a = makeProfile("FontA");
    auto b = makeProfile("FontB");
    // panose[0] = 0 in both → comparison skipped entirely
    EXPECT_EQ(a.panose[0], 0);
    EXPECT_EQ(b.panose[0], 0);
    float dist = a.distanceTo(b);
    EXPECT_FLOAT_EQ(dist, 0.0f);
}

TEST_F(FontMetricProfileTest, CoverageFlags) {
    FontMetricProfile p;
    EXPECT_FALSE(p.hasLatinBasic);
    EXPECT_FALSE(p.hasLatinExtA);
    EXPECT_FALSE(p.hasCyrillic);
    EXPECT_FALSE(p.hasCJK);

    p.hasLatinBasic = true;
    p.hasCyrillic = true;
    EXPECT_TRUE(p.hasLatinBasic);
    EXPECT_TRUE(p.hasCyrillic);
    EXPECT_FALSE(p.hasLatinExtA);
    EXPECT_FALSE(p.hasCJK);
}

// ---------------------------------------------------------------------------
// Additive penalty restructuring tests (Karen QA - zero coverage)
//
// Architecture: distanceTo() computes sqrt(weighted Euclidean distance) for
// continuous metrics, then adds categorical penalties OUTSIDE the sqrt.
// This prevents large penalties from being dampened by the sqrt (e.g., a 50pt
// penalty inside sqrt(50) = 7.07, but additive outside gives the full +8.0).
// ---------------------------------------------------------------------------

// Establish a baseline: two identical profiles have distance 0.
// This confirms makeProfile() produces a symmetric, zero-distance pair so
// subsequent tests can isolate exactly one variable at a time.
TEST_F(FontMetricProfileTest, AdditiveBaseline_IdenticalProfilesAreZero) {
    auto a = makeProfile("FontA");
    auto b = makeProfile("FontB");
    // All metric fields match; panose[0]=0 so PANOSE block is skipped.
    EXPECT_FLOAT_EQ(a.distanceTo(b), 0.0f);
    EXPECT_FLOAT_EQ(b.distanceTo(a), 0.0f);
}

// Serif mismatch must add exactly +4.0 on top of the base Euclidean distance.
// With identical continuous metrics (base = 0), the total distance must be 4.0.
TEST_F(FontMetricProfileTest, SerifMismatch_AddsExactly4Point0) {
    auto sans = makeProfile("Sans");
    sans.isSerif = false;

    auto serif = makeProfile("Serif");
    serif.isSerif = true;

    // Sanity: same-serif pair has distance 0
    EXPECT_FLOAT_EQ(sans.distanceTo(sans), 0.0f);
    EXPECT_FLOAT_EQ(serif.distanceTo(serif), 0.0f);

    // Serif mismatch: additive +4.0 applied after sqrt(0) = 0
    EXPECT_FLOAT_EQ(sans.distanceTo(serif), 4.0f);
    EXPECT_FLOAT_EQ(serif.distanceTo(sans), 4.0f);  // symmetrical
}

// The serif penalty must be exactly +4.0 regardless of what the continuous
// metrics contribute. Verify by adding a known base distance and checking
// that the penalty is still +4.0 on top (not dampened by the sqrt).
TEST_F(FontMetricProfileTest, SerifPenalty_IsAdditiveOutsideSqrt) {
    // Introduce a known stemWeightV difference: diff=0.04, weight=25
    // contribution = 25 * 0.04^2 = 0.04  → sqrt(0.04) = 0.2
    auto a = makeProfile("FontA");
    a.isSerif = false;
    a.stemWeightV = 0.08f;

    auto b = makeProfile("FontB");
    b.isSerif = true;
    b.stemWeightV = 0.12f;  // diff = 0.04

    float baseOnly = [&]{
        auto bSameSerif = b;
        bSameSerif.isSerif = false;  // remove serif mismatch
        return a.distanceTo(bSameSerif);
    }();

    float withSerifPenalty = a.distanceTo(b);

    // The difference must be exactly 4.0, not sqrt(4.0)=2.0
    EXPECT_NEAR(withSerifPenalty - baseOnly, 4.0f, 1e-5f);
}

// Script mismatch must add exactly +8.0 on top of base distance.
// Two otherwise-identical profiles differing only in primaryScript (both
// non-Unknown) must produce distance 8.0.
TEST_F(FontMetricProfileTest, ScriptMismatch_AddsExactly8Point0) {
    auto latin = makeProfile("Latin");
    latin.primaryScript = FontScript::Latin;

    auto arabic = makeProfile("Arabic");
    arabic.primaryScript = FontScript::Arabic;

    EXPECT_FLOAT_EQ(latin.distanceTo(arabic), 8.0f);
    EXPECT_FLOAT_EQ(arabic.distanceTo(latin), 8.0f);  // symmetrical
}

// Script penalty must NOT fire when either side is Unknown.
// Unknown means "we couldn't determine the script" — it should be allowed
// to match any other script to enable fallback matching.
TEST_F(FontMetricProfileTest, ScriptPenalty_NotAppliedWhenEitherIsUnknown) {
    auto unknown = makeProfile("Unknown");
    unknown.primaryScript = FontScript::Unknown;

    auto latin = makeProfile("Latin");
    latin.primaryScript = FontScript::Latin;

    auto cjk = makeProfile("CJK");
    cjk.primaryScript = FontScript::CJK;

    // Unknown vs Latin — no penalty
    EXPECT_FLOAT_EQ(unknown.distanceTo(latin), 0.0f);
    EXPECT_FLOAT_EQ(latin.distanceTo(unknown), 0.0f);

    // Unknown vs CJK — no penalty
    EXPECT_FLOAT_EQ(unknown.distanceTo(cjk), 0.0f);
    EXPECT_FLOAT_EQ(cjk.distanceTo(unknown), 0.0f);

    // Unknown vs Unknown — no penalty
    EXPECT_FLOAT_EQ(unknown.distanceTo(unknown), 0.0f);
}

// When script matches (same non-Unknown value), no penalty is applied.
TEST_F(FontMetricProfileTest, ScriptPenalty_NotAppliedWhenScriptMatches) {
    auto a = makeProfile("LatinA");
    a.primaryScript = FontScript::Latin;

    auto b = makeProfile("LatinB");
    b.primaryScript = FontScript::Latin;

    EXPECT_FLOAT_EQ(a.distanceTo(b), 0.0f);

    auto c = makeProfile("CjkA");
    c.primaryScript = FontScript::CJK;

    auto d = makeProfile("CjkB");
    d.primaryScript = FontScript::CJK;

    EXPECT_FLOAT_EQ(c.distanceTo(d), 0.0f);
}

// Both penalties must stack: serif mismatch (+4.0) and script mismatch (+8.0)
// together must produce exactly +12.0 on top of the base distance.
TEST_F(FontMetricProfileTest, SerifAndScriptPenalties_Stack) {
    // Base profile: Latin, sans-serif, identical continuous metrics
    auto a = makeProfile("LatinSans");
    a.primaryScript = FontScript::Latin;
    a.isSerif = false;

    // Profile differs in both serif and script
    auto b = makeProfile("ArabicSerif");
    b.primaryScript = FontScript::Arabic;
    b.isSerif = true;

    // Both penalties fire: sqrt(0) + 4.0 (serif) + 8.0 (script) = 12.0
    EXPECT_FLOAT_EQ(a.distanceTo(b), 12.0f);
    EXPECT_FLOAT_EQ(b.distanceTo(a), 12.0f);
}

// Verify that the script penalty is additive-outside-sqrt by introducing a
// known base distance and confirming the penalty adds exactly +8.0 on top.
TEST_F(FontMetricProfileTest, ScriptPenalty_IsAdditiveOutsideSqrt) {
    // stemWeightV diff=0.04: contribution 25*0.04^2=0.04 → sqrt(0.04)=0.2
    auto a = makeProfile("FontA");
    a.primaryScript = FontScript::Latin;
    a.stemWeightV = 0.08f;

    auto b = makeProfile("FontB");
    b.primaryScript = FontScript::Cyrillic;
    b.stemWeightV = 0.12f;  // diff = 0.04

    float baseOnly = [&]{
        auto bSameScript = b;
        bSameScript.primaryScript = FontScript::Latin;  // remove script mismatch
        return a.distanceTo(bSameScript);
    }();

    float withScriptPenalty = a.distanceTo(b);

    EXPECT_NEAR(withScriptPenalty - baseOnly, 8.0f, 1e-5f);
}

// Enumerate every non-Unknown script pair to confirm mismatch always gives +8.0.
// This guards against future accidentally-equal enum values or missing cases.
TEST_F(FontMetricProfileTest, ScriptMismatch_AllNonUnknownPairs_Give8Point0) {
    const FontScript scripts[] = {
        FontScript::Latin,
        FontScript::Arabic,
        FontScript::Cyrillic,
        FontScript::CJK,
        FontScript::Devanagari,
        FontScript::Other,
    };

    for (auto s1 : scripts) {
        for (auto s2 : scripts) {
            if (s1 == s2) continue;  // same script → no penalty

            auto a = makeProfile("A");
            a.primaryScript = s1;

            auto b = makeProfile("B");
            b.primaryScript = s2;

            float dist = a.distanceTo(b);
            EXPECT_FLOAT_EQ(dist, 8.0f)
                << "Script pair (" << static_cast<int>(s1)
                << ", " << static_cast<int>(s2) << ") should give 8.0";
        }
    }
}

// Regression: the old implementation put serif/script penalties inside the sqrt,
// which dampened them. Confirm the OLD behaviour would NOT give integer penalties.
// Under the old scheme, isSerif mismatch added 15.0 inside the sqrt:
//   sqrt(15.0) ≈ 3.873, NOT 4.0.
// This test verifies we are NOT on the old code path.
TEST_F(FontMetricProfileTest, SerifPenalty_IsNotDampenedBySqrt_OldCodeRegression) {
    auto a = makeProfile("Sans");
    a.isSerif = false;

    auto b = makeProfile("Serif");
    b.isSerif = true;

    float dist = a.distanceTo(b);

    // Old path would give sqrt(15.0) ≈ 3.873; new path gives 4.0 exactly.
    // Confirm we are clearly not equal to the old value.
    EXPECT_FLOAT_EQ(dist, 4.0f);
    EXPECT_GT(dist, 3.9f);  // not the old 3.873
}
