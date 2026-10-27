// -*- coding: utf-8 -*-
#include "core/AutosaveManager.h"

#include "core/Document.h"
#include "core/UndoManager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QTextStream>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <mutex>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

#include <cstdint>

namespace pdfe::core {

namespace {

// Suffix that identifies a recovery metadata sidecar in the recovery dir.
constexpr const char* kSidecarSuffix = ".pdfe_recovery.json";

// Sidecar keys carrying the owner stamp (GitHub #97). Both are written as
// STRINGS: a Windows creation FILETIME is ~1.3e17, well past the 2^53 that a
// JSON double represents exactly, so storing it as a JSON number would silently
// round it and make every comparison a coin flip.
constexpr const char* kOwnerPidKey = "ownerPid";
constexpr const char* kOwnerStartTokenKey = "ownerStartToken";

std::uint64_t parseU64(const QJsonValue& v) {
    if (v.isString()) {
        bool ok = false;
        const qulonglong n = v.toString().toULongLong(&ok);
        return ok ? static_cast<std::uint64_t>(n) : 0;
    }
    if (v.isDouble()) {
        const double d = v.toDouble();
        return d > 0 ? static_cast<std::uint64_t>(d) : 0;
    }
    return 0;
}

std::filesystem::path fsFromQString(const QString& s) {
    return std::filesystem::path(s.toStdU16String());
}

QString qStringFromFs(const std::filesystem::path& p) {
    return QString::fromStdU16String(p.u16string());
}

}  // namespace

AutosaveManager::AutosaveManager(Document* doc,
                                 pdfe::commands::UndoManager* undo,
                                 std::filesystem::path sourcePath,
                                 std::filesystem::path recoveryDir,
                                 QObject* parent)
    : QObject(parent),
      doc_(doc),
      undo_(undo),
      sourcePath_(std::move(sourcePath)) {
    recoveryDir_ = recoveryDir.empty() ? defaultRecoveryDir()
                                       : std::move(recoveryDir);

    // Ensure the recovery directory exists.
    std::error_code ec;
    std::filesystem::create_directories(recoveryDir_, ec);

    // A unique, filesystem-safe session id (no braces/dashes surprises).
    sessionId_ = QUuid::createUuid()
                     .toString(QUuid::WithoutBraces)
                     .toStdString();

    recoveryFilePath_ = recoveryDir_ / (sessionId_ + ".pdf");
    sidecarPath_ = recoveryDir_ / (sessionId_ + kSidecarSuffix);

    // Owner stamp for the cross-instance filter (GitHub #97).
    ownerPid_ = currentProcessId();
    ownerStartToken_ = processStartToken(ownerPid_);

    // Baseline the content-version signal to the document's version at
    // construction, so "changed since we started tracking" is exact and a
    // freshly-opened (or freshly-saved) document does not spuriously autosave.
    lastAutosaveContentVersion_ = doc_ ? doc_->contentVersion() : 0;
}

AutosaveManager::~AutosaveManager() {
    // Do NOT clear recovery in the destructor: a crash-driven teardown must
    // LEAVE the sidecar behind so the next launch can recover. Clean exits call
    // clearRecovery() explicitly. Just stop the timer.
    stop();
}

void AutosaveManager::setSourcePath(std::filesystem::path sourcePath) {
    sourcePath_ = std::move(sourcePath);
}

void AutosaveManager::setPreSnapshotFlush(std::function<void()> fn) {
    preSnapshotFlush_ = std::move(fn);
}

void AutosaveManager::start(int intervalMs) {
    if (!timer_) {
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, [this]() { autosaveNow(); });
    }
    timer_->start(intervalMs);
}

void AutosaveManager::stop() {
    if (timer_) {
        timer_->stop();
    }
}

bool AutosaveManager::hasRecovery() const {
    std::error_code ec;
    return std::filesystem::exists(sidecarPath_, ec);
}

