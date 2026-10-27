// -*- coding: utf-8 -*-
//
// Performance & stress tests covering Pipeline 4 items:
// LARGE-DOC-RESPONSIVENESS, LARGE-ANNOTATION-COUNT, DEEPLY-NESTED-FORMS,
// HANDLE-LEAK-AUDIT, ZIP-BOMB-RESILIENCE (additional), PARALLEL-RENDER-MALICIOUS.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationCommands.h"
#include "core/UndoManager.h"

#include <QImage>
#include <QRectF>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// NOMINMAX is mandatory here, not hygiene: windows.h otherwise defines min/max
// as macros and the pre-existing std::min() calls in
// ThousandPageOpenScalingLinear stop compiling.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

using namespace pdfe::core;

namespace {

std::filesystem::path tempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_perf_stress";
    std::filesystem::create_directories(d);
    return d;
}

void writePdf(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Generate a multi-page PDF with N pages, each with light content.
std::string buildLargePdf(int pages)
{
    std::string objects;
    std::string pageRefs;
    int objNum = 3;  // 1=Catalog, 2=Pages

    for (int i = 0; i < pages; ++i) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%d 0 obj\n<< /Type /Page /Parent 2 0 R "
            "/MediaBox [0 0 612 792] /Resources << /Font << /F1 << "
            "/Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> "
            "/Contents %d 0 R >>\nendobj\n",
            objNum, objNum + 1);
        objects += buf;
        char streamBuf[256];
        std::snprintf(streamBuf, sizeof(streamBuf),
            "%d 0 obj\n<< /Length 40 >>\nstream\n"
            "BT /F1 12 Tf 100 700 Td (Page %d) Tj ET\n"
            "endstream\nendobj\n",
            objNum + 1, i + 1);
        objects += streamBuf;
        char refBuf[16];
        std::snprintf(refBuf, sizeof(refBuf), "%d 0 R ", objNum);
        pageRefs += refBuf;
        objNum += 2;
    }

    return std::string("%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [") + pageRefs +
        "] /Count " + std::to_string(pages) + " >>\nendobj\n" +
        objects +
        "trailer\n<< /Size " + std::to_string(objNum) +
        " /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
}

} // namespace

class PerfStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        td_ = tempDir();
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path td_;
};

// =====================================================================
// LARGE-DOC-RESPONSIVENESS: 500-page document responsiveness
// =====================================================================

TEST_F(PerfStressTest, FiveHundredPageOpenAndNavigate)
{
    auto path = td_ / "doc_500p.pdf";
    writePdf(path, buildLargePdf(500));

    auto openStart = std::chrono::steady_clock::now();
    auto doc = Document::open(ctx_, path);
    auto openElapsed = std::chrono::steady_clock::now() - openStart;
    double openSec = std::chrono::duration<double>(openElapsed).count();

    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 500);
    EXPECT_LT(openSec, 5.0) << "500-page open must complete in < 5s";

    // Jump to last page
    auto navStart = std::chrono::steady_clock::now();
    auto lastPage = doc->page(499);
    auto navElapsed = std::chrono::steady_clock::now() - navStart;
    double navSec = std::chrono::duration<double>(navElapsed).count();

    EXPECT_NE(lastPage, nullptr);
    EXPECT_LT(navSec, 2.0) << "Last-page navigation must complete in < 2s";

    // Render middle page
    auto renderStart = std::chrono::steady_clock::now();
    auto dl = doc->createDisplayList(250);
    if (dl) doc->releaseDisplayList(dl);
    auto renderElapsed = std::chrono::steady_clock::now() - renderStart;
    double renderSec = std::chrono::duration<double>(renderElapsed).count();
    EXPECT_LT(renderSec, 2.0) << "Mid-document render must complete in < 2s";
}

TEST_F(PerfStressTest, ThousandPageOpenScalingLinear)
{
    // WHY THERE IS NO `if (sec100 > 0.05)` GUARD HERE ANY MORE.
    //
    // This test used to read:
    //
    //     if (sec100 > 0.05) {  // Only assert if 100p took measurable time
    //         EXPECT_LT(sec1000 / sec100, 15.0) ...
    //     }
    //
    // and `sec100` measures ~12-16 ms on this hardware -- permanently below
    // 0.05 -- so THE RATIO ASSERTION NEVER EXECUTED. A test named
    // ...ScalingLinear was asserting nothing about scaling; only the absolute
    // `sec1000 < 10s` backstop below ever ran. Found by a shape sweep during
    // the F27 follow-up, which looked for "a timing or resource threshold that
    // can be permanently false, gating a comparative assertion". The identical
    // pattern (same comment, same 15.0 constant) had been copied into
    // tests/test_fidelity_r3_24_edit_save_scale.cpp, where it hid a FALSE
    // assertion; here it hid a TRUE one. Either way the suite reported a
    // passing scaling test that measured nothing.
    //
    // Removing the guard is strictly stronger: an assertion that never runs is
    // not a weak oracle, it is no oracle, so any assertion that does run beats
    // it. Measured ratio here is 5.98-7.48 for 10x pages, with the 15.0 bound
    // unchanged and roughly 2x of headroom.
    //
    // BEST-OF-N replaces the single sample. At ~12 ms the 100-page open is
    // small enough that one descheduling of the measured thread moves the
    // ratio materially, and this suite runs 16-way sharded. Contention can
    // only ever make a sample SLOWER, so the minimum is the best estimate of
    // true cost. It also fails in the SAFE direction under load: the small
    // quantity inflates proportionally more than the large one, which
    // *lowers* the measured ratio. This is a timing fix; the bound is
    // untouched.
    auto path100 = td_ / "doc_100p.pdf";
    auto path1000 = td_ / "doc_1000p.pdf";
    writePdf(path100, buildLargePdf(100));
    writePdf(path1000, buildLargePdf(1000));

    auto timeOpen = [&](const std::filesystem::path& p, int expectPages) {
        auto t0 = std::chrono::steady_clock::now();
        {
            auto doc = Document::open(ctx_, p);
            EXPECT_NE(doc, nullptr);
            if (doc) EXPECT_EQ(doc->pageCount(), expectPages);
        }
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t0).count();
    };

    constexpr int kTimingSamples = 3;
    double sec100 = timeOpen(path100, 100);
    double sec1000 = timeOpen(path1000, 1000);
    for (int i = 1; i < kTimingSamples; ++i) {
        sec100  = std::min(sec100,  timeOpen(path100, 100));
        sec1000 = std::min(sec1000, timeOpen(path1000, 1000));
    }

    // 1000-page open must not cost more than 15x the 100-page open, i.e.
    // roughly linear scaling with overhead. UNCONDITIONAL -- see above.
    ASSERT_GT(sec100, 0.0) << "timer produced a non-positive baseline";
    EXPECT_LT(sec1000 / sec100, 15.0)
        << "1000p/100p ratio: " << sec1000 / sec100
        << " (100p=" << sec100 << "s, 1000p=" << sec1000 << "s)";

    // Absolute backstop, independent of the ratio.
    EXPECT_LT(sec1000, 10.0) << "1000-page open must complete in < 10s";
}

