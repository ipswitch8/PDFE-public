// -*- coding: utf-8 -*-
// test_resource_bomb_sandbox.cpp
//
// Meta-audit follow-up F16, gap PDF-109 (merges SEC-37).
//
// THE GAP
// -------
// tests/fixtures/flaw_corpus/COVERAGE.md §8 excluded the upstream `bomb-giant`
// fixture "(needs a resource-limited sandbox harness)". No such harness
// existed, in this repo or anywhere else, so the exclusion was conditioned on
// something that was never coming -- a rationale that reads as "deferred
// pending work" while nothing is pending. What decompression-bomb resistance
// actually rested on was two tests whose names promise more than they do:
//
//   PerfStressTest.ExtremeCompressionRatioBounded  compresses the five bytes
//       "Hello" -- expansion ratio 0.4:1, i.e. the opposite of a bomb;
//   MaliciousPdfTest.DecompressionBombHandledGracefully  declares
//       /Filter /FlateDecode over ten literal 'x' bytes, which is not valid
//       zlib at all, so the decompressor rejects it at the first byte.
//
// Neither one can distinguish a streaming decompressor from a materialising
// one, which is the entire question a bomb asks.
//
// WHAT THIS FILE IS
// -----------------
// The harness the exclusion named. A real 1029:1 bomb
// (tests/fixtures/flaw_corpus/corrupt/decompression-bomb.pdf, 509 KiB on disk,
// 512 MiB decompressed, built by tests/fixtures/gen_decompression_bomb.py) is
// opened and rendered IN A CHILD PROCESS inside a Windows Job Object carrying
// hard process- and job-memory caps, a hard active-process cap and
// KILL_ON_JOB_CLOSE, with the parent enforcing a wall-clock timeout. A bomb
// that defeats the containment therefore kills its own child and is reported,
// instead of wedging the suite or the machine.
//
// The four oracles, and what each would catch:
//
//   JobCapsActuallyKillARunawayChild
//       Spawns a child that deliberately touches far more memory than the cap
//       and asserts the JOB kills it. Without this, every other assertion here
//       could pass with the limits silently not applied -- a sandbox that
//       does not sandbox is the failure mode a bomb harness is most likely to
//       ship with. Catches: caps not set, child not assigned to the job,
//       child breaking away from the job.
//
//   BombDecompressesToItsDeclaredSizeUnderTheCap
//       A child streams the bomb's content stream through a fixed 64 KiB
//       buffer and returns the decompressed size in MiB as its exit code. This
//       is what proves the fixture is a real bomb (512 MiB out of 509 KiB in)
//       rather than a small file with a big number written next to it, AND
//       that reading it whole does not require holding it whole. Catches: a
//       regenerated/truncated fixture, and any change that makes stream reads
//       buffer the entire decompressed extent.
//
//   BombOpensAndRendersWithinHardCaps
//       The product path: Document::open + Page::render of the bomb page,
//       inside the cap. Catches a content-stream reader that materialises its
//       input -- 512 MiB against a 384 MiB cap is not survivable, so the job
//       would kill the child and this test would report a non-zero exit.
//
//   PageTreeBombOpensWithinHardCaps  (GitHub #51 / SEC-35)
//       The OBJECT-COUNT dimension the other three do not touch: a
//       structurally valid document carrying 100,000 /Page objects, generated
//       at build time rather than committed. The three above all measure
//       BYTES; a file can be small in both compressed and decompressed size
//       and still exhaust a reader through object count alone. Catches a
//       page-tree walk whose per-page cost is unbounded, a lazily-built page
//       index that degrades at the far end of the tree, and a reader that
//       refuses a large-but-legal document outright.
//
// The child roles are selected by the PDFE_BOMB_SANDBOX_ROLE environment
// variable and run as the SAME executable re-invoked with a --gtest_filter,
// the pattern tests/support/SaveFailureInjection.cpp established. In an
// ordinary suite run the role variable is unset and the child-side test skips
// with that reason stated; it is not skipped unconditionally, and it is the
// parent tests above that give it its coverage.
//
// Non-Windows: the Job Object is a Win32 facility and PDFE ships Windows-only
// today (Linux does not compile -- see docs/security-testing.md). The file
// compiles to a single explicit "not implemented on this platform" test
// elsewhere rather than silently vanishing.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;

fs::path bombFixture() {
    return fs::path(TEST_FIXTURES_DIR) / "flaw_corpus" / "corrupt" /
           "decompression-bomb.pdf";
}

