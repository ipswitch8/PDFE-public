// -*- coding: utf-8 -*-
//
// Verify Windows process mitigation policies actually take effect
// inside the test runner process. We call applyProcessMitigations()
// at test-runtime, then query GetProcessMitigationPolicy and assert
// the bits we requested are set.
//
// On non-Windows builds the entire test suite is skipped (the
// applyProcessMitigations function itself is a no-op there).

#include <gtest/gtest.h>
#include "app/ProcessMitigations.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <processthreadsapi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

template <typename PolicyT>
bool queryPolicy(PROCESS_MITIGATION_POLICY kind, PolicyT& out) {
    return GetProcessMitigationPolicy(GetCurrentProcess(), kind, &out, sizeof(out)) != 0;
}

} // namespace

class ProcessMitigationsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // pdfe_tests has its own main (test_main.cpp). The mitigation
        // init isn't called there because the test binary uses a
        // separate entry point. Apply now so all asserts below
        // observe the post-apply state. Idempotent — calling twice
        // is harmless.
        pdfe::app::applyProcessMitigations();
    }
};

TEST_F(ProcessMitigationsTest, DepEnabled) {
    PROCESS_MITIGATION_DEP_POLICY p{};
    ASSERT_TRUE(queryPolicy(ProcessDEPPolicy, p))
        << "GetProcessMitigationPolicy(DEP) failed: GLE=" << GetLastError();
    EXPECT_EQ(p.Enable, 1u) << "DEP not enabled";
    EXPECT_EQ(p.Permanent, 1u) << "DEP not permanent";
}

TEST_F(ProcessMitigationsTest, AslrAllVariantsEnabled) {
    PROCESS_MITIGATION_ASLR_POLICY p{};
    ASSERT_TRUE(queryPolicy(ProcessASLRPolicy, p))
        << "GetProcessMitigationPolicy(ASLR) failed: GLE=" << GetLastError();
    EXPECT_EQ(p.EnableForceRelocateImages, 1u) << "MandatoryASLR not enabled";
    EXPECT_EQ(p.EnableBottomUpRandomization, 1u) << "BottomUpASLR not enabled";
    EXPECT_EQ(p.EnableHighEntropy, 1u) << "HighEntropyASLR not enabled";
}

TEST_F(ProcessMitigationsTest, ImageLoadPolicyApplied) {
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY p{};
    ASSERT_TRUE(queryPolicy(ProcessImageLoadPolicy, p))
        << "GetProcessMitigationPolicy(ImageLoad) failed: GLE=" << GetLastError();
    EXPECT_EQ(p.NoRemoteImages, 1u) << "NoRemoteImages not enabled";
    EXPECT_EQ(p.NoLowMandatoryLabelImages, 1u) << "NoLowMandatoryLabelImages not enabled";
    EXPECT_EQ(p.PreferSystem32Images, 1u) << "PreferSystem32Images not enabled";
}

TEST_F(ProcessMitigationsTest, StrictHandleCheckEnabled) {
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY p{};
    ASSERT_TRUE(queryPolicy(ProcessStrictHandleCheckPolicy, p))
        << "GetProcessMitigationPolicy(StrictHandle) failed: GLE=" << GetLastError();
    EXPECT_EQ(p.RaiseExceptionOnInvalidHandleReference, 1u);
}

// CFG status query: the test binary may or may not be /guard:cf. We
// just assert the query succeeds. Production PDFE.exe is built with
// /guard:cf in Release; the test is structural, not a positive assert.
TEST_F(ProcessMitigationsTest, CfgStatusQueryable) {
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY p{};
    ASSERT_TRUE(queryPolicy(ProcessControlFlowGuardPolicy, p))
        << "GetProcessMitigationPolicy(CFG) failed: GLE=" << GetLastError();
    SUCCEED() << "CFG.EnableControlFlowGuard=" << p.EnableControlFlowGuard;
}


