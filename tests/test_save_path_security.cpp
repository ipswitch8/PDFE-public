// -*- coding: utf-8 -*-
//
// Pipeline f8a12c4d phase-3 (SEC-SAVE-PATH): Save/export path traversal
// security tests. Verifies Document::saveAs guards reject dangerous paths.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace pdfe::core;

namespace {
std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }
}

class SavePathSecurityTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        auto fixture = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (!std::filesystem::exists(fixture))
            GTEST_SKIP() << "test_single.pdf missing";
        doc_ = Document::open(ctx_, fixture);
        if (!doc_) GTEST_SKIP() << "Could not open fixture";
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
};

TEST_F(SavePathSecurityTest, EmptyPathRejected)
{
    EXPECT_THROW(doc_->saveAs(std::filesystem::path()), PdfeException);
}

TEST_F(SavePathSecurityTest, NullByteInPathRejected)
{
    std::wstring pathWithNull = L"C:\\temp\\test";
    pathWithNull += L'\0';
    pathWithNull += L"evil.pdf";
    EXPECT_THROW(doc_->saveAs(std::filesystem::path(pathWithNull)), PdfeException);
}

TEST_F(SavePathSecurityTest, WindowsADSRejected)
{
#ifndef _WIN32
    GTEST_SKIP() << "Alternate Data Streams are NTFS-only; rejection "
                    "only runs on Windows";
#else
    auto tmp = std::filesystem::temp_directory_path() / "test:hidden_stream.pdf";
    EXPECT_THROW(doc_->saveAs(tmp), PdfeException);
#endif
}

TEST_F(SavePathSecurityTest, RelativePathTraversalAllowed)
{
    // Document::saveAs explicitly allows ../ (documented design decision).
    // This test confirms the design: legitimate user-chosen paths like
    // saving to a sibling directory are not blocked.
    auto tmp = std::filesystem::temp_directory_path() / "pdfe_save_test";
    std::filesystem::create_directories(tmp);
    auto target = tmp / ".." / "pdfe_save_test" / "traversal_test.pdf";
    EXPECT_NO_THROW(doc_->saveAs(target));
    std::filesystem::remove(target);
}

// ─── GitHub #156 — the long-path contract, asserted definitively ─────────
//
// This test and UNCPathInSaveTarget below both used to be a bare
// try { doc_->saveAs(p); } catch (...) { } with NO assertion in either arm.
// A test that catches an exception and asserts nothing is indistinguishable
// from a test that never ran: it passes whether the save succeeds, throws,
// truncates the target or writes half a file. #95 filed that shape; #156
// records the decision it was waiting on.
//
// THE DECIDED CONTRACT (#95): long paths are OFF. A save target beyond
// MAX_PATH must fail with a clean, actionable error naming the length, and
// must leave nothing behind. See the enforcing guard and its full rationale
// in Document::saveAs (search "THE LONG-PATH CONTRACT IS *OFF*").

