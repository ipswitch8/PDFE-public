#include <gtest/gtest.h>

#include "fonts/Type3FontBuilder.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QImage>
#include <QPainter>
#include <QRect>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::fonts::Type3FontBuilder;
using pdfe::fonts::Type3Glyph;

namespace {

class Type3FontBuilderTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        // Open a small writable PDF document. test_blank.pdf is a single
        // empty page that's fine for adding new font objects to.
        std::filesystem::path p =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_blank.pdf";
        if (std::filesystem::exists(p))
            doc_ = Document::open(ctx_, p);
    }

    QImage makeGlyphImage(int w, int h, int alpha = 200) {
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(QColor(0, 0, 0, alpha));
        p.setPen(Qt::NoPen);
        // Paint a filled rectangle covering most of the bitmap so we
        // have non-trivial alpha pixels to encode.
        p.drawRect(2, 2, w - 4, h - 4);
        p.end();
        return img;
    }

    Type3Glyph makeGlyph(char32_t cp, int w = 32, int h = 40,
                         int adv = 600,
                         QRect bbox = QRect(0, 0, 600, 700)) {
        Type3Glyph g;
        g.codepoint = cp;
        g.image = makeGlyphImage(w, h);
        g.advanceWidth = adv;
        g.bbox = bbox;
        return g;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
};

// ─── Empty input + null-arg defensive checks ────────────────────────────────

TEST_F(Type3FontBuilderTest, EmptyGlyphsReturnsNull) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    void* ref = Type3FontBuilder::build(ctx_.get(), doc_.get(), {});
    EXPECT_EQ(ref, nullptr);
}

TEST_F(Type3FontBuilderTest, NullContextReturnsNull) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    void* ref = Type3FontBuilder::build(nullptr, doc_.get(), {makeGlyph(U'A')});
    EXPECT_EQ(ref, nullptr);
}

TEST_F(Type3FontBuilderTest, NullDocumentReturnsNull) {
    void* ref = Type3FontBuilder::build(ctx_.get(), nullptr, {makeGlyph(U'A')});
    EXPECT_EQ(ref, nullptr);
}

TEST_F(Type3FontBuilderTest, NullImageGlyphsAreSkipped) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    Type3Glyph empty;
    empty.codepoint = U'X';  // image is null
    void* ref = Type3FontBuilder::build(ctx_.get(), doc_.get(), {empty});
    EXPECT_EQ(ref, nullptr)
        << "All-null-image input is equivalent to empty input";
}

// ─── Codepoint → name encoding ──────────────────────────────────────────────

TEST(Type3FontBuilder_Naming, BasicMultilingualPlaneFormat) {
    EXPECT_EQ(Type3FontBuilder::nameForCodepoint(U'A'), "u0041");
    EXPECT_EQ(Type3FontBuilder::nameForCodepoint(U'a'), "u0061");
    EXPECT_EQ(Type3FontBuilder::nameForCodepoint(static_cast<char32_t>(0x05D0)),
              "u05D0");  // Hebrew Alef
    EXPECT_EQ(Type3FontBuilder::nameForCodepoint(static_cast<char32_t>(0xFFFF)),
              "uFFFF");
}

TEST(Type3FontBuilder_Naming, SupplementaryPlaneFormat) {
    EXPECT_EQ(Type3FontBuilder::nameForCodepoint(static_cast<char32_t>(0x10000)),
              "u00010000");
    EXPECT_EQ(Type3FontBuilder::nameForCodepoint(static_cast<char32_t>(0x1F600)),
              "u0001F600");  // emoji
}

TEST(Type3FontBuilder_Naming, IsStableAcrossCalls) {
    // Same codepoint → same name byte-for-byte. Tests rely on this for
    // /Differences encoding determinism.
    auto a1 = Type3FontBuilder::nameForCodepoint(U'd');
    auto a2 = Type3FontBuilder::nameForCodepoint(U'd');
    EXPECT_EQ(a1, a2);
    EXPECT_EQ(a1, "u0064");
}