bool AutosaveManager::autosaveNow() {
    if (!doc_) {
        return false;
    }

    // Dirty signal. TWO independent sources of "unsaved edits":
    //   (1) an undo stack NOT at its clean marker — every command-driven edit;
    //   (2) the document's content version moved since our last autosave — this
    //       catches DIRECT mutations that push no undo command (redaction
    //       burn-in, page-label rewrite, direct layer ops). Without (2) those
    //       edits were silently lost on crash (Phase-1 gap-register).
    // No undo manager → (1) is simply never true; (2) still applies.
    const uint64_t contentVersion = doc_->contentVersion();
    const bool undoDirty = undo_ && !undo_->isClean();
    const bool contentChanged = contentVersion != lastAutosaveContentVersion_;
    if (!undoDirty && !contentChanged) {
        return false;
    }

    // "since the last autosave": if NOTHING changed on the undo stack AND the
    // content version is unchanged since our previous autosave, the existing
    // recovery copy is already current — skip the (potentially expensive)
    // rewrite. Either a new/undone command (stack size) or a direct mutation
    // (content version) reliably re-triggers a write.
    const int stackSize = undo_ ? undo_->undoStackSize() : 0;
    if (wroteRecovery_ && stackSize == lastAutosaveStackSize_ &&
        contentVersion == lastAutosaveContentVersion_) {
        return false;
    }

    // Pre-snapshot flush: commit any deferred in-panel edits into the live
    // document BEFORE snapshotting it (e.g. the bookmark tree). A flush failure
    // must never prevent the recovery write, so swallow exceptions.
    if (preSnapshotFlush_) {
        try {
            preSnapshotFlush_();
        } catch (...) {
            // best-effort; a flush error must not abort crash-recovery
        }
    }

    // Write the recovery PDF copy first, THEN the sidecar. Ordering matters for
    // crash-safety: a sidecar must never point at a missing/partial copy. If
    // the copy write fails, we never write a dangling sidecar.
    //
    // GitHub #104 (WIN-73): the write must not THROW OUT OF HERE.
    //
    // autosaveNow() is called from a QTimer callback (see start()). An
    // exception escaping a Qt slot does not become a handled error -- it
    // propagates out of the event loop and terminates the application. So the
    // one job autosave exists to do, protecting unsaved work, would be
    // destroyed by the same conditions it is supposed to survive: a recovery
    // directory that is unwritable because a scanner holds a handle, an ACL
    // denies write, or the path no longer resolves.
    //
    // Measured before this change: with an uncreatable recovery directory,
    // writeRecoveryCopy threw MuPdfException ("cannot open file ... No such
    // file or directory") straight through autosaveNow to the caller.
    //
    // Failure is reported by RETURNING FALSE, which is already the documented
    // "wrote nothing" answer, so no caller needs to change. The bookkeeping
    // below is deliberately NOT updated on failure: the next tick must try
    // again rather than believe a copy exists.
    try {
        doc_->writeRecoveryCopy(recoveryFilePath_);
    } catch (const std::exception&) {
        return false;
    } catch (...) {
        return false;
    }
    writeSidecar();

    lastAutosaveStackSize_ = stackSize;
    // Re-read: the pre-snapshot flush may itself have mutated the document
    // (bumping the version), so the POST-flush version is the new baseline.
    lastAutosaveContentVersion_ = doc_->contentVersion();
    wroteRecovery_ = true;
    return true;
}

void AutosaveManager::writeSidecar() {
    QJsonObject obj;
    obj["originalPath"] = qStringFromFs(sourcePath_);
    obj["recoveryFilePath"] = qStringFromFs(recoveryFilePath_);
    obj["timestamp"] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    obj["sessionId"] = QString::fromStdString(sessionId_);
    // GitHub #97 owner stamp. Schema bumped to 2; a schema-1 sidecar simply has
    // no stamp and is treated as "owner unknown" (offered) by the scan.
    obj[kOwnerPidKey] = QString::number(static_cast<qulonglong>(ownerPid_));
    obj[kOwnerStartTokenKey] =
        QString::number(static_cast<qulonglong>(ownerStartToken_));
    obj["schema"] = 2;

    const QByteArray json =
        QJsonDocument(obj).toJson(QJsonDocument::Indented);

    // QSaveFile writes atomically (temp + rename) so a scan never sees a
    // half-written sidecar.
    //
    // The whole open/write/commit is timed (GitHub #106). commit() is inside
    // the region deliberately: it performs the RENAME, and the rename is where
    // an on-access scanner blocks -- timing only the write() would report a few
    // microseconds while the caller stalls for a second in commit().
    const auto probeStart = std::chrono::steady_clock::now();

    QSaveFile f(qStringFromFs(sidecarPath_));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(json);
        if (auto hook = sidecarWriteProbeHook()) hook();
        f.commit();
    }

    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - probeStart)
            .count();
    noteSidecarWriteDuration(static_cast<long long>(elapsedMs));
}

