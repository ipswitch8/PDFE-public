// -*- coding: utf-8 -*-
// Meta-audit F13 fuzz harness (SEC-09 / SEC-54 / SEC-58): the FONT-PROGRAM
// parse surface — attacker-controlled sfnt / CFF / Type1 bytes reaching
// FreeType through PDFE's own font analyzer.
//
// WHY THIS HARNESS EXISTS, AND WHY tests/test_font_fuzz.cpp IS NOT IT
// -------------------------------------------------------------------
// tests/test_font_fuzz.cpp is a GoogleTest, and — more importantly — it is a
// FONT DICTIONARY harness. Every one of its cases mutates the PDF /Font
// dictionary (bad /Widths, circular /BaseFont refs, invalid /Encoding, ...)
// and drives Document::open + Page::render. That exercises MuPDF's PDF object
// parser. It never puts attacker bytes inside a FONT PROGRAM, so the code that
// actually reads a hostile sfnt table directory is untouched by it.
//
// The uncovered surface is real and specific:
//
//     src/fonts/EmbeddedFontAnalyzer.cpp:278
//         FT_New_Memory_Face(ftLibrary_, fi.fontData.data(), ...)
//
// `fi.fontData` is a /FontFile2 (or /FontFile3) stream loaded verbatim out of
// the opened document. It is 100% attacker-controlled, it is handed straight
// to FreeType, and PDFE then walks the resulting face reading table metrics,
// PANOSE bytes, glyph outlines and charmaps. FreeType's sfnt/CFF/Type1
// backends are a historically rich CVE surface (CVE-2020-15999, the sfnt
// table-directory family, the CFF interpreter family), and nothing in this
// repository was fuzzing them.
//
// TWO ARMS, BOTH DRIVEN ON EVERY INPUT
// ------------------------------------
//   (A) DIRECT — the fuzz bytes are written to a file and passed to
//       EmbeddedFontAnalyzer::analyzeFromFile, i.e. FT_New_Face on hostile
//       bytes plus the whole downstream metric/coverage walk.
//   (B) WRAPPED — the fuzz bytes are embedded as the /FontFile2 stream of a
//       synthesised 1-page PDF, opened with Document::open, and analysed with
//       analyzePageFonts, i.e. the exact production path at
//       EmbeddedFontAnalyzer.cpp:278 including the PDF stream extraction.
// Both run for every input rather than one being chosen by a selector byte,
// so the mutator cannot starve either arm.
//
// THE SELF-CHECK — the lesson carried over from fuzz_image_decode (F4)
// --------------------------------------------------------------------
// fuzz_image_decode had to enable the #50 JPX gate and then READ THE FLAG BACK,
// aborting if it did not take, because a harness that silently fuzzes a
// placeholder manufactures a clean campaign report out of nothing. The same
// failure mode exists here in a different shape: if the analyzer never reaches
// FT_New_Memory_Face (a refactor moves the call, /FontFile2 extraction breaks,
// FreeType is not initialised), every input would be "handled" and the campaign
// would look perfect while fuzzing an early return.
//
// So before the first input is processed, this harness runs a KNOWN-GOOD font
// program — MuPDF's built-in base-14 Helvetica, obtained via
// fz_lookup_base14_font, so no fixture file is required — through BOTH arms and
// requires each to produce a valid FontMetricProfile. If either arm fails, the
// harness aborts LOUDLY on stderr instead of running a meaningless campaign.
#include "fuzz_common.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontMetricProfile.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

namespace {

// ---------------------------------------------------------------
// Write `bytes` to a uniquely named temp file. Returned path is empty on
// failure. Mirrors fuzz_support.h's write_temp_pdf, but for font programs
// (extension matters to nothing here, but keeps the temp dir readable).
// ---------------------------------------------------------------
std::filesystem::path write_temp_font(const std::uint8_t* data, std::size_t size)
{
    static std::atomic<std::uint64_t> counter{0};
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    const std::uint64_t n = counter.fetch_add(1);
    std::filesystem::path p =
        dir / ("pdfe_fuzz_fontprog_" + std::to_string(n) + ".bin");
    std::ofstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    if (size) f.write(reinterpret_cast<const char*>(data),
                      static_cast<std::streamsize>(size));
    f.close();
    return p;
}

struct TempFontFile {
    explicit TempFontFile(const std::uint8_t* d, std::size_t n)
        : path_(write_temp_font(d, n)) {}
    ~TempFontFile()
    {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }
    TempFontFile(const TempFontFile&) = delete;
    TempFontFile& operator=(const TempFontFile&) = delete;
    const std::filesystem::path& path() const { return path_; }
    bool valid() const { return !path_.empty(); }

private:
    std::filesystem::path path_;
};

// ---------------------------------------------------------------
// Synthesise a 1-page PDF whose single TrueType font carries `program` as its
// /FontFile2 stream. No xref offsets are emitted (startxref 0); MuPDF's repair
// path rescans the objects — the same idiom used by fuzz_image_decode and
// pdfe::fuzz::build_encrypted_pdf.
// ---------------------------------------------------------------
std::string build_font_pdf(const std::string& program)
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
    out += "5 0 obj\n<< /Type /Font /Subtype /TrueType /BaseFont /FuzzFont "
           "/FirstChar 32 /LastChar 126 /Encoding /WinAnsiEncoding "
           "/FontDescriptor 6 0 R >>\nendobj\n";
    out += "6 0 obj\n<< /Type /FontDescriptor /FontName /FuzzFont /Flags 32 "
           "/ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 "
           "/StemV 80 /FontBBox [0 -200 1000 800] /FontFile2 7 0 R >>\nendobj\n";
    out += "7 0 obj\n<< /Length " + std::to_string(program.size()) +
           " /Length1 " + std::to_string(program.size()) + " >>\nstream\n";
    out += program;
    out += "\nendstream\nendobj\n";
    out += "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

// ---------------------------------------------------------------
// Arm (A): fuzz bytes -> file -> FT_New_Face + full metric walk.
// Returns true when a profile came back (only used by the self-check).
// ---------------------------------------------------------------
bool run_direct_arm(pdfe::core::MuPdfContext& ctx,
                    const std::uint8_t* data, std::size_t size)
{
    TempFontFile f(data, size);
    if (!f.valid()) return false;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&ctx);
    try {
        auto profile = analyzer.analyzeFromFile(f.path().string());
        return profile.has_value();
    } catch (const std::exception& e) {
        pdfe::fuzz::triage_or_rethrow(e);   // benign refusal is swallowed
        return false;
    }
}