// ═══════════════════════════════════════════════════════════════════════════
// SEC-60 (phase-8) — ACG + CIG on the pdfe_preflight helper
// ═══════════════════════════════════════════════════════════════════════════
//
// The "INTENTIONALLY NOT APPLIED" section of src/app/ProcessMitigations.cpp
// rules out ProhibitDynamicCode (ACG) and MicrosoftSignedOnly (CIG) for the
// main application: ACG breaks Qt's QML/JS and MuJS, CIG would block PDFE's
// own non-Microsoft-signed binaries. Both objections are about what the main
// process LOADS, and neither applies to pdfe_preflight, which links MuPDF and
// nothing else — while being the process that deliberately handles the most
// hostile input in the product.
//
// The read-back cannot happen in THIS process. pdfe_tests.exe is a Qt program
// and cannot take ACG on itself, and querying another process's dynamic-code
// policy needs a handle and access this test has no business acquiring. So the
// helper reports on itself under `--report-mitigations` and these cases spawn
// it — the queryOne pattern, one process further out.
//
// THE SECOND CASE IS THE ONE THAT MAKES THIS SAFE TO SHIP. Applying ACG to the
// preflight child would, if it broke that child, break EVERY File > Open in
// the product — the parent reads an abnormal exit as "unsafe" and refuses the
// document, so a hardening failure would present as "PDFE cannot open any
// PDF". Asserting the policies are on is not enough; the helper has to still
// do its job with them on.

namespace {

std::filesystem::path preflightHelperPath()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path() / L"pdfe_preflight.exe";
}

// Run the helper with one argument, capture stdout, return the exit code.
// -1 means the process could not be started or observed at all, which the
// cases treat as a failure rather than as a pass.
int runHelper(const std::wstring& arg, std::string& out, int timeoutMs = 20000)
{
    out.clear();
    const auto helper = preflightHelperPath();
    if (helper.empty() || !std::filesystem::exists(helper)) return -1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return -1;
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + helper.wstring() + L"\" \"" + arg + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                                     TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                     &si, &pi);
    ::CloseHandle(wr);   // the child owns the write end now
    if (!ok) { ::CloseHandle(rd); return -1; }

    char buf[512];
    DWORD got = 0;
    while (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    ::CloseHandle(rd);

    DWORD code = static_cast<DWORD>(-1);
    if (::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs))
        == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(pi.hProcess, &code);
    } else {
        ::TerminateProcess(pi.hProcess, 0xDEAD);
        code = static_cast<DWORD>(-1);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

unsigned readReportedFlag(const std::string& report, const char* key)
{
    const std::string k = std::string(key) + "=";
    const auto p = report.find(k);
    if (p == std::string::npos) return 0xFFFFFFFFu;   // absent, not zero
    return static_cast<unsigned>(std::atoi(report.c_str() + p + k.size()));
}

} // namespace

TEST(PreflightHardening, AcgAndCigAreActiveInTheHelperProcess)
{
    ASSERT_TRUE(std::filesystem::exists(preflightHelperPath()))
        << "pdfe_preflight.exe was not deployed beside the test binary; the "
           "case did not run";

    std::string report;
    const int rc = runHelper(L"--report-mitigations", report);
    ASSERT_EQ(rc, 0) << "helper exited " << rc << "; output:\n" << report;

    EXPECT_EQ(readReportedFlag(report, "ProhibitDynamicCode"), 1u)
        << "ACG is not active in the preflight helper — an attacker with a "
           "write primitive in the MuPDF parser can still allocate executable "
           "memory.\n" << report;

    EXPECT_EQ(readReportedFlag(report, "AllowThreadOptOut"), 0u)
        << "ACG was applied with a per-thread opt-out; an opt-out a thread "
           "can take is an opt-out an exploit can take.\n" << report;

    EXPECT_EQ(readReportedFlag(report, "MicrosoftSignedOnly"), 1u)
        << "CIG is not active in the preflight helper — an unsigned DLL can "
           "still be loaded into the process that parses hostile input.\n"
        << report;
}