// Gap #20 (pipeline b3f19d72 phase-15): directly assert the /uXXXX encoding
// format produced by nameForCodepoint (internally used as buildCharName in
// the PDF Encoding /Differences array). Verifies:
//   - Leading 'u' prefix (lowercase, no leading slash — the slash is the PDF
//     operator; the name itself starts with 'u').
//   - Uppercase hex digits.
//   - BMP codepoints: exactly 4 hex digits (zero-padded).
//   - Supplementary plane: 8 hex digits (zero-padded).
//   - Mixed-case input codepoints: output is always uppercase hex.
TEST(Type3FontBuilder_Naming, BuildCharName_ProducesCorrectUnicodeHexFormat)
{
    // ASCII letter: U+0041 → "u0041" (4 hex digits, uppercase)
    {
        auto name = Type3FontBuilder::nameForCodepoint(U'A');
        ASSERT_FALSE(name.empty());
        EXPECT_EQ(name[0], 'u')
            << "nameForCodepoint must start with 'u', got: " << name;
        // Remaining characters must be uppercase hex only.
        for (std::size_t i = 1; i < name.size(); ++i) {
            char c = name[i];
            bool isUpperHex = (c >= '0' && c <= '9')
                           || (c >= 'A' && c <= 'F');
            EXPECT_TRUE(isUpperHex)
                << "nameForCodepoint must use uppercase hex digits; "
                   "got char '" << c << "' at index " << i
                << " in \"" << name << "\"";
        }
        EXPECT_EQ(name, "u0041");
    }

    // Non-ASCII BMP: U+00E9 LATIN SMALL LETTER E WITH ACUTE → "u00E9"
    {
        auto name = Type3FontBuilder::nameForCodepoint(
            static_cast<char32_t>(0x00E9));
        EXPECT_EQ(name.size(), 5u)  // 'u' + 4 hex digits
            << "BMP codepoint name must be exactly 5 chars (u + 4 hex digits)";
        EXPECT_EQ(name, "u00E9");
    }

    // High BMP: U+FFFF → "uFFFF"
    {
        auto name = Type3FontBuilder::nameForCodepoint(
            static_cast<char32_t>(0xFFFF));
        EXPECT_EQ(name, "uFFFF");
    }

    // Supplementary plane: U+10000 → "u00010000" (8 hex digits)
    {
        auto name = Type3FontBuilder::nameForCodepoint(
            static_cast<char32_t>(0x10000));
        EXPECT_EQ(name.size(), 9u)  // 'u' + 8 hex digits
            << "Supplementary-plane codepoint must be 'u' + 8 hex digits";
        EXPECT_EQ(name, "u00010000");
    }

    // Emoji U+1F600 → "u0001F600"
    {
        auto name = Type3FontBuilder::nameForCodepoint(
            static_cast<char32_t>(0x1F600));
        EXPECT_EQ(name, "u0001F600");
    }
}

// ─── Round-trip the resulting font dict via MuPDF ───────────────────────────
//
// Helper: read a field off the indirect ref through MuPDF and assert.

namespace {

void runOnFontDict(MuPdfContext* ctx, void* indirectRef,
                   const std::function<void(fz_context*, pdf_obj*)>& fn) {
    ctx->execute([&](fz_context* c) {
        auto* ref = static_cast<pdf_obj*>(indirectRef);
        ASSERT_NE(ref, nullptr);
        pdf_obj* dict = pdf_resolve_indirect(c, ref);
        ASSERT_NE(dict, nullptr);
        fn(c, dict);
    });
}

}  // namespace

TEST_F(Type3FontBuilderTest, SingleGlyphRoundTrip) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    auto g = makeGlyph(U'A', 16, 20, 600,
                       QRect(0, 0, 600, 700));
    void* ref = Type3FontBuilder::build(ctx_.get(), doc_.get(), {g});
    ASSERT_NE(ref, nullptr);

    runOnFontDict(ctx_.get(), ref, [](fz_context* c, pdf_obj* d) {
        // /Type /Font, /Subtype /Type3
        pdf_obj* type = pdf_dict_get(c, d, PDF_NAME(Type));
        EXPECT_TRUE(pdf_name_eq(c, type, PDF_NAME(Font)));
        pdf_obj* sub = pdf_dict_get(c, d, PDF_NAME(Subtype));
        EXPECT_TRUE(pdf_name_eq(c, sub, PDF_NAME(Type3)));

        // /CharProcs has exactly one entry
        pdf_obj* cp = pdf_dict_get(c, d, PDF_NAME(CharProcs));
        ASSERT_NE(cp, nullptr);
        EXPECT_EQ(pdf_dict_len(c, cp), 1);

        // /FirstChar=1, /LastChar=1
        EXPECT_EQ(pdf_dict_get_int(c, d, PDF_NAME(FirstChar)), 1);
        EXPECT_EQ(pdf_dict_get_int(c, d, PDF_NAME(LastChar)), 1);
    });
}