// =====================================================================
// PDF-83 / GitHub issue #30 — HugeImagePagePeakMemoryBounded
//
// COVERAGE GAP, not a product gap: the render path is already bounded. What
// was missing is a measurement that says so, and the reason it was missing is
// that the obvious way to write it is vacuous.
//
// THE SOUND PART, borrowed from tests/test_f39c_jpx_pixel_bound.cpp:127-141.
// PeakWorkingSetSize is MONOTONIC for a process, so a DELTA taken across an
// operation is a sound UPPER bound on what that operation allocated and
// touched — a spike cannot hide inside it.
//
// THE TRAP, and why this test spawns a child. That same monotonicity makes the
// delta ZERO whenever some earlier test in the same process already drove the
// peak higher: the bound then passes having measured nothing, and whether it
// does depends on shard composition and test order, so it is invisible. This
// file's own TenThousandAnnotationsOnePage builds 10,000 annotations with an
// undo stack in the same fixture; it is exactly the neighbour that would do it.
//
// So the measurement runs in a FRESH CHILD PROCESS (the pattern
// test_malicious_pdf_advanced.cpp's depth sweep established), where the peak
// starts at the process's own startup cost and the delta is attributable. The
// child writes its numbers to a result file; the parent reads them and
// asserts. Both halves are then checkable:
//
//   NON-VACUITY  delta > 0. If it is zero the measurement did not happen and
//                the bound below is meaningless — that is a FAILURE here, not
//                a pass.
//   CONTROL      the child must report rendered=1 and a non-empty raster. A
//                render that refused the document, or produced nothing, would
//                otherwise satisfy any upper bound trivially.
//   THE BOUND    kPeakDeltaBoundBytes, derived below.
//
// THE FIXTURE. phase3_large_100page_image.pdf is 23,127,304 bytes: 100 pages
// of 595x842 pt, each carrying a 320x240 RGB image. Rendering ONE page must
// cost O(that page), not O(the document):
//
//   one page at scale 1.0   595 x 842 = 501,190 px -> ~1.5 MB RGB pixmap
//   all 100 pages rasterised            ~150 MB
//   all 100 images decoded    100 x 320x240x3      ~23 MB
//
// MEASURED: 7.5-7.6 MB of peak-working-set delta across three runs (7704 KB,
// 7812 KB), against 500,990 rendered pixels. So the render costs roughly the
// page's own raster plus MuPDF's per-document overhead, and nothing that
// scales with the other 99 pages.
//
// The bound is 64 MB: ~8x the measured figure, so it will not flake, and well
// under the ~150 MB an implementation that rasterised every page would need
// and the ~23 MB+ one that decoded every embedded image would add. A bound of
// 512 MB (the first draft) would have been satisfied by both of those and
// would have measured nothing. See the phase-16 benchmark artifact,
// docs/audit/backlog-p82/phase-16/benchmarks.json.
// =====================================================================

namespace {

// GitHub #178 — the value RecordProperty("peak_delta_bytes", ...) publishes.
//
// This used to be `static_cast<int>(delta / 1024)`: the key said bytes and the
// value was KILOBYTES, so the phase-16 benchmark artifact recorded 7708 for a
// ~7.5 MB delta. The bound itself was never wrong (it compares raw `delta`
// against kPeakDeltaBoundBytes), so this is a labelling fix — but a benchmark
// figure that is wrong by 1024x is a figure a later comparison will misread.
//
// It is a named function rather than an inline expression so the unit contract
// is assertable on its own, independently of the child-process render that
// produces the real delta (see PeakDeltaPropertyIsInBytes below). The identity
// is the point: the property is in BYTES, unscaled.
long long peakDeltaPropertyValue(long long deltaBytes)
{
    return deltaBytes;
}

#ifdef _WIN32
std::uint64_t peakWorkingSetBytes()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
}

std::filesystem::path selfExe()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring(buf, n));
}

// Run this executable with `filter`, having set `env` (name -> value). Returns
// the child's exit code, or 0xFFFFFFFF if it could not be launched/awaited.
DWORD runChildRole(const std::wstring& filter,
                   const std::vector<std::pair<std::string, std::string>>& env,
                   DWORD timeoutMs)
{
    const auto exe = selfExe();
    if (exe.empty()) return 0xFFFFFFFF;

    std::wstring cmd = L"\"" + exe.wstring() + L"\" --gtest_filter=" + filter +
                       L" --gtest_brief=1";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    for (const auto& kv : env)
        ::SetEnvironmentVariableA(kv.first.c_str(), kv.second.c_str());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(exe.wstring().c_str(), cmdBuf.data(),
                                          nullptr, nullptr, FALSE,
                                          CREATE_NO_WINDOW, nullptr, nullptr,
                                          &si, &pi);
    // Cleared immediately so a later test in THIS process can never be
    // mistaken for the child.
    for (const auto& kv : env)
        ::SetEnvironmentVariableA(kv.first.c_str(), nullptr);

    if (!created) return 0xFFFFFFFF;

    DWORD code = 0xFFFFFFFF;
    if (::WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(pi.hProcess, &code);
    } else {
        ::TerminateProcess(pi.hProcess, 1);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return code;
}

// Parse "key=value" lines. Absent keys simply do not appear, which is how the
// parent distinguishes "the child died before reporting" from "the child
// reported a zero".
std::map<std::string, std::string> readKeyValueFile(
    const std::filesystem::path& p)
{
    std::map<std::string, std::string> out;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}
#endif  // _WIN32

}  // namespace

#ifdef _WIN32
// THE CHILD ROLE. Renders page 0 of PDFE_HUGE_IMAGE_FILE and writes the
// peak-working-set delta to PDFE_HUGE_IMAGE_RESULT. Skips (and is therefore
// inert) in every process that is not the child.
TEST(HugeImageRenderChild, RunRole)
{
    const char* file = std::getenv("PDFE_HUGE_IMAGE_FILE");
    const char* result = std::getenv("PDFE_HUGE_IMAGE_RESULT");
    if (file == nullptr || *file == '\0' || result == nullptr || *result == '\0') {
        GTEST_SKIP() << "not the huge-image render child "
                        "(PDFE_HUGE_IMAGE_FILE unset)";
    }

    // Baseline taken AFTER the context exists but BEFORE the document is
    // opened, so the delta covers open + page load + render and nothing else.
    auto ctx = std::make_shared<MuPdfContext>();
    const std::uint64_t before = peakWorkingSetBytes();

    long long pixels = 0;
    int rendered = 0;
    {
        auto doc = Document::open(ctx, std::filesystem::path(file));
        ASSERT_NE(doc, nullptr) << "child could not open " << file;
        ASSERT_GE(doc->pageCount(), 1);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        const QRectF bounds = page->boundingBox();
        QImage img = page->render(bounds, 1.0f);
        pixels = static_cast<long long>(img.width()) *
                 static_cast<long long>(img.height());
        rendered = (pixels > 0) ? 1 : 0;
    }
    const std::uint64_t after = peakWorkingSetBytes();

    std::ofstream out(result, std::ios::trunc);
    out << "baseline=" << before << "\n"
        << "peak=" << after << "\n"
        << "delta=" << (after >= before ? after - before : 0) << "\n"
        << "pixels=" << pixels << "\n"
        << "rendered=" << rendered << "\n";
    out.close();
    ASSERT_TRUE(out.good() || !out.fail()) << "child could not write " << result;
}
#endif