TEST_F(SavePathSecurityTest, LongPathIsRejectedWithALengthNamingError)
{
#ifndef _WIN32
    GTEST_SKIP() << "MAX_PATH is a Win32 limit; the OFF contract is "
                    "Windows-only";
#else
    // 300-char filename — past MAX_PATH (260) whatever the temp directory is.
    std::string longName(300, 'x');
    longName += ".pdf";
    const auto target = std::filesystem::temp_directory_path() / longName;
    ASSERT_GT(target.native().size(), 259u)
        << "fixture check: the target must actually exceed MAX_PATH for this "
           "test to be testing anything";

    // 1. It FAILS — not "either outcome".
    std::string what;
    try {
        doc_->saveAs(target);
        FAIL() << "long-path save must be refused (contract: long paths OFF), "
                  "but saveAs() returned normally for a "
               << target.native().size() << "-character path";
    } catch (const PdfeException& e) {
        what = e.what();
    }

    // 2. The error NAMES the length problem — an actionable message, not a
    //    generic "could not save". Both the offending length and the limit
    //    have to appear, so a message that merely says "too long" without
    //    saying how long, or one that names some other limit, is not enough.
    EXPECT_NE(what.find("length"), std::string::npos)
        << "error must name the length problem; got: " << what;
    EXPECT_NE(what.find(std::to_string(target.native().size())),
              std::string::npos)
        << "error must state the offending path length ("
        << target.native().size() << "); got: " << what;
    EXPECT_NE(what.find("259"), std::string::npos)
        << "error must state the limit it exceeded; got: " << what;

    // 3. NO PARTIAL FILE. Neither at the target nor at the staging name the
    //    full-rewrite path would have used (target + kFullRewriteTempSuffix).
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(target, ec))
        << "a refused long-path save must leave no file at the target";
    auto stagingName = target.native();
    stagingName += L".pdfe_tmp";
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(stagingName), ec))
        << "a refused long-path save must leave no staging residue either";

    // 4. THE CONTROL. Without it, a mutation that made EVERY saveAs throw
    //    would satisfy 1-3 and this test would still be green.
    const auto control =
        std::filesystem::temp_directory_path() / "pdfe_longpath_control.pdf";
    std::filesystem::remove(control, ec);
    ASSERT_LT(control.native().size(), 260u)
        << "control must be a normal-length path";
    EXPECT_NO_THROW(doc_->saveAs(control))
        << "a normal-length save must still succeed";
    EXPECT_TRUE(std::filesystem::exists(control, ec))
        << "the control save must actually produce a file";
    EXPECT_GT(std::filesystem::file_size(control, ec), 0u);
    std::filesystem::remove(control, ec);
#endif
}

// ─── GitHub #156 / #95 — the OTHER direction of the same contract ────────
//
// #95 asked for the contract to be asserted "definitively both directions".
// LongPathIsRejectedWithALengthNamingError is the refusal direction. This is
// the acceptance direction, and without it the `\\?\` exemption in
// Document::saveAs is unguarded here — its only other coverage lives in a
// different suite (CliTest), so a change to this guard could withdraw a
// working capability while every test in THIS file stayed green.
//
// The distinction being pinned: an unprefixed 300-char path is refused
// because the manifest contract is OFF, while an EXPLICIT extended-length
// path of the same length is accepted, because `\\?\` needs neither the
// manifest nor the LongPathsEnabled policy and therefore behaves the same on
// every Windows machine.
TEST_F(SavePathSecurityTest, ExplicitExtendedLengthLongPathIsAccepted)
{
#ifndef _WIN32
    GTEST_SKIP() << "the \\\\?\\ prefix is a Win32 mechanism";
#else
    namespace fs = std::filesystem;
    std::error_code ec;

    // Append a component FIRST. temp_directory_path() retains its trailing
    // separator, and `\\?\` disables path normalisation entirely — so building
    // from the bare temp path yields "...Temp\\seg_" with a doubled separator
    // and every subsequent call fails. This cost one silently SKIPPED run,
    // which is the false green this whole file exists to prevent.
    const fs::path base = fs::temp_directory_path() / "pdfe_p84_extlen_accept";
    std::wstring w = base.wstring();
    while (w.size() < 300)
        w += L"\\seg_2345678901234567890123456789";
    const fs::path deepDir(L"\\\\?\\" + w);

    fs::remove_all(deepDir, ec);
    ec.clear();
    fs::create_directories(deepDir, ec);
    // Deliberately ASSERT rather than GTEST_SKIP: a skip here would report
    // green while proving nothing about the exemption, and the environment is
    // demonstrably capable — two other suites create the same shape.
    ASSERT_TRUE(fs::exists(deepDir))
        << "extended-length mkdir failed: " << ec.message();

    const fs::path target = deepDir / "accepted.pdf";
    ASSERT_GT(target.native().size(), 259u)
        << "fixture check: the target must actually exceed MAX_PATH, or this "
           "test proves nothing about the boundary it exists to cover";
    ASSERT_EQ(target.native().rfind(L"\\\\?\\", 0), 0u)
        << "fixture check: the target must carry the extended-length prefix, "
           "or this test is just re-testing the refusal case";

    EXPECT_NO_THROW(doc_->saveAs(target))
        << "an EXPLICIT \\\\?\\ path must be accepted: it needs neither the "
           "longPathAware manifest nor the LongPathsEnabled policy, so it is "
           "not covered by the OFF decision recorded on #95";
    EXPECT_TRUE(fs::exists(target, ec))
        << "saveAs reported success but wrote nothing to the deep path";
    EXPECT_GT(fs::file_size(target, ec), 0u)
        << "a zero-byte file is not a successful save";

    fs::remove_all(deepDir, ec);
#endif
}