// The regression guard, and the reason the mitigations are applied
// unconditionally rather than behind a switch.
TEST(PreflightHardening, TheHardenedHelperStillClassifiesDocumentsCorrectly)
{
    ASSERT_TRUE(std::filesystem::exists(preflightHelperPath()))
        << "pdfe_preflight.exe was not deployed beside the test binary";

    const auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
    const auto good = fixtures / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(good)) << good.string();

    std::string out;
    EXPECT_EQ(runHelper(good.wstring(), out), 0)
        << "a well-formed PDF is no longer classified safe with ACG/CIG on. "
           "Every File > Open in the product goes through this exit code, so "
           "this is a total-loss regression, not a hardening nit.\n" << out;

    // A file that is not a PDF must still be REJECTED (exit 1), not crash
    // (abnormal exit) and not be accepted.
    const auto notPdf = std::filesystem::temp_directory_path()
                      / "pdfe_sec60_not_a_pdf.pdf";
    { std::ofstream f(notPdf, std::ios::binary); f << "this is not a PDF"; }
    const int rcBad = runHelper(notPdf.wstring(), out);
    EXPECT_EQ(rcBad, 1)
        << "a non-PDF should be an ordinary rejection (1), not " << rcBad
        << ".\n" << out;

    // Usage error path still reachable.
    std::error_code ec;
    std::filesystem::remove(notPdf, ec);
}

// The main application must NOT have acquired these — the reasons
// ProcessMitigations.cpp gives are still true of it, and a future commit that
// "helpfully" applies them process-wide would break Qt's JS engines and PDF
// form JavaScript. This is the counterpart assertion that keeps SEC-60 scoped
// to the helper.
TEST_F(ProcessMitigationsTest, AcgIsNotAppliedToTheMainProcess)
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn{};
    ASSERT_TRUE(queryPolicy(ProcessDynamicCodePolicy, dyn));
    EXPECT_EQ(dyn.ProhibitDynamicCode, 0u)
        << "ProhibitDynamicCode is set on a Qt process. Qt's QML/JS and MuPDF's "
           "MuJS both generate code at runtime; PDF form JavaScript will stop "
           "working. SEC-60 scopes ACG to pdfe_preflight for this reason.";
}