TEST_F(Type3FontBuilderTest, FontMatrixIsThousandthsScale) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    void* ref = Type3FontBuilder::build(
        ctx_.get(), doc_.get(), {makeGlyph(U'A')});
    ASSERT_NE(ref, nullptr);

    runOnFontDict(ctx_.get(), ref, [](fz_context* c, pdf_obj* d) {
        pdf_obj* fm = pdf_dict_get(c, d, PDF_NAME(FontMatrix));
        ASSERT_NE(fm, nullptr);
        ASSERT_EQ(pdf_array_len(c, fm), 6);
        EXPECT_NEAR(pdf_array_get_real(c, fm, 0), 0.001, 1e-9);
        EXPECT_EQ(pdf_array_get_int(c, fm, 1), 0);
        EXPECT_EQ(pdf_array_get_int(c, fm, 2), 0);
        EXPECT_NEAR(pdf_array_get_real(c, fm, 3), 0.001, 1e-9);
        EXPECT_EQ(pdf_array_get_int(c, fm, 4), 0);
        EXPECT_EQ(pdf_array_get_int(c, fm, 5), 0);
    });
}

TEST_F(Type3FontBuilderTest, FontBBoxCoversAllGlyphBBoxes) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    Type3Glyph a = makeGlyph(U'A');  a.bbox = QRect(0,   0, 500, 700);
    Type3Glyph b = makeGlyph(U'B');  b.bbox = QRect(50, -50, 600, 800);
    void* ref = Type3FontBuilder::build(
        ctx_.get(), doc_.get(), {a, b});
    ASSERT_NE(ref, nullptr);

    runOnFontDict(ctx_.get(), ref, [](fz_context* c, pdf_obj* d) {
        pdf_obj* fb = pdf_dict_get(c, d, PDF_NAME(FontBBox));
        ASSERT_NE(fb, nullptr);
        ASSERT_EQ(pdf_array_len(c, fb), 4);
        // Union of (0,0,500,700) and (50,-50,650,750) → (0,-50,650,750)
        EXPECT_EQ(pdf_array_get_int(c, fb, 0), 0);
        EXPECT_EQ(pdf_array_get_int(c, fb, 1), -50);
        EXPECT_EQ(pdf_array_get_int(c, fb, 2), 650);
        EXPECT_EQ(pdf_array_get_int(c, fb, 3), 750);
    });
}

TEST_F(Type3FontBuilderTest, WidthsArrayMatchesGlyphAdvanceOrder) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    Type3Glyph a = makeGlyph(U'A'); a.advanceWidth = 333;
    Type3Glyph b = makeGlyph(U'B'); b.advanceWidth = 666;
    Type3Glyph cg = makeGlyph(U'C'); cg.advanceWidth = 999;
    void* ref = Type3FontBuilder::build(
        ctx_.get(), doc_.get(), {a, b, cg});
    ASSERT_NE(ref, nullptr);

    runOnFontDict(ctx_.get(), ref, [](fz_context* c, pdf_obj* d) {
        pdf_obj* w = pdf_dict_get(c, d, PDF_NAME(Widths));
        ASSERT_NE(w, nullptr);
        ASSERT_EQ(pdf_array_len(c, w), 3);
        EXPECT_EQ(pdf_array_get_int(c, w, 0), 333);
        EXPECT_EQ(pdf_array_get_int(c, w, 1), 666);
        EXPECT_EQ(pdf_array_get_int(c, w, 2), 999);
    });
}

TEST_F(Type3FontBuilderTest, EncodingDifferencesMapsCodesToGlyphNames) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    void* ref = Type3FontBuilder::build(
        ctx_.get(), doc_.get(), {makeGlyph(U'A'), makeGlyph(U'a')});
    ASSERT_NE(ref, nullptr);

    runOnFontDict(ctx_.get(), ref, [](fz_context* c, pdf_obj* d) {
        pdf_obj* enc = pdf_dict_get(c, d, PDF_NAME(Encoding));
        ASSERT_NE(enc, nullptr);
        pdf_obj* type = pdf_dict_get(c, enc, PDF_NAME(Type));
        EXPECT_TRUE(pdf_name_eq(c, type, PDF_NAME(Encoding)));

        pdf_obj* diffs = pdf_dict_get(c, enc, PDF_NAME(Differences));
        ASSERT_NE(diffs, nullptr);
        // Expected: [1 /u0041 /u0061]  (single base code 1, names follow)
        ASSERT_GE(pdf_array_len(c, diffs), 3);
        EXPECT_EQ(pdf_array_get_int(c, diffs, 0), 1);
        EXPECT_STREQ(pdf_to_name(c, pdf_array_get(c, diffs, 1)), "u0041");
        EXPECT_STREQ(pdf_to_name(c, pdf_array_get(c, diffs, 2)), "u0061");
    });
}

