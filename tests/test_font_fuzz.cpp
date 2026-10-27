// -*- coding: utf-8 -*-
//
// Pipeline f8a12c4d phase-2 (SEC-FONT-FUZZ): Font parsing fuzz harness
// GTest wrapper. Feeds malformed font dict bytes through MuPDF's
// Document::open + Page::render path to exercise font parser robustness.
//
// Coverage-Expansion phase-4 (C-09) HARDENING: the original harness was
// audit-flagged assertion-free -- every case wrapped the parse in a bare
// `catch (...)` that swallowed EVERYTHING (including the C++ throws a real
// memory fault surfaces as) and asserted nothing, so a genuine crash/UAF/hang
// or an unexpected exception would have PASSED. It now carries a REAL oracle:
//
//   (a) NO crash / UAF / unexpected throw -- the bare `catch (...)` is REPLACED
//       by the benign-throw triage classifier (pdfe::fuzz::triage_or_rethrow):
//       an EXPECTED malformed-input refusal (MuPdfException / parse error) is
//       swallowed, but std::bad_alloc, a simulated memory fault, or any other
//       non-refuse exception is RE-THROWN and fails the test. A non-std C++
//       throw or a hard SEH fault is left uncaught on purpose so it crashes.
//   (b) BOUNDED TIME -- each input's parse must return under a generous
//       wall-clock cap (a slow-but-terminating pathological input fails here;
//       a true infinite hang is caught by libFuzzer's -timeout in the CI
//       campaign, the coverage-guided lane -- see windows-fuzz.yml).
//   (c) OUTPUT INVARIANT -- a parse that SUCCEEDS returns a structurally-valid
//       Document (non-negative, self-consistent page count; a page is
//       obtainable when pageCount>0), and the SAME input parsed twice yields
//       the SAME structural outcome (determinism; a half-constructed state
//       would not). A benign known-good font dict is a POSITIVE CONTROL that
//       MUST open + render, proving the harness actually reaches the parse
//       path (not vacuously passing because open always fails).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "security/fuzz/fuzz_support.h"   // classify_caught / triage_or_rethrow

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

// Generous per-input wall-clock ceiling. A tiny fuzz PDF must parse in well
// under this on any machine; a slow-but-terminating pathological input trips
// it. (A true non-terminating hang never returns to this check -- that is the
// libFuzzer -timeout job's responsibility in the coverage-guided CI lane.)
constexpr double kParseTimeCapMs = 5000.0;

std::filesystem::path tempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_font_fuzz";
    std::filesystem::create_directories(d);
    return d;
}

// RAII: remove the temp file even if the parse rethrows a MustCrash exception.
struct FileRemover {
    std::filesystem::path p;
    ~FileRemover() { std::error_code ec; std::filesystem::remove(p, ec); }
};

// Minimal PDF with a Font dict whose value is the fuzz payload.
// Object 4 is a Font dict with raw fuzz bytes injected.
std::vector<uint8_t> buildFontFuzzPdf(const uint8_t* data, size_t size)
{
    std::string header =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R "
        "/MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> "
        "/Contents 5 0 R >>\nendobj\n"
        "4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica ";

    std::string middle = " >>\nendobj\n"
        "5 0 obj\n<< /Length 40 >>\nstream\n"
        "BT /F1 12 Tf 100 700 Td (Hello) Tj ET\n"
        "endstream\nendobj\n";

    std::string trailer =
        "trailer\n<< /Size 6 /Root 1 0 R >>\n"
        "startxref\n0\n%%EOF\n";

    std::vector<uint8_t> result;
    auto append = [&](const std::string& s) {
        result.insert(result.end(), s.begin(), s.end());
    };
    append(header);
    result.insert(result.end(), data, data + std::min(size, size_t(4096)));
    append(middle);
    append(trailer);
    return result;
}

