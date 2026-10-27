// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F13 (SEC-54 / SEC-55): DETERMINISTIC
// font-PROGRAM cases — the everywhere-runnable companion to the coverage-guided
// harness at tests/security/fuzz/fuzz_font_program.cpp.
//
// THE GAP THIS CLOSES. tests/test_font_fuzz.cpp is named like a font fuzzer but
// every one of its cases mutates a font DICTIONARY (/Widths, /Encoding,
// /BaseFont, nesting) and drives the PDF object parser. Not one of them places
// attacker bytes inside a font PROGRAM, so this line —
//
//     src/fonts/EmbeddedFontAnalyzer.cpp:278
//         FT_New_Memory_Face(ftLibrary_, fi.fontData.data(), ...)
//
// — which hands a verbatim /FontFile2 stream to FreeType, had no test at all.
// FreeType's sfnt / CFF backends are a well-populated CVE surface, and PDFE
// walks the resulting face reading table metrics, PANOSE bytes, glyph outlines
// and charmaps.
//
// THE ORACLE (deliberately not "it didn't crash"). Each malformed program must:
//   (a) not crash, hang, or throw anything the benign-refusal triage classifier
//       calls a real defect — std::bad_alloc and unknown throws propagate;
//   (b) return within a wall-clock bound, so a quadratic table walk fails here
//       rather than becoming a nightly-only finding;
//   (c) produce a SELF-CONSISTENT result: either no profile at all, or a
//       profile whose metrics are finite and in range — a half-initialised
//       profile read out of a corrupt face is a real defect;
//   (d) be DETERMINISTIC across two identical runs;
// and the POSITIVE CONTROL (an unmodified base-14 program) must produce a valid
// profile, so none of the above can pass vacuously by the analyzer refusing
// everything.
//
// The known-good program is MuPDF's built-in base-14 Helvetica
// (fz_lookup_base14_font), so no font fixture is committed and no exploit
// sample is added to the repository — every malformed program is derived from
// that good font at run time.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontMetricProfile.h"

#include "security/fuzz/fuzz_support.h"   // triage_or_rethrow

#include <QTemporaryDir>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

namespace {

// A generous ceiling: a few-hundred-KB font program must be accepted or
// rejected in well under this even on a loaded debug CI box.
constexpr auto kPerCaseBudget = std::chrono::seconds(20);

std::string buildFontPdf(const std::string& program)
{
    const std::string content = "BT /F1 24 Tf 20 100 Td (Az) Tj ET\n";
    std::string out;
    out += "%PDF-1.7\n";
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] "
           "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n";
    out += "4 0 obj\n<< /Length " + std::to_string(content.size()) +
           " >>\nstream\n" + content + "endstream\nendobj\n";
    out += "5 0 obj\n<< /Type /Font /Subtype /TrueType /BaseFont /ProbeFont "
           "/FirstChar 32 /LastChar 126 /Encoding /WinAnsiEncoding "
           "/FontDescriptor 6 0 R >>\nendobj\n";
    out += "6 0 obj\n<< /Type /FontDescriptor /FontName /ProbeFont /Flags 32 "
           "/ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 "
           "/StemV 80 /FontBBox [0 -200 1000 800] /FontFile2 7 0 R >>\nendobj\n";
    out += "7 0 obj\n<< /Length " + std::to_string(program.size()) +
           " /Length1 " + std::to_string(program.size()) + " >>\nstream\n";
    out += program;
    out += "\nendstream\nendobj\n";
    out += "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

/// A profile that came back must be internally coherent. A corrupt face that
/// yielded garbage metrics would be a real defect, not a benign refusal.
::testing::AssertionResult profileIsCoherent(
    const pdfe::fonts::FontMetricProfile& p, const std::string& label)
{
    if (p.unitsPerEm < 0 || p.unitsPerEm > 65536)
        return ::testing::AssertionFailure()
               << label << ": unitsPerEm out of range (" << p.unitsPerEm << ")";
    const float metrics[] = { p.ascender, p.descender, p.capHeight, p.xHeight,
                              p.avgCharWidth, p.stemWeightV, p.stemWeightH,
                              p.italicAngle, p.serifExtent };
    for (float v : metrics) {
        if (!std::isfinite(v))
            return ::testing::AssertionFailure()
                   << label << ": a metric is not finite";
        if (v < -100000.0f || v > 100000.0f)
            return ::testing::AssertionFailure()
                   << label << ": a metric is wildly out of range (" << v << ")";
    }
    return ::testing::AssertionSuccess();
}

} // namespace

class FontProgramFuzzTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());

        int len = 0;
        const unsigned char* data = nullptr;
        ctx_->execute([&](fz_context* c) {
            data = fz_lookup_base14_font(c, "Helvetica", &len);
        });
        ASSERT_NE(data, nullptr)
            << "MuPDF returned no built-in Helvetica; every case below would "
               "be vacuous without a known-good program to mutate";
        ASSERT_GT(len, 1024);
        good_.assign(reinterpret_cast<const char*>(data),
                     reinterpret_cast<const char*>(data) + len);
    }

    std::filesystem::path writeBytes(const std::string& name,
                                     const std::string& bytes)
    {
        const std::filesystem::path p =
            std::filesystem::path(tempDir_->path().toStdString()) / name;
        std::ofstream f(p, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.close();
        return p;
    }

    /// Arm (A): FT_New_Face on the raw program. Returns the profile (possibly
    /// empty) and enforces the time bound + triage rules.
    std::optional<pdfe::fonts::FontMetricProfile> analyzeDirect(
        const std::string& program, const std::string& label)
    {
        const std::filesystem::path p = writeBytes(label + ".bin", program);
        pdfe::fonts::EmbeddedFontAnalyzer analyzer(ctx_.get());
        const auto start = std::chrono::steady_clock::now();
        std::optional<pdfe::fonts::FontMetricProfile> result;
        try {
            result = analyzer.analyzeFromFile(p.string());
        } catch (const std::exception& e) {
            // Re-throws (fails the test) for anything the classifier calls a
            // possible real defect; swallows an expected malformed-input
            // refusal.
            pdfe::fuzz::triage_or_rethrow(e);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_LT(elapsed, kPerCaseBudget)
            << label << ": font-program analysis exceeded the wall-clock bound "
                        "(possible quadratic/unbounded table walk)";
        if (result) EXPECT_TRUE(profileIsCoherent(*result, label));
        return result;
    }

    /// Arm (B): the production path — /FontFile2 stream -> Document::open ->
    /// analyzePageFonts -> FT_New_Memory_Face. Returns the profile count.
    std::size_t analyzeWrapped(const std::string& program,
                               const std::string& label)
    {
        const std::filesystem::path p =
            writeBytes(label + ".pdf", buildFontPdf(program));
        const auto start = std::chrono::steady_clock::now();
        std::size_t count = 0;
        try {
            auto doc = pdfe::core::Document::open(ctx_, p);
            if (doc && doc->pageCount() > 0) {
                auto page = doc->page(0);
                if (page) {
                    pdfe::fonts::EmbeddedFontAnalyzer analyzer(ctx_.get());
                    auto profiles = analyzer.analyzePageFonts(page.get(), doc.get());
                    count = profiles.size();
                    for (const auto& pr : profiles)
                        EXPECT_TRUE(profileIsCoherent(pr, label));
                }
            }
        } catch (const std::exception& e) {
            pdfe::fuzz::triage_or_rethrow(e);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_LT(elapsed, kPerCaseBudget) << label << ": wrapped-arm bound";
        return count;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<QTemporaryDir> tempDir_;
    std::string good_;
};

// ---------------------------------------------------------------------------
// POSITIVE CONTROL. Everything below is meaningless without it: it proves the
// analyzer really reaches FreeType and really parses a font program, so a
// "no profile" result for a malformed program is a REFUSAL and not an
// unconditional early return.
// ---------------------------------------------------------------------------
TEST_F(FontProgramFuzzTest, KnownGoodProgramProducesAValidProfile)
{
    auto direct = analyzeDirect(good_, "good_direct");
    ASSERT_TRUE(direct.has_value())
        << "the known-good base-14 program produced no profile: the font-"
           "program path is not being reached and every case in this file "
           "would be vacuous";
    EXPECT_GT(direct->unitsPerEm, 0);

    EXPECT_GT(analyzeWrapped(good_, "good_wrapped"), 0u)
        << "the production /FontFile2 -> FT_New_Memory_Face path produced no "
           "profile for a valid embedded font program";
}

// ---------------------------------------------------------------------------
// Truncation sweep — the classic sfnt overflow shape: a table directory that
// promises more than the buffer holds.
// ---------------------------------------------------------------------------
TEST_F(FontProgramFuzzTest, TruncatedProgramsAreHandled)
{
    const std::size_t n = good_.size();
    const std::size_t cuts[] = { 1, 4, 11, 12, 13, 16, 64, 256,
                                 n / 8, n / 4, n / 2, n - 1 };
    int idx = 0;
    for (std::size_t cut : cuts) {
        if (cut == 0 || cut >= n) continue;
        const std::string label = "trunc_" + std::to_string(idx++);
        (void)analyzeDirect(good_.substr(0, cut), label);
        (void)analyzeWrapped(good_.substr(0, cut), label);
    }
}

// ---------------------------------------------------------------------------
// Table-directory corruption: absurd numTables / offsets / lengths, which is
// where sfnt parsers historically read out of bounds.
// ---------------------------------------------------------------------------
TEST_F(FontProgramFuzzTest, CorruptTableDirectoryIsHandled)
{
    auto put16 = [](std::string& s, std::size_t off, std::uint16_t v) {
        if (off + 1 >= s.size()) return;
        s[off]     = static_cast<char>((v >> 8) & 0xFF);
        s[off + 1] = static_cast<char>(v & 0xFF);
    };
    auto put32 = [](std::string& s, std::size_t off, std::uint32_t v) {
        if (off + 3 >= s.size()) return;
        s[off]     = static_cast<char>((v >> 24) & 0xFF);
        s[off + 1] = static_cast<char>((v >> 16) & 0xFF);
        s[off + 2] = static_cast<char>((v >> 8) & 0xFF);
        s[off + 3] = static_cast<char>(v & 0xFF);
    };

    {   // numTables = 0xFFFF (directory claims 65535 entries)
        std::string m = good_;
        put16(m, 4, 0xFFFF);
        (void)analyzeDirect(m, "dir_numtables_max");
        (void)analyzeWrapped(m, "dir_numtables_max");
    }
    {   // numTables = 0
        std::string m = good_;
        put16(m, 4, 0);
        (void)analyzeDirect(m, "dir_numtables_zero");
        (void)analyzeWrapped(m, "dir_numtables_zero");
    }
    {   // first table record: offset far past EOF
        std::string m = good_;
        put32(m, 12 + 8, 0x7FFFFFFFu);
        (void)analyzeDirect(m, "dir_offset_overflow");
        (void)analyzeWrapped(m, "dir_offset_overflow");
    }
    {   // first table record: length far past EOF
        std::string m = good_;
        put32(m, 12 + 12, 0xFFFFFFF0u);
        (void)analyzeDirect(m, "dir_length_overflow");
        (void)analyzeWrapped(m, "dir_length_overflow");
    }
    {   // sfnt version replaced with the CFF/OTTO tag, header otherwise intact
        std::string m = good_;
        put32(m, 0, 0x4F54544Fu);  // 'OTTO'
        (void)analyzeDirect(m, "dir_otto_tag");
        (void)analyzeWrapped(m, "dir_otto_tag");
    }
    {   // Type1 PFB-ish header on TrueType body
        std::string m = good_;
        m[0] = '\x80'; m[1] = '\x01';
        (void)analyzeDirect(m, "dir_pfb_header");
        (void)analyzeWrapped(m, "dir_pfb_header");
    }
}

// ---------------------------------------------------------------------------
// Deterministic bit-flip sweep across the whole program. Reproducible (fixed
// LCG seed) so a failure is always re-runnable.
// ---------------------------------------------------------------------------
TEST_F(FontProgramFuzzTest, DeterministicBitFlipsAreHandled)
{
    std::uint64_t state = 0xF13F0117ull ^ 0x9E3779B97F4A7C15ull;
    auto next = [&state]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    };

    for (int i = 0; i < 24; ++i) {
        std::string m = good_;
        // Bias toward the header/table directory, where the parser trusts
        // structure, while still reaching deep into the glyph data.
        const std::size_t pos = (i < 12)
            ? (next() % 128)
            : (next() % m.size());
        m[pos] = static_cast<char>(m[pos] ^ (1u << (next() % 8)));
        const std::string label = "bitflip_" + std::to_string(i);
        (void)analyzeDirect(m, label);
        (void)analyzeWrapped(m, label);
    }
}

// ---------------------------------------------------------------------------
// Degenerate inputs.
// ---------------------------------------------------------------------------
TEST_F(FontProgramFuzzTest, DegenerateProgramsAreHandled)
{
    (void)analyzeDirect("", "empty");
    (void)analyzeWrapped("", "empty");

    (void)analyzeDirect(std::string(64, '\0'), "zeros");
    (void)analyzeWrapped(std::string(64, '\0'), "zeros");

    (void)analyzeDirect(std::string(4096, '\xFF'), "ones");
    (void)analyzeWrapped(std::string(4096, '\xFF'), "ones");

    (void)analyzeDirect("not a font at all, just prose", "prose");
    (void)analyzeWrapped("not a font at all, just prose", "prose");
}

// ---------------------------------------------------------------------------
// Determinism: the same malformed program must produce the same outcome twice.
// A differing outcome means state leaked between analyses (a stale FT_Face,
// an uninitialised read), which is a defect in its own right.
// ---------------------------------------------------------------------------
TEST_F(FontProgramFuzzTest, MalformedProgramOutcomeIsDeterministic)
{
    std::string m = good_;
    m[6] = static_cast<char>(0xFF);
    m[7] = static_cast<char>(0xFF);
    m[40] = static_cast<char>(m[40] ^ 0x5A);

    auto a = analyzeDirect(m, "determinism_a");
    auto b = analyzeDirect(m, "determinism_b");
    EXPECT_EQ(a.has_value(), b.has_value())
        << "the same malformed font program yielded different outcomes on two "
           "runs — analyzer state is leaking between analyses";
    if (a && b) {
        EXPECT_EQ(a->unitsPerEm, b->unitsPerEm);
        EXPECT_FLOAT_EQ(a->ascender, b->ascender);
        EXPECT_FLOAT_EQ(a->descender, b->descender);
    }

    EXPECT_EQ(analyzeWrapped(m, "determinism_w1"),
              analyzeWrapped(m, "determinism_w2"));
}

// ---------------------------------------------------------------------------
// Repetition: analysing a hostile program many times must not accumulate
// FT_Face leaks (each iteration re-enters FT_New_Face / FT_Done_Face).
// ---------------------------------------------------------------------------
TEST_F(FontProgramFuzzTest, RepeatedAnalysisOfAHostileProgramIsStable)
{
    std::string m = good_.substr(0, good_.size() / 3);
    for (int i = 0; i < 25; ++i) {
        (void)analyzeDirect(m, "repeat");
    }
    // The good program must still analyse correctly afterwards — proving the
    // analyzer was not left in a broken state by the hostile inputs.
    auto after = analyzeDirect(good_, "repeat_control");
    EXPECT_TRUE(after.has_value())
        << "after repeated hostile analyses the analyzer could no longer parse "
           "a valid font program";
}
