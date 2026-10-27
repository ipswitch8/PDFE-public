#include <gtest/gtest.h>
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "fonts/FontSubstitutionEngine.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <filesystem>
#include <memory>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace pdfe::textedit;
using namespace pdfe::fonts;
using namespace pdfe::core;

// ─────────────────────────────────────────────────────────────────────────────
// RENDER / GLYPH-QUAD ORACLES for the font-SUBSTITUTION edit path
// (afterTextObjectWithSubstitutionCallback). The extraction-only oracles below
// are BLIND to mirror / colour-loss / half-size (stext reports the same
// codepoints whether the run is drawn mirrored, black, or half-scale). Phase-3
// mutation testing proved this: M04 (hScale -100 → mirror), M06 (drop fill
// colour), M07 (initialFontSize ×0.5 → half-size) all SURVIVED the
// ContentStreamSubstitutionTest suite. These helpers read the SAVED glyph quads
// + argb back through a fresh fitz context — exactly what a viewer renders — so
// those defects now fail the suite.
//
// Fixture: neg_tz_grey_text.pdf carries a grey (#dbd0d0) "SHIP TO" span under an
// ambient "-100 Tz" mirror + a "KEEP ME" neighbour at the same 20pt — giving a
// colour signal (grey vs dark ambient), a mirror signal, and a size reference.
namespace {

struct RBGlyph { char32_t c; uint32_t argb; fz_quad quad; };

struct SubstReadback {
    std::vector<RBGlyph> edited;   // glyphs of the reopened "SHOP" line
    float keepGlyphH = 0.0f;       // "KEEP ME" glyph height (points) — size ref
    bool ok = false;
};

// Edit the grey "SHIP TO" → "SHOP TO" via applyEditsWithSubstitution, save,
// reopen in an isolated fitz context, and read back the edited line's glyph
// quads + argb. `perRun` drives the per-run Tf/Tj branch (runSegments
// populated), which keeps the DEFAULT hScale (M04 mirror target); the legacy
// self-substitution branch keeps initialFontSize == inj.fontSize (M07 half-size
// target). Both share the fill-colour path (M06).
SubstReadback substEditNegTz(bool perRun) {
    SubstReadback rb;
    auto context = std::make_shared<MuPdfContext>();
    std::filesystem::path fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "neg_tz_grey_text.pdf";
    if (!std::filesystem::exists(fixture)) return rb;

    auto document = Document::open(context, fixture);
    if (!document) return rb;
    auto page = document->page(0);
    if (!page) return rb;

    auto blocks = page->extractText();
    TextEditModel model(blocks);
    std::vector<TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));
    int target = -1;
    for (int i = 0; i < static_cast<int>(spans.size()); ++i)
        if (spans[i].text.find(U"SHIP TO") != std::u32string::npos) { target = i; break; }
    if (target < 0) return rb;

    const std::u32string newText = U"SHOP TO";
    std::vector<std::pair<int, std::u32string>> mods{{target, newText}};

    SubstitutionInfo sub;
    sub.spanIndex = target;
    sub.originalFontName = spans[target].fontName;
    sub.originalFontSize = spans[target].fontSize;
    sub.horizontalScale = 1.0f;
    if (perRun) {
        // Per-run: the segment's fontResName references the span's real page font
        // so it resolves + renders; the per-run branch does NOT overwrite the
        // default hScale, so an M04 "-100" default mirrors the run.
        sub.substituteResName = "";
        RunSegment seg;
        seg.charStart = 0;
        seg.charEnd = static_cast<int>(newText.size());
        seg.fontResName = spans[target].fontName;
        seg.fontSize = spans[target].fontSize;
        sub.runSegments = { seg };
    } else {
        // Legacy self-substitution: keep the original font (hScale 1.0, upright);
        // initialFontSize stays inj.fontSize — the M07 half-size target.
        sub.substituteResName = spans[target].fontName;
    }

    ContentStreamEditor editor(context.get());
    editor.applyEditsWithSubstitution(page.get(), document.get(), spans, mods, { sub });

    std::filesystem::path out = std::filesystem::temp_directory_path() /
        (perRun ? "pdfe_subst_perrun_negtz.pdf" : "pdfe_subst_legacy_negtz.pdf");
    document->saveAs(out);
    page.reset(); document.reset(); context.reset();

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, out.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, 0, nullptr);
        for (fz_stext_block* b = stext->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
                std::u32string lineText;
                std::vector<RBGlyph> lineGlyphs;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next) {
                    lineText.push_back(static_cast<char32_t>(ch->c));
                    lineGlyphs.push_back({static_cast<char32_t>(ch->c), ch->argb, ch->quad});
                }
                if (lineText.find(U"SHOP") != std::u32string::npos)
                    rb.edited = lineGlyphs;
                if (lineText.find(U"KEEP") != std::u32string::npos &&
                    !lineGlyphs.empty()) {
                    const fz_quad& q = lineGlyphs.front().quad;
                    rb.keepGlyphH = std::fabs(q.ll.y - q.ul.y);
                }
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    rb.ok = !rb.edited.empty();
    return rb;
}

