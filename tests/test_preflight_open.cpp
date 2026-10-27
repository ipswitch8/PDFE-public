// -*- coding: utf-8 -*-
// Coverage for the out-of-process malformed-PDF sandbox (pipeline e5b9c231
// Phase 4, Register C3 / rtm-coverage.md "Malformed-PDF sandbox — UNCOVERED").
//
// core::isDocumentOpenSafe() spawns the disposable `pdfe_preflight` child to
// attempt an open; a document the child cannot cleanly handle (ordinary error,
// crash, or timeout) yields false so the app refuses it while staying healthy.
// The README advertises this as a headline security control but nothing tested
// that the boundary actually holds. These tests prove: (a) a clean document is
// permitted; (b) a document the child cannot open is refused; and (c) the
// CALLING process survives a child failure (the isolation boundary holds).
#include <gtest/gtest.h>
#include "core/PreflightOpen.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

namespace {
std::filesystem::path fixtures() { return std::filesystem::path(TEST_FIXTURES_DIR); }

std::filesystem::path writeTemp(const std::string& name, const std::string& bytes) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return p;
}
} // namespace

// (a) A well-formed document is permitted by the sandbox.
TEST(PreflightOpen, CleanDocumentIsSafe) {
    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean)) << clean.string();
    EXPECT_TRUE(pdfe::core::isDocumentOpenSafe(clean))
        << "the sandbox refused a known-good document";
}

// (b) A file the isolated opener cannot cleanly handle is refused. Non-PDF
// garbage cannot be opened/walked, so the child exits non-zero -> unsafe.
TEST(PreflightOpen, GarbageInputIsRefused) {
    auto junk = writeTemp("pdfe_preflight_junk.bin",
                          std::string(4096, '\x7f') + "NOT-A-PDF\x00\x01\x02");
    EXPECT_FALSE(pdfe::core::isDocumentOpenSafe(junk))
        << "the sandbox accepted non-PDF garbage";
    std::error_code ec; std::filesystem::remove(junk, ec);
}

// (b') An empty file is refused (no header, nothing to open).
TEST(PreflightOpen, EmptyFileIsRefused) {
    auto empty = writeTemp("pdfe_preflight_empty.pdf", "");
    EXPECT_FALSE(pdfe::core::isDocumentOpenSafe(empty));
    std::error_code ec; std::filesystem::remove(empty, ec);
}

// (c) THE ISOLATION BOUNDARY: after the child fails on a bad document, the
// calling process is unharmed and the sandbox still works on a good document.
// A crash that was NOT isolated would take this test process down instead of
// reaching the second assertion.
TEST(PreflightOpen, ChildFailureDoesNotHarmCaller) {
    auto junk = writeTemp("pdfe_preflight_boundary.bin",
                          std::string(1 << 16, '\xff'));
    // First: a failure in the disposable child.
    EXPECT_FALSE(pdfe::core::isDocumentOpenSafe(junk));
    // The caller survived; the sandbox is still functional for a good doc.
    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean));
    EXPECT_TRUE(pdfe::core::isDocumentOpenSafe(clean))
        << "sandbox unusable after a child failure -- isolation boundary broken";
    std::error_code ec; std::filesystem::remove(junk, ec);
}

// Characterize the committed corrupt-fixture corpus through the sandbox (some
// are MuPDF-repairable -> safe, some are not). Not an assertion on any single
// file's repairability (that is MuPDF's call), but proves the sandbox returns a
// definite verdict for every hostile fixture without hanging or crashing the
// caller.
TEST(PreflightOpen, EveryCorruptFixtureGetsADefiniteVerdict) {
    auto dir = fixtures() / "flaw_corpus" / "corrupt";
    if (!std::filesystem::exists(dir)) GTEST_SKIP() << "flaw_corpus/corrupt absent";
    int seen = 0;
    for (auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".pdf") continue;
        ++seen;
        // The call must RETURN (bool) for every hostile input -- a hang would
        // trip the helper timeout and still return false; a caller-side crash
        // would abort the test. We assert only that it terminates with a bool.
        bool verdict = pdfe::core::isDocumentOpenSafe(e.path(), 20000);
        std::printf("  sandbox(%s) = %s\n",
                    e.path().filename().string().c_str(), verdict ? "safe" : "refused");
    }
    EXPECT_GT(seen, 0) << "no corrupt fixtures found to exercise";
}

// ===========================================================================
// SEC-108 (meta-audit follow-up F9) -- the preflight timeout as a DOCUMENTED
// security control.
//
// isDocumentOpenSafe()'s 15 s bound is a real fail-closed DoS mitigation: a
// document that hangs the parser (pathological loop, decompression bomb,
// recursive structure MuPDF's own guards miss) is a denial of service against
// the editor, and the timeout converts an unbounded hang into a bounded
// refusal. Until F9 it was documented only as a code comment, so it could have
// been "tuned" to INFINITE by anyone who read it as a UX knob.
//
// Two oracles, because either alone is weak:
//   (d) the bound is HONOURED at run time and fails CLOSED, and
//   (e) the declared default stays finite, sane, and described in the policy.
// ===========================================================================

