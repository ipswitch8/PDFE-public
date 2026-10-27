// tests/test_serif_ascender_descriptor.cpp
//
// Pipeline d4e7f921 Phase 3 — serif/sans fallback from the EmbeddedFontAnalyzer
// FontMetricProfile (PANOSE / measured serif extent) preferred over font-name
// fragments, and TextEditTool::ascenderRatioForFont derived from the metric
// profile with a per-family table tier between the measured cache and the
// generic 0.75 default.
//
// Two behaviours under test:
//
//   Serif classification (items #3): the overlay's serif/sans fallback
//   (TextEditTool::buildOverlayFont) and the new-character fallback
//   (AutoMissingGlyphResolver::resolve) now FIRST consult the profile serif
//   flag (name-INDEPENDENT), and fall back to the name-fragment heuristic only
//   when no profile is available. We prove the profile drives the decision via
//   the new buildOverlayFont profileSerif argument (forcing serif on a sans
//   name and vice-versa), prove the name fragments still work when the profile
//   is absent (profileSerif == -1), and prove the resolver path picks a serif
//   substitute for the embedded serif fixture.
//
//   Ascender ratio (item #5): ascenderRatioForFont returns, in descending
//   priority, the measured profile value (warmed from EmbeddedFontAnalyzer),
//   then the per-family table value, then 0.75. We prove all three tiers.

#include <gtest/gtest.h>

#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "tre/AutoMissingGlyphResolver.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontFamilyResolver.h"
#include "fonts/FontMetricProfile.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <QFont>
#include <QFontInfo>
#include <QRectF>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::textedit;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::tre::AutoMissingGlyphResolver;
using pdfe::fonts::EmbeddedFontAnalyzer;
using pdfe::fonts::FontMetricProfile;