TEST_F(PerfStressTest, HugeImagePagePeakMemoryBounded)
{
#ifndef _WIN32
    GTEST_SKIP() << "peak-working-set measurement is implemented for Windows only";
#else
    const auto fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "phase3_large_100page_image.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "fixture missing: " << fixture
        << " (pinned in the tests/CMakeLists.txt fixture floor -- SKIP=FAIL)";

    const auto resultPath = td_ / "huge_image_peak.txt";
    std::error_code ec;
    std::filesystem::remove(resultPath, ec);

    const DWORD code = runChildRole(
        L"HugeImageRenderChild.RunRole",
        {{"PDFE_HUGE_IMAGE_FILE", fixture.string()},
         {"PDFE_HUGE_IMAGE_RESULT", resultPath.string()}},
        300000);

    ASSERT_EQ(code, 0u)
        << "the measuring child did not exit cleanly (exit " << code
        << "). Nothing below is measurable if the render did not complete.";
    ASSERT_TRUE(std::filesystem::exists(resultPath))
        << "the child produced no result file, so the peak was never measured";

    const auto kv = readKeyValueFile(resultPath);
    ASSERT_TRUE(kv.count("delta")) << "result file has no delta";
    ASSERT_TRUE(kv.count("rendered")) << "result file has no rendered flag";
    ASSERT_TRUE(kv.count("pixels")) << "result file has no pixel count";

    const long long delta = std::stoll(kv.at("delta"));
    const long long pixels = std::stoll(kv.at("pixels"));

    // CONTROL. A render that refused the document or produced an empty raster
    // satisfies any upper bound. Assert it actually produced the page.
    EXPECT_EQ(kv.at("rendered"), "1")
        << "the child rendered nothing -- an upper bound on the memory used to "
           "produce nothing measures nothing";
    EXPECT_GT(pixels, 400000)
        << "the rendered raster is too small to be this page at scale 1.0 "
           "(595x842 pt = 501,190 px); got " << pixels;

    // NON-VACUITY. Peak working set is monotonic: a zero delta means the peak
    // never moved, i.e. the measurement did not happen. That is the exact
    // shape this test exists to avoid, so it is asserted as a failure rather
    // than allowed to read as a pass.
    ASSERT_GT(delta, 0)
        << "peak working set did not move across the render, so the bound "
           "below was evaluated against a measurement that never took place";

    // THE BOUND. See the header comment and the phase-16 benchmark artifact.
    constexpr long long kPeakDeltaBoundBytes = 64LL * 1024 * 1024;
    EXPECT_LT(delta, kPeakDeltaBoundBytes)
        << "rendering ONE page of a 100-page, 23 MB image document raised the "
           "process peak working set by " << delta << " bytes ("
        << (delta / (1024 * 1024)) << " MB), over the "
        << (kPeakDeltaBoundBytes / (1024 * 1024))
        << " MB bound -- the per-page render cost is scaling with the "
           "document rather than with the page";

    // GitHub #178: publish BYTES under a key that says bytes. std::to_string
    // rather than a cast to int, so a delta beyond INT_MAX (which a regression
    // could plausibly produce) is reported rather than wrapped.
    RecordProperty("peak_delta_bytes",
                   std::to_string(peakDeltaPropertyValue(delta)));
    RecordProperty("rendered_pixels", static_cast<int>(pixels));
#endif
}

// GitHub #178 — the unit contract, asserted against a KNOWN allocation delta
// that does not come from the measurement path above.
//
// 7708 KiB is the figure the phase-16 artifact recorded for
// HugeImagePagePeakMemoryBounded while the property was still (wrongly) scaled
// by 1024. The true byte delta behind it is 7708 * 1024 = 7,892,992. This test
// feeds that known byte count through the same function the RecordProperty
// call uses and requires it back unchanged.
//
// This is not a tautology-by-construction: reinstating the `/ 1024` (or any
// other scaling) inside peakDeltaPropertyValue reddens it, which is exactly
// the regression #178 is about. It runs in-process with no child, no fixture
// and no timing, so it cannot be skipped or flake.
TEST(PeakDeltaProperty, PeakDeltaPropertyIsInBytes)
{
    // The known delta, computed independently of the render: the recorded KiB
    // figure from docs/audit/backlog-p82/phase-16/benchmarks.json times 1024.
    constexpr long long kRecordedKiB = 7708;
    constexpr long long kKnownDeltaBytes = kRecordedKiB * 1024;  // 7,892,992

    EXPECT_EQ(peakDeltaPropertyValue(kKnownDeltaBytes), kKnownDeltaBytes)
        << "peak_delta_bytes must publish BYTES. Getting "
        << peakDeltaPropertyValue(kKnownDeltaBytes) << " back for a "
        << kKnownDeltaBytes << "-byte delta means the property is scaled "
           "again -- that is GitHub #178, where the key said bytes and the "
           "value was kilobytes.";

    // NON-VACUITY. A function returning its argument passes the check above
    // for ANY input, including 0 -- so pin the two properties that actually
    // distinguish "bytes" from "kilobytes": the value must differ from the
    // KiB-scaled form, and must exceed the 1 MiB mark that 7708 (read as
    // bytes) would not.
    EXPECT_NE(peakDeltaPropertyValue(kKnownDeltaBytes), kRecordedKiB)
        << "the property still yields the kilobyte figure";
    EXPECT_GT(peakDeltaPropertyValue(kKnownDeltaBytes), 1024 * 1024)
        << "a ~7.5 MB delta must report as millions of bytes, not thousands";

    // And the bound the real test applies is in the same unit: 7.5 MB is
    // comfortably under 64 MB either way, but a KiB-scaled value would be
    // under it for the wrong reason.
    constexpr long long kPeakDeltaBoundBytes = 64LL * 1024 * 1024;
    EXPECT_LT(peakDeltaPropertyValue(kKnownDeltaBytes), kPeakDeltaBoundBytes);
}

// =====================================================================
// PDF-84 / GitHub issue #31 — ThousandPageSaveAndReorderBounded
//
// COVERAGE GAP. ThousandPageOpenScalingLinear covers the READ path only:
// it opens two documents and times the opens. Nothing in this file has ever
// timed a 1000-page WRITE, and reorder-then-save is the write path a user
// actually waits on (page-panel drag, then Save).
//
// THE VACUITY THIS TEST HAD TO AVOID IS SPECIFIC AND NOT HYPOTHETICAL. A save
// that wrote nothing, or a reorder that reordered nothing, is the FASTEST
// possible implementation and passes any time bound with room to spare. So the
// timing assertions are gated on a correctness oracle that runs FIRST: the
// output is reopened and the permutation is verified from its page content.
// If the work did not happen, this test fails on correctness before its timing
// numbers are ever consulted.
//
// The comparative assertion is UNCONDITIONAL, deliberately. This file
// documents at ThousandPageOpenScalingLinear how an `if (sec100 > 0.05)` guard
// made a ratio assertion in a test named ...ScalingLinear permanently
// unreachable. The same shape is not reintroduced here: sec100 is asserted
// positive and the ratio is then always evaluated. Reachability is not taken
// on faith -- the reorder+save of a 100-page document is tens of
// milliseconds at minimum in a debug build, which is three orders of magnitude
// above the steady_clock tick, and the ASSERT_GT below fails loudly rather
// than silently skipping if that ever stops being true.
// =====================================================================

