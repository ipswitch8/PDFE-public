// -*- coding: utf-8 -*-
//
// GitHub #151 — pdfe.log is enabled by default, appended forever with no
// rotation or cap, and nothing proves the configured minimum level actually
// filters what reaches the file.
//
// TWO INDEPENDENT HALVES, TWO INDEPENDENT ORACLES.
//
// 1. RETENTION. The log was opened Append at every launch and never bounded.
//    The oracle is the BYTES ON DISK after writing far more than the cap: not
//    "rotationCount() went up" alone (a counter can be incremented by code that
//    deletes nothing) and not "the active file is small" alone (it would also
//    be small if rotation kept every generation forever under other names).
//    Both, plus the absence of a third generation.
//
// 2. FILTERING. The pre-existing tests in test_log_handler.cpp assert that
//    setMinLevel/minLevel round-trips, and that lastEntry_ captures a message
//    REGARDLESS of level — lastEntry_ is deliberately written before the level
//    check, so it cannot distinguish a working filter from a broken one. There
//    was no test anywhere that changed the preference and looked at what was
//    WRITTEN. This one does: the same qWarning, at two different minimum
//    levels, and the file must differ.
#include <gtest/gtest.h>

#include "app/LogHandler.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#if defined(_WIN32)
#include <windows.h>
#include <string>
#endif

using pdfe::app::LogHandler;