namespace {

std::string fixturesDir() { return TEST_FIXTURES_DIR; }
// The embedded, subsetted, serif (PANOSE serif) bold-italic fixture used by
// test_textedit_font_identity.cpp; its generator self-validates PANOSE serif.
const char* kSerifFixture = "embedded_italic_serif_subset.pdf";

// Local serif-name heuristic, mirroring the resolver/overlay fragment lists.
// Used only to classify the RESOLVED Qt family string (platform-dependent:
// "Times New Roman"/"Arial" on Windows, "Liberation Serif"/"Liberation Sans"
// on Linux), not the code under test.
bool nameLooksSerif(const std::string& nameIn) {
    std::string n = nameIn;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (n.find("sans") != std::string::npos) return false;  // "...Sans" is sans
    static const char* frags[] = {
        "serif", "times", "roman", "georgia", "cambria", "palatino",
        "palladio", "garamond", "book antiqua", "minion", "century",
        "tinos", "nimbus roman", "liberation serif"};
    for (const char* f : frags)
        if (n.find(f) != std::string::npos) return true;
    return false;
}

// Classify a resolved .ttf/.otf path as serif via PANOSE (name fallback).
bool ttfIsSerif(const std::string& path) {
    if (path.empty() || !std::filesystem::exists(path)) return false;
    bool serif = false;
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return false;
    FT_Face face = nullptr;
    if (FT_New_Face(lib, path.c_str(), 0, &face) == 0 && face) {
        bool decided = false;
        auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
        if (os2 && os2->panose[0] == 2) {
            const unsigned char ser = os2->panose[1];
            if (ser >= 2 && ser <= 10) { serif = true;  decided = true; }
            else if (ser >= 11 && ser <= 15) { serif = false; decided = true; }
        }
        if (!decided)
            serif = nameLooksSerif(face->family_name ? face->family_name : "");
        FT_Done_Face(face);
    }
    FT_Done_FreeType(lib);
    return serif;
}

TextSpan makeSpan(const std::string& fontName, float fontSize = 20.0f) {
    TextSpan span;
    span.fontName = fontName;
    span.fontSize = fontSize;
    span.text = U"Tale";
    span.originalText = U"Tale";
    span.boundingBox = QRectF(0, 0, 120, 40);
    return span;
}

// The first non-standard-14 embedded font's postScript name on page 0.
std::string embeddedBaseFont(MuPdfContext* ctx, Document* doc) {
    if (!doc) return {};
    auto page = doc->page(0);
    if (!page) return {};
    EmbeddedFontAnalyzer ana(ctx);
    auto profiles = ana.analyzePageFonts(page.get(), doc);
    static const std::vector<std::string> std14 = {
        "Helvetica", "Helvetica-Bold", "Helvetica-Oblique",
        "Helvetica-BoldOblique", "Times-Roman", "Times-Bold",
        "Times-Italic", "Times-BoldItalic", "Courier", "Courier-Bold",
        "Courier-Oblique", "Courier-BoldOblique", "Symbol", "ZapfDingbats"};
    for (const auto& pr : profiles) {
        if (pr.postScriptName.empty()) continue;
        if (std::find(std14.begin(), std14.end(), pr.postScriptName) == std14.end())
            return pr.postScriptName;
    }
    return {};
}

// The embedded font's measured profile (page 0, first non-std14).
std::optional<FontMetricProfile> embeddedProfile(MuPdfContext* ctx, Document* doc) {
    if (!doc) return std::nullopt;
    auto page = doc->page(0);
    if (!page) return std::nullopt;
    EmbeddedFontAnalyzer ana(ctx);
    auto profiles = ana.analyzePageFonts(page.get(), doc);
    static const std::vector<std::string> std14 = {
        "Helvetica", "Helvetica-Bold", "Helvetica-Oblique",
        "Helvetica-BoldOblique", "Times-Roman", "Times-Bold",
        "Times-Italic", "Times-BoldItalic", "Courier", "Courier-Bold",
        "Courier-Oblique", "Courier-BoldOblique", "Symbol", "ZapfDingbats"};
    for (const auto& pr : profiles) {
        if (pr.postScriptName.empty()) continue;
        if (std::find(std14.begin(), std14.end(), pr.postScriptName) == std14.end())
            return pr;
    }
    return std::nullopt;
}

class SerifAscenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        auto p = std::filesystem::path(fixturesDir()) / kSerifFixture;
        if (std::filesystem::exists(p))
            doc_ = Document::open(ctx_, p);
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// (a) PANOSE serif classification → profile.isSerif == true for the serif font.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(SerifAscenderTest, ProfileClassifiesEmbeddedSerifFont_AsSerif) {
    if (!doc_) GTEST_SKIP() << kSerifFixture << " not available";
    auto prof = embeddedProfile(ctx_.get(), doc_.get());
    ASSERT_TRUE(prof.has_value()) << "embedded font profile not found on page 0";
    EXPECT_TRUE(prof->isSerif)
        << "PANOSE/serif-extent classification must report the embedded "
           "serif fixture as serif (postScript=" << prof->postScriptName << ")";
}