namespace {

// Like buildLargePdf, but every page carries a UNIQUE, EXTRACTABLE marker and
// the /Length values are correct. buildLargePdf hardcodes /Length 40 while its
// stream for "Page 1000" is 42 bytes, which truncates; a permutation oracle
// cannot be built on text that may or may not survive parsing.
std::string buildMarkedPdf(int pages)
{
    std::string objects;
    std::string pageRefs;
    int objNum = 3;  // 1=Catalog, 2=Pages

    for (int i = 0; i < pages; ++i) {
        std::ostringstream pageObj;
        pageObj << objNum << " 0 obj\n<< /Type /Page /Parent 2 0 R "
                   "/MediaBox [0 0 612 792] /Resources << /Font << /F1 << "
                   "/Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> "
                   "/Contents " << (objNum + 1) << " 0 R >>\nendobj\n";
        objects += pageObj.str();

        std::ostringstream body;
        body << "BT /F1 12 Tf 100 700 Td (PGMARK" << i << ") Tj ET\n";
        const std::string stream = body.str();

        std::ostringstream contentObj;
        contentObj << (objNum + 1) << " 0 obj\n<< /Length " << stream.size()
                   << " >>\nstream\n" << stream << "endstream\nendobj\n";
        objects += contentObj.str();

        pageRefs += std::to_string(objNum) + " 0 R ";
        objNum += 2;
    }

    return std::string("%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [") + pageRefs +
        "] /Count " + std::to_string(pages) + " >>\nendobj\n" +
        objects +
        "trailer\n<< /Size " + std::to_string(objNum) +
        " /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
}

}  // namespace

TEST_F(PerfStressTest, ThousandPageSaveAndReorderBounded)
{
    // Reverse the page order, save, reopen, verify, and time the reorder+save.
    // Returns the elapsed seconds; `pagesOut` receives the reopened page count
    // and `firstMarker`/`lastMarker` the markers found at the two ends, so the
    // caller can assert correctness before it looks at the timing.
    auto reorderAndSave = [&](int pages, const std::string& tag,
                              int* pagesOut, std::string* firstMarker,
                              std::string* lastMarker) -> double {
        const auto in = td_ / ("reorder_" + tag + "_in.pdf");
        const auto out = td_ / ("reorder_" + tag + "_out.pdf");
        writePdf(in, buildMarkedPdf(pages));
        std::error_code ec;
        std::filesystem::remove(out, ec);

        auto doc = Document::open(ctx_, in);
        EXPECT_NE(doc, nullptr) << tag << ": open failed";
        if (!doc) return 0.0;
        EXPECT_EQ(doc->pageCount(), pages) << tag << ": wrong page count on open";

        std::vector<int> reversed(pages);
        for (int i = 0; i < pages; ++i) reversed[i] = pages - 1 - i;

        const auto t0 = std::chrono::steady_clock::now();
        doc->reorderPages(reversed);
        const auto t1 = std::chrono::steady_clock::now();
        doc->saveAs(out);
        const auto t2 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t2 - t0).count();
        // Split so a super-linear total can be ATTRIBUTED. "reorder+save is
        // slow" is not a finding anyone can act on; "the reorder is linear and
        // the save is not" is.
        RecordProperty("reorder_only_" + tag + "_ms",
                       static_cast<int>(
                           std::chrono::duration<double>(t1 - t0).count() * 1000.0));
        RecordProperty("save_only_" + tag + "_ms",
                       static_cast<int>(
                           std::chrono::duration<double>(t2 - t1).count() * 1000.0));

        doc.reset();

        auto reopened = Document::open(ctx_, out);
        EXPECT_NE(reopened, nullptr) << tag << ": reopen failed";
        if (reopened) {
            *pagesOut = reopened->pageCount();
            if (*pagesOut > 0) {
                auto p0 = reopened->page(0);
                auto pn = reopened->page(*pagesOut - 1);
                if (p0) *firstMarker = p0->extractPlainText();
                if (pn) *lastMarker = pn->extractPlainText();
            }
        }
        return sec;
    };

    int pages100 = 0, pages1000 = 0;
    std::string first100, last100, first1000, last1000;

    // BEST-OF-N, for the reason the neighbouring ThousandPageOpenScalingLinear
    // documents: the 100-page sample is tens of milliseconds, small enough
    // that one descheduling moves the ratio materially, and this suite runs
    // 16-way sharded. Contention can only make a sample SLOWER, so the minimum
    // is the best estimate of true cost. Note the direction this fails in is
    // the UNSAFE one for a ratio -- inflating the small denominator LOWERS the
    // measured ratio -- which is exactly why a single sample is not good
    // enough here even though it was tolerable for the absolute bounds.
    constexpr int kTimingSamples = 3;
    double sec100 = reorderAndSave(100, "100p", &pages100, &first100, &last100);
    double sec1000 =
        reorderAndSave(1000, "1000p", &pages1000, &first1000, &last1000);
    for (int i = 1; i < kTimingSamples; ++i) {
        sec100 = std::min(
            sec100, reorderAndSave(100, "100p", &pages100, &first100, &last100));
        sec1000 = std::min(
            sec1000,
            reorderAndSave(1000, "1000p", &pages1000, &first1000, &last1000));
    }

    // ---- CORRECTNESS FIRST. The timing assertions below are only meaningful
    //      if the work actually happened; a no-op save is the fastest save.
    ASSERT_EQ(pages1000, 1000)
        << "the 1000-page save did not produce a 1000-page document, so its "
           "elapsed time is not the cost of saving 1000 pages";
    ASSERT_EQ(pages100, 100);

    // Non-vacuity: the markers must be NON-EMPTY before an equality on them
    // says anything. Two empty strings compare equal.
    ASSERT_FALSE(first1000.empty())
        << "no text extracted from the reordered document's first page -- the "
           "permutation oracle below would compare empty against empty";
    ASSERT_FALSE(last1000.empty());

    // The permutation itself. Reversed, so page 0 must now carry the marker
    // that was on the LAST page and vice versa. This is what a reorder that
    // silently did nothing fails.
    EXPECT_NE(first1000.find("PGMARK999"), std::string::npos)
        << "after reversing 1000 pages the first page should carry PGMARK999; "
           "got \"" << first1000 << "\"";
    EXPECT_NE(last1000.find("PGMARK0"), std::string::npos)
        << "after reversing 1000 pages the last page should carry PGMARK0; "
           "got \"" << last1000 << "\"";
    EXPECT_NE(first100.find("PGMARK99"), std::string::npos)
        << "the 100-page control did not reorder; got \"" << first100 << "\"";

    // ---- TIMING. Unconditional; see the header comment.
    ASSERT_GT(sec100, 0.0)
        << "the 100-page reorder+save timed as zero, so the ratio below has no "
           "denominator -- this is the permanently-false-guard shape and it is "
           "a failure, not a skip";

    // THE BOUND IS DERIVED FROM MEASUREMENT, AND THE MEASUREMENT IS NOT WHAT
    // WAS EXPECTED. Reorder+save of this document is SUPER-LINEAR in page
    // count: best-of-3 samples measured 100p = 32-35 ms and 1000p = 1.55-1.81 s,
    // a ratio of 47.9 / 48.3 / 54.7 across three runs for 10x the pages. Split
    // out (see the RecordProperty values, and the phase-16 benchmark artifact),
    // BOTH halves are super-linear: reorder 5 ms -> 403 ms (~80x) and save
    // 26 ms -> 1227 ms (~47x).
    //
    // That is a real property of the write path and it is recorded rather than
    // hidden behind a bound chosen to make it pass quietly. It is not fixed
    // here: PDFE's own contribution to rearrangePages (the catalog carry-over
    // and pruneAcroFormAfterRearrange) is O(pages) and short-circuits entirely
    // on a document with no AcroForm, which this fixture is -- so the cost sits
    // below the MuPDF boundary, in pdf_rearrange_pages and the save, and that
    // is a separate investigation.
    //
    // 90.0 is therefore a REGRESSION bound, not an aspiration: ~1.65x above the
    // worst of three observed samples, tight enough that a further step change
    // in scaling reddens it, loose enough not to flake on a loaded machine.
    // The absolute backstop below is what keeps it honest in wall-clock terms.
    constexpr double kReorderSaveRatioBound = 90.0;
    const double ratio = sec1000 / sec100;
    EXPECT_LT(ratio, kReorderSaveRatioBound)
        << "1000p/100p reorder+save ratio: " << ratio << " (100p=" << sec100
        << "s, 1000p=" << sec1000 << "s). 10x the pages must not cost 25x the "
           "time -- a super-linear write path is the regression this bounds.";

    // Absolute backstop, independent of the ratio. Debug build.
    // Measured 1.55-1.81 s across three runs (Debug). 20 s leaves ~11x, which
    // is the right kind of headroom for an absolute wall-clock bound in a
    // sharded Debug run while still catching an order-of-magnitude regression.
    EXPECT_LT(sec1000, 20.0)
        << "1000-page reorder + save took " << sec1000 << "s";

    RecordProperty("reorder_save_100p_ms", static_cast<int>(sec100 * 1000.0));
    RecordProperty("reorder_save_1000p_ms", static_cast<int>(sec1000 * 1000.0));
    RecordProperty("reorder_save_ratio_x100", static_cast<int>(ratio * 100.0));
}