// ---------------------------------------------------------------------------
// Autosave write-latency probe (GitHub #106 / WIN-89)
// ---------------------------------------------------------------------------
//
// State is process-wide rather than per-instance: the condition being detected
// is a property of the MACHINE (a scanner inspecting our writes), not of one
// document, so a second open document must not earn a second warning.

namespace {

std::atomic<int>& sidecarWarnCount() {
    static std::atomic<int> count{0};
    return count;
}

std::atomic<bool>& sidecarWarnLatched() {
    static std::atomic<bool> latched{false};
    return latched;
}

std::mutex& sidecarProbeHookMutex() {
    static std::mutex m;
    return m;
}

std::function<void()>& sidecarProbeHookStorage() {
    static std::function<void()> hook;
    return hook;
}

}  // namespace

std::function<void()> AutosaveManager::sidecarWriteProbeHook() {
    std::lock_guard<std::mutex> lock(sidecarProbeHookMutex());
    return sidecarProbeHookStorage();
}

void AutosaveManager::setSidecarWriteProbeHook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(sidecarProbeHookMutex());
    sidecarProbeHookStorage() = std::move(hook);
}

int AutosaveManager::sidecarWriteWarningCount() {
    return sidecarWarnCount().load();
}

void AutosaveManager::resetSidecarWriteLatencyProbe() {
    sidecarWarnCount().store(0);
    sidecarWarnLatched().store(false);
}

void AutosaveManager::noteSidecarWriteDuration(long long elapsedMs) {
    if (elapsedMs < kSidecarWriteWarnMs) return;
    // exchange(), not a load-then-store: two documents autosaving on their own
    // timers can reach here concurrently, and a check-then-set would let both
    // through and emit the warning twice.
    if (sidecarWarnLatched().exchange(true)) return;
    sidecarWarnCount().fetch_add(1);
    qWarning().nospace()
        << "Autosave sidecar write took " << elapsedMs << " ms (threshold "
        << kSidecarWriteWarnMs
        << " ms). Autosave writes are being measurably delayed -- on Windows "
           "this is usually on-access antimalware inspecting each freshly "
           "created file, which interacts with code signing (see "
           "docs/code-signing.md). Autosave still succeeded. This warning is "
           "emitted once per process.";
}

void AutosaveManager::clearRecovery() {
    std::error_code ec;
    std::filesystem::remove(sidecarPath_, ec);
    std::filesystem::remove(recoveryFilePath_, ec);
    wroteRecovery_ = false;
    lastAutosaveStackSize_ = -1;
    // Re-baseline the content-version signal to the document's CURRENT (now
    // clean) version, so an autosave after a clean save/close does not re-fire
    // for content already persisted.
    lastAutosaveContentVersion_ = doc_ ? doc_->contentVersion() : 0;
}

// Diagnostic seam (GitHub #285 cluster G). Appends to the file named by
// PDFE_RECOVERY_TRACE, inert when unset. Every `continue` below is a silent
// rejection, and with eight of them "the scan found nothing" was impossible to
// attribute from outside the process -- the recovery prompt simply never
// appeared. Each rejection now says which filter dropped the record.
static void scanTrace(const std::string& what) {
    const QByteArray dest = qgetenv("PDFE_RECOVERY_TRACE");
    if (dest.isEmpty()) return;
    QFile f(QString::fromLocal8Bit(dest));
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream(&f) << "  [scan] " << QString::fromStdString(what) << "\n";
}

