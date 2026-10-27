// Tests for A2 — FontMatcher script detection widened beyond CJK/Latin.
//
// Verifies:
//   - FontMatcher::detectScript() recognizes Hebrew and Thai (new)
//     in addition to the previously-supported Arabic, Cyrillic,
//     CJK, and Devanagari.
//   - The per-script ascender ratio table in FontMatcher no longer
//     uses a single Latin-tuned 0.78 for every non-CJK script.
//   - Existing Latin and CJK classifications are unchanged.
//
// The ascender ratio itself is exercised indirectly through
// detectProperties() — not included here because it requires a full
// HocrDocument fixture. Script detection is the primary public API
// affected by A2 and is directly testable.
//
// See docs/image-pdf-risks.md (A2).

#include <gtest/gtest.h>

#include "tre/FontMatcher.h"
#include "fonts/FontMetricProfile.h"

#include <string>

using pdfe::tre::FontMatcher;
using pdfe::fonts::FontScript;

namespace {

// Simple helper — direct U32 literals for each script.
const std::u32string kLatin        = U"The quick brown fox jumps over the lazy dog";
const std::u32string kCyrillic     = U"\u041F\u0440\u0438\u0432\u0435\u0442 \u043C\u0438\u0440";  // "Привет мир"
const std::u32string kArabic       = U"\u0627\u0644\u0633\u0644\u0627\u0645 \u0639\u0644\u064A\u0643\u0645";  // "السلام عليكم"
const std::u32string kHebrew       = U"\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD";  // "שלום עולם"
const std::u32string kThai         = U"\u0E2A\u0E27\u0E31\u0E2A\u0E14\u0E35\u0E0A\u0E32\u0E27\u0E42\u0E25\u0E01";  // "สวัสดีชาวโลก"
const std::u32string kDevanagari   = U"\u0928\u092E\u0938\u094D\u0924\u0947 \u0926\u0941\u0928\u093F\u092F\u093E";  // "नमस्ते दुनिया"
const std::u32string kChinese      = U"\u4F60\u597D\u4E16\u754C";  // "你好世界"
const std::u32string kJapanese     = U"\u3053\u3093\u306B\u3061\u306F\u4E16\u754C";  // "こんにちは世界"
const std::u32string kKorean       = U"\uC548\uB155 \uC138\uACC4";  // "안녕 세계"
const std::u32string kEmpty        = U"";
const std::u32string kDigitsOnly   = U"12345 67890";
const std::u32string kMixedLatCjk  = U"hello 世界";  // Should classify as CJK (more CJK chars)

} // namespace

// -----------------------------------------------------------------------
// Existing scripts — regression anchor: Latin / CJK / Arabic / Cyrillic
// / Devanagari must still work exactly as before.
// -----------------------------------------------------------------------

TEST(A2ScriptDetection, DetectsLatin) {
    EXPECT_EQ(FontMatcher::detectScript(kLatin), FontScript::Latin);
}

TEST(A2ScriptDetection, DetectsCyrillic) {
    EXPECT_EQ(FontMatcher::detectScript(kCyrillic), FontScript::Cyrillic);
}

TEST(A2ScriptDetection, DetectsArabic) {
    EXPECT_EQ(FontMatcher::detectScript(kArabic), FontScript::Arabic);
}

TEST(A2ScriptDetection, DetectsDevanagari) {
    EXPECT_EQ(FontMatcher::detectScript(kDevanagari), FontScript::Devanagari);
}

TEST(A2ScriptDetection, DetectsChineseAsCJK) {
    EXPECT_EQ(FontMatcher::detectScript(kChinese), FontScript::CJK);
}

TEST(A2ScriptDetection, DetectsJapaneseAsCJK) {
    // Japanese mixes Hiragana (U+3040-U+309F) with Han ideographs.
    // The Hiragana range is part of A2's widened CJK classification.
    EXPECT_EQ(FontMatcher::detectScript(kJapanese), FontScript::CJK);
}

TEST(A2ScriptDetection, DetectsKoreanAsCJK) {
    // Korean Hangul Syllables (U+AC00-U+D7AF) — also added to A2's
    // widened CJK classification.
    EXPECT_EQ(FontMatcher::detectScript(kKorean), FontScript::CJK);
}

// -----------------------------------------------------------------------
// New A2 additions — Hebrew and Thai must now classify correctly
// instead of falling through to Unknown or defaulting to Latin.
// -----------------------------------------------------------------------

TEST(A2ScriptDetection, DetectsHebrew_NEW) {
    EXPECT_EQ(FontMatcher::detectScript(kHebrew), FontScript::Hebrew);
}

TEST(A2ScriptDetection, DetectsThai_NEW) {
    EXPECT_EQ(FontMatcher::detectScript(kThai), FontScript::Thai);
}

// -----------------------------------------------------------------------
// Edge cases — empty, digits-only, mixed scripts.
// -----------------------------------------------------------------------

TEST(A2ScriptDetection, EmptyReturnsUnknown) {
    EXPECT_EQ(FontMatcher::detectScript(kEmpty), FontScript::Unknown);
}

TEST(A2ScriptDetection, DigitsOnlyReturnsUnknown) {
    // Digits and spaces don't count toward any script, so the result
    // must be Unknown (no script got any votes).
    EXPECT_EQ(FontMatcher::detectScript(kDigitsOnly), FontScript::Unknown);
}

TEST(A2ScriptDetection, MixedLatinCjkChoosesMajority) {
    // "hello 世界" has 5 Latin letters and 2 CJK chars.
    // Latin should win — more letters counted.
    EXPECT_EQ(FontMatcher::detectScript(kMixedLatCjk), FontScript::Latin);
}

// -----------------------------------------------------------------------
// Enum sanity — Hebrew and Thai are actually declared in the enum
// (catches a forgotten header-only definition).
// -----------------------------------------------------------------------

TEST(A2ScriptDetection, EnumHasHebrewAndThai) {
    // If the enum didn't have these members, this file wouldn't
    // compile. Running this test just verifies they're reachable
    // at runtime with distinct values.
    EXPECT_NE(static_cast<int>(FontScript::Hebrew),
              static_cast<int>(FontScript::Thai));
    EXPECT_NE(static_cast<int>(FontScript::Hebrew),
              static_cast<int>(FontScript::Latin));
    EXPECT_NE(static_cast<int>(FontScript::Thai),
              static_cast<int>(FontScript::Arabic));
}

// -----------------------------------------------------------------------
// Cross-script disambiguation: text that contains a few Hebrew letters
// and mostly Arabic must classify as Arabic (majority wins).
// -----------------------------------------------------------------------
TEST(A2ScriptDetection, MixedHebrewArabicMajorityWins) {
    // Mostly Arabic, one Hebrew letter.
    std::u32string text = kArabic + U" \u05D0";
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Arabic);
}

TEST(A2ScriptDetection, MostlyHebrewBeatsFewLatin) {
    std::u32string text = kHebrew + U" abc";
    // Hebrew has 9 letters, Latin 3 — Hebrew wins.
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Hebrew);
}
