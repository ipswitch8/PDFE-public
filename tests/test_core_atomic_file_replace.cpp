// -*- coding: utf-8 -*-
//
// Unit tests for pdfe::core::atomicReplaceFile and the retry contract on
// pdfe::core::StagingFile::commitOverWithRetry (the path-based
// atomicReplaceFileWithRetry was deleted in phase-58 — see the fixture's
// staged() helper for why these cases moved rather than went away).
// (src/core/AtomicFileReplace.{h,cpp}).
//
// GAP A1-37 (meta-audit-2026-07 status-register): src/core had zero dedicated
// test files. AtomicFileReplace had zero coverage of ANY kind -- no test file
// in tests/ so much as named it -- even though it is the single helper every
// save/export/redaction/batch rename in the product now routes through
// (WIN-84 / WIN-85). A silent regression here is data loss, not a wrong pixel.
//
// The oracles are the CONTRACT stated verbatim in AtomicFileReplace.h, not
// current behaviour:
//
//   * On success `to` becomes `from`; `from` no longer exists.
//   * On ANY failure `to` is left COMPLETELY UNTOUCHED -- there is no window
//     in which the destination is absent or truncated. The destination is
//     never removed first.
//   * Failure is reported through `ec`; nothing throws.
//   * Unicode file names round-trip losslessly (UTF-16 native on Windows).
//   * The retry form defaults to 10 attempts x 100 ms and clamps attempts < 1
//     up to a single attempt.
//
// The "destination is never removed first" clause is the whole point of the
// helper -- ImageExporter's old remove()+rename() fallback could delete the
// user's existing file and then fail. It is asserted here against a REAL
// exclusively-opened destination, which is the exact A/V / indexer condition
// the retry loop was calibrated for.

#include <gtest/gtest.h>

#include "core/AtomicFileReplace.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using pdfe::core::atomicReplaceFile;
using pdfe::core::StagingFile;

namespace {

class AtomicFileReplaceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        static std::atomic<int> counter{0};
        dir_ = fs::temp_directory_path() /
               ("pdfe_atomic_replace_" +
                std::to_string(::testing::UnitTest::GetInstance()
                                   ->current_test_info()
                                   ->line())
                + "_" + std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
        ASSERT_FALSE(ec) << "could not create temp dir " << dir_.string();
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);  // best effort; a held handle is not fatal
    }

    fs::path write(const fs::path& name, const std::string& body) const
    {
        const fs::path p = dir_ / name;
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << body;
        f.close();
        EXPECT_TRUE(fs::exists(p));
        return p;
    }

    /// A StagingFile beside `target`, already carrying `body` and still
    /// holding its exclusive handle — WIN-87 phase-58.
    ///
    /// WHY THESE TESTS MOVED. They used to drive
    /// `atomicReplaceFileWithRetry(from, to, ...)`, the PATH-based publish
    /// helper, which phase-58 DELETED once its last production caller was
    /// migrated: a helper that takes paths re-resolves the name at rename time
    /// with nothing held, which is the WIN-87 substitution defect itself, and
    /// leaving it in the header keeps that shape one #include away from being
    /// reached for again.
    ///
    /// The four properties below were never about the path-vs-handle question —
    /// they are about the RETRY CONTRACT (clamp, no-sleep-on-success, survives a
    /// transient hold, fails safe on exhaustion), and that contract lives on
    /// `StagingFile::commitOverWithRetry` unchanged. So the coverage is
    /// RE-POINTED rather than deleted. Deleting tests along with a deleted
    /// function is correct only when the behaviour goes with it; here it did
    /// not.
    std::unique_ptr<StagingFile> staged(const fs::path& target,
                                        const std::string& body) const
    {
        std::error_code ec;
        auto s = StagingFile::createBeside(target, ec);
        EXPECT_TRUE(s) << "createBeside failed: " << ec.message();
        if (!s) return nullptr;
        std::FILE* fp = s->openDuplicateStream(ec);
        EXPECT_NE(fp, nullptr) << "openDuplicateStream failed: " << ec.message();
        if (!fp) return nullptr;
        EXPECT_EQ(std::fwrite(body.data(), 1, body.size(), fp), body.size());
        EXPECT_EQ(std::fflush(fp), 0);
        std::fclose(fp);
        return s;
    }

    static std::string read(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    }

    fs::path dir_;
};

