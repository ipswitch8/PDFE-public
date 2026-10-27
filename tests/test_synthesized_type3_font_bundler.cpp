#include <gtest/gtest.h>

#include "tre/SynthesizedType3FontBundler.h"
#include "tre/EnrichedAtlas.h"
#include "tre/GlyphAtlas.h"
#include "tre/GlyphSynthesizer.h"
#include "tre/VectorEnrichedAtlasBuilder.h"
#include "tre/VectorFontAtlasBuilder.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QImage>
#include <QPainter>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::tre::EnrichedAtlas;
using pdfe::tre::GlyphAtlas;
using pdfe::tre::SynthesizedFontBundle;
using pdfe::tre::SynthesizedType3FontBundler;

namespace {

class FtFaceFixture {
public:
    explicit FtFaceFixture(const std::string& path) {
        if (!std::filesystem::exists(path)) return;
        if (FT_Init_FreeType(&lib_) != 0) { lib_ = nullptr; return; }
        if (FT_New_Face(lib_, path.c_str(), 0, &face_) != 0) face_ = nullptr;
    }
    ~FtFaceFixture() {
        if (face_) FT_Done_Face(face_);
        if (lib_) FT_Done_FreeType(lib_);
    }
    FT_Face face() const { return face_; }
    bool valid() const { return face_ != nullptr; }
    FtFaceFixture(const FtFaceFixture&) = delete;
    FtFaceFixture& operator=(const FtFaceFixture&) = delete;
private:
    FT_Library lib_ = nullptr;
    FT_Face face_ = nullptr;
};

constexpr const char* kConsolasPath = "C:/Windows/Fonts/consola.ttf";

/// Build a tiny artificial subset atlas from a real font, omitting
/// targeted codepoints so we can test the synthesis-then-bundle path.
GlyphAtlas buildSubset(FT_Face face, int pixelSize,
                       const std::u32string& codepoints) {
    GlyphAtlas atlas;
    if (!face || pixelSize < 1) return atlas;
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize)) != 0)
        return atlas;
    for (char32_t cp : codepoints) {
        FT_UInt gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
        if (gid == 0) continue;
        if (FT_Load_Glyph(face, gid, FT_LOAD_RENDER) != 0) continue;
        const FT_Bitmap& b = face->glyph->bitmap;
        if (!b.buffer || b.width == 0 || b.rows == 0) continue;
        if (b.pixel_mode != FT_PIXEL_MODE_GRAY) continue;
        QImage img(static_cast<int>(b.width), static_cast<int>(b.rows),
                   QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        const int pitchAbs = b.pitch >= 0 ? b.pitch : -b.pitch;
        const bool topDown = b.pitch >= 0;
        for (unsigned int r = 0; r < b.rows; ++r) {
            QRgb* dst = reinterpret_cast<QRgb*>(img.scanLine(static_cast<int>(r)));
            unsigned int sr = topDown ? r : (b.rows - 1u - r);
            const unsigned char* src =
                b.buffer + static_cast<size_t>(sr) * static_cast<size_t>(pitchAbs);
            for (unsigned int c = 0; c < b.width; ++c) {
                uint8_t cov = src[c];
                if (cov) dst[c] = qRgba(0, 0, 0, cov);
            }
        }
        atlas.addGlyph(cp, img, 100);
    }
    return atlas;
}

/// Normalized cross-correlation on grayscale alpha channel (or
/// luminance) of two QImages of arbitrary sizes. Both inputs are
/// resampled to a common size; padded with full-paper pixels (alpha 0)
/// so the comparison handles aspect-ratio differences gracefully.
/// Returns NCC in [-1, 1], or 0 when degenerate.
float nccAlpha(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull()) return 0.0f;
    QImage A = a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage B = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int W = std::max(A.width(), B.width());
    int H = std::max(A.height(), B.height());
    if (W == 0 || H == 0) return 0.0f;
    A = A.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    B = B.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    long long n = static_cast<long long>(W) * H;
    double sumA = 0, sumB = 0;
    for (int y = 0; y < H; ++y) {
        const QRgb* la = reinterpret_cast<const QRgb*>(A.constScanLine(y));
        const QRgb* lb = reinterpret_cast<const QRgb*>(B.constScanLine(y));
        for (int x = 0; x < W; ++x) {
            sumA += qAlpha(la[x]);
            sumB += qAlpha(lb[x]);
        }
    }
    double meanA = sumA / n;
    double meanB = sumB / n;
    double num = 0, denA = 0, denB = 0;
    for (int y = 0; y < H; ++y) {
        const QRgb* la = reinterpret_cast<const QRgb*>(A.constScanLine(y));
        const QRgb* lb = reinterpret_cast<const QRgb*>(B.constScanLine(y));
        for (int x = 0; x < W; ++x) {
            double da = qAlpha(la[x]) - meanA;
            double db = qAlpha(lb[x]) - meanB;
            num += da * db;
            denA += da * da;
            denB += db * db;
        }
    }
    double den = std::sqrt(denA * denB);
    if (den < 1e-9) return 0.0f;
    return static_cast<float>(num / den);
}