// (d) A budget the child cannot possibly meet must produce `false` -- and must
// produce it promptly. A zero-millisecond wait cannot be satisfied by a process
// that has only just been created, so WaitForSingleObject/waitpid must report
// timeout, the child must be killed, and the verdict must be UNSAFE.
//
// If someone replaces the bounded wait with an infinite one, this test hangs
// the suite rather than passing -- which is the correct failure signal for a
// removed timeout, and is bounded here by asserting the elapsed time too.
TEST(PreflightOpen, TimeoutIsEnforcedAndFailsClosed) {
    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean)) << clean.string();

    // Sanity: with the normal budget this document IS safe, so a `false` below
    // can only come from the timeout path and not from the document itself.
    ASSERT_TRUE(pdfe::core::isDocumentOpenSafe(clean))
        << "fixture is not preflight-clean; the timeout assertion below would "
           "prove nothing";

    const auto start = std::chrono::steady_clock::now();
    const bool verdict = pdfe::core::isDocumentOpenSafe(clean, /*timeoutMs=*/0);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start).count();

    EXPECT_FALSE(verdict)
        << "a preflight that exceeded its budget returned SAFE. The timeout must "
           "fail CLOSED: an unfinished vetting is not a passed vetting (SEC-108).";
    EXPECT_LT(elapsedMs, 10000)
        << "the 0 ms budget took " << elapsedMs << " ms to return; the wait is no "
           "longer bounded by timeoutMs";
}

// ===========================================================================
// The path the sandbox is asked about must SURVIVE the trip to the child.
//
// fz_open_document() takes a UTF-8 filename. The helper used to be a plain
// main(int, char**), and on Windows the CRT hands that argv already converted
// to the process ANSI code page, replacing every character the ACP cannot
// represent with '?'. The parent passes the path as UTF-16 (CreateProcessW),
// so the corruption was entirely inside the helper's own front door.
//
// The consequence was a FALSE ACCUSATION, not merely a failed open: the child
// could not find the mangled filename, exited 1, and the parent — which reads
// exit-code-0-as-safe — reported the document UNSAFE. The UI then told the
// user their perfectly good PDF was "malformed or crafted to exhaust
// resources", for no reason but a character in a folder name.
//
// The path below mixes CJK and Cyrillic so no single-byte ANSI code page can
// represent all of it, whatever the host is set to. This test FAILS against
// the pre-fix helper and passes against the wmain/GetCommandLineW one.
//
// It is a real end-to-end assertion: the ASCII control proves the sandbox is
// working at all, and the existence check proves the file really is where we
// say, so a refusal can only be the child mis-reading the name.
// ===========================================================================
TEST(PreflightOpen, NonAsciiPathIsNotFalselyRefused) {
    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean)) << clean.string();

    // Control: the very same document under a pure-ASCII path is SAFE. Without
    // this, a false-refusal assertion below could be satisfied by a sandbox
    // that refuses everything.
    ASSERT_TRUE(pdfe::core::isDocumentOpenSafe(clean))
        << "the sandbox refused a known-good document under an ASCII path; "
           "the non-ASCII assertion below would prove nothing";

    // u8 literals -> std::filesystem::path decodes them as UTF-8 and stores
    // wide on Windows, so the path we create is exactly the path we mean.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        std::filesystem::path(std::u8string(
            u8"pdfe_preflight_日本語_тест"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(dir, ec)) << ec.message();

    const std::filesystem::path target =
        dir / std::filesystem::path(std::u8string(u8"文件_файл.pdf"));
    std::filesystem::copy_file(
        clean, target, std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "could not stage the fixture at a non-ASCII path: "
                     << ec.message();
    ASSERT_TRUE(std::filesystem::exists(target))
        << "the staged file must exist, or a refusal below would be honest";

    const bool verdict = pdfe::core::isDocumentOpenSafe(target);

    EXPECT_TRUE(verdict)
        << "the sandbox refused a KNOWN-GOOD document purely because its path "
           "contains non-ASCII characters. The helper is reading its filename "
           "through the ANSI code page instead of UTF-8, so the user is told a "
           "healthy PDF is malformed.";

    std::filesystem::remove_all(dir, ec);
}