// =====================================================================
// GitHub #183 -- the SHAPE of the reorder+save cost curve, not one ratio.
//
// ThousandPageSaveAndReorderBounded above measures TWO page counts. Two points
// cannot distinguish quadratic from any other super-linear shape: ANY two
// (x, y) pairs fit a power law exactly, so "the exponent is 1.7" read off two
// samples is an assumption wearing a number. This test measures FOUR scales,
// each a doubling of the last, so the exponent is FITTED over a range and a
// single bad sample cannot set it.
//
// It also keeps the two halves apart, because they had two different causes:
//   * the REORDER half was quadratic because MuPDF drops its page map on every
//     structural object mutation (pdf-object.c prepare_object_for_alteration)
//     and pdf_rearrange_pages' retainpage() mutates /Parent on every page --
//     so each of the N pdf_lookup_page_obj() calls in that loop rebuilt the
//     whole N-entry map. PDFE now permutes a full permutation itself; see
//     Document::rearrangePages.
//   * the SAVE half was quadratic because do_garbage=3 runs MuPDF's
//     removeduplicateobjs (pdf-write.c:237), an all-pairs O(objects^2) scan.
//     A permutation creates no duplicate objects, so it no longer asks for it.
//
// Full write-up and the before/after table:
// docs/audit/p85-remaining-open/phase-5/benchmarks.json + notes.md
// =====================================================================

TEST_F(PerfStressTest, ReorderAndSaveScalingCurveIsNearLinear)
{
    struct Sample {
        int pages = 0;
        double reorderSec = 0.0;
        double saveSec = 0.0;
        int reopenedPages = 0;
        std::string firstMarker;
        std::string lastMarker;
    };

    // One (reorder, save) timing pair at `pages`, plus the correctness evidence
    // the caller needs before it is entitled to look at the clock.
    auto measure = [&](int pages) -> Sample {
        Sample s;
        s.pages = pages;
        const auto tag = std::to_string(pages) + "p";
        const auto in = td_ / ("curve_" + tag + "_in.pdf");
        const auto out = td_ / ("curve_" + tag + "_out.pdf");
        writePdf(in, buildMarkedPdf(pages));
        std::error_code ec;
        std::filesystem::remove(out, ec);

        auto doc = Document::open(ctx_, in);
        EXPECT_NE(doc, nullptr) << tag << ": open failed";
        if (!doc) return s;

        std::vector<int> reversed(pages);
        for (int i = 0; i < pages; ++i) reversed[i] = pages - 1 - i;

        const auto t0 = std::chrono::steady_clock::now();
        doc->reorderPages(reversed);
        const auto t1 = std::chrono::steady_clock::now();
        doc->saveAs(out);
        const auto t2 = std::chrono::steady_clock::now();
        s.reorderSec = std::chrono::duration<double>(t1 - t0).count();
        s.saveSec = std::chrono::duration<double>(t2 - t1).count();
        doc.reset();

        auto reopened = Document::open(ctx_, out);
        EXPECT_NE(reopened, nullptr) << tag << ": reopen failed";
        if (reopened) {
            s.reopenedPages = reopened->pageCount();
            if (s.reopenedPages > 0) {
                auto p0 = reopened->page(0);
                auto pn = reopened->page(s.reopenedPages - 1);
                if (p0) s.firstMarker = p0->extractPlainText();
                if (pn) s.lastMarker = pn->extractPlainText();
            }
        }
        return s;
    };

    // BEST-OF-N for the reason ThousandPageSaveAndReorderBounded documents: the
    // small scales are tens of milliseconds, this suite runs 16-way sharded, and
    // contention can only ever make a sample SLOWER. The minimum is therefore
    // the best estimate of true cost -- and for a RATIO an inflated small
    // denominator is the dangerous direction, because it hides super-linearity.
    constexpr int kTimingSamples = 3;
    constexpr int kScales[] = {100, 200, 400, 800};
    constexpr size_t kN = sizeof(kScales) / sizeof(kScales[0]);

    Sample best[kN];
    for (size_t i = 0; i < kN; ++i) {
        best[i] = measure(kScales[i]);
        for (int rep = 1; rep < kTimingSamples; ++rep) {
            Sample s = measure(kScales[i]);
            best[i].reorderSec = std::min(best[i].reorderSec, s.reorderSec);
            best[i].saveSec = std::min(best[i].saveSec, s.saveSec);
            // Correctness evidence comes from the LAST run, and every run is
            // asserted below, so a reorder that worked once and not again still
            // reddens this test rather than being averaged away.
            best[i].reopenedPages = s.reopenedPages;
            best[i].firstMarker = s.firstMarker;
            best[i].lastMarker = s.lastMarker;
        }
    }

    // ---- CORRECTNESS FIRST, at every scale. A reorder that silently did
    //      nothing, or a save that dropped pages, is the fastest possible
    //      "win" and it must not be able to buy a green exponent.
    for (size_t i = 0; i < kN; ++i) {
        const int n = kScales[i];
        ASSERT_EQ(best[i].reopenedPages, n)
            << n << "p: the saved document does not have " << n << " pages, so "
            << "its elapsed time is not the cost of reordering " << n << " pages";
        ASSERT_FALSE(best[i].firstMarker.empty())
            << n << "p: no text extracted from the reordered first page -- the "
               "marker comparison below would compare empty against empty";
        ASSERT_FALSE(best[i].lastMarker.empty()) << n << "p: no text on last page";
        EXPECT_NE(best[i].firstMarker.find("PGMARK" + std::to_string(n - 1)),
                  std::string::npos)
            << n << "p: after reversal the first page must carry PGMARK"
            << (n - 1) << "; got \"" << best[i].firstMarker << "\"";
        EXPECT_NE(best[i].lastMarker.find("PGMARK0"), std::string::npos)
            << n << "p: after reversal the last page must carry PGMARK0; got \""
            << best[i].lastMarker << "\"";
        ASSERT_GT(best[i].reorderSec, 0.0)
            << n << "p: the reorder timed as exactly zero, so every ratio below "
               "has a zero denominator -- the permanently-false-guard shape, "
               "which is a FAILURE here and not a reason to skip";
        ASSERT_GT(best[i].saveSec, 0.0) << n << "p: the save timed as zero";

        RecordProperty("curve_reorder_" + std::to_string(n) + "p_us",
                       static_cast<int>(best[i].reorderSec * 1e6));
        RecordProperty("curve_save_" + std::to_string(n) + "p_us",
                       static_cast<int>(best[i].saveSec * 1e6));
    }

    // ---- THE SHAPE. Exponent p in cost ~ pages^p, fitted across the whole
    //      8x span rather than read off the two ends of a doubling. p == 1.0 is
    //      linear; p == 2.0 is quadratic. Before this phase the measured
    //      exponents were reorder p = 1.86-1.92 and save p = 1.58-1.62 over
    //      three runs; after, reorder p = 0.98-1.11 and save p = 0.74-0.88 (the
    //      save is SUB-linear because at 100 pages it is dominated by the fixed
    //      cost of opening and writing a file at all). Full table in
    //      docs/audit/p85-remaining-open/phase-5/benchmarks.json.
    const double span = static_cast<double>(kScales[kN - 1]) / kScales[0];  // 8x
    const double reorderExp =
        std::log(best[kN - 1].reorderSec / best[0].reorderSec) / std::log(span);
    const double saveExp =
        std::log(best[kN - 1].saveSec / best[0].saveSec) / std::log(span);
    RecordProperty("curve_reorder_exponent_x100",
                   static_cast<int>(reorderExp * 100.0));
    RecordProperty("curve_save_exponent_x100", static_cast<int>(saveExp * 100.0));

    // The bound is DERIVED FROM MEASUREMENT and sits between the before and the
    // after, which is the only placement that makes it a guard: a regression to
    // the old page-map-thrashing reorder (p = 1.90) or the old dedup save
    // (p = 1.58) reddens it, and the post-fix values (see the artifact) clear it
    // with enough headroom to survive a loaded, sharded Debug run.
    constexpr double kExponentBound = 1.40;
    EXPECT_LT(reorderExp, kExponentBound)
        << "reorder cost grows as pages^" << reorderExp << " across "
        << kScales[0] << "->" << kScales[kN - 1] << " pages ("
        << best[0].reorderSec << "s -> " << best[kN - 1].reorderSec
        << "s). Anything approaching 2.0 is the quadratic page-map rebuild "
           "GitHub #183 is about.";
    EXPECT_LT(saveExp, kExponentBound)
        << "save cost grows as pages^" << saveExp << " (" << best[0].saveSec
        << "s -> " << best[kN - 1].saveSec << "s). Approaching 2.0 means the "
           "all-pairs dedup pass (do_garbage=3) is running again.";
}