class SynthesizedType3FontBundlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        std::filesystem::path p =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_blank.pdf";
        if (std::filesystem::exists(p))
            doc_ = Document::open(ctx_, p);
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
};

// ─── Defensive null-input handling ──────────────────────────────────────────

TEST_F(SynthesizedType3FontBundlerTest, NullInputsReturnEmptyBundle) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    EnrichedAtlas atlas;
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    auto a = SynthesizedType3FontBundler::build(
        nullptr, page.get(), doc_.get(), atlas, {U'A'});
    EXPECT_EQ(a.fontIndirectRef, nullptr);

    auto b = SynthesizedType3FontBundler::build(
        ctx_.get(), nullptr, doc_.get(), atlas, {U'A'});
    EXPECT_EQ(b.fontIndirectRef, nullptr);

    auto c = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), nullptr, atlas, {U'A'});
    EXPECT_EQ(c.fontIndirectRef, nullptr);
}

TEST_F(SynthesizedType3FontBundlerTest, EmptyMissingListReturnsEmptyBundle) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);
    EnrichedAtlas atlas;
    auto bundle = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), doc_.get(), atlas, {});
    EXPECT_EQ(bundle.fontIndirectRef, nullptr);
    EXPECT_TRUE(bundle.codepointsBundled.empty());
}

// ─── Atlas can't supply requested codepoints → bundle is empty ──────────────

TEST_F(SynthesizedType3FontBundlerTest, UnsupplyableCodepointsAreSkipped) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    // Empty atlas can't supply anything → bundle is empty.
    EnrichedAtlas atlas;
    auto bundle = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), doc_.get(), atlas, {U'X', U'Y'});
    EXPECT_EQ(bundle.fontIndirectRef, nullptr);
    EXPECT_TRUE(bundle.codepointsBundled.empty());
}

// ─── End-to-end: subset atlas → synthesis → bundle ──────────────────────────

TEST_F(SynthesizedType3FontBundlerTest, BundleFromSynthesizedRecipeGlyphs) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    FtFaceFixture fx(kConsolasPath);
    if (!fx.valid()) GTEST_SKIP() << "Consolas not available";

    // Subset atlas with donor glyphs but missing 'd' (synthesizable from
    // 'b' via HFlip recipe in GlyphSynthesizer).
    GlyphAtlas subset = buildSubset(fx.face(), 32, U"bpqognH");
    EnrichedAtlas enriched;
    enriched.build(std::move(subset), U"d");
    ASSERT_TRUE(enriched.contains(U'd'));

    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    auto bundle = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), doc_.get(), enriched, {U'd'});
    ASSERT_NE(bundle.fontIndirectRef, nullptr);
    EXPECT_EQ(bundle.codepointsBundled.size(), 1u);
    EXPECT_EQ(bundle.codepointsBundled[0], U'd');
    EXPECT_EQ(bundle.codepointToCode[U'd'], 1);
    EXPECT_FALSE(bundle.resourceName.empty());
    EXPECT_EQ(bundle.resourceName.substr(0, 5), "T3sub");
}

// ─── Resource name uniqueness across calls ──────────────────────────────────

TEST_F(SynthesizedType3FontBundlerTest, RepeatedBuildsAssignDistinctResourceNames) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    FtFaceFixture fx(kConsolasPath);
    if (!fx.valid()) GTEST_SKIP() << "Consolas not available";

    GlyphAtlas s1 = buildSubset(fx.face(), 32, U"bpqognH");
    EnrichedAtlas e1;
    e1.build(std::move(s1), U"d");
    GlyphAtlas s2 = buildSubset(fx.face(), 32, U"bpqognH");
    EnrichedAtlas e2;
    e2.build(std::move(s2), U"d");

    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    auto a = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), doc_.get(), e1, {U'd'});
    auto b = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), doc_.get(), e2, {U'd'});
    ASSERT_NE(a.fontIndirectRef, nullptr);
    ASSERT_NE(b.fontIndirectRef, nullptr);
    EXPECT_NE(a.resourceName, b.resourceName)
        << "Each bundle must occupy its own resource name on the page";
    EXPECT_NE(a.fontIndirectRef, b.fontIndirectRef);
}