// GitHub #51 (SEC-35). The 100k-object page-tree fixture, generated as a BUILD
// STEP into the build tree by tests/fixtures/gen_page_tree_bomb.py -- never
// committed. PAGE_TREE_BOMB_PDF is baked in by tests/CMakeLists.txt for the
// same reason PDFE_REPO_ROOT is: an environment-variable opt-in is a switch
// that turns a check off.
fs::path pageTreeBombFixture() {
#if defined(PAGE_TREE_BOMB_PDF)
    return fs::path(PAGE_TREE_BOMB_PDF);
#else
    return {};
#endif
}

// Must equal the --pages the build step passes the generator.
constexpr int kPageTreeBombPages = 100000;

// Must equal EXPANSION_BYTES in tests/fixtures/gen_decompression_bomb.py.
// The child returns the measured value and the parent compares against this,
// so a fixture regenerated at a different size fails loudly rather than
// quietly weakening every other assertion in the file.
constexpr long long kDeclaredExpansionBytes = 512LL * 1024 * 1024;
constexpr int kDeclaredExpansionMiB = static_cast<int>(kDeclaredExpansionBytes >> 20);

std::string roleEnv() {
#if defined(_WIN32)
    char buf[64] = {};
    const DWORD n = ::GetEnvironmentVariableA("PDFE_BOMB_SANDBOX_ROLE", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return {};
    return std::string(buf, n);
#else
    const char* v = std::getenv("PDFE_BOMB_SANDBOX_ROLE");
    return v ? std::string(v) : std::string();
#endif
}

} // namespace

// ===========================================================================
// Child side -- runs only when re-invoked by a parent test below.
// ===========================================================================
//
// Exit codes are the channel back to the parent, so each role calls _exit-like
// std::quick_exit paths through ::TerminateProcess-free normal exit: gtest's
// return value would collapse every outcome to 0/1. Roles therefore call
// ::ExitProcess with a role-specific code.

TEST(ResourceBombSandboxChild, RunRole) {
    const std::string role = roleEnv();
    if (role.empty()) {
        GTEST_SKIP() << "child role not requested (PDFE_BOMB_SANDBOX_ROLE unset); "
                        "this case is driven by the ResourceBombSandbox parent "
                        "tests in this file, which re-invoke this binary inside a "
                        "capped Job Object";
    }

#if defined(_WIN32)
    if (role == "hog") {
        // Deliberately exceed any sane cap, touching every page so the memory is
        // committed rather than merely reserved. Under a working job cap this
        // process never reaches the exit below.
        const size_t chunk = 32u * 1024 * 1024;
        std::vector<void*> held;
        for (int i = 0; i < 256; ++i) {           // up to 8 GiB
            void* p = ::VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
            if (!p) ::ExitProcess(90);            // allocation refused (cap bit)
            std::memset(p, 0x5A, chunk);
            held.push_back(p);
        }
        ::ExitProcess(91);                        // 8 GiB committed: cap did NOT bite
    }

    if (role == "measure") {
        // Stream the bomb's content stream through a fixed small buffer and
        // report the decompressed size in MiB. Bounded memory by construction:
        // nothing here holds more than 64 KiB of the expansion at a time.
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        long long total = 0;
        const std::string path = bombFixture().string();
        try {
            total = ctx->executeReturn<long long>([&](fz_context* c) -> long long {
                long long n = 0;
                pdf_document* doc = pdf_open_document(c, path.c_str());
                fz_try(c) {
                    pdf_obj* page = pdf_lookup_page_obj(c, doc, 0);
                    pdf_obj* contents = pdf_dict_get(c, page, PDF_NAME(Contents));
                    fz_stream* stm = pdf_open_stream(c, contents);
                    fz_try(c) {
                        unsigned char buf[64 * 1024];
                        size_t got = 0;
                        while ((got = fz_read(c, stm, buf, sizeof(buf))) > 0)
                            n += static_cast<long long>(got);
                    }
                    fz_always(c) { fz_drop_stream(c, stm); }
                    fz_catch(c) { fz_rethrow(c); }
                }
                fz_always(c) { pdf_drop_document(c, doc); }
                fz_catch(c) { fz_rethrow(c); }
                return n;
            });
        } catch (...) {
            ::ExitProcess(80);                    // decompression threw
        }
        if (total <= 0 || (total >> 20) > 4095) ::ExitProcess(81);
        ::ExitProcess(static_cast<UINT>(total >> 20));   // MiB decompressed
    }

    if (role == "render") {
        // The product path: open the bomb and render its page.
        try {
            auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
            auto doc = pdfe::core::Document::open(ctx, bombFixture());
            if (!doc || doc->pageCount() != 1) ::ExitProcess(70);
            auto page = doc->page(0);
            if (!page) ::ExitProcess(71);
            const QImage img = page->render(page->boundingBox(), 1.0f);
            if (img.isNull()) ::ExitProcess(72);
            ::ExitProcess(0);
        } catch (...) {
            ::ExitProcess(73);
        }
    }

    if (role == "pagetree") {
        // GitHub #51 -- the object-count dimension. Open a document whose page
        // tree carries 100,000 /Page objects and touch both ends of it. The
        // exit code carries the PAGE COUNT back so the parent can say what it
        // actually got; 50-54 are the failure codes and none of them collide
        // with a plausible page count.
        const fs::path fixture = pageTreeBombFixture();
        if (fixture.empty() || !fs::exists(fixture)) ::ExitProcess(50);
        try {
            auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
            auto doc = pdfe::core::Document::open(ctx, fixture);
            if (!doc) ::ExitProcess(51);
            const int n = doc->pageCount();
            if (n <= 0 || n > 1000000) ::ExitProcess(52);
            // Both ends: a lazily-built page index that is correct at page 0
            // and wrong (or unboundedly expensive) at page n-1 is the shape
            // this fixture exists to find.
            if (!doc->page(0)) ::ExitProcess(53);
            if (!doc->page(n - 1)) ::ExitProcess(54);
            ::ExitProcess(static_cast<UINT>(n));
        } catch (...) {
            ::ExitProcess(55);
        }
    }

    ::ExitProcess(60);                            // unknown role
#else
    FAIL() << "child roles are Windows-only; role=" << role;
#endif
}