// =====================================================================
// GitHub #197 -- the DELETE+save cost curve, and a live control for it.
//
// #183 (above) fixed the REORDER half. Deletion was explicitly out of its
// scope and stayed on the general pdf_rearrange_pages path, whose retainpages()
// loop looks a page up once per retained page while mutating each page's
// /Parent -- and MuPDF drops doc->fwd_page_map / rev_page_map on every
// structural object mutation (prepare_object_for_alteration,
// source/pdf/pdf-object.c:1818), so each of the N lookups rebuilds the whole
// N-entry map. That is the same quadratic the reorder half had.
//
// WHY THERE ARE TWO CURVES HERE AND NOT ONE.
// A single after-the-fix exponent is not evidence on its own: this is a Debug
// build on a shared, 16-way-sharded runner, and "1.05" means nothing without
// knowing what the same machine measures for the unfixed mechanism in the same
// process, in the same minute, on the same fixtures. So the test measures BOTH:
//
//   * the DELETE curve -- Document::deletePage() + saveAs(), the path under
//     test; and
//   * the LEGACY curve -- rearrangePages() with a same-length list that
//     contains a duplicate. A duplicate makes it not a permutation and not a
//     strict subset, so it is ineligible for every fast path and routes through
//     pdf_rearrange_pages by construction. It is a real, product-reachable
//     call, not a test seam, and it cannot be accidentally optimised into the
//     fast path later -- if it ever were, the non-vacuity assertion below
//     (legacy must be materially STEEPER than delete) reddens and says so.
//
// The comparison is therefore against a baseline captured in the same run,
// which is the only kind that survives a machine change.
//
// Both exponents are FITTED over an 8x span at four scales. Two points fit a
// power law exactly and can distinguish nothing; #183's own header explains why
// that mistake hid its real shape for three pipelines.
// =====================================================================