// ---------------------------------------------------------------------------
// Success semantics
// ---------------------------------------------------------------------------

// Contract: "On success `to` becomes `from`; `from` no longer exists."
// KILLS: an implementation that copies instead of renames (source would
// survive), or one that drops MOVEFILE_REPLACE_EXISTING (the call would fail
// outright because the destination already exists).
TEST_F(AtomicFileReplaceTest, ReplacesExistingDestinationAndConsumesSource)
{
    const fs::path from = write("new.tmp", "NEW-CONTENT");
    const fs::path to = write("target.pdf", "OLD-CONTENT");

    std::error_code ec;
    EXPECT_TRUE(atomicReplaceFile(from, to, ec));
    EXPECT_FALSE(ec) << ec.message();

    EXPECT_TRUE(fs::exists(to));
    EXPECT_EQ(read(to), "NEW-CONTENT");
    EXPECT_FALSE(fs::exists(from))
        << "the source must be consumed by the rename, not left behind as a "
           "stray temp file";
}

// A destination that does not yet exist must simply be created -- the helper is
// used for first-time saves as well as replacements.
// KILLS: an implementation that requires the destination to pre-exist.
TEST_F(AtomicFileReplaceTest, CreatesDestinationWhenAbsent)
{
    const fs::path from = write("new.tmp", "FRESH");
    const fs::path to = dir_ / "does_not_exist_yet.pdf";
    ASSERT_FALSE(fs::exists(to));

    std::error_code ec;
    EXPECT_TRUE(atomicReplaceFile(from, to, ec));
    EXPECT_FALSE(ec);
    EXPECT_EQ(read(to), "FRESH");
    EXPECT_FALSE(fs::exists(from));
}

// Contract: `ec.clear()` is the first statement, so a caller reusing an
// error_code across calls never sees a stale failure reported as a new one.
// KILLS: deleting the `ec.clear()` line.
TEST_F(AtomicFileReplaceTest, SuccessClearsAPreviouslySetErrorCode)
{
    const fs::path from = write("new.tmp", "X");
    const fs::path to = dir_ / "out.pdf";

    std::error_code ec = std::make_error_code(std::errc::permission_denied);
    ASSERT_TRUE(static_cast<bool>(ec));

    EXPECT_TRUE(atomicReplaceFile(from, to, ec));
    EXPECT_FALSE(ec) << "a successful replace must leave ec cleared, got: "
                     << ec.message();
}

// Contract: "Both paths are UTF-16 native, so Unicode file names round-trip
// losslessly (unlike a narrow ANSI path)."
// KILLS: switching to MoveFileExA / a narrow-char path conversion, which
// mangles or fails on any name outside the active ANSI code page.
TEST_F(AtomicFileReplaceTest, UnicodeFileNamesRoundTripLosslessly)
{
    // Cyrillic + CJK + precomposed accented Latin. Written as \u escapes so
    // the literals do not depend on how the compiler decodes this source file.
    const fs::path fromName =
        fs::path(u8"\u0438\u0441\u0445\u043e\u0434\u043d\u0438\u043a-\u6e2c\u8a66-caf\u00e9.tmp");
    const fs::path toName =
        fs::path(u8"\u0440\u0435\u0437\u0443\u043b\u044c\u0442\u0430\u0442-\u6587\u4ef6-na\u00efve.pdf");

    const fs::path from = write(fromName, "UNICODE-BODY");
    const fs::path to = write(toName, "OLD");

    std::error_code ec;
    ASSERT_TRUE(atomicReplaceFile(from, to, ec)) << ec.message();

    EXPECT_TRUE(fs::exists(to))
        << "the destination must exist under its exact Unicode name";
    EXPECT_EQ(read(to), "UNICODE-BODY");
    EXPECT_FALSE(fs::exists(from));
    // The name must be byte-identical, not a best-fit transliteration.
    EXPECT_EQ(to.filename(), toName);
}

// ---------------------------------------------------------------------------
// Failure semantics -- the destination survives, always
// ---------------------------------------------------------------------------

