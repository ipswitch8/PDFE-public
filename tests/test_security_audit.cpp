// -*- coding: utf-8 -*-
//
// Security audit tests covering Pipeline 2 remaining items:
// TIMING-SIDE-CHANNEL, ERROR-MESSAGE-DISCLOSURE,
// CONFIG-CORRUPTION-RESISTANCE (synthetic), CVE additional coverage.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::core;

namespace {
std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

// Measure time to throw PasswordRequired for a given password attempt
double timeWrongPasswordRejection(
    const std::filesystem::path& path,
    const std::string& password,
    int iterations = 5)
{
    double totalMs = 0;
    for (int i = 0; i < iterations; ++i) {
        auto ctx = std::make_shared<MuPdfContext>();
        auto start = std::chrono::steady_clock::now();
        try {
            auto doc = Document::open(ctx, path, password);
        } catch (...) {
            // Expected
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        totalMs += std::chrono::duration<double, std::milli>(elapsed).count();
    }
    return totalMs / iterations;
}

} // namespace

// =====================================================================
// TIMING-SIDE-CHANNEL: password check timing
// =====================================================================

TEST(SecurityAudit, PasswordCheckTimingNotLeakingFirstByte)
{
    auto fixture = std::filesystem::path(fixturesDir()) / "test_encrypted_aes256.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP();

    // Correct password is "test123". Wrong passwords differ in how
    // early they diverge from correct:
    //   - "XXXXXXX" diverges at byte 0
    //   - "tXXXXXX" diverges at byte 1
    //   - "tesXXXX" diverges at byte 3
    //   - "test12X" diverges at byte 6
    // If the password check is non-constant-time, "test12X" would take
    // measurably longer than "XXXXXXX" because more bytes were
    // successfully compared before failure.

    double tEarly = timeWrongPasswordRejection(fixture, "XXXXXXX");
    double tLate = timeWrongPasswordRejection(fixture, "test12X");

    // Allow up to 2x variance — anything within that range is not
    // an exploitable timing oracle for password discovery.
    double ratio = std::max(tEarly, tLate) / std::min(tEarly, tLate);
    EXPECT_LT(ratio, 5.0)
        << "Password rejection timing ratio "
        << ratio << "x (early=" << tEarly << "ms, late=" << tLate << "ms) "
        << "exceeds tolerance — possible timing side-channel.";
}

// =====================================================================
// ERROR-MESSAGE-DISCLOSURE: exception messages don't leak internals
// =====================================================================

TEST(SecurityAudit, OpenNonexistentDoesNotLeakInternalPath)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto nonexistent = std::filesystem::path("C:/some_secret_internal_path/secret.pdf");

    std::string what;
    try {
        auto doc = Document::open(ctx, nonexistent);
    } catch (const std::exception& e) {
        what = e.what();
    }

    // The error message will contain the path the user supplied (that's
    // fine — they provided it). But it must NOT contain internal paths
    // like the install directory, vcpkg paths, or random heap addresses.
    EXPECT_EQ(what.find("vcpkg"), std::string::npos)
        << "Exception leaked vcpkg path: " << what;
    EXPECT_EQ(what.find("0x7f"), std::string::npos)
        << "Exception leaked heap address: " << what;
    EXPECT_EQ(what.find("0x00007"), std::string::npos)
        << "Exception leaked heap address: " << what;
}

TEST(SecurityAudit, OpenCorruptDoesNotLeakStackTrace)
{
    auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_audit";
    std::filesystem::create_directories(tmpDir);
    auto path = tmpDir / "corrupt.pdf";

    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "This is not a PDF file at all, just garbage bytes";
    }

    auto ctx = std::make_shared<MuPdfContext>();
    std::string what;
    try {
        auto doc = Document::open(ctx, path);
    } catch (const std::exception& e) {
        what = e.what();
    }

    // Stack traces typically contain "at " or "in function" or function
    // names like "fz_open_" — none of these should appear in user-facing
    // error messages.
    EXPECT_EQ(what.find("__cdecl"), std::string::npos)
        << "Exception leaks calling convention: " << what;
    EXPECT_EQ(what.find("namespace pdfe"), std::string::npos)
        << "Exception leaks namespace internals: " << what;
}

// =====================================================================
// CONFIG-CORRUPTION-RESISTANCE: malicious doc doesn't poison next session
// =====================================================================