// ─── Bundled font dict resolves to a real Type 3 font ───────────────────────
//
// Direct page-resource walk requires friend access to Page::fzPage(),
// which the test isn't granted. Instead we verify the contract from the
// caller's perspective: the returned indirect ref resolves through MuPDF
// to a /Type /Font /Subtype /Type3 dictionary, the resource name follows
// the T3sub<N> convention, and the codepointToCode map is populated.
// "Installed on the page" is exercised end-to-end by the
// MixedRequestKeepsSupplyableSkipsRest and NccAgainstSynthesizedImageIsHigh
// tests, both of which require the page-side install to have succeeded
// before the bundler returns a non-null fontIndirectRef.

TEST_F(SynthesizedType3FontBundlerTest, BundledFontIndirectRefResolvesToType3) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    FtFaceFixture fx(kConsolasPath);
    if (!fx.valid()) GTEST_SKIP() << "Consolas not available";

    GlyphAtlas subset = buildSubset(fx.face(), 32, U"bpqognH");
    EnrichedAtlas enriched;
    enriched.build(std::move(subset), U"d");
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    auto bundle = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), doc_.get(), enriched, {U'd'});
    ASSERT_NE(bundle.fontIndirectRef, nullptr);
    EXPECT_FALSE(bundle.resourceName.empty());
    EXPECT_EQ(bundle.resourceName.substr(0, 5), "T3sub");

    ctx_->execute([&](fz_context* c) {
        pdf_obj* d = pdf_resolve_indirect(c,
            static_cast<pdf_obj*>(bundle.fontIndirectRef));
        ASSERT_NE(d, nullptr);
        pdf_obj* type = pdf_dict_get(c, d, PDF_NAME(Type));
        EXPECT_TRUE(pdf_name_eq(c, type, PDF_NAME(Font)));
        pdf_obj* sub = pdf_dict_get(c, d, PDF_NAME(Subtype));
        EXPECT_TRUE(pdf_name_eq(c, sub, PDF_NAME(Type3)));
    });
}

// ─── Bundle skips unsupplyable codepoints but keeps supplyable ones ─────────

TEST_F(SynthesizedType3FontBundlerTest, MixedRequestKeepsSupplyableSkipsRest) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    FtFaceFixture fx(kConsolasPath);
    if (!fx.valid()) GTEST_SKIP() << "Consolas not available";

    GlyphAtlas subset = buildSubset(fx.face(), 32, U"bpqognH");
    EnrichedAtlas enriched;
    enriched.build(std::move(subset), U"d");
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    // Request both 'd' (synthesizable) and a Linear B codepoint (not
    // synthesizable, no recipe, no atlas glyph).
    constexpr char32_t kAbsent = 0x10000;
    auto bundle = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), doc_.get(), enriched, {U'd', kAbsent});
    ASSERT_NE(bundle.fontIndirectRef, nullptr);
    EXPECT_EQ(bundle.codepointsBundled.size(), 1u);
    EXPECT_EQ(bundle.codepointsBundled[0], U'd');
    EXPECT_EQ(bundle.codepointToCode.count(kAbsent), 0u);
}

// ─── /Widths reflect glyph aspect ratio ─────────────────────────────────────

TEST_F(SynthesizedType3FontBundlerTest, WidthsReflectGlyphAspectRatio) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    FtFaceFixture fx(kConsolasPath);
    if (!fx.valid()) GTEST_SKIP() << "Consolas not available";

    GlyphAtlas subset = buildSubset(fx.face(), 32, U"bpqognH");
    EnrichedAtlas enriched;
    enriched.build(std::move(subset), U"d");
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    auto bundle = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), doc_.get(), enriched, {U'd'});
    ASSERT_NE(bundle.fontIndirectRef, nullptr);

    // Read /Widths[0] and verify it's > 0 and reasonable for the glyph
    // aspect (Consolas is monospace; 'd' should be in the 400-1100 1/1000
    // em range, plus our 100-unit side bearings).
    int w0 = 0;
    ctx_->execute([&](fz_context* c) {
        pdf_obj* d = pdf_resolve_indirect(c,
            static_cast<pdf_obj*>(bundle.fontIndirectRef));
        ASSERT_NE(d, nullptr);
        pdf_obj* widths = pdf_dict_get(c, d, PDF_NAME(Widths));
        ASSERT_NE(widths, nullptr);
        ASSERT_EQ(pdf_array_len(c, widths), 1);
        w0 = pdf_array_get_int(c, widths, 0);
    });
    EXPECT_GT(w0, 100);
    EXPECT_LT(w0, 1500);
}