// First non-space glyph's sRGB (r,g,b), for the colour oracle.
void firstInkColor(const std::vector<RBGlyph>& gs, int& r, int& g, int& b) {
    for (const auto& gl : gs) {
        if (gl.c == U' ') continue;
        r = (gl.argb >> 16) & 0xFF; g = (gl.argb >> 8) & 0xFF; b = gl.argb & 0xFF;
        return;
    }
    const uint32_t a = gs.empty() ? 0u : gs.front().argb;
    r = (a >> 16) & 0xFF; g = (a >> 8) & 0xFF; b = a & 0xFF;
}

}  // namespace

class ContentStreamSubstitutionTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        editor_ = std::make_unique<ContentStreamEditor>(context_.get());

        std::filesystem::path testPdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(testPdf)) {
            hasTestPdf_ = false;
            return;
        }
        doc_ = Document::open(context_, testPdf);
        if (doc_ && doc_->pageCount() > 0) {
            page_ = doc_->page(0);
            hasTestPdf_ = true;
        }
    }

    TextSpan makeSpan(const std::string& fontName, float fontSize, const std::u32string& text) {
        TextSpan span;
        span.fontName = fontName;
        span.fontSize = fontSize;
        span.text = text;
        span.originalText = text;
        span.boundingBox = QRectF(0, 0, 100, 20);
        return span;
    }

    // Real spans extracted from the page, so an edit actually matches page
    // content and lands (a fabricated bbox never matches -> silent no-op, which
    // is exactly what the old EXPECT_NO_THROW-only tests failed to notice).
    std::vector<TextSpan> extractSpans() {
        auto blocks = page_->extractText();
        TextEditModel model(blocks);
        std::vector<TextSpan> spans;
        for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));
        return spans;
    }

    // Full plain text of the current page (for a no-op / unchanged oracle).
    std::string currentPageText() { return page_->extractPlainText(); }

    // Apply the edit, save, reopen in a fresh context, and return the reopened
    // page's plain text -- the user-visible result of the substitution.
    std::string applyAndReadback(
        const std::vector<TextSpan>& spans,
        const std::vector<std::pair<int, std::u32string>>& mods,
        const std::vector<SubstitutionInfo>& subs) {
        editor_->applyEditsWithSubstitution(page_.get(), doc_.get(), spans, mods, subs);
        auto out = std::filesystem::temp_directory_path() / "pdfe_subst_readback.pdf";
        doc_->saveAs(out);
        auto ctx = std::make_shared<MuPdfContext>();
        auto d = Document::open(ctx, out);
        if (!d || d->pageCount() == 0) return {};
        return d->page(0)->extractPlainText();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ContentStreamEditor> editor_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    bool hasTestPdf_ = false;
};

// The fixture page reads "Single Page Test Document". We edit a real extracted
// span and assert the substituted text is actually present in the saved output.

// Test 1: Empty substitutions delegates to applyEdits — the edit must LAND.
TEST_F(ContentStreamSubstitutionTest, EmptySubstitutionsDelegatesToApplyEdits) {
    if (!hasTestPdf_) GTEST_SKIP() << "No test PDF available";
    auto spans = extractSpans();
    ASSERT_FALSE(spans.empty()) << "fixture must yield at least one span";

    std::vector<std::pair<int, std::u32string>> mods{{0, U"REPLACED"}};
    std::string result = applyAndReadback(spans, mods, {});

    EXPECT_NE(result.find("REPLACED"), std::string::npos)
        << "edited text 'REPLACED' not present after save/reopen -- edit was "
           "silently dropped (result: '" << result << "')";
    // The edited span's original word must be gone (real replacement, not append).
    EXPECT_EQ(result.find("Single"), std::string::npos)
        << "original 'Single' still present -- span replaced by append, not edit";
}