#if defined(_WIN32)

namespace {

struct SandboxResult {
    bool spawned = false;
    bool timedOut = false;
    DWORD exitCode = 0xFFFFFFFF;
    unsigned long long peakJobMemory = 0;
    double seconds = 0.0;
    std::string detail;
};

fs::path selfExecutable() {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf);
}

// Run this same binary, filtered to ResourceBombSandboxChild.RunRole, inside a
// FRESH job object carrying hard caps. The test process itself already lives in
// a limit-free job (test_main.cpp assigns it for spawn scoping), so the child is
// created SUSPENDED, assigned to the capped job -- which nests under the
// existing one on Win8+ -- and only then resumed. Assigning limits to the
// process's own job would cap the test runner instead of the child.
SandboxResult runChildUnderCaps(const char* role, unsigned long long capBytes,
                                DWORD timeoutMs) {
    SandboxResult r;

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (!job) { r.detail = "CreateJobObject failed"; return r; }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    li.BasicLimitInformation.ActiveProcessLimit = 1;
    li.ProcessMemoryLimit = static_cast<SIZE_T>(capBytes);
    li.JobMemoryLimit     = static_cast<SIZE_T>(capBytes);
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &li, sizeof(li))) {
        r.detail = "SetInformationJobObject failed, err=" +
                   std::to_string(::GetLastError());
        ::CloseHandle(job);
        return r;
    }

    std::wstring cmd = L"\"" + selfExecutable().wstring() +
                       L"\" --gtest_filter=ResourceBombSandboxChild.RunRole "
                       L"--gtest_brief=1";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    // Child environment = ours plus the role selector. SetEnvironmentVariable on
    // the parent then CreateProcess with a null environment block is the least
    // error-prone form here; the variable is cleared immediately afterwards so a
    // later test in this process is never mistaken for a child.
    ::SetEnvironmentVariableA("PDFE_BOMB_SANDBOX_ROLE", role);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                                     CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                     nullptr, nullptr, &si, &pi);
    ::SetEnvironmentVariableA("PDFE_BOMB_SANDBOX_ROLE", nullptr);
    if (!ok) {
        r.detail = "CreateProcess failed, err=" + std::to_string(::GetLastError());
        ::CloseHandle(job);
        return r;
    }

    if (!::AssignProcessToJobObject(job, pi.hProcess)) {
        r.detail = "AssignProcessToJobObject failed, err=" +
                   std::to_string(::GetLastError());
        ::TerminateProcess(pi.hProcess, 0);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(job);
        return r;
    }

    r.spawned = true;
    const auto t0 = std::chrono::steady_clock::now();
    ::ResumeThread(pi.hThread);

    if (::WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) {
        r.timedOut = true;
        ::TerminateProcess(pi.hProcess, 0xDEAD);
        ::WaitForSingleObject(pi.hProcess, 5000);
    }
    r.seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    ::GetExitCodeProcess(pi.hProcess, &r.exitCode);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION out{};
    DWORD ret = 0;
    if (::QueryInformationJobObject(job, JobObjectExtendedLimitInformation,
                                    &out, sizeof(out), &ret))
        r.peakJobMemory = out.PeakJobMemoryUsed;

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(job);        // KILL_ON_JOB_CLOSE reaps anything left
    return r;
}