// ---------------------------------------------------------------------------
// GitHub #58 / SEC-59 -- the INTENTIONALLY NOT APPLIED set is now an assertion,
// not just a comment.
//
// WHAT WAS ACTUALLY MISSING. The `INTENTIONALLY NOT APPLIED` block in
// src/app/ProcessMitigations.cpp already existed and already carried a reason
// for each declined policy -- that half of #58 was satisfied before this phase
// started, and is not claimed as new work. What did NOT exist was anything that
// notices a change to it. Every other test in this file asserts a policy that IS
// applied; none asserted the set that is deliberately NOT.
//
// So a policy could be silently dropped from the block (losing its rationale),
// or silently added (declining a mitigation with no recorded decision), and
// nothing observable would change. A security decision recorded only in a
// comment is a decision nobody is holding.
//
// WHY THIS READS THE SOURCE RATHER THAN THE PROCESS. Three of the four declined
// policies cannot be distinguished, by querying the running process, from
// "supported by the OS but left at its default": GetProcessMitigationPolicy
// reports the same zeroed struct either way. Only ProcessDynamicCodePolicy has a
// meaningful runtime query, and AcgIsNotAppliedToTheMainProcess above already
// makes it. The property #58 asks about -- "the set of intentionally-declined
// policies matches a named list" -- is a property of the SOURCE, so the source
// is what this reads. That is the same shape as
// tests/test_page_load_guard_sites.cpp, which re-derives its call-site list from
// src/ for the same reason.
// ---------------------------------------------------------------------------
TEST(ProcessMitigationsArchitecture, TheIntentionallyDeclinedSetMatchesTheNamedList)
{
    // The named list. Changing the block in ProcessMitigations.cpp without
    // changing this list is exactly what should redden.
    const std::vector<std::string> kDeclined = {
        "ProcessSignaturePolicy",
        "ProcessDynamicCodePolicy",
        "ProcessExtensionPointDisablePolicy",
        "ProcessFontDisablePolicy",
    };

    const std::filesystem::path src =
        std::filesystem::path(PDFE_REPO_ROOT) / "src" / "app" / "ProcessMitigations.cpp";
    std::ifstream in(src, std::ios::binary);
    ASSERT_TRUE(in.good()) << "cannot read " << src.string();
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    const auto blockStart = text.find("INTENTIONALLY NOT APPLIED");
    ASSERT_NE(blockStart, std::string::npos)
        << "the INTENTIONALLY NOT APPLIED block is gone from "
        << src.string()
        << ". If the policies are now applied, this test and the block must "
           "change together -- which is the point of this test.";

    // The block runs to the end of the #ifdef _WIN32 body. Bound it explicitly
    // rather than scanning the rest of the file: an unbounded search would keep
    // passing after someone moved a policy name somewhere else entirely.
    const auto blockEnd = text.find("#endif", blockStart);
    ASSERT_NE(blockEnd, std::string::npos)
        << "no #endif after the INTENTIONALLY NOT APPLIED block; the bound this "
           "test relies on is gone";
    const std::string block = text.substr(blockStart, blockEnd - blockStart);

    // Non-vacuity: an empty or tiny block would satisfy a "no unexpected
    // policies" check trivially. This is the guard against the block being
    // gutted rather than edited.
    EXPECT_GT(block.size(), 400u)
        << "the INTENTIONALLY NOT APPLIED block is suspiciously short ("
        << block.size() << " bytes) -- has it been gutted?";

    // 1. Every named policy is still declined, and still carries a reason.
    for (const auto& policy : kDeclined) {
        const auto at = block.find(policy);
        EXPECT_NE(at, std::string::npos)
            << policy << " is no longer in the INTENTIONALLY NOT APPLIED block. "
               "If it is now applied, add an assertion for it above and remove "
               "it from this list -- deliberately, not by deleting this line.";
        if (at == std::string::npos) continue;

        // A bare name with no rationale is the thing #58 was filed about. Every
        // entry in the block is "Policy -- reason"; require the em-dash marker
        // and some prose after it.
        const auto lineEnd = block.find('\n', at);
        const std::string rest = block.substr(at, (lineEnd == std::string::npos)
                                                      ? std::string::npos
                                                      : lineEnd - at);
        EXPECT_NE(rest.find("—"), std::string::npos)
            << policy << " is listed with no rationale marker on its line. The "
               "block records DECISIONS; a policy name alone is not one.";
    }

    // 2. No policy has been added to the block without updating the list. This
    //    is the direction that would otherwise be completely silent: declining a
    //    mitigation is a security decision, and one made without a record is the
    //    defect #58 names.
    std::size_t found = 0;
    for (std::size_t i = block.find("Process"); i != std::string::npos;
         i = block.find("Process", i + 1)) {
        // Collect the identifier starting here.
        std::size_t j = i;
        while (j < block.size() &&
               (std::isalnum(static_cast<unsigned char>(block[j])) || block[j] == '_')) {
            ++j;
        }
        const std::string ident = block.substr(i, j - i);
        // Only count things shaped like a policy enum, not prose using the word.
        if (ident.size() <= std::string("Process").size()) continue;
        if (ident.rfind("Policy") != ident.size() - std::string("Policy").size()) continue;
        ++found;
        EXPECT_NE(std::find(kDeclined.begin(), kDeclined.end(), ident),
                  kDeclined.end())
            << ident << " appears in the INTENTIONALLY NOT APPLIED block but is "
               "not in this test's named list. A mitigation was declined without "
               "the decision being recorded here.";
    }
    EXPECT_EQ(found, kDeclined.size())
        << "expected exactly " << kDeclined.size()
        << " declined *Policy identifiers in the block, found " << found
        << ". A count mismatch means the block and this list have drifted.";
}

#else  // _WIN32

TEST(ProcessMitigationsTest, NoOpOnNonWindows) {
    // The function is a no-op on non-Windows. Calling it should not
    // crash and should not affect anything observable.
    pdfe::app::applyProcessMitigations();
    SUCCEED();
}

#endif // _WIN32