std::vector<RecoveryRecord> AutosaveManager::scanForRecoverable(
    const std::filesystem::path& recoveryDir) {
    std::vector<RecoveryRecord> out;

    const std::filesystem::path dir =
        recoveryDir.empty() ? defaultRecoveryDir() : recoveryDir;
    scanTrace("dir = " + dir.string());

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return out;
    }

    for (const auto& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        const std::string suffix = kSidecarSuffix;
        if (name.size() < suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;  // not a sidecar; not traced (every .pdf would log)
        }
        scanTrace("candidate " + name);

        // Read + parse the sidecar.
        QFile f(qStringFromFs(entry.path()));
        if (!f.open(QIODevice::ReadOnly)) { scanTrace("  REJECT: unreadable"); continue; }
        const QByteArray bytes = f.readAll();
        f.close();

        QJsonParseError perr{};
        const QJsonDocument jdoc = QJsonDocument::fromJson(bytes, &perr);
        if (perr.error != QJsonParseError::NoError || !jdoc.isObject()) {
            scanTrace("  REJECT: bad JSON");
            continue;
        }
        const QJsonObject obj = jdoc.object();

        RecoveryRecord rec;
        rec.originalPath = fsFromQString(obj.value("originalPath").toString());
        rec.recoveryFilePath =
            fsFromQString(obj.value("recoveryFilePath").toString());
        rec.sidecarPath = entry.path();
        rec.timestamp = obj.value("timestamp").toString().toStdString();
        rec.sessionId = obj.value("sessionId").toString().toStdString();
        rec.ownerPid = parseU64(obj.value(kOwnerPidKey));
        rec.ownerStartToken = parseU64(obj.value(kOwnerStartTokenKey));

        // A sidecar missing its sessionId is corrupt: clearRecovery() /
        // discardRecovery() session-matching cannot address it and the startup
        // recovery prompt would show a blank/garbage record. Reject it as
        // unusable rather than surfacing a partial record.
        if (rec.sessionId.empty()) {
            scanTrace("  REJECT: empty sessionId");
            continue;
        }

        // Only surface records whose recovery copy still exists — a sidecar
        // pointing at a missing copy is unrecoverable noise.
        std::error_code eec;
        if (rec.recoveryFilePath.empty() ||
            !std::filesystem::exists(rec.recoveryFilePath, eec)) {
            scanTrace("  REJECT: recovery copy missing: "
                      + rec.recoveryFilePath.string());
            continue;
        }

        // GitHub #97 — cross-instance leak. The recovery directory is per-USER,
        // so a second concurrently-running instance owned by the same user
        // writes here too, and its sidecar exists for the ordinary reason that
        // it is still editing. Offering that record lets instance 1 open
        // instance 2's LIVE work as a "crashed session" and save over it.
        // A record whose owner is still alive is therefore withheld — its work
        // is not lost and needs no recovering.
        if (isForeignLiveInstance(rec)) {
            scanTrace("  REJECT: owner still live (pid "
                      + std::to_string(rec.ownerPid) + ")");
            continue;
        }

        scanTrace("  ACCEPT");
        out.push_back(std::move(rec));
    }

    return out;
}

// ---------------------------------------------------------------------------
// GitHub #97 — owner identity and the cross-instance liveness filter.
// ---------------------------------------------------------------------------

std::uint64_t AutosaveManager::currentProcessId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t AutosaveManager::processStartToken(std::uint64_t pid) {
    if (pid == 0) {
        return 0;
    }
#ifdef _WIN32
    // SYNCHRONIZE is requested alongside the query right so the exit check
    // below can wait on the handle; see the EXITED-BUT-OPENABLE note.
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                             FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        // No such pid, or a process we may not query (another user's — the
        // recovery dir is per-user, so that is by definition not one of our
        // instances). Either way we cannot establish that a PDFE of ours is
        // alive under this pid: return "unknown", which the filter reads as
        // "offer the record".
        return 0;
    }

    // EXITED BUT STILL OPENABLE — the data-loss bug this check exists to stop
    // (GitHub #285 cluster G).
    //
    // A terminated process keeps its pid and its kernel object alive for as
    // long as ANYONE holds a handle to it. OpenProcess therefore still
    // succeeds, and GetProcessTimes still reports the ORIGINAL creation time —
    // so the incarnation token matches, isForeignLiveInstance() concludes "the
    // owner is still running", and a genuinely crashed session's recovery is
    // WITHHELD. That is failure direction (b) in isForeignLiveInstance's own
    // comment, which it calls strictly worse because the user's unsaved work
    // becomes silently unreachable.
    //
    // It is not hypothetical or test-only: anything holding a handle to the
    // dead process does it — a parent launcher, a debugger, a crash reporter,
    // Task Manager with the process selected. Measured: PDFE force-killed at
    // pid 4992, relaunched, and the scan rejected the sidecar with "owner still
    // live (pid 4992)" while the recovery prompt never appeared.
    //
    // A signalled process handle means exited, unambiguously. GetExitCodeProcess
    // is NOT used: it reports STILL_ACTIVE (259) for a live process, and 259 is
    // also a perfectly legal exit code, so it cannot distinguish the two.
    if (::WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        ::CloseHandle(h);
        return 0;  // exited -> "unknown owner" -> offer the record
    }

    FILETIME creation{}, exitT{}, kernelT{}, userT{};
    std::uint64_t token = 0;
    if (::GetProcessTimes(h, &creation, &exitT, &kernelT, &userT)) {
        token = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) |
                static_cast<std::uint64_t>(creation.dwLowDateTime);
    }
    ::CloseHandle(h);
    // A creation time of 0 would be indistinguishable from "unknown"; it is not
    // a value Windows produces for a live process, so no special case is needed.
    return token;