// Contract: "On ANY failure `to` is left COMPLETELY UNTOUCHED". A missing
// source is the cheapest way to force a failure on every platform.
// KILLS: any "remove the destination, then move" implementation, and any
// implementation that reports success when nothing was moved.
TEST_F(AtomicFileReplaceTest, MissingSourceFailsAndLeavesDestinationIntact)
{
    const fs::path from = dir_ / "no_such_source.tmp";
    const fs::path to = write("target.pdf", "PRECIOUS-USER-DATA");
    ASSERT_FALSE(fs::exists(from));

    std::error_code ec;
    EXPECT_FALSE(atomicReplaceFile(from, to, ec));
    EXPECT_TRUE(static_cast<bool>(ec))
        << "failure must be reported through ec, not silently swallowed";

    ASSERT_TRUE(fs::exists(to))
        << "a failed replace must never delete the destination";
    EXPECT_EQ(read(to), "PRECIOUS-USER-DATA")
        << "a failed replace must not truncate or alter the destination";
}

// A second, structurally different failure shape: the DESTINATION DIRECTORY
// does not exist, so the rename cannot be satisfied no matter what the source
// is. Distinct from "source missing" above and from the locked-destination case
// below, and it pins a contract those two do not: a failed replace must not
// CONSUME the source, or the caller's only copy of the data is gone.
//
// NOTE ON AN EARLIER VERSION OF THIS TEST. It used a DIRECTORY as the source
// and expected the replace to fail. That premise is simply false on Windows:
// MoveFileExW with MOVEFILE_REPLACE_EXISTING will happily move a directory over
// an existing file, so the call SUCCEEDED and the test failed. It was not
// detecting a product defect — atomicReplaceFile is only ever handed a temp
// FILE — it was asserting an OS behaviour that does not hold. Replaced with a
// shape that genuinely fails rather than deleted, so the noexcept contract stays
// covered.
TEST_F(AtomicFileReplaceTest, FailureIsReportedNotThrown)
{
    const fs::path from = write("source.tmp", "SOURCE-DATA");
    const fs::path to = dir_ / "no_such_dir" / "target.pdf";
    ASSERT_FALSE(fs::exists(to.parent_path()));

    std::error_code ec;
    bool ok = true;
    // noexcept in the declaration; prove it by failing the test on any escape.
    ASSERT_NO_THROW({ ok = atomicReplaceFile(from, to, ec); });
    EXPECT_FALSE(ok)
        << "a rename into a non-existent directory cannot succeed";
    EXPECT_TRUE(static_cast<bool>(ec))
        << "failure must be reported through ec, not silently swallowed";

    EXPECT_TRUE(fs::exists(from))
        << "a failed replace must not consume the source — otherwise the "
           "caller's only copy of the data is gone";
    EXPECT_EQ(read(from), "SOURCE-DATA");
    EXPECT_FALSE(fs::exists(to));
}

// ---------------------------------------------------------------------------
// Retry wrapper
// ---------------------------------------------------------------------------

// Contract: "if (attempts < 1) attempts = 1" -- a caller passing 0 or a
// negative count still gets one honest attempt rather than an instant false.
// KILLS: deleting the clamp (the loop body would never run and a perfectly
// possible replace would be reported as a failure).
TEST_F(AtomicFileReplaceTest, RetryClampsNonPositiveAttemptsToOne)
{
    for (int attempts : {0, -1, -1000}) {
        const fs::path to = dir_ / ("dst" + std::to_string(attempts) + ".pdf");

        std::error_code ec;
        auto s = staged(to, "BODY");
        ASSERT_TRUE(s);
        EXPECT_TRUE(s->commitOverWithRetry(to, ec, attempts,
                                           std::chrono::milliseconds(1)))
            << "attempts=" << attempts
            << " must be clamped to a single real attempt: " << ec.message();
        EXPECT_EQ(read(to), "BODY");
    }
}

// The retry wrapper must be a transparent pass-through on the happy path.
TEST_F(AtomicFileReplaceTest, RetrySucceedsImmediatelyWhenNothingBlocks)
{
    const fs::path to = write("target.pdf", "OLD");
    auto s = staged(to, "BODY");
    ASSERT_TRUE(s);

    const auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    ASSERT_TRUE(s->commitOverWithRetry(to, ec)) << ec.message();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(read(to), "BODY");
    // Defaults are 10 x 100 ms; an unblocked replace must not sleep at all.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                  .count(),
              100)
        << "the retry loop must return on the first successful attempt, not "
           "sleep between iterations unconditionally";
}