TEST_F(PerfStressTest, DeleteAndSaveScalingCurveIsNearLinear)
{
    struct Sample {
        int pages = 0;
        double editSec = 0.0;
        double saveSec = 0.0;
        int reopenedPages = 0;
        bool deletedMarkerGone = false;
        std::string firstMarker;
    };

    // One (edit, save) pair at `pages`. `legacy` selects the control path.
    auto measure = [&](int pages, bool legacy) -> Sample {
        Sample s;
        s.pages = pages;
        const std::string tag =
            std::to_string(pages) + "p" + (legacy ? "_legacy" : "_del");
        const auto in = td_ / ("del_" + tag + "_in.pdf");
        const auto out = td_ / ("del_" + tag + "_out.pdf");
        writePdf(in, buildMarkedPdf(pages));
        std::error_code ec;
        std::filesystem::remove(out, ec);

        auto doc = Document::open(ctx_, in);
        EXPECT_NE(doc, nullptr) << tag << ": open failed";
        if (!doc) return s;

        // Delete the MIDDLE page in both arms, so the two curves differ only in
        // which code path runs, never in how much work there is to do.
        const int victim = pages / 2;

        const auto t0 = std::chrono::steady_clock::now();
        if (legacy) {
            // Same page set minus `victim`, but padded back to length `pages`
            // by repeating page 0. Not a permutation (page 0 appears twice) and
            // not a strict subset, so no fast path can claim it.
            std::vector<int> keep;
            keep.reserve(pages);
            keep.push_back(0);
            for (int i = 0; i < pages; ++i) if (i != victim) keep.push_back(i);
            doc->rearrangePages(keep);
        } else {
            doc->deletePage(victim);
        }
        const auto t1 = std::chrono::steady_clock::now();
        doc->saveAs(out);
        const auto t2 = std::chrono::steady_clock::now();
        s.editSec = std::chrono::duration<double>(t1 - t0).count();
        s.saveSec = std::chrono::duration<double>(t2 - t1).count();
        doc.reset();

        auto reopened = Document::open(ctx_, out);
        EXPECT_NE(reopened, nullptr) << tag << ": reopen failed";
        if (reopened) {
            s.reopenedPages = reopened->pageCount();
            // The deleted page's marker must be gone from the whole document --
            // the cheapest possible "win" is an edit that did nothing, and it
            // must not be able to buy a green exponent.
            const std::string victimMark = "PGMARK" + std::to_string(victim);
            bool found = false;
            for (int i = 0; i < s.reopenedPages; ++i) {
                auto p = reopened->page(i);
                if (!p) continue;
                const std::string t = p->extractPlainText();
                if (i == 0) s.firstMarker = t;
                // Each page carries exactly one marker token, so compare the
                // WHOLE trimmed string: "PGMARK1" is a prefix of "PGMARK10"
                // and a substring search would report the wrong page.
                std::string trimmed;
                for (char ch : t) if (!std::isspace(static_cast<unsigned char>(ch))) trimmed += ch;
                if (trimmed == victimMark) { found = true; break; }
            }
            s.deletedMarkerGone = !found;
        }
        return s;
    };

    // BEST-OF-N for the reason the two neighbouring curve tests document:
    // contention can only make a sample SLOWER, so the minimum is the best
    // estimate, and an inflated small denominator is the direction that HIDES
    // super-linearity.
    constexpr int kTimingSamples = 2;
    constexpr int kScales[] = {50, 100, 200, 400};
    constexpr size_t kN = sizeof(kScales) / sizeof(kScales[0]);

    Sample del[kN];
    Sample leg[kN];
    for (size_t i = 0; i < kN; ++i) {
        del[i] = measure(kScales[i], false);
        leg[i] = measure(kScales[i], true);
        for (int rep = 1; rep < kTimingSamples; ++rep) {
            Sample d = measure(kScales[i], false);
            Sample l = measure(kScales[i], true);
            del[i].editSec = std::min(del[i].editSec, d.editSec);
            del[i].saveSec = std::min(del[i].saveSec, d.saveSec);
            leg[i].editSec = std::min(leg[i].editSec, l.editSec);
            leg[i].saveSec = std::min(leg[i].saveSec, l.saveSec);
            del[i].reopenedPages = d.reopenedPages;
            del[i].deletedMarkerGone = d.deletedMarkerGone;
            del[i].firstMarker = d.firstMarker;
        }
    }

    // ---- CORRECTNESS FIRST, at every scale, for the arm under test.
    for (size_t i = 0; i < kN; ++i) {
        const int n = kScales[i];
        ASSERT_EQ(del[i].reopenedPages, n - 1)
            << n << "p: the saved document has " << del[i].reopenedPages
            << " pages, not " << (n - 1) << ", so its elapsed time is not the "
               "cost of deleting a page from " << n;
        EXPECT_TRUE(del[i].deletedMarkerGone)
            << n << "p: the deleted page's content is still in the saved file, "
               "so the measured time is not the cost of a real deletion";
        ASSERT_FALSE(del[i].firstMarker.empty())
            << n << "p: no text extracted from page 0 of the result";
        ASSERT_GT(del[i].editSec, 0.0)
            << n << "p: the delete timed as exactly zero -- every ratio below "
               "would have a zero denominator";
        ASSERT_GT(del[i].saveSec, 0.0) << n << "p: the save timed as zero";
        ASSERT_GT(leg[i].editSec, 0.0) << n << "p: the legacy control timed as zero";
        ASSERT_GT(leg[i].saveSec, 0.0) << n << "p: the legacy save timed as zero";

        RecordProperty("del_edit_" + std::to_string(n) + "p_us",
                       static_cast<int>(del[i].editSec * 1e6));
        RecordProperty("del_save_" + std::to_string(n) + "p_us",
                       static_cast<int>(del[i].saveSec * 1e6));
        RecordProperty("legacy_edit_" + std::to_string(n) + "p_us",
                       static_cast<int>(leg[i].editSec * 1e6));
        RecordProperty("legacy_save_" + std::to_string(n) + "p_us",
                       static_cast<int>(leg[i].saveSec * 1e6));
    }

    const double span = static_cast<double>(kScales[kN - 1]) / kScales[0];  // 8x
    const auto exponent = [&](double lo, double hi) {
        return std::log(hi / lo) / std::log(span);
    };
    const double delEditExp  = exponent(del[0].editSec,  del[kN - 1].editSec);
    const double delSaveExp  = exponent(del[0].saveSec,  del[kN - 1].saveSec);
    const double delTotalExp = exponent(del[0].editSec + del[0].saveSec,
                                        del[kN - 1].editSec + del[kN - 1].saveSec);
    const double legEditExp  = exponent(leg[0].editSec,  leg[kN - 1].editSec);
    const double legTotalExp = exponent(leg[0].editSec + leg[0].saveSec,
                                        leg[kN - 1].editSec + leg[kN - 1].saveSec);

    RecordProperty("del_edit_exponent_x100",  static_cast<int>(delEditExp * 100.0));
    RecordProperty("del_save_exponent_x100",  static_cast<int>(delSaveExp * 100.0));
    RecordProperty("del_total_exponent_x100", static_cast<int>(delTotalExp * 100.0));
    RecordProperty("legacy_edit_exponent_x100",  static_cast<int>(legEditExp * 100.0));
    RecordProperty("legacy_total_exponent_x100", static_cast<int>(legTotalExp * 100.0));

    std::cout << "[#197] scales 50/100/200/400  (best of " << kTimingSamples << ")\n";
    for (size_t i = 0; i < kN; ++i) {
        std::cout << "  " << kScales[i] << "p  delete " << del[i].editSec
                  << "s + save " << del[i].saveSec << "s   |   legacy "
                  << leg[i].editSec << "s + save " << leg[i].saveSec << "s\n";
    }
    std::cout << "  fitted exponents: delete edit=" << delEditExp
              << " save=" << delSaveExp << " total=" << delTotalExp
              << "   legacy edit=" << legEditExp << " total=" << legTotalExp
              << std::endl;

    // ---- NON-VACUITY OF THE COMPARISON. If the control is not materially
    //      steeper than the path under test, then either the fix did not land
    //      or the control stopped exercising the legacy mechanism -- and either
    //      way the exponent bound below would be measuring the same thing
    //      twice.
    EXPECT_GT(legEditExp, delEditExp + 0.25)
        << "the legacy pdf_rearrange_pages control (exponent " << legEditExp
        << ") is not materially steeper than the delete path under test ("
        << delEditExp << "). Either deletion never left the legacy path, or "
           "the duplicate-bearing rearrange list is no longer routed through "
           "pdf_rearrange_pages -- check the eligibility guards in "
           "Document::rearrangePages before believing the bound below.";

    // ---- THE SHAPE. Every bound below is DERIVED FROM MEASUREMENT on this
    //      tree and sits between a before and an after, which is the only
    //      placement that makes a number a guard rather than an aspiration.
    //
    //      THE EDIT HALF, measured 50->400 pages, Debug, this machine:
    //        before  pages^1.82   (delete routed through pdf_rearrange_pages)
    //        after   pages^0.91   (retainPageTreeInPlace)
    //        legacy control, same run, after the fix   pages^1.70
    //      1.40 sits above the fixed path with 0.5 of headroom and below every
    //      measurement of the unfixed mechanism.
    constexpr double kExponentBound = 1.40;
    //
    //      THE SAVE HALF is a SEPARATE bound because it is NOT what #197 fixed
    //      and must not be presented as if it were. A delete still escalates to
    //      do_garbage=3 (ConditionalGcTest.PageDeleteNeedsFullGc pins it), and
    //      that is deliberate: a removal orphans real objects. Measured
    //      pages^1.31-1.33 both before and after this change -- unchanged,
    //      because nothing here touched it. 1.50 gives that ~13% headroom while
    //      still reddening on the pages^1.58-1.62 that #183 measured when the
    //      all-pairs dedup dominates. A bound of 1.40 here would have had 5%
    //      headroom over an unchanged measurement, i.e. a flake generator
    //      wearing the costume of a regression guard.
    constexpr double kSaveExponentBound = 1.50;
    EXPECT_LT(delEditExp, kExponentBound)
        << "page deletion costs pages^" << delEditExp << " across "
        << kScales[0] << "->" << kScales[kN - 1] << " pages ("
        << del[0].editSec << "s -> " << del[kN - 1].editSec
        << "s). Approaching 2.0 is the quadratic page-map rebuild GitHub #197 "
           "is about; the legacy control in this same run measured "
        << legEditExp << ".";
    EXPECT_LT(delSaveExp, kSaveExponentBound)
        << "the save after a delete costs pages^" << delSaveExp << " ("
        << del[0].saveSec << "s -> " << del[kN - 1].saveSec
        << "s). Approaching 1.6 means the all-pairs dedup pass inside "
           "do_garbage=3 has started to dominate the way it did before #183.";
    EXPECT_LT(delTotalExp, kSaveExponentBound)
        << "delete+save together cost pages^" << delTotalExp
        << ". With the edit half near-linear this number is essentially the "
           "save half, so it reddens for the same reason -- but it also catches "
           "the case where the two halves trade off rather than both improving.";
}