namespace {

/// Raw bytes, or empty if the file is absent OR unreadable. The caller must
/// distinguish those two cases itself where it matters — an "it survived"
/// assertion comparing two empty QByteArrays would pass for a deleted file, so
/// AFailedRotationKeepsThePreviousGeneration asserts `before` is non-empty
/// before it is used as an oracle.
QByteArray readBytes(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

QString readLog(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

class LogRetentionTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        handler_ = &LogHandler::instance();
        ASSERT_TRUE(dir_.isValid());
        path_ = dir_.path() + "/pdfe.log";

        // The handler is a process-wide singleton shared with every other test
        // in this binary. Take it to a known state and hand it back the same
        // way, or a neighbour's leftover level silently decides our result.
        savedMinLevel_ = handler_->minLevel();
        savedMaxBytes_ = handler_->maxFileBytes();
        handler_->setFileLogging(false);
        handler_->setMaxFileBytes(LogHandler::kDefaultMaxFileBytes);
        handler_->setMinLevel(LogHandler::Warning);
        handler_->setFileLogging(true, path_);
        handler_->install();
    }

    void TearDown() override
    {
        handler_->uninstall();
        handler_->setFileLogging(false);
        handler_->setMaxFileBytes(savedMaxBytes_);
        handler_->setMinLevel(savedMinLevel_);
    }

    LogHandler* handler_ = nullptr;
    QTemporaryDir dir_;
    QString path_;
    LogHandler::Level savedMinLevel_ = LogHandler::Warning;
    qint64 savedMaxBytes_ = LogHandler::kDefaultMaxFileBytes;
};

// -------------------------------------------------------------------------
// Half 1 — the log is bounded.
// -------------------------------------------------------------------------
TEST_F(LogRetentionTest, LogIsRotatedAndTotalRetentionStaysBounded)
{
    const qint64 cap = LogHandler::kMinMaxFileBytes;   // 1024
    handler_->setMaxFileBytes(cap);
    ASSERT_EQ(handler_->maxFileBytes(), cap);

    const int rotationsBefore = handler_->rotationCount();

    // ~150 bytes per formatted line x 400 lines = ~60 KB written into a 1 KB
    // cap. Any surviving append-forever behaviour is off by a factor of ~60.
    const QString payload(100, QChar('x'));
    const int lines = 400;
    for (int i = 0; i < lines; ++i) {
        qWarning("%s", qPrintable(payload));
    }

    const qint64 activeSize = QFileInfo(path_).size();
    const QString rotatedPath = handler_->rotatedLogFilePath();
    ASSERT_EQ(rotatedPath, path_ + ".1");
    const qint64 rotatedSize =
        QFile::exists(rotatedPath) ? QFileInfo(rotatedPath).size() : 0;

    EXPECT_GT(handler_->rotationCount(), rotationsBefore)
        << "writing ~60 KB into a 1 KB cap performed NO rotation — the log is "
           "still append-forever (GitHub #151)";

    // The real bound: EVERYTHING the logger retains, not just the active file.
    // A rotation scheme that kept each generation under a new name would pass
    // an active-file-only assertion while retaining just as much.
    EXPECT_LE(activeSize + rotatedSize, 4 * cap)
        << "total retained log bytes (" << activeSize << " active + "
        << rotatedSize << " rotated) exceed 4x the " << cap
        << "-byte cap after writing " << lines << " lines";

    // Exactly one backup generation is kept. A ".2" would mean retention grows
    // with time again, which is the defect wearing a different filename.
    EXPECT_FALSE(QFile::exists(path_ + ".2"))
        << "a second backup generation exists — retention is unbounded again, "
           "just spread across more files";

    // And logging still WORKS after all that rotating: the cap must not have
    // been achieved by quietly breaking the writer.
    const QString marker = "PDFE_POST_ROTATION_MARKER_151";
    qWarning("%s", qPrintable(marker));
    EXPECT_TRUE(readLog(path_).contains(marker))
        << "nothing is being written after rotation — the log was 'bounded' by "
           "breaking it";
}

// -------------------------------------------------------------------------
// Half 2 — the configured minimum level decides what reaches the file.
// -------------------------------------------------------------------------
TEST_F(LogRetentionTest, MinLevelPreferenceChangesWhatIsWrittenToTheFile)
{
    // Warning vs Critical is the unambiguous pair: a qWarning is BELOW Critical
    // and AT Warning, so one identical call site must be suppressed under the
    // first setting and emitted under the second. Nothing about the message,
    // the file or the handler changes between the two — only the preference.
    const QString suppressed = "PDFE_LEVEL_FILTER_SUPPRESSED_151";
    const QString emitted    = "PDFE_LEVEL_FILTER_EMITTED_151";

    handler_->setMinLevel(LogHandler::Critical);
    qWarning("%s", qPrintable(suppressed));

    const QString afterCritical = readLog(path_);
    EXPECT_FALSE(afterCritical.contains(suppressed))
        << "minLevel=Critical did not stop a qWarning reaching the log file — "
           "the level preference does not filter what is written (GitHub #151)";

    handler_->setMinLevel(LogHandler::Warning);
    qWarning("%s", qPrintable(emitted));

    const QString afterWarning = readLog(path_);
    EXPECT_TRUE(afterWarning.contains(emitted))
        << "minLevel=Warning suppressed a qWarning — the filter is not "
           "level-sensitive, it is simply off";

    // The control that makes the pair mean something: lowering the level must
    // not retroactively admit the message that was already refused. Without
    // this, a handler that buffered everything and flushed on the second call
    // would pass both assertions above.
    EXPECT_FALSE(afterWarning.contains(suppressed))
        << "the message suppressed under minLevel=Critical appeared in the "
           "file after the level was lowered — it was buffered, not filtered";
}

// -------------------------------------------------------------------------
// Half 3 — a rotation that CANNOT publish must not have destroyed the
// generation it was going to replace. (GitHub #254)
//
// WHY THIS IS A SEPARATE ORACLE. Both tests above pass against the ORIGINAL
// implementation and against the fixed one, so neither can tell them apart. The
// original was:
//
//     QFile::remove(backup);                  // destroy generation N-1
//     if (QFile::rename(logPath_, backup))    // publish generation N
//
// The remove exists only because Qt's rename will not replace an existing name
// on Windows. It runs unconditionally, BEFORE the operation that is supposed to
// supply the replacement. If the rename then fails — the case the truncate
// fallback in rotateIfNeededLocked() exists for, an indexer or A/V holding a
// handle to the active log — retention has already dropped to zero and the
// generation that was deleted is gone.
//
// The fix routes the publish through core::atomicReplaceFile (MoveFileExW with
// MOVEFILE_REPLACE_EXISTING): one directory operation, so `backup` names the old
// generation until the instant it names the new one, and a failed publish leaves
// the old one untouched.
//
// THE INJECTED FAILURE IS THE REAL ONE. An exclusive handle on the active log
// with no sharing mode is exactly what an A/V scanner holds, and it is what makes
// MoveFileExW fail with ERROR_SHARING_VIOLATION. Nothing is stubbed.
// -------------------------------------------------------------------------
#if defined(_WIN32)
TEST_F(LogRetentionTest, AFailedRotationKeepsThePreviousGeneration)
{
    const qint64 cap = LogHandler::kMinMaxFileBytes;
    handler_->setMaxFileBytes(cap);

    const QString backup = handler_->rotatedLogFilePath();
    ASSERT_EQ(backup, path_ + ".1");

    // 1. Rotate once normally, so there IS a previous generation to lose.
    const QString firstGen = "PDFE_254_FIRST_GENERATION_CONTENT";
    const QString filler(100, QChar('a'));
    qWarning("%s", qPrintable(firstGen));
    for (int i = 0; i < 40; ++i) qWarning("%s", qPrintable(filler));

    ASSERT_TRUE(QFile::exists(backup))
        << "no rotation happened, so this test cannot observe a generation "
           "being lost — the setup is wrong, not the product";
    const QByteArray before = readBytes(backup);
    ASSERT_FALSE(before.isEmpty())
        << "the previous generation is empty, so a test asserting it SURVIVES "
           "would pass even if it were deleted";

    // 2. Make the next publish fail, the way the real world does it: hold a
    //    handle to the active log that permits reading and writing but NOT
    //    deletion. Renaming a file requires DELETE access to it, so this blocks
    //    MoveFileExW with ERROR_SHARING_VIOLATION while leaving the logger's own
    //    append handle working — which is precisely what an indexer or an A/V
    //    scanner does, and the case rotateIfNeededLocked()'s truncate fallback
    //    was written for.
    //
    //    FILE_SHARE_DELETE is deliberately absent and FILE_SHARE_READ/WRITE are
    //    deliberately present. Omitting all sharing instead fails to open at all
    //    (ERROR_SHARING_VIOLATION against the logger's own handle, measured),
    //    and granting share-delete would let the publish succeed — so neither
    //    injects the failure this test needs.
    const std::wstring active = path_.toStdWString();
    HANDLE block = ::CreateFileW(active.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(block, INVALID_HANDLE_VALUE)
        << "could not open the active log with delete denied (GetLastError="
        << ::GetLastError() << "), so no publish failure was injected and a "
           "PASS here would prove nothing";

    // And the injection has to actually BITE. If MoveFileExW succeeds under this
    // handle on some filesystem, the rest of the test is measuring a successful
    // rotation and would pass for either implementation.
    const std::wstring probeDest = (path_ + ".movetest").toStdWString();
    const BOOL moved = ::MoveFileExW(active.c_str(), probeDest.c_str(),
                                     MOVEFILE_REPLACE_EXISTING);
    if (moved) {
        ::MoveFileExW(probeDest.c_str(), active.c_str(),
                      MOVEFILE_REPLACE_EXISTING);
        ::CloseHandle(block);
        FAIL() << "a publish of the active log SUCCEEDED while a delete-denying "
                  "handle was held, so this environment cannot inject the "
                  "failure the test depends on. Treat this as an unusable "
                  "environment, not as the product being correct.";
    }
    ASSERT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION))
        << "the publish failed for some reason other than the sharing violation "
           "this test injects; the oracle below would be measuring something "
           "else";

    // 3. Drive another rotation. The publish must fail; the handler falls back
    //    to truncating. Either way, the PREVIOUS generation must still be there.
    for (int i = 0; i < 40; ++i) qWarning("%s", qPrintable(filler));

    const bool stillThere = QFile::exists(backup);
    const QByteArray after = readBytes(backup);

    ::CloseHandle(block);

    EXPECT_TRUE(stillThere)
        << "the previous log generation was DELETED by a rotation whose publish "
           "could not succeed. That is the remove-then-rename shape: retention "
           "dropped to zero because the destroy ran unconditionally before the "
           "operation meant to supply the replacement (GitHub #254).";
    EXPECT_EQ(after, before)
        << "the previous generation survived but its contents changed under a "
           "failed publish, which means something wrote a partial file over it "
           "rather than replacing it atomically";
}
#endif  // _WIN32

}  // namespace