// Test 2: Null page — no crash AND the real document is left unchanged.
TEST_F(ContentStreamSubstitutionTest, NullPageIsNoOp) {
    if (!hasTestPdf_) GTEST_SKIP() << "No test PDF available";
    auto spans = extractSpans();
    std::vector<std::pair<int, std::u32string>> mods{{0, U"World"}};
    const std::string before = currentPageText();
    EXPECT_NO_THROW(
        editor_->applyEditsWithSubstitution(nullptr, doc_.get(), spans, mods, {}));
    EXPECT_EQ(currentPageText(), before)
        << "null-page edit mutated the document (must be a no-op)";
}

// Test 3: Null document — no crash AND no mutation.
TEST_F(ContentStreamSubstitutionTest, NullDocIsNoOp) {
    if (!hasTestPdf_) GTEST_SKIP() << "No test PDF available";
    auto spans = extractSpans();
    std::vector<std::pair<int, std::u32string>> mods{{0, U"World"}};
    const std::string before = currentPageText();
    EXPECT_NO_THROW(
        editor_->applyEditsWithSubstitution(page_.get(), nullptr, spans, mods, {}));
    EXPECT_EQ(currentPageText(), before)
        << "null-doc edit mutated the page (must be a no-op)";
}

// Test 4: Empty modifications — no crash AND the page text is unchanged.
TEST_F(ContentStreamSubstitutionTest, EmptyModificationsIsNoOp) {
    if (!hasTestPdf_) GTEST_SKIP() << "No test PDF available";
    auto spans = extractSpans();
    std::string result = applyAndReadback(spans, {}, {});
    EXPECT_NE(result.find("Single"), std::string::npos)
        << "empty-modifications edit lost the original text (result: '" << result << "')";
    EXPECT_NE(result.find("Document"), std::string::npos)
        << "empty-modifications edit lost the original text";
}

// Test 5: Substitution with accented replacement — the accented text must
// actually appear (the whole point of substitution: cover glyphs the original
// font may lack), not be dropped to tofu/.notdef.
TEST_F(ContentStreamSubstitutionTest, AccentedSubstitutionTextIsPresent) {
    if (!hasTestPdf_) GTEST_SKIP() << "No test PDF available";
    auto spans = extractSpans();
    ASSERT_FALSE(spans.empty());

    std::vector<std::pair<int, std::u32string>> mods{{0, U"Café résumé"}};
    std::vector<SubstitutionInfo> subs;
    SubstitutionInfo s;
    s.spanIndex = 0; s.substituteResName = "Fsub1"; s.horizontalScale = 1.0f;
    s.originalFontSize = 12.0f; s.originalFontName = "F1";
    subs.push_back(s);

    std::string result = applyAndReadback(spans, mods, subs);
    // The ASCII skeleton must survive (accents may or may not encode depending
    // on the base font, but the run must not be dropped entirely).
    EXPECT_NE(result.find("Caf"), std::string::npos)
        << "substituted accented run dropped entirely (result: '" << result << "')";
    EXPECT_NE(result.find("sum"), std::string::npos)
        << "second substituted word dropped (result: '" << result << "')";
}

// Test 6: Backward-compatible applyEdits — the edit must land in the output.
TEST_F(ContentStreamSubstitutionTest, BackwardCompatibleEditLands) {
    if (!hasTestPdf_) GTEST_SKIP() << "No test PDF available";
    auto spans = extractSpans();
    ASSERT_FALSE(spans.empty());

    std::vector<std::pair<int, std::u32string>> mods{{0, U"CHANGED"}};
    editor_->applyEdits(page_.get(), doc_.get(), spans, mods);
    auto out = std::filesystem::temp_directory_path() / "pdfe_subst_bc.pdf";
    doc_->saveAs(out);
    auto ctx = std::make_shared<MuPdfContext>();
    auto d = Document::open(ctx, out);
    ASSERT_NE(d, nullptr);
    std::string result = d->page(0)->extractPlainText();
    EXPECT_NE(result.find("CHANGED"), std::string::npos)
        << "applyEdits did not land the edit (result: '" << result << "')";
}