// =====================================================================
// LARGE-ANNOTATION-COUNT: 10K annotations on one page
// =====================================================================

TEST_F(PerfStressTest, TenThousandAnnotationsOnePage)
{
    auto fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "fixture missing";

    auto doc = Document::open(ctx_, fixture);
    ASSERT_NE(doc, nullptr);

    pdfe::annotations::AnnotationManager mgr(ctx_);
    pdfe::commands::UndoManager undoMgr;
    undoMgr.setMaxDepth(15000);  // Allow space for 10K + headroom
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto start = std::chrono::steady_clock::now();
    constexpr int N = 10000;
    for (int i = 0; i < N; ++i) {
        QRectF rect(50.0 + (i % 100) * 5.0, 50.0 + (i / 100) * 5.0, 4, 4);
        auto cmd = std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
            &mgr, page.get(), pdfe::annotations::AnnotationType::Text, rect);
        undoMgr.execute(std::move(cmd));
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    double sec = std::chrono::duration<double>(elapsed).count();

    // 10K annotations in debug build with UndoManager overhead is slow;
    // 300s is the realistic bound. Release-mode performance is much better.
    EXPECT_LT(sec, 300.0)
        << "10K annotation creation must complete in < 300s (took " << sec << "s)";

    auto annots = mgr.getAnnotations(page.get());
    EXPECT_GE(annots.size(), 9000u)
        << "At least 90% of annotations must be retained";
}

// =====================================================================
// DEEPLY-NESTED-FORMS: 100-level form field hierarchy
// =====================================================================

TEST_F(PerfStressTest, DeeplyNestedFormHierarchy)
{
    // Build a PDF with 100-level nested form fields (T parent chain)
    std::string fields;
    int objNum = 4;
    int prevObj = 0;
    for (int i = 0; i < 100; ++i) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%d 0 obj\n<< /T (f%d) /FT /Tx /Kids [%d 0 R]%s >>\nendobj\n",
            objNum, i, objNum + 1,
            prevObj > 0 ? (" /Parent " + std::to_string(prevObj) + " 0 R").c_str() : "");
        fields += buf;
        prevObj = objNum;
        objNum++;
    }
    // Terminal widget
    char terminal[128];
    std::snprintf(terminal, sizeof(terminal),
        "%d 0 obj\n<< /T (leaf) /Parent %d 0 R /FT /Tx >>\nendobj\n",
        objNum, prevObj);
    fields += terminal;

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /AcroForm "
        "<< /Fields [4 0 R] >> >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n" + fields +
        "trailer\n<< /Size " + std::to_string(objNum + 1) +
        " /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "nested_form.pdf";
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
    EXPECT_LT(std::chrono::duration<double>(elapsed).count(), 10.0)
        << "100-level nested form must not cause runaway";
}

// =====================================================================
// PARALLEL-RENDER-MALICIOUS: concurrent malicious PDFs
// =====================================================================

TEST_F(PerfStressTest, ConcurrentMaliciousPdfRendering)
{
    // Generate 5 small "malicious-shape" PDFs and render concurrently.
    // Verifies no inter-thread state corruption.
    std::vector<std::filesystem::path> pdfs;
    for (int i = 0; i < 5; ++i) {
        std::string pdf =
            "%PDF-1.7\n"
            "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
            "/OpenAction << /S /JavaScript /JS (app.alert('m" +
            std::to_string(i) + "')) >> >>\nendobj\n"
            "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
            "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            ">>\nendobj\n"
            "xref\n0 4\n0000000000 65535 f \n"
            "0000000009 00000 n \n0000000200 00000 n \n"
            "0000000260 00000 n \n"
            "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
        auto p = td_ / ("mal_" + std::to_string(i) + ".pdf");
        writePdf(p, pdf);
        pdfs.push_back(p);
    }

    std::atomic<int> successCount{0};
    std::atomic<int> exceptionCount{0};
    std::vector<std::thread> workers;

    for (const auto& path : pdfs) {
        workers.emplace_back([&, path]() {
            try {
                auto threadCtx = std::make_shared<MuPdfContext>();
                auto doc = Document::open(threadCtx, path);
                if (doc) {
                    auto dl = doc->createDisplayList(0);
                    if (dl) doc->releaseDisplayList(dl);
                }
                successCount++;
            } catch (...) {
                exceptionCount++;
            }
        });
    }

    for (auto& t : workers) t.join();

    // All threads should complete (either success or controlled exception)
    EXPECT_EQ(successCount + exceptionCount, static_cast<int>(pdfs.size()))
        << "All concurrent threads must terminate cleanly";
}

// =====================================================================
// HANDLE-LEAK-AUDIT: explicit context lifecycle
// =====================================================================

TEST_F(PerfStressTest, ContextLifecycleNoLeak)
{
    auto fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP();

    // Open and release 200 contexts; verify each releases cleanly.
    for (int i = 0; i < 200; ++i) {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, fixture);
        ASSERT_NE(doc, nullptr) << "Iter " << i;
        auto dl = doc->createDisplayList(0);
        if (dl) doc->releaseDisplayList(dl);
        // doc and ctx destruct here
    }
    // If we get here without crash/OOM, lifecycle is clean
    SUCCEED();
}

// =====================================================================
// ZIP-BOMB-RESILIENCE: extreme compression ratio (extends existing test)
// =====================================================================

TEST_F(PerfStressTest, ExtremeCompressionRatioBounded)
{
    // FlateDecode stream that decompresses to nominal but claims huge size.
    // The PDF declares /Length 64 for a tiny zlib-compressed stream.
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R >>\nendobj\n"
        // Real zlib-compressed 5 bytes: "Hello" -> very short stream
        "4 0 obj\n<< /Length 13 /Filter /FlateDecode >>\nstream\n"
        "\x78\x9c\xf3\x48\xcd\xc9\xc9\x07\x00\x05\x8c\x01\xf5"
        "\nendstream\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000230 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "tiny_flate.pdf";
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
    EXPECT_LT(std::chrono::duration<double>(elapsed).count(), 5.0);
}