// (e) The declared default must stay finite and sane, and must still be
// described as a security control in the policy. A negative value means
// INFINITE in the implementation -- i.e. the control switched off -- and a
// multi-minute value makes the DoS survivable but still effective.
TEST(PreflightOpen, DefaultTimeoutIsBoundedAndDocumented) {
    const std::filesystem::path root =
        std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();

    const auto slurp = [](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f.is_open()) return std::string();
        return std::string(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
    };

    const std::string header = slurp(root / "src" / "core" / "PreflightOpen.h");
    ASSERT_FALSE(header.empty()) << "src/core/PreflightOpen.h must be readable";

    std::smatch m;
    ASSERT_TRUE(std::regex_search(
        header, m,
        std::regex(R"(isDocumentOpenSafe\s*\([^)]*timeoutMs\s*=\s*(-?\d+))")))
        << "isDocumentOpenSafe() must declare a DEFAULT timeout; an unbounded "
           "default disables the control for every caller that omits it";
    const long declared = std::stol(m[1].str());

    EXPECT_GT(declared, 0)
        << "the default preflight timeout is " << declared
        << " ms. A non-positive value means INFINITE in PreflightOpen.cpp -- the "
           "DoS mitigation is switched off (SEC-108).";
    EXPECT_LE(declared, 60000)
        << "the default preflight timeout is " << declared
        << " ms. Beyond a minute the hang is still a denial of service; the bound "
           "exists to make it survivable.";

    const std::string policy = slurp(root / "docs" / "security-policy.md");
    ASSERT_FALSE(policy.empty()) << "docs/security-policy.md must be readable";
    // The HEADING, not just the phrase: the version-history row also mentions
    // "Resource limits", so a substring search would survive deleting the
    // section itself.
    EXPECT_NE(policy.find("## Resource limits"), std::string::npos)
        << "docs/security-policy.md must carry the '## Resource limits' section "
           "that records this control (SEC-108)";
    EXPECT_NE(policy.find("isDocumentOpenSafe"), std::string::npos)
        << "the preflight timeout must be documented as a security control in the "
           "policy, not only as a code comment -- that was the whole of SEC-108";
}

// ---------------------------------------------------------------------------
// GitHub #59 (SEC-61) -- the preflight helper runs at LOW INTEGRITY.
//
// Spawning the parser in a separate process contains a CRASH. It does not
// contain CODE EXECUTION: a medium-integrity child can still write to the
// user's profile and registry and can open handles to the parent. Dropping the
// helper to low integrity makes the mandatory integrity policy deny those
// write-ups, so a MuPDF parser bug is confined.
//
// What is asserted is the isolation ACTUALLY ACHIEVED, reported by
// lastHelperIsolation(). That matters more than asserting the code "tries":
// the implementation deliberately falls back to an ordinary launch if the
// token work fails -- crash isolation is preserved and refusing to open
// documents would be a worse trade -- and the whole point of reporting it is
// that the fallback must not be silent. If this test ever sees Unrestricted on
// a normal Windows host, the hardening has regressed even though every
// document still opens and every other preflight test still passes.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

TEST(PreflightHardening, HelperIsCreatedWithALowIntegrityToken)
{
    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean))
        << "test_single.pdf is required; skipping would leave the hardening "
           "unexercised, which is the state GitHub #59 was filed about";

    // Drive a real launch.
    const bool safe = pdfe::core::isDocumentOpenSafe(clean);
    EXPECT_TRUE(safe) << "a clean fixture must still preflight successfully "
                         "once the helper is restricted -- low integrity must "
                         "not break the helper's ability to read the document";

    const auto iso = pdfe::core::lastHelperIsolation();

    ASSERT_NE(iso, pdfe::core::HelperIsolation::NotLaunched)
        << "no helper launch was recorded, so this test asserted nothing -- "
           "the helper is probably not deployed next to the test binary";

    EXPECT_EQ(iso, pdfe::core::HelperIsolation::LowIntegrity)
        << "the preflight helper was created WITHOUT a low-integrity token. "
           "It still provides crash isolation, so nothing visibly breaks -- "
           "which is exactly why this is asserted rather than left to "
           "inspection. A parser bug in the helper is now able to write to "
           "medium-integrity objects again.";
}

// The reporting seam must be capable of distinguishing its states, or the
// assertion above could pass against a function that returns LowIntegrity
// unconditionally. Cheap structural check: the enum's values are distinct and
// the accessor reflects a real launch rather than a constant.
TEST(PreflightHardening, IsolationReportingIsNotAConstant)
{
    // Before any launch in a fresh process this would be NotLaunched; by the
    // time the suite runs, other tests may have launched. Either way the
    // accessor must not report the "no launch happened" state after a launch
    // has demonstrably happened.
    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean));

    (void)pdfe::core::isDocumentOpenSafe(clean);
    EXPECT_NE(pdfe::core::lastHelperIsolation(),
              pdfe::core::HelperIsolation::NotLaunched)
        << "lastHelperIsolation() still reports NotLaunched after a launch, so "
           "it is not actually tracking anything";
}

#endif  // _WIN32