// Test 7: A scaled substitution edit lands (exercises the SubstitutionInfo
// horizontalScale path with a real span + output assertion).
TEST_F(ContentStreamSubstitutionTest, ScaledSubstitutionEditLands) {
    if (!hasTestPdf_) GTEST_SKIP() << "No test PDF available";
    auto spans = extractSpans();
    ASSERT_FALSE(spans.empty());

    std::vector<std::pair<int, std::u32string>> mods{{0, U"SCALED"}};
    std::vector<SubstitutionInfo> subs;
    SubstitutionInfo s;
    s.spanIndex = 0; s.substituteResName = "Fsub1"; s.horizontalScale = 0.8f;
    s.originalFontSize = 12.0f; s.originalFontName = "F1";
    subs.push_back(s);

    std::string result = applyAndReadback(spans, mods, subs);
    EXPECT_NE(result.find("SCALED"), std::string::npos)
        << "scaled-substitution edit dropped (result: '" << result << "')";
}

// ─────────────────────────────────────────────────────────────────────────────
// RENDER ORACLES — kill the phase-3 survivors M04 / M06 / M07.
// ─────────────────────────────────────────────────────────────────────────────

// M04 (hScale 100 → -100): a per-run substitution edit keeps the DEFAULT hScale,
// so the mirror shows up as a flipped glyph quad. Also asserts colour (M06).
TEST_F(ContentStreamSubstitutionTest, PerRunSubstitutionEditNotMirroredKeepsColor) {
    SubstReadback rb = substEditNegTz(/*perRun=*/true);
    ASSERT_TRUE(rb.ok) << "per-run substitution edit produced no 'SHOP' glyphs";
    ASSERT_GE(rb.edited.size(), 2u) << "need >=2 edited glyphs to check direction";

    // M04: the first edited glyph must not be horizontally mirrored, and the run
    // must advance left-to-right. Under hScale=-100 each glyph flips (ur.x<=ul.x)
    // and origins march right-to-left.
    const fz_quad& q = rb.edited.front().quad;
    EXPECT_GT(q.ur.x, q.ul.x)
        << "edited glyph is horizontally MIRRORED (quad ur.x <= ul.x) — "
           "substitution path leaked hScale=-100";
    EXPECT_GT(rb.edited.back().quad.ul.x, rb.edited.front().quad.ul.x)
        << "edited run advances right-to-left — text is mirrored";

    // M06: the grey (#dbd0d0) source fill must survive — not fall to the dark
    // ambient fill. Extraction is blind to this; the rendered argb is not.
    int r = 0, g = 0, b = 0; firstInkColor(rb.edited, r, g, b);
    EXPECT_GT(r, 0x60) << "edited glyph is (near) dark — original grey fill lost "
                          "(r=" << r << " g=" << g << " b=" << b << ")";
    EXPECT_NEAR(r, 0xDB, 28);
    EXPECT_NEAR(g, 0xD0, 28);
    EXPECT_NEAR(b, 0xD0, 28);
}

// M07 (initialFontSize → ×0.5): a legacy substitution edit keeps
// initialFontSize == inj.fontSize, so a half-size mutant renders the edited run
// at ~0.5 the "KEEP ME" neighbour's height. Also re-asserts colour (M06) and
// upright.
TEST_F(ContentStreamSubstitutionTest, LegacySubstitutionEditFullSizeKeepsColor) {
    SubstReadback rb = substEditNegTz(/*perRun=*/false);
    ASSERT_TRUE(rb.ok) << "legacy substitution edit produced no 'SHOP' glyphs";
    ASSERT_GT(rb.keepGlyphH, 0.1f) << "KEEP ME neighbour height unavailable";

    const fz_quad& q = rb.edited.front().quad;
    const float editedH = std::fabs(q.ll.y - q.ul.y);
    const float ratio = editedH / rb.keepGlyphH;

    // M07: the edited run must be the SAME point size as the same-size neighbour.
    // The half-size mutant drives this to ~0.5.
    EXPECT_GT(ratio, 0.80f)
        << "edited text is HALF-SIZE (sizeRatio=" << ratio
        << ") — substitution path halved initialFontSize";
    EXPECT_LT(ratio, 1.25f)
        << "edited text is OVER-SIZED (sizeRatio=" << ratio << ")";

    // Upright (defensive) + colour (M06).
    EXPECT_GT(q.ur.x, q.ul.x) << "edited glyph is mirrored";
    int r = 0, g = 0, b = 0; firstInkColor(rb.edited, r, g, b);
    EXPECT_GT(r, 0x60) << "edited glyph lost its grey fill (r=" << r << ")";
    EXPECT_NEAR(r, 0xDB, 28);
    EXPECT_NEAR(g, 0xD0, 28);
    EXPECT_NEAR(b, 0xD0, 28);
}