// (a, end-to-end) the new-character resolver picks a SERIF substitute for the
// serif source — and that decision now flows from profile.isSerif. Windows-gated
// (system-font fallback uses Windows SystemFontDiscovery, like the font-identity
// resolver tests). Serif faces ship with the Server-2019 CI image.
#ifdef _WIN32
TEST_F(SerifAscenderTest, ResolverPicksSerifFallback_ForSerifSource) {
    if (!doc_) GTEST_SKIP() << kSerifFixture << " not available";
    const std::string baseFont = embeddedBaseFont(ctx_.get(), doc_.get());
    ASSERT_FALSE(baseFont.empty()) << "embedded subset font not found";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    // 'z' is unencodable in the subset → routed to the system-font fallback.
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(), baseFont, 33.0f, U"z");
    ASSERT_GE(runs.size(), 1u) << "resolver produced no run for absent 'z'";

    bool sawSystemFont = false;
    for (const auto& r : runs) {
        if (r.fontFilePath.empty()) continue;  // Type3 path (not expected for 'z')
        sawSystemFont = true;
        EXPECT_TRUE(ttfIsSerif(r.fontFilePath))
            << "serif source must resolve to a serif substitute (profile-driven); "
            << "got sans: " << r.fontFilePath;
    }
    ASSERT_TRUE(sawSystemFont) << "no system-font run for 'z'";
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// (b) profile serif flag drives buildOverlayFont's fallback INDEPENDENT of name:
// a serif classification forces a serif family even for a sans-looking/unknown
// name; a sans classification forces sans even for a serif-looking name.
// ─────────────────────────────────────────────────────────────────────────────
TEST(SerifAscenderOverlay, ProfileSerifOverridesName_ForcesSerifFamily) {
    // Unknown family name (misresolves → fallback branch fires); profileSerif=1.
    QFont font = TextEditTool::buildOverlayFont(
        makeSpan("ZZQXNeverAFontFamily12345"), 1.0f, {}, 0.0f, 0.85f,
        /*profileSerif=*/1);
    EXPECT_TRUE(nameLooksSerif(font.family().toStdString()))
        << "profileSerif=1 must select a serif fallback family; got "
        << font.family().toStdString();
}

TEST(SerifAscenderOverlay, ProfileSansClassification_ForcesSansFamily) {
    // The SAME unresolvable name as the serif test above — only profileSerif
    // differs. profileSerif=0 must select a sans fallback. Pairing identical
    // input with opposite profileSerif proves the profile flag (not the name)
    // drives the decision. (A serif-LOOKING name that *maps* to an installed
    // family would skip the fallback branch entirely, so a neutral name is used
    // here; the name-fragment path is covered by NameFragmentFallback below.)
    QFont font = TextEditTool::buildOverlayFont(
        makeSpan("ZZQXNeverAFontFamily12345"), 1.0f, {}, 0.0f, 0.85f,
        /*profileSerif=*/0);
    EXPECT_FALSE(nameLooksSerif(font.family().toStdString()))
        << "profileSerif=0 must select a sans fallback family; got "
        << font.family().toStdString();
}

// ─────────────────────────────────────────────────────────────────────────────
// (c) name-fragment fallback is retained and used when no profile (profileSerif
// == -1): a serif name fragment selects serif, a neutral name selects sans.
// ─────────────────────────────────────────────────────────────────────────────
TEST(SerifAscenderOverlay, NameFragmentFallback_WhenNoProfile) {
    // The serif fragment must be one that NO installed Windows font contains as
    // a token, otherwise Qt fuzzy-resolves the family (looksWrong=false) and the
    // fallback branch never fires — proving nothing. "doulos" (SIL Doulos) is in
    // buildOverlayFont's serif-fragment list but ships on no default Windows
    // image, so the name genuinely misresolves and the fragment path is what
    // selects serif.
    QFont serifByName = TextEditTool::buildOverlayFont(
        makeSpan("XXXXXX+DoulosVariantX"), 1.0f, {}, 0.0f, 0.85f,
        /*profileSerif=*/-1);
    EXPECT_TRUE(nameLooksSerif(serifByName.family().toStdString()))
        << "serif name fragment must select serif when no profile is available; "
        << "got " << serifByName.family().toStdString();

    QFont sansByName = TextEditTool::buildOverlayFont(
        makeSpan("XXXXXX+ZorblatNeutralName"), 1.0f, {}, 0.0f, 0.85f,
        /*profileSerif=*/-1);
    EXPECT_FALSE(nameLooksSerif(sansByName.family().toStdString()))
        << "neutral name must select sans when no profile is available; got "
        << sansByName.family().toStdString();
}