#else
    // /proc/<pid>/stat field 22 (starttime, in clock ticks since boot) is the
    // POSIX analogue of the Windows creation time.
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%llu/stat",
                  static_cast<unsigned long long>(pid));
    std::FILE* f = std::fopen(path, "re");
    if (!f) {
        return 0;
    }
    char buf[4096];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (n == 0) {
        return 0;
    }
    buf[n] = '\0';
    // The comm field (2) is parenthesised and may contain spaces, so scan from
    // the LAST ')' and count fields from there: field 3 follows it.
    const char* p = std::strrchr(buf, ')');
    if (!p) {
        return 0;
    }
    ++p;
    int field = 2;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        ++field;
        if (field == 22) {
            return std::strtoull(p, nullptr, 10);
        }
        while (*p && *p != ' ') ++p;
    }
    return 0;
#endif
}

bool AutosaveManager::isForeignLiveInstance(const RecoveryRecord& rec) {
    // FAILURE DIRECTION, stated explicitly because it is the whole design
    // question here. Two ways to be wrong:
    //
    //   (a) call a live foreign instance DEAD  -> we offer its in-progress file.
    //       Cost: a spurious recovery prompt, and if accepted, the user opens a
    //       point-in-time COPY (recoveryFilePath, a separate file) — the other
    //       instance's document is untouched until the user deliberately saves
    //       over it.
    //   (b) call a crashed instance LIVE       -> we hide a real recovery.
    //       Cost: the user's unsaved work is silently unreachable. That is data
    //       loss, and it defeats the only reason this subsystem exists.
    //
    // (b) is strictly worse, so EVERY uncertain case below resolves to "not
    // foreign-live" (i.e. OFFER the record). We withhold a record only on
    // positive proof that its owner is running: pid present, incarnation token
    // present, and the token of the live process under that pid MATCHES.

    if (rec.ownerPid == 0) {
        // Legacy (schema-1) sidecar, or a stamp we could not read. Unknown
        // owner -> offer.
        return false;
    }
    if (rec.ownerPid == currentProcessId()) {
        // Our own sidecar. Not "foreign", and deliberately still offered: the
        // simulated-crash contract this component is tested under (see the
        // header) drops the manager and re-scans within one process, and at a
        // real startup this process has not written any sidecar yet.
        return false;
    }
    if (rec.ownerStartToken == 0) {
        // Stamped with a pid but no incarnation token. A bare pid cannot be
        // trusted (the OS recycles them), so treat the owner as unknown ->
        // offer, per (b).
        return false;
    }
    const std::uint64_t liveToken = processStartToken(rec.ownerPid);
    if (liveToken == 0) {
        // Nothing running under that pid, or unqueryable -> the owner is gone
        // as far as we can tell -> offer. This is the CRASHED-INSTANCE path.
        return false;
    }
    // Same pid AND same incarnation: the writing process is genuinely still
    // running. Withhold.
    return liveToken == rec.ownerStartToken;
}

void AutosaveManager::discardRecovery(const RecoveryRecord& rec) {
    std::error_code ec;
    if (!rec.sidecarPath.empty()) {
        std::filesystem::remove(rec.sidecarPath, ec);
    }
    if (!rec.recoveryFilePath.empty()) {
        std::filesystem::remove(rec.recoveryFilePath, ec);
    }
}

std::size_t AutosaveManager::discardAllRecoverable(
    const std::filesystem::path& recoveryDir) {
    std::size_t n = 0;
    try {
        for (const auto& rec : scanForRecoverable(recoveryDir)) {
            discardRecovery(rec);
            ++n;
        }
    } catch (...) {
        // best-effort cleanup; never propagate a scan/IO error
    }
    return n;
}

std::filesystem::path AutosaveManager::defaultRecoveryDir() {
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString dir = base.isEmpty() ? QDir::tempPath() : base;
    std::filesystem::path p = fsFromQString(dir) / "recovery";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

}  // namespace pdfe::core