constexpr unsigned long long kMiB = 1024ull * 1024ull;

} // namespace

// ---------------------------------------------------------------------------
// 1. The sandbox must actually sandbox.
// ---------------------------------------------------------------------------

TEST(ResourceBombSandbox, JobCapsActuallyKillARunawayChild) {
    const SandboxResult r = runChildUnderCaps("hog", 256 * kMiB, 60000);
    ASSERT_TRUE(r.spawned) << r.detail;

    EXPECT_FALSE(r.timedOut)
        << "the runaway child was still alive after 60 s; the job cap did not "
           "stop it and the parent had to terminate it";
    // 91 == the child committed 8 GiB and exited normally, i.e. NOTHING
    // constrained it. 90 == VirtualAlloc was refused (the cap bit). Any other
    // code means the job killed the process outright, which is also a pass.
    EXPECT_NE(r.exitCode, 91u)
        << "the child committed 8 GiB inside a 256 MiB job -- the limits were "
           "not in force, so every other assertion in this file would pass "
           "vacuously";
    EXPECT_LE(r.peakJobMemory, 400 * kMiB)
        << "peak job memory " << (r.peakJobMemory / kMiB) << " MiB exceeded the "
           "256 MiB cap by more than the accounting slack; the memory limit is "
           "not being enforced";
}

// ---------------------------------------------------------------------------
// 2. The fixture is a real bomb, and reading it whole does not hold it whole.
// ---------------------------------------------------------------------------

TEST(ResourceBombSandbox, BombDecompressesToItsDeclaredSizeUnderTheCap) {
    ASSERT_TRUE(fs::exists(bombFixture()))
        << bombFixture().string() << " is missing; regenerate with "
           "python tests/fixtures/gen_decompression_bomb.py";
    const auto onDisk = static_cast<long long>(fs::file_size(bombFixture()));
    ASSERT_GT(onDisk, 0);

    const SandboxResult r = runChildUnderCaps("measure", 384 * kMiB, 180000);
    ASSERT_TRUE(r.spawned) << r.detail;
    ASSERT_FALSE(r.timedOut)
        << "streaming the bomb's content stream did not finish within 180 s";

    EXPECT_EQ(r.exitCode, static_cast<DWORD>(kDeclaredExpansionMiB))
        << "the child reported " << r.exitCode << " MiB decompressed; the "
           "generator declares " << kDeclaredExpansionMiB << " MiB. Codes 80/81 "
           "mean the stream threw or measured out of range. If the fixture was "
           "regenerated at a different size, update kDeclaredExpansionBytes and "
           "EXPANSION_BYTES together.";

    // The bomb must be a bomb: a ratio this file's assertions depend on.
    const double ratio = static_cast<double>(kDeclaredExpansionBytes) /
                         static_cast<double>(onDisk);
    EXPECT_GT(ratio, 500.0)
        << "the fixture expands only " << ratio << ":1; below a few hundred to "
           "one it no longer distinguishes a streaming decompressor from a "
           "materialising one, which is the whole point";

    // Reading 512 MiB through a 64 KiB buffer must not need 384 MiB of RAM.
    EXPECT_LT(r.peakJobMemory, 384 * kMiB)
        << "peak job memory was " << (r.peakJobMemory / kMiB) << " MiB while "
           "streaming a " << kDeclaredExpansionMiB << " MiB expansion -- the "
           "read path is buffering the decompressed extent";
}

// ---------------------------------------------------------------------------
// 3. The product path survives the bomb inside the cap.
// ---------------------------------------------------------------------------

TEST(ResourceBombSandbox, BombOpensAndRendersWithinHardCaps) {
    ASSERT_TRUE(fs::exists(bombFixture()))
        << bombFixture().string() << " is missing; regenerate with "
           "python tests/fixtures/gen_decompression_bomb.py";

    const SandboxResult r = runChildUnderCaps("render", 384 * kMiB, 180000);
    ASSERT_TRUE(r.spawned) << r.detail;

    EXPECT_FALSE(r.timedOut)
        << "Document::open + Page::render on a " << kDeclaredExpansionMiB
        << " MiB decompression bomb did not finish within 180 s";
    EXPECT_EQ(r.exitCode, 0u)
        << "the child exited " << r.exitCode << ". 70-73 are PDFE-side failures "
           "(open returned null, page null, null image, exception); anything "
           "else means the job killed it -- i.e. opening or rendering the bomb "
           "needed more than the 384 MiB cap.";
    EXPECT_LT(r.peakJobMemory, 384 * kMiB)
        << "peak job memory " << (r.peakJobMemory / kMiB) << " MiB against a "
           "384 MiB cap while rendering a " << kDeclaredExpansionMiB
        << " MiB expansion";
}