// ─────────────────────────────────────────────────────────────────────────────
// (d) ascenderRatioForFont returns the measured profile value (within 0.02) for
// a font in the warmed EmbeddedFontAnalyzer cache.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(SerifAscenderTest, AscenderRatio_ProfileDerived_WithinTolerance) {
    if (!doc_) GTEST_SKIP() << kSerifFixture << " not available";
    const std::string baseFont = embeddedBaseFont(ctx_.get(), doc_.get());
    ASSERT_FALSE(baseFont.empty());
    auto prof = embeddedProfile(ctx_.get(), doc_.get());
    ASSERT_TRUE(prof.has_value());

    // Expected = the same value warmFontCacheForPage caches: profile.ascender
    // (with the synthetic fallback only when ascender<=0), clamped to [0.55,0.98].
    float expected = prof->ascender;
    if (expected <= 0.0f)
        expected = prof->capHeight > 0.0f ? (prof->capHeight + 0.10f) : 0.75f;
    expected = std::clamp(expected, 0.55f, 0.98f);

    TextEditTool tool(doc_.get(), ctx_);
    tool.setPage(0);  // warms the per-font caches for page 0

    const float actual = tool.ascenderRatioForFont(baseFont);
    EXPECT_NEAR(actual, expected, 0.02f)
        << "ascenderRatioForFont must return the measured profile ascender for a "
           "cached font; baseFont=" << baseFont;
    // And it must NOT be the generic default (proves the profile tier engaged).
    EXPECT_NE(actual, 0.75f)
        << "profile-derived ratio collapsed to the 0.75 default — cache not warmed";
}

// (g) serifClassForFont reflects the cached profile serif flag after warming.
TEST_F(SerifAscenderTest, SerifClassForFont_AfterWarm_ReturnsSerif) {
    if (!doc_) GTEST_SKIP() << kSerifFixture << " not available";
    const std::string baseFont = embeddedBaseFont(ctx_.get(), doc_.get());
    ASSERT_FALSE(baseFont.empty());

    TextEditTool tool(doc_.get(), ctx_);
    tool.setPage(0);
    EXPECT_EQ(tool.serifClassForFont(baseFont), 1)
        << "serifClassForFont must report serif (1) for the cached serif fixture";
    // An unseen font is unknown (-1).
    EXPECT_EQ(tool.serifClassForFont("ZorblatUnseenFont"), -1);
}

// ─────────────────────────────────────────────────────────────────────────────
// (e) ascenderRatioForFont returns the per-family table value for a tabled font
// with NO measured profile (empty cache).
// ─────────────────────────────────────────────────────────────────────────────
TEST(SerifAscenderTable, AscenderRatio_TabledValue_WhenNoProfile) {
    TextEditTool tool(nullptr, nullptr);  // no document → empty caches

    const float arialTable = TextEditTool::tabledAscenderRatio("Arial");
    ASSERT_GT(arialTable, 0.0f) << "Arial must be in the ascender table";
    EXPECT_FLOAT_EQ(tool.ascenderRatioForFont("Arial"), arialTable);
    EXPECT_NE(tool.ascenderRatioForFont("Arial"), 0.75f)
        << "tabled font must not collapse to the generic default";

    // Subset-prefixed name still hits the family fragment.
    EXPECT_FLOAT_EQ(tool.ascenderRatioForFont("ABCDEF+Times New Roman Bold"),
                    TextEditTool::tabledAscenderRatio("Times New Roman"));
}

// ─────────────────────────────────────────────────────────────────────────────
// (f) ascenderRatioForFont returns 0.75 for a completely unknown font (no
// profile, no table entry); tabledAscenderRatio reports the negative sentinel.
// ─────────────────────────────────────────────────────────────────────────────
TEST(SerifAscenderTable, AscenderRatio_UnknownFont_DefaultsTo075) {
    TextEditTool tool(nullptr, nullptr);
    EXPECT_FLOAT_EQ(tool.ascenderRatioForFont("ZorblatNonexistentFont"), 0.75f);
    EXPECT_LT(TextEditTool::tabledAscenderRatio("ZorblatNonexistentFont"), 0.0f)
        << "an untabled name must return the negative sentinel";
    EXPECT_LT(TextEditTool::tabledAscenderRatio(""), 0.0f)
        << "empty name must return the negative sentinel";
}