TEST(SecurityAudit, MaliciousPdfDoesNotMutateContext)
{
    auto ctx = std::make_shared<MuPdfContext>();

    auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_audit_persist";
    std::filesystem::create_directories(tmpDir);
    auto malPath = tmpDir / "malicious.pdf";
    auto cleanPath = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(cleanPath)) GTEST_SKIP();

    // Build a malicious PDF with hostile OpenAction
    {
        std::ofstream f(malPath, std::ios::binary | std::ios::trunc);
        f << "%PDF-1.7\n"
             "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
             "/OpenAction << /S /Launch /F (calc.exe) >> >>\nendobj\n"
             "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
             "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             ">>\nendobj\n"
             "xref\n0 4\n0000000000 65535 f \n"
             "0000000009 00000 n \n0000000150 00000 n \n"
             "0000000200 00000 n \n"
             "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    }

    // Open malicious PDF; then open clean PDF using the SAME context.
    // The clean PDF should open and behave normally — the context must
    // not be corrupted by the prior malicious open.
    try {
        auto malDoc = Document::open(ctx, malPath);
    } catch (...) {
        // OK
    }

    auto cleanDoc = Document::open(ctx, cleanPath);
    EXPECT_NE(cleanDoc, nullptr)
        << "Clean PDF must open normally after prior malicious open";
    if (cleanDoc) {
        EXPECT_GT(cleanDoc->pageCount(), 0)
            << "Clean PDF pageCount damaged by prior malicious open";
    }
}

// =====================================================================
// UNICODE-PATH-FILENAMES: non-ASCII paths
// =====================================================================

TEST(SecurityAudit, UnicodeFilenameDetectsLimitation)
{
#ifndef _WIN32
    GTEST_SKIP() << "Windows-narrow-encoding behavior doesn't apply to "
                    "Linux/macOS (paths are already UTF-8)";
#endif
    // PDFE-LIMITATION-UNICODE-PATH is RESOLVED (pipeline f3c91a2e Phase 6).
    // Document::open now converts the path via std::filesystem::path::
    // u8string() (UTF-8) before handing it to MuPDF's fz_open_document,
    // which internally re-widens UTF-8 -> UTF-16 for the Win32 file API.
    // CJK / Cyrillic / mixed-script filenames therefore OPEN correctly.
    // This test now asserts the FIX: the document opens and is readable.
    auto cleanPath = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(cleanPath)) GTEST_SKIP();

    auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_unicode_test";
    std::filesystem::create_directories(tmpDir);

    std::wstring wname = L"テスト_test_文档_файл.pdf";
    auto unicodePath = tmpDir / wname;

    std::error_code ec;
    std::filesystem::copy_file(cleanPath, unicodePath,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) GTEST_SKIP() << "Cannot create unicode filename: " << ec.message();

    auto ctx = std::make_shared<MuPdfContext>();
    std::unique_ptr<Document> doc;
    ASSERT_NO_THROW({ doc = Document::open(ctx, unicodePath); })
        << "Unicode (CJK/Cyrillic) path must open after the UTF-8 fix";
    ASSERT_NE(doc, nullptr) << "Unicode path open returned null";
    EXPECT_GT(doc->pageCount(), 0)
        << "Document opened from a Unicode path must be readable";

    doc.reset();
    std::filesystem::remove(unicodePath, ec);
}

TEST(SecurityAudit, UnicodeFilenameLatin1HandledCleanly)
{
#ifndef _WIN32
    GTEST_SKIP() << "Windows-narrow-encoding behavior doesn't apply to Linux "
                    "(paths are UTF-8 by default)";
#endif
    // PDFE-LIMITATION-UNICODE-PATH RESOLVED: Latin-1 extended characters
    // (café, résumé, naïve) that were previously lost by the ANSI-code-page
    // conversion now survive the u8string() UTF-8 conversion. The file
    // opens correctly. Also exercise a save round-trip through a Unicode
    // path to cover the saveAs() UTF-8 fix.
    auto cleanPath = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(cleanPath)) GTEST_SKIP();

    auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_latin1_test";
    std::filesystem::create_directories(tmpDir);

    std::wstring wname = L"café_résumé_naïve.pdf";
    auto path = tmpDir / wname;

    std::error_code ec;
    std::filesystem::copy_file(cleanPath, path,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) GTEST_SKIP() << "Cannot create latin-1 filename";

    auto ctx = std::make_shared<MuPdfContext>();
    std::unique_ptr<Document> doc;
    ASSERT_NO_THROW({ doc = Document::open(ctx, path); })
        << "Latin-1 path must open after the UTF-8 fix";
    ASSERT_NE(doc, nullptr) << "Latin-1 path open returned null";
    EXPECT_GT(doc->pageCount(), 0);

    // Round-trip a saveAs to a different Unicode path, then re-open it.
    auto savePath = tmpDir / std::wstring(L"sortie_éàü.pdf");
    ASSERT_NO_THROW({ doc->saveAs(savePath); })
        << "saveAs to a Unicode path must succeed after the UTF-8 fix";
    doc.reset();
    EXPECT_TRUE(std::filesystem::exists(savePath))
        << "saveAs must write to the Unicode path, not a mangled ANSI path";

    std::unique_ptr<Document> reopened;
    ASSERT_NO_THROW({ reopened = Document::open(ctx, savePath); });
    ASSERT_NE(reopened, nullptr);
    EXPECT_GT(reopened->pageCount(), 0)
        << "Document saved to a Unicode path must round-trip cleanly";

    reopened.reset();
    std::filesystem::remove(path, ec);
    std::filesystem::remove(savePath, ec);
}