TEST_F(Type3FontBuilderTest, CharProcContentStreamIsParseable) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    Type3Glyph g = makeGlyph(U'A', 16, 20, 600, QRect(0, 0, 600, 700));
    void* ref = Type3FontBuilder::build(ctx_.get(), doc_.get(), {g});
    ASSERT_NE(ref, nullptr);

    runOnFontDict(ctx_.get(), ref, [](fz_context* c, pdf_obj* d) {
        pdf_obj* charProcs = pdf_dict_get(c, d, PDF_NAME(CharProcs));
        ASSERT_NE(charProcs, nullptr);
        ASSERT_EQ(pdf_dict_len(c, charProcs), 1);

        pdf_obj* sref = pdf_dict_get_val(c, charProcs, 0);
        ASSERT_NE(sref, nullptr);
        // Read the stream contents
        fz_buffer* buf = pdf_load_stream(c, sref);
        ASSERT_NE(buf, nullptr);
        unsigned char* data = nullptr;
        size_t len = fz_buffer_storage(c, buf, &data);
        ASSERT_GT(len, 0u);
        std::string s(reinterpret_cast<const char*>(data), len);
        fz_drop_buffer(c, buf);

        // Must contain the canonical Type 3 + inline-image sequence.
        EXPECT_NE(s.find(" d0"), std::string::npos)
            << "CharProc must use d0 operator";
        EXPECT_NE(s.find("\nq\n"), std::string::npos);
        EXPECT_NE(s.find(" cm\n"), std::string::npos);
        EXPECT_NE(s.find("BI\n"), std::string::npos);
        EXPECT_NE(s.find("ID\n"), std::string::npos);
        EXPECT_NE(s.find("\nEI\n"), std::string::npos);
        EXPECT_NE(s.find("\nQ\n"), std::string::npos);
    });
}

TEST_F(Type3FontBuilderTest, ResourcesIncludesProcSetForImageB) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    void* ref = Type3FontBuilder::build(
        ctx_.get(), doc_.get(), {makeGlyph(U'A')});
    ASSERT_NE(ref, nullptr);

    runOnFontDict(ctx_.get(), ref, [](fz_context* c, pdf_obj* d) {
        pdf_obj* r = pdf_dict_get(c, d, PDF_NAME(Resources));
        ASSERT_NE(r, nullptr);
        pdf_obj* ps = pdf_dict_get(c, r, PDF_NAME(ProcSet));
        ASSERT_NE(ps, nullptr);
        ASSERT_EQ(pdf_array_len(c, ps), 2);
        EXPECT_TRUE(pdf_name_eq(c, pdf_array_get(c, ps, 0), PDF_NAME(PDF)));
        EXPECT_TRUE(pdf_name_eq(c, pdf_array_get(c, ps, 1), PDF_NAME(ImageB)));
    });
}

// ─── Independence: two calls produce independent indirect refs ──────────────

TEST_F(Type3FontBuilderTest, RepeatedBuildsProduceIndependentRefs) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    void* a = Type3FontBuilder::build(
        ctx_.get(), doc_.get(), {makeGlyph(U'A')});
    void* b = Type3FontBuilder::build(
        ctx_.get(), doc_.get(), {makeGlyph(U'A')});
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b)
        << "Each build call must produce its own font dict, even with "
           "identical input. Callers may need both as separate page resources.";
}

// ─── Codepoint dedup ────────────────────────────────────────────────────────

TEST_F(Type3FontBuilderTest, RepeatedCodepointInInputIsDeduped) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    // First-occurrence wins. Here both entries are U+0041; the result
    // must contain exactly one CharProc, not two.
    void* ref = Type3FontBuilder::build(
        ctx_.get(), doc_.get(), {makeGlyph(U'A'), makeGlyph(U'A')});
    ASSERT_NE(ref, nullptr);
    runOnFontDict(ctx_.get(), ref, [](fz_context* c, pdf_obj* d) {
        pdf_obj* cp = pdf_dict_get(c, d, PDF_NAME(CharProcs));
        EXPECT_EQ(pdf_dict_len(c, cp), 1);
        EXPECT_EQ(pdf_dict_get_int(c, d, PDF_NAME(LastChar)), 1);
    });
}

}  // namespace