#if defined(_WIN32)

namespace {
/// RAII exclusive handle on `p` (dwShareMode 0), reproducing the transient
/// A/V / search-indexer / backup-agent hold that MOVEFILE_REPLACE_EXISTING
/// cannot punch through. Not a mock: this is the real OS condition.
class ExclusiveHold {
public:
    explicit ExclusiveHold(const fs::path& p)
        : h_(::CreateFileW(p.native().c_str(), GENERIC_READ,
                           0 /* no sharing at all */, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }
    ~ExclusiveHold() { release(); }
    void release()
    {
        if (h_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }
    bool held() const { return h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_;
};
}  // namespace

// THE central data-loss guard. With the destination exclusively held the
// replace cannot succeed -- and the user's file must still be there, whole,
// afterwards, with the new content still parked in the temp.
// KILLS: any remove()-then-rename() fallback (the destination would be gone),
// and any implementation that truncates/opens the destination for writing.
TEST_F(AtomicFileReplaceTest, LockedDestinationFailsWithBothFilesIntact)
{
    const fs::path from = write("new.tmp", "NEW-CONTENT");
    const fs::path to = write("target.pdf", "PRECIOUS-USER-DATA");

    ExclusiveHold hold(to);
    ASSERT_TRUE(hold.held()) << "could not take an exclusive hold on "
                             << to.string();

    std::error_code ec;
    EXPECT_FALSE(atomicReplaceFile(from, to, ec));
    EXPECT_TRUE(static_cast<bool>(ec));

    hold.release();

    ASSERT_TRUE(fs::exists(to)) << "destination must survive a failed replace";
    EXPECT_EQ(read(to), "PRECIOUS-USER-DATA");
    ASSERT_TRUE(fs::exists(from))
        << "the source must survive too, so the caller can retry or report "
           "without having lost the new content";
    EXPECT_EQ(read(from), "NEW-CONTENT");
}

// Contract: the retry exists precisely so a hold that clears within ~1 s does
// not surface to the user as a failed save.
// KILLS: deleting the sleep/loop (a 250 ms hold would fail the very first
// attempt and the call would return false).
TEST_F(AtomicFileReplaceTest, RetryOutlastsATransientDestinationHold)
{
    const fs::path to = write("target.pdf", "OLD");
    auto s = staged(to, "NEW-CONTENT");
    ASSERT_TRUE(s);

    auto hold = std::make_unique<ExclusiveHold>(to);
    ASSERT_TRUE(hold->held());

    std::thread releaser([&hold] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        hold->release();
    });

    std::error_code ec;
    const bool ok = s->commitOverWithRetry(to, ec, 10,
                                           std::chrono::milliseconds(100));
    releaser.join();

    EXPECT_TRUE(ok) << "a 250 ms hold is well inside the documented "
                       "10 x 100 ms window: "
                    << ec.message();
    EXPECT_EQ(read(to), "NEW-CONTENT");
    // The staging artifact is CONSUMED by the publish — commitOver renames it
    // through its own handle, so nothing is left at the staged name.
    EXPECT_FALSE(fs::exists(s->path()));
}

// A hold that never clears must still fail SAFELY after the attempts run out.
// KILLS: a "give up and remove the destination" escalation.
TEST_F(AtomicFileReplaceTest, RetryExhaustionStillLeavesDestinationIntact)
{
    const fs::path to = write("target.pdf", "PRECIOUS-USER-DATA");
    auto s = staged(to, "NEW-CONTENT");
    ASSERT_TRUE(s);

    ExclusiveHold hold(to);
    ASSERT_TRUE(hold.held());

    std::error_code ec;
    EXPECT_FALSE(s->commitOverWithRetry(to, ec, 3,
                                        std::chrono::milliseconds(5)));
    EXPECT_TRUE(static_cast<bool>(ec));

    hold.release();

    ASSERT_TRUE(fs::exists(to));
    EXPECT_EQ(read(to), "PRECIOUS-USER-DATA");
    // The unpublished staging artifact still exists while the object lives —
    // the point being that the failure path never escalated to removing the
    // DESTINATION, which is the property this case guards.
    EXPECT_TRUE(fs::exists(s->path()));
}

#endif  // _WIN32

}  // namespace