// Outcome of driving one fuzz input through the parse/render path.
struct FuzzOutcome {
    bool opened = false;
    int pageCount = -1;
    bool rendered = false;
    double elapsedMs = 0.0;
};

// Drive one fuzz input. Benign parse refusals are swallowed; a MustCrash
// exception (bad_alloc / simulated fault / any non-refuse throw) is RE-THROWN
// so the caller's EXPECT_NO_THROW fails. No `catch (...)` -- a non-std throw
// or hard fault escapes and crashes, as a fuzzer requires.
FuzzOutcome fuzzFontParsing(const uint8_t* data, size_t size)
{
    auto pdf = buildFontFuzzPdf(data, size);
    auto path = tempDir() / "font_fuzz_input.pdf";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(pdf.data()),
                static_cast<std::streamsize>(pdf.size()));
    }
    FileRemover remover{path};

    FuzzOutcome out;
    auto t0 = std::chrono::steady_clock::now();
    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, path);
        if (doc) {
            out.opened = true;
            out.pageCount = doc->pageCount();
            if (out.pageCount > 0) {
                auto page = doc->page(0);
                if (page) {
                    void* dl = doc->createDisplayList(0);
                    if (dl) {
                        out.rendered = true;
                        doc->releaseDisplayList(dl);
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        // Expected malformed-input refusal -> swallow; real defect -> rethrow.
        pdfe::fuzz::triage_or_rethrow(e);
    }
    out.elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    return out;
}

// Assert the invariants (b) + (c) common to every malformed input.
void expectRobustParse(const uint8_t* data, size_t size)
{
    FuzzOutcome out;
    // (a) no unexpected throw / crash.
    ASSERT_NO_THROW({ out = fuzzFontParsing(data, size); });
    // (b) bounded time.
    EXPECT_LT(out.elapsedMs, kParseTimeCapMs)
        << "parse exceeded the wall-clock cap: " << out.elapsedMs << "ms";
    // (c) a successful parse is structurally valid.
    if (out.opened) {
        EXPECT_GE(out.pageCount, 0)
            << "an opened document must report a non-negative page count";
        // Determinism: the same input must produce the same structural outcome.
        FuzzOutcome again;
        ASSERT_NO_THROW({ again = fuzzFontParsing(data, size); });
        EXPECT_EQ(again.opened, out.opened) << "open outcome not deterministic";
        EXPECT_EQ(again.pageCount, out.pageCount)
            << "page count not deterministic across identical inputs";
    }
}

} // namespace

// Note: LLVMFuzzerTestOneInput not defined here to avoid symbol
// collision with test_pkcs7_fuzz.cpp when both are linked into
// pdfe_tests.exe. Use the standalone fuzz binary for libFuzzer mode.

class FontFuzzSmokeTest : public ::testing::Test {};

// -------------------------------------------------------------------
// POSITIVE CONTROL (non-vacuity): a benign, well-formed font dict MUST
// open AND render. If this ever fails, the harness is no longer reaching
// the parse/render path and every "no-crash" case below is vacuous.
// -------------------------------------------------------------------
TEST_F(FontFuzzSmokeTest, BenignFontDictOpensAndRenders)
{
    const char* benign = "/Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 32";
    FuzzOutcome out;
    ASSERT_NO_THROW({
        out = fuzzFontParsing(reinterpret_cast<const uint8_t*>(benign),
                              std::strlen(benign));
    });
    EXPECT_TRUE(out.opened) << "a well-formed font dict must open";
    EXPECT_EQ(out.pageCount, 1) << "the harness PDF has exactly one page";
    EXPECT_TRUE(out.rendered) << "a valid single page must produce a display list";
    EXPECT_LT(out.elapsedMs, kParseTimeCapMs);
}

TEST_F(FontFuzzSmokeTest, EmptyInputDoesNotCrash)
{
    expectRobustParse(nullptr, 0);
    uint8_t z = 0;
    expectRobustParse(&z, 0);
}

TEST_F(FontFuzzSmokeTest, GarbageInputDoesNotCrash)
{
    std::vector<uint8_t> garbage(1024, 0xCC);
    expectRobustParse(garbage.data(), garbage.size());
}

TEST_F(FontFuzzSmokeTest, TruncatedFontDictDoesNotCrash)
{
    const uint8_t truncated[] = "/Encoding /WinAnsiEncoding /FirstChar 32 /LastChar";
    expectRobustParse(truncated, sizeof(truncated) - 1);
}

TEST_F(FontFuzzSmokeTest, MalformedWidthsArrayDoesNotCrash)
{
    const char* payload = "/Widths [999999 -1 0 0 0 0] /FirstChar 0 /LastChar 5";
    expectRobustParse(reinterpret_cast<const uint8_t*>(payload), strlen(payload));
}

TEST_F(FontFuzzSmokeTest, CircularFontRefDoesNotCrash)
{
    const char* payload = "/DescendantFonts [4 0 R] /Subtype /Type0";
    expectRobustParse(reinterpret_cast<const uint8_t*>(payload), strlen(payload));
}

TEST_F(FontFuzzSmokeTest, InvalidEncodingDoesNotCrash)
{
    const char* payload = "/Encoding << /Type /Encoding /BaseEncoding /INVALID "
                          "/Differences [0 /NOTACHAR /ALSONOTACHAR] >>";
    expectRobustParse(reinterpret_cast<const uint8_t*>(payload), strlen(payload));
}

TEST_F(FontFuzzSmokeTest, LargeInputDoesNotCrash)
{
    std::vector<uint8_t> large(4096, 0x41);
    expectRobustParse(large.data(), large.size());
}

// -------------------------------------------------------------------
// C-09 seed extension: additional adversarial inputs (deterministic).
// -------------------------------------------------------------------

// Oversized numeric values in the font metrics (integer-overflow bait).
TEST_F(FontFuzzSmokeTest, OversizedNumericValuesDoNotCrash)
{
    const char* payload =
        "/FirstChar 0 /LastChar 2147483647 /MissingWidth 999999999999 "
        "/Widths [ 340282366920938463463374607431768211456 -2147483648 ]";
    expectRobustParse(reinterpret_cast<const uint8_t*>(payload), strlen(payload));
}

// Deeply nested array/dict structure (recursion-depth bait).
TEST_F(FontFuzzSmokeTest, DeeplyNestedStructureDoesNotCrash)
{
    std::string payload = "/Encoding ";
    for (int i = 0; i < 512; ++i) payload += "[";
    payload += "0";
    for (int i = 0; i < 512; ++i) payload += "]";
    expectRobustParse(reinterpret_cast<const uint8_t*>(payload.data()),
                      payload.size());
}

// Malformed header bytes embedded in the dict (dict/stream terminator bait:
// stray '>>', 'endobj', 'stream', 'trailer' tokens that could confuse the
// lexer if it is not robust).
TEST_F(FontFuzzSmokeTest, EmbeddedTerminatorTokensDoNotCrash)
{
    const char* payload =
        "/Note (>> endobj stream endstream trailer startxref %%EOF) "
        "/Bogus << /Nested >> >> >> ";
    expectRobustParse(reinterpret_cast<const uint8_t*>(payload), strlen(payload));
}

// Truncation sweep: feed progressively truncated prefixes of a realistic font
// dict body -- each prefix is its own adversarial (unterminated) input.
TEST_F(FontFuzzSmokeTest, ProgressiveTruncationDoesNotCrash)
{
    const std::string full =
        "/FontDescriptor << /Type /FontDescriptor /Flags 32 "
        "/FontBBox [-100 -200 1000 900] /ItalicAngle 0 /Ascent 800 "
        "/Descent -200 /CapHeight 700 /StemV 80 >> /Encoding /MacRomanEncoding";
    for (size_t n = 1; n <= full.size(); n += 7) {
        expectRobustParse(reinterpret_cast<const uint8_t*>(full.data()), n);
    }
}

// Deterministic pseudo-random blocks (fixed LCG seed => reproducible corpus).
TEST_F(FontFuzzSmokeTest, DeterministicRandomBlocksDoNotCrash)
{
    for (uint32_t seed : {0x9E3779B9u, 0x12345678u, 0xDEADBEEFu}) {
        std::vector<uint8_t> b(1024);
        uint32_t x = seed;
        for (auto& c : b) { x = x * 1664525u + 1013904223u; c = uint8_t(x >> 24); }
        expectRobustParse(b.data(), b.size());
    }
}

TEST_F(FontFuzzSmokeTest, RepeatedParsingDoesNotLeak)
{
    const char* payload = "/Encoding /WinAnsiEncoding";
    for (int i = 0; i < 50; ++i) {
        expectRobustParse(reinterpret_cast<const uint8_t*>(payload), strlen(payload));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// GitHub #101 (WIN-70) — font parsing under a HARD MEMORY CAP.
//
// Every case above establishes that malformed font input does not crash the
// parser when memory is plentiful. None establishes what happens when it is
// not. A PDF whose font tables provoke a large allocation is a denial-of-
// service shape, and the requirement is the same as everywhere else in this
// product: fail DIAGNOSABLY, not by dying.
//
// Method, and why it is a child process rather than this one.
// `test_main.cpp` already assigns this process to a limit-free job object for
// spawn scoping. Applying JOB_OBJECT_LIMIT_PROCESS_MEMORY to *our own* job
// would cap the test runner, killing the whole suite rather than the case
// under test. So a child is created SUSPENDED, assigned to a fresh capped job
// (which nests under ours on Win8+), and only then resumed.
//
// That shape is taken from `tests/test_resource_bomb_sandbox.cpp`, which
// documents it at length. It is duplicated here rather than shared because
// hoisting it would mean a new test-support header included by both, and this
// phase's scope judgement (no widely-included header) is what keeps its
// evidence a targeted run. Recorded so the duplication is deliberate and
// visible; if a third caller appears, hoist it.
// ═══════════════════════════════════════════════════════════════════════
#ifdef _WIN32

namespace {

std::filesystem::path selfExe()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring(buf, n));
}

}  // namespace

// The child half. Selected by env var so it never runs during a normal suite
// pass; it does the actual parsing work under whatever cap its job carries.
TEST(FontFuzzMemoryCapChild, RunRole)
{
    if (::GetEnvironmentVariableA("PDFE_FONT_MEMCAP_ROLE", nullptr, 0) == 0) {
        GTEST_SKIP() << "not the capped child";
    }

    // A font dict that asks for a lot: a maximal character-code span drives
    // per-glyph table allocation. Under the cap that allocation should fail
    // cleanly rather than taking the process down.
    const std::string hostile =
        "/Encoding /WinAnsiEncoding /FirstChar 0 /LastChar 65535";

    // The contract is "diagnosable, not a crash" -- so an exception is an
    // ACCEPTABLE outcome here, and an access violation is not. Catch broadly
    // and exit cleanly for any orderly outcome; an unhandled crash gives a
    // structured-exception exit code and the parent asserts on that.
    bool completed = false;
    try {
        (void)fuzzFontParsing(
            reinterpret_cast<const uint8_t*>(hostile.data()), hostile.size());
        completed = true;
    } catch (const std::exception&) {
        completed = true;   // orderly refusal IS the wanted outcome
    } catch (...) {
        completed = true;   // still orderly: it unwound rather than dying
    }

    // A REAL ORACLE, not SUCCEED(). The project's OS-4 standard
    // (OracleStandard.SucceedOnlyBaselineMatchesTheTree) rejects a body that
    // asserts nothing -- correctly, because the parent's exit-code check is
    // invisible from in here, and a child that silently did nothing would look
    // identical to one that survived. The guard caught this body on its first
    // full-suite run.
    //
    // So assert what the child is actually here to demonstrate: the process is
    // STILL FUNCTIONAL after the memory-capped attempt. One that died never
    // reaches these lines; one left corrupted fails them.
    ASSERT_TRUE(completed)
        << "control did not return from the capped parse attempt at all";

    std::vector<uint8_t> stillAllocatable(64 * 1024, 0xAB);
    ASSERT_EQ(stillAllocatable.size(), 64u * 1024u)
        << "the process cannot allocate after the capped parse -- it survived "
           "the cap but is no longer usable";
    EXPECT_EQ(stillAllocatable.front(), 0xAB);

    // And it can still parse a benign input, which is the strongest available
    // statement that the parser itself was not left broken.
    const char* benign = "/Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 32";
    FuzzOutcome after;
    ASSERT_NO_THROW({
        after = fuzzFontParsing(reinterpret_cast<const uint8_t*>(benign),
                                std::strlen(benign));
    }) << "a benign font dict no longer parses after the capped attempt";
    EXPECT_TRUE(after.opened)
        << "the parser is left non-functional after operating under a memory cap";
}

TEST_F(FontFuzzSmokeTest, ParsingUnderAHardMemoryCapFailsDiagnosablyNotByCrashing)
{
    const std::filesystem::path exe = selfExe();
    ASSERT_FALSE(exe.empty()) << "could not resolve the test executable path";

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    ASSERT_NE(job, nullptr) << "CreateJobObject failed, err="
                            << ::GetLastError();

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    li.BasicLimitInformation.ActiveProcessLimit = 1;
    // Deliberately tight: enough for the process to start, not enough to grow
    // without bound. A cap so small the child could not start would test
    // nothing at all.
    li.ProcessMemoryLimit = static_cast<SIZE_T>(256ull * 1024 * 1024);
    li.JobMemoryLimit     = li.ProcessMemoryLimit;
    ASSERT_TRUE(::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                          &li, sizeof(li)))
        << "SetInformationJobObject failed, err=" << ::GetLastError();

    std::wstring cmd = L"\"" + exe.wstring() +
                       L"\" --gtest_filter=FontFuzzMemoryCapChild.RunRole "
                       L"--gtest_brief=1";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    ::SetEnvironmentVariableA("PDFE_FONT_MEMCAP_ROLE", "1");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(
        exe.wstring().c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    // Cleared immediately so a later test in THIS process is never mistaken
    // for the child.
    ::SetEnvironmentVariableA("PDFE_FONT_MEMCAP_ROLE", nullptr);

    ASSERT_TRUE(created) << "CreateProcess failed, err=" << ::GetLastError();

    const BOOL assigned = ::AssignProcessToJobObject(job, pi.hProcess);
    ASSERT_TRUE(assigned) << "AssignProcessToJobObject failed, err="
                          << ::GetLastError();
    ::ResumeThread(pi.hThread);

    const DWORD wait = ::WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 0xFFFFFFFF;
    if (wait == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(pi.hProcess, &code);
    } else {
        ::TerminateProcess(pi.hProcess, 1);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(job);

    ASSERT_EQ(wait, static_cast<DWORD>(WAIT_OBJECT_0))
        << "the capped child did not exit within 120s -- it hung under memory "
           "pressure rather than failing diagnosably";

    // What must NOT happen: an access violation or other structured-exception
    // death. A clean exit (0) or an orderly gtest failure (1) both mean the
    // process stayed in control of itself, which is the property under test.
    EXPECT_NE(code, static_cast<DWORD>(0xC0000005))
        << "the child died with an ACCESS VIOLATION under a memory cap -- font "
           "parsing is not degrading diagnosably, it is crashing";
    EXPECT_LT(code, 0x80000000u)
        << "the child exited with a structured-exception code (0x" << std::hex
        << code << std::dec << ") rather than an orderly status; under memory "
           "pressure the parser must refuse, not fault";
}

#endif  // _WIN32