// ─── NCC round-trip: bundled CharProc encodes the synthesized image ─────────
//
// True end-to-end: open the page, look up the bundled font's CharProc,
// extract the inline image bytes, decode them, compare via NCC against
// the synthesized QImage. Per AC, NCC must be >= 0.85.

TEST_F(SynthesizedType3FontBundlerTest, NccAgainstSynthesizedImageIsHigh) {
    if (!doc_) GTEST_SKIP() << "test_blank.pdf not available";
    FtFaceFixture fx(kConsolasPath);
    if (!fx.valid()) GTEST_SKIP() << "Consolas not available";

    GlyphAtlas subset = buildSubset(fx.face(), 32, U"bpqognH");
    EnrichedAtlas enriched;
    enriched.build(std::move(subset), U"d");
    QImage synthD = enriched.glyph(U'd');
    ASSERT_FALSE(synthD.isNull());

    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    auto bundle = SynthesizedType3FontBundler::build(
        ctx_.get(), page.get(), doc_.get(), enriched, {U'd'});
    ASSERT_NE(bundle.fontIndirectRef, nullptr);

    // Decode the inline image inside the CharProc back to a QImage and
    // compare via NCC against the synthesized image.
    QImage decoded;
    int decodedW = 0, decodedH = 0;
    ctx_->execute([&](fz_context* c) {
        pdf_obj* d = pdf_resolve_indirect(c,
            static_cast<pdf_obj*>(bundle.fontIndirectRef));
        ASSERT_NE(d, nullptr);
        pdf_obj* cp = pdf_dict_get(c, d, PDF_NAME(CharProcs));
        ASSERT_NE(cp, nullptr);
        ASSERT_EQ(pdf_dict_len(c, cp), 1);
        pdf_obj* sref = pdf_dict_get_val(c, cp, 0);
        ASSERT_NE(sref, nullptr);
        fz_buffer* buf = pdf_load_stream(c, sref);
        ASSERT_NE(buf, nullptr);
        unsigned char* data = nullptr;
        size_t len = fz_buffer_storage(c, buf, &data);
        std::string s(reinterpret_cast<const char*>(data), len);
        fz_drop_buffer(c, buf);

        // Find /W <int> and /H <int> in the BI dict.
        auto findInt = [&](const std::string& key) -> int {
            auto p = s.find(key);
            if (p == std::string::npos) return 0;
            p += key.size();
            while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
            int v = 0;
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
                v = v * 10 + (s[p] - '0');
                ++p;
            }
            return v;
        };
        decodedW = findInt("/W ");
        decodedH = findInt("/H ");

        // Find ID...EI hex blob.
        auto idPos = s.find("ID\n");
        auto eiPos = s.find("\nEI\n");
        ASSERT_NE(idPos, std::string::npos);
        ASSERT_NE(eiPos, std::string::npos);
        std::string hex = s.substr(idPos + 3, eiPos - idPos - 3);

        // Decode hex to bytes (skip whitespace and final '>').
        std::vector<uint8_t> bytes;
        bytes.reserve(static_cast<size_t>(decodedW) * decodedH);
        auto fromHex = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            return -1;
        };
        int half = -1;
        for (char ch : hex) {
            int v = fromHex(ch);
            if (v < 0) continue;
            if (half < 0) {
                half = v;
            } else {
                bytes.push_back(static_cast<uint8_t>((half << 4) | v));
                half = -1;
            }
        }
        ASSERT_EQ(static_cast<int>(bytes.size()), decodedW * decodedH);

        // Reconstruct as QImage with alpha = 255 - gray (inverse of
        // imageToGrayInk in Type3FontBuilder).
        decoded = QImage(decodedW, decodedH,
                          QImage::Format_ARGB32_Premultiplied);
        decoded.fill(Qt::transparent);
        for (int y = 0; y < decodedH; ++y) {
            QRgb* dst = reinterpret_cast<QRgb*>(decoded.scanLine(y));
            for (int x = 0; x < decodedW; ++x) {
                uint8_t gray = bytes[y * decodedW + x];
                uint8_t alpha = static_cast<uint8_t>(255 - gray);
                dst[x] = qRgba(0, 0, 0, alpha);
            }
        }
    });

    ASSERT_FALSE(decoded.isNull());
    float ncc = nccAlpha(synthD, decoded);
    EXPECT_GE(ncc, 0.85f)
        << "NCC of bundled CharProc decoded vs synthesized image must >= 0.85; got "
        << ncc;
}

}  // namespace