// ---------------------------------------------------------------------------
// 4. The OBJECT-COUNT dimension: a 100k-object page tree, inside the same cap.
//     GitHub #51 (SEC-35).
// ---------------------------------------------------------------------------
//
// The three oracles above all measure BYTES: a 509 KiB file that expands to
// 512 MiB. A resource bomb does not have to be a decompression bomb. A file can
// be modest in both compressed and decompressed size and still exhaust a reader
// through sheer OBJECT COUNT -- 100,000 page objects is ~14 MB on disk and
// expands to nothing at all, but a reader that holds one materialised node per
// page, walks the tree recursively per page, or rebuilds a /Parent chain per
// lookup, does not survive it. Nothing in this file measured that.
//
// The fixture is generated as a build step (tests/fixtures/gen_page_tree_bomb.py
// via tests/CMakeLists.txt) rather than committed, and it is STRUCTURALLY VALID:
// this is not a corruption test, and "the parser rejected it" would be a
// failure, not a pass.
//
// It reuses runChildUnderCaps() -- the same job object, the same caps, the same
// timeout enforcement -- so it inherits the guarantee oracle 1 provides, that
// the limits are genuinely in force.

TEST(ResourceBombSandbox, PageTreeBombOpensWithinHardCaps) {
    const fs::path fixture = pageTreeBombFixture();

    // FAIL, never skip. The fixture is a build product; if the build did not
    // produce it the sandbox has not been exercised, and a green here would say
    // it had. #189: a missing input is a failure.
    ASSERT_FALSE(fixture.empty())
        << "PAGE_TREE_BOMB_PDF was not defined at compile time -- "
           "tests/CMakeLists.txt did not bake in the generated fixture path, so "
           "this oracle examined nothing";
    ASSERT_TRUE(fs::exists(fixture))
        << fixture.string() << " is missing. It is generated at BUILD time by "
           "tests/fixtures/gen_page_tree_bomb.py; rebuild the pdfe_tests target, "
           "or regenerate by hand with: python "
           "tests/fixtures/gen_page_tree_bomb.py --out <that path> --pages "
        << kPageTreeBombPages;

    const auto onDisk = static_cast<long long>(fs::file_size(fixture));
    ASSERT_GT(onDisk, 1024 * 1024)
        << "the page-tree fixture is only " << onDisk << " bytes; 100,000 page "
           "objects cannot fit in that, so the file is not the fixture this "
           "oracle thinks it is";

    const SandboxResult r = runChildUnderCaps("pagetree", 384 * kMiB, 180000);
    ASSERT_TRUE(r.spawned) << r.detail;

    EXPECT_FALSE(r.timedOut)
        << "opening a " << kPageTreeBombPages << "-page document and touching "
           "both ends of its page tree did not finish within 180 s inside a "
           "384 MiB job -- the per-page cost is not bounded";
    EXPECT_EQ(r.exitCode, static_cast<DWORD>(kPageTreeBombPages))
        << "the child exited " << r.exitCode << "; " << kPageTreeBombPages
        << " means it opened the document and reached both the first and the "
           "last page. 50 = fixture absent in the child, 51 = Document::open "
           "returned null (the reader REJECTED a structurally valid document), "
           "52 = implausible page count, 53/54 = first/last page would not "
           "load, 55 = exception. Anything else means the job killed the "
           "child: 100,000 page objects needed more than the 384 MiB cap.";
    EXPECT_LT(r.peakJobMemory, 384 * kMiB)
        << "peak job memory " << (r.peakJobMemory / kMiB) << " MiB against a "
           "384 MiB cap for a " << (onDisk / (1024 * 1024)) << " MiB file -- "
           "the page-tree walk is materialising per-object state without bound";
}

#else   // !_WIN32

TEST(ResourceBombSandbox, NotImplementedOnThisPlatform) {
    GTEST_SKIP() << "the bomb sandbox is built on Windows Job Objects; PDFE "
                    "currently ships Windows-only";
}

#endif