TEST_F(SavePathSecurityTest, UNCPathInSaveTarget)
{
#ifndef _WIN32
    GTEST_SKIP() << "UNC paths are a Win32 path shape";
#else
    // Same non-assertion shape as the long-path test above, fixed the same
    // way. The contract here is narrower but still definitive: a UNC path is
    // a LEGITIMATE save target shape — saveAs must never reject it as
    // malformed — and whichever way it goes, it must not leave a partial file
    // behind.
    //
    // Which arm applies is decided by an INPUT (can this machine write to the
    // share at all?), probed with a plain ofstream that shares no code with
    // Document. It is never decided by the outcome of the call under test, so
    // neither arm can be satisfied vacuously.
    const std::filesystem::path uncDir("\\\\localhost\\C$\\Windows\\Temp");
    const auto target = uncDir / "pdfe_unc_test.pdf";

    std::error_code ec;
    std::filesystem::remove(target, ec);

    bool shareWritable = false;
    {
        const auto probe = uncDir / "pdfe_unc_probe.tmp";
        std::ofstream os(probe, std::ios::binary);
        if (os.is_open()) {
            os << "probe";
            os.close();
            shareWritable = std::filesystem::exists(probe, ec);
        }
        std::filesystem::remove(probe, ec);
    }

    if (shareWritable) {
        EXPECT_NO_THROW(doc_->saveAs(target))
            << "a writable UNC target is an ordinary save destination";
        EXPECT_TRUE(std::filesystem::exists(target, ec))
            << "a successful UNC save must produce the file";
        EXPECT_GT(std::filesystem::file_size(target, ec), 0u);
        std::filesystem::remove(target, ec);
    } else {
        std::string what;
        try {
            doc_->saveAs(target);
            FAIL() << "the share is not writable, yet saveAs() reported "
                      "success for " << target.string();
        } catch (const PdfeException& e) {
            what = e.what();
        }
        // It must fail because the DESTINATION could not be written, never
        // because the UNC shape was judged malformed. Those are the four
        // messages the pre-flight path guards emit; none may appear here.
        EXPECT_EQ(what.find("Refusing to save"), std::string::npos)
            << "UNC is a legitimate path shape and must not be refused by "
               "the path-validity guards; got: " << what;
        EXPECT_FALSE(std::filesystem::exists(target, ec))
            << "a failed UNC save must leave no partial file behind";
    }
#endif
}

// GitHub #156, second half: keep the CODE contract and the SHIPPED manifest
// from drifting apart. Document::saveAs refuses paths past MAX_PATH because
// PDFE is not long-path aware; if someone later adds a `longPathAware`
// manifest without revisiting that guard, the product would claim a support
// level the save path actively refuses. AtomicFileReplace.cpp only NOTES the
// absence in a comment — this makes it binding.
TEST(SaveLongPathManifestContract, NoShippedManifestDeclaresLongPathAware)
{
    const std::filesystem::path root(PDFE_REPO_ROOT);
    const std::filesystem::path src = root / "src";
    if (!std::filesystem::exists(src))
        GTEST_SKIP() << "source tree not present next to the test binary";

    int scanned = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(src, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const auto ext = it->path().extension().string();
        if (ext != ".manifest" && ext != ".rc" && ext != ".xml") continue;
        ++scanned;
        std::ifstream in(it->path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        // Case-folded search: the manifest element is longPathAware, but the
        // XML is authored by hand and casing has drifted in the wild.
        std::string lowered = text;
        for (auto& c : lowered)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        EXPECT_EQ(lowered.find("longpathaware"), std::string::npos)
            << it->path().string()
            << " declares longPathAware, but Document::saveAs refuses paths "
               "past MAX_PATH (GitHub #156 / #95 decided long paths are OFF). "
               "Change both or neither.";
    }
    // Guard the measurement itself: a walk that inspected nothing would pass
    // for the wrong reason.
    EXPECT_GT(scanned, 0)
        << "no manifest/rc/xml candidates under src/ were inspected — the "
           "scan proved nothing";
}