// ---------------------------------------------------------------
// Arm (B): fuzz bytes -> /FontFile2 -> Document::open ->
//          analyzePageFonts -> FT_New_Memory_Face (the production path).
// Returns the number of profiles produced (only used by the self-check).
// ---------------------------------------------------------------
std::size_t run_wrapped_arm(pdfe::core::MuPdfContext& ctx,
                            const std::uint8_t* data, std::size_t size)
{
    const std::string program(reinterpret_cast<const char*>(data), size);
    const std::string pdf = build_font_pdf(program);
    pdfe::fuzz::TempPdfFile tmp(reinterpret_cast<const std::uint8_t*>(pdf.data()),
                                pdf.size());
    if (!tmp.valid()) return 0;

    try {
        auto shared = std::shared_ptr<pdfe::core::MuPdfContext>(
            &ctx, [](pdfe::core::MuPdfContext*) {});   // non-owning
        auto doc = pdfe::core::Document::open(shared, tmp.path());
        if (!doc || doc->pageCount() < 1) return 0;
        auto page = doc->page(0);
        if (!page) return 0;
        pdfe::fonts::EmbeddedFontAnalyzer analyzer(&ctx);
        return analyzer.analyzePageFonts(page.get(), doc.get()).size();
    } catch (const std::exception& e) {
        pdfe::fuzz::triage_or_rethrow(e);
        return 0;
    }
}

// ---------------------------------------------------------------
// The self-check. See the header comment: a harness that cannot reach
// FreeType must refuse to run rather than report a clean campaign.
// ---------------------------------------------------------------
void ensure_font_path_live()
{
    static const bool kLive = [] {
        pdfe::core::MuPdfContext ctx;

        // MuPDF's built-in base-14 Helvetica is a real, valid sfnt font
        // program available without any fixture file.
        int len = 0;
        const unsigned char* base14 = nullptr;
        ctx.execute([&](fz_context* c) {
            base14 = fz_lookup_base14_font(c, "Helvetica", &len);
        });
        if (!base14 || len <= 0) {
            std::fprintf(stderr,
                "\n!!! fuzz_font_program: FATAL — fz_lookup_base14_font returned "
                "no Helvetica program; the self-check cannot run.\n\n");
            std::fflush(stderr);
            std::abort();
        }

        const bool directOk =
            run_direct_arm(ctx, base14, static_cast<std::size_t>(len));
        const bool wrappedOk =
            run_wrapped_arm(ctx, base14, static_cast<std::size_t>(len)) > 0;

        if (!directOk || !wrappedOk) {
            std::fprintf(stderr,
                "\n"
                "!!! fuzz_font_program: FATAL — the font-program path is NOT\n"
                "!!! reachable from this harness (direct arm ok=%d, wrapped arm\n"
                "!!! ok=%d) even for a KNOWN-GOOD base-14 font program.\n"
                "!!! That means FT_New_Face / FT_New_Memory_Face is never\n"
                "!!! reached and the campaign would fuzz an early return while\n"
                "!!! reporting a clean night. Refusing to run.\n"
                "!!! Check src/fonts/EmbeddedFontAnalyzer.cpp (analyzeFromFile /\n"
                "!!! analyzePageFonts) and the /FontFile2 extraction.\n\n",
                static_cast<int>(directOk), static_cast<int>(wrappedOk));
            std::fflush(stderr);
            std::abort();
        }
        return true;
    }();
    (void)kLive;
}

} // namespace

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data, std::size_t size)
{
    ensure_font_path_live();

    // A font program smaller than an sfnt header cannot reach any interesting
    // FreeType code; skipping keeps the corpus dense.
    if (size < 12) return;
    // Bound the input so a mutator cannot turn the campaign into an I/O test.
    if (size > (4u << 20)) return;

    pdfe::core::MuPdfContext ctx;
    (void)run_direct_arm(ctx, data, size);
    (void)run_wrapped_arm(ctx, data, size);
}
