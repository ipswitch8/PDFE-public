// -*- coding: utf-8 -*-
#pragma once

// R3-04 — Periodic autosave + crash-recovery.
//
// AutosaveManager is a HEADLESS, GoogleTest-able component: it depends only on
// the core document model (Document) and the undo stack (UndoManager) for its
// dirty signal — no UI. MainWindow merely wires it (start/stop with the open
// document, and a startup recovery prompt). Because the engine is UI-free, the
// "kill-mid-edit → recovery" contract is testable WITHOUT a real process kill:
// a test drives autosaveNow() directly, then DROPS the Document +
// AutosaveManager without a clean save (simulated crash) and runs a fresh
// static scanForRecoverable().
//
// Mechanism:
//   * On a periodic trigger (QTimer) OR an explicit synchronous autosaveNow(),
//     if the document is DIRTY (undo not clean) since the last autosave, the
//     current in-memory document — unsaved edits included — is written to a
//     RECOVERY SIDECAR: a full PDF copy in a per-user recovery directory, via
//     Document::writeRecoveryCopy() (which does NOT perturb the live document's
//     save-routing state). A small JSON metadata record is written alongside
//     it, mapping the recovery copy back to the original source path.
//   * On a CLEAN save or explicit close, clearRecovery() removes this session's
//     sidecar — a clean exit leaves nothing to recover.
//   * scanForRecoverable() reads the recovery directory and returns a record
//     for every session whose sidecar is still present (i.e. did NOT exit
//     cleanly). Recovery itself is just opening recoveryFilePath as a normal
//     Document.

#include <QObject>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class QTimer;

namespace pdfe::commands {
class UndoManager;
}

namespace pdfe::core {

class Document;

/// One recoverable session, reconstructed from a recovery sidecar on disk.
struct RecoveryRecord {
    std::filesystem::path originalPath;      ///< source doc path (empty if never saved)
    std::filesystem::path recoveryFilePath;  ///< the recovery PDF copy to reopen
    std::filesystem::path sidecarPath;       ///< the JSON metadata file
    std::string timestamp;                   ///< ISO-8601 UTC of the last autosave
    std::string sessionId;                   ///< unique per editing session
    /// OS process id of the instance that wrote the sidecar. 0 for a legacy
    /// sidecar written before owner stamping existed.
    std::uint64_t ownerPid = 0;
    /// Process-identity token of that pid at the time of writing — see
    /// AutosaveManager::processStartToken(). 0 when unknown/unavailable.
    std::uint64_t ownerStartToken = 0;
};

class AutosaveManager : public QObject {
    Q_OBJECT

public:
    /// @param doc         The live document (not owned). Must outlive this.
    /// @param undo        The undo manager whose isClean() gives the dirty
    ///                    signal (not owned). May be null (then never dirty).
    /// @param sourcePath  The document's source path; may be empty for a
    ///                    never-saved document.
    /// @param recoveryDir Directory for recovery sidecars. Empty → the default
    ///                    per-user location (defaultRecoveryDir()). Tests inject
    ///                    a temp dir for isolation.
    /// @param parent      Optional QObject parent.
    AutosaveManager(Document* doc,
                    pdfe::commands::UndoManager* undo,
                    std::filesystem::path sourcePath,
                    std::filesystem::path recoveryDir = {},
                    QObject* parent = nullptr);

    ~AutosaveManager() override;

    /// Perform ONE autosave synchronously. If the document is dirty (undo not
    /// clean) and something changed since the last autosave, writes the
    /// recovery PDF copy + JSON sidecar and returns true. If the document is
    /// clean (or unchanged since the last autosave), writes nothing and returns
    /// false. Exposed so tests drive autosave deterministically without waiting
    /// on the timer.
    bool autosaveNow();

    /// Start the periodic autosave timer (default 30s). No-op detail: the timer
    /// simply calls autosaveNow() on each tick.
    void start(int intervalMs = 30000);

    /// Stop the periodic timer.
    void stop();

    /// Remove THIS session's recovery sidecar + copy (call on clean save/close).
    /// Idempotent.
    void clearRecovery();

    /// Update the source path (e.g. after Save As gives a never-saved doc a
    /// path). Recorded into future sidecars.
    void setSourcePath(std::filesystem::path sourcePath);

    /// Register a callback invoked from autosaveNow() AFTER the decision to
    /// write and immediately BEFORE the recovery snapshot is taken. It gives
    /// the UI a chance to commit deferred, in-panel edits into the live
    /// document (e.g. the bookmark tree, which lives in BookmarkManager until
    /// committed) so the recovery copy carries them. Mirrors the pre-save
    /// flush the UI already performs before a real File>Save. A throwing flush
    /// must never abort the snapshot — exceptions are swallowed.
    void setPreSnapshotFlush(std::function<void()> fn);

    /// Whether a recovery sidecar for this session currently exists on disk.
    [[nodiscard]] bool hasRecovery() const;

    [[nodiscard]] const std::filesystem::path& recoveryDir() const { return recoveryDir_; }
    [[nodiscard]] const std::filesystem::path& recoveryFilePath() const { return recoveryFilePath_; }
    [[nodiscard]] const std::filesystem::path& sidecarPath() const { return sidecarPath_; }
    [[nodiscard]] const std::string& sessionId() const { return sessionId_; }

    /// Scan a recovery directory for sessions that did NOT exit cleanly (their
    /// sidecar is still present). @p recoveryDir empty → defaultRecoveryDir().
    /// Only records whose recovery PDF copy still exists on disk are returned.
    ///
    /// GitHub #97 — the recovery directory is per-USER, not per-instance, so a
    /// second concurrently-running PDFE owned by the same user writes its
    /// sidecars into the same directory. Records belonging to ANOTHER STILL-LIVE
    /// instance are filtered out here: that instance's work is not lost, and
    /// offering its in-progress snapshot lets one instance open (and then
    /// re-save over) another's live editing state.
    ///
    /// A CRASHED instance is still offered — that is the entire point of
    /// recovery — so the filter turns on liveness, not merely on "written by a
    /// different process". See isForeignLiveInstance() in the .cpp for the exact
    /// rule and its fail-open direction.
    static std::vector<RecoveryRecord> scanForRecoverable(
        const std::filesystem::path& recoveryDir = {});

    /// This process's OS process id.
    static std::uint64_t currentProcessId();

    /// A token that identifies the *incarnation* of @p pid: the process creation
    /// time (Windows FILETIME / POSIX start ticks). Returns 0 when @p pid is not
    /// running, or when the token cannot be obtained.
    ///
    /// A pid alone is not an identity — the OS recycles pids, so a dead
    /// instance's pid can be held by an unrelated live process. Pairing the pid
    /// with its creation time makes the owner stamp unforgeable by coincidence.
    /// Exposed because the cross-instance filter's safety rests on it, and a
    /// test that cannot call it cannot construct a genuinely-live foreign owner.
    static std::uint64_t processStartToken(std::uint64_t pid);

    /// True when @p rec was written by a DIFFERENT process that is still alive
    /// (same pid AND same incarnation token). Such a record must not be offered
    /// for recovery. Exposed for the same reason as processStartToken().
    static bool isForeignLiveInstance(const RecoveryRecord& rec);

    /// Discard a recovered session: remove its sidecar and recovery copy.
    static void discardRecovery(const RecoveryRecord& rec);

    /// Discard EVERY recoverable session in @p recoveryDir (empty →
    /// defaultRecoveryDir()). Best-effort: swallows scan/IO errors and returns
    /// the number of records discarded. Used by the UI-test determinism seam to
    /// clear stale sidecars a force-killed dirty test left behind, so they can
    /// never fire the startup recovery prompt on a later launch.
    static std::size_t discardAllRecoverable(
        const std::filesystem::path& recoveryDir = {});

    /// The default per-user recovery directory
    /// (QStandardPaths::AppDataLocation / "recovery"). Created if missing.
    static std::filesystem::path defaultRecoveryDir();

    // -----------------------------------------------------------------------
    // Autosave write-latency probe (GitHub #106 / meta-audit row WIN-89)
    // -----------------------------------------------------------------------
    //
    // WHY. Code signing and on-access antimalware interact badly with the
    // atomic QSaveFile write this class uses for its sidecar: QSaveFile writes
    // a temporary file and then RENAMES it into place, and a real-time scanner
    // inspects each freshly-created file before releasing the handle. On a
    // signed binary the scanner may additionally verify the publisher on first
    // touch. The sidecar is a few hundred bytes of JSON, so any measurable
    // duration at all is scanner time rather than IO time. See
    // docs/code-signing.md, "Autosave write latency under antimalware".
    //
    // The probe does not fix that -- it makes it VISIBLE, which is what WIN-89
    // asked for, because the symptom otherwise presents as unexplained UI
    // stalls on the autosave tick with nothing in any log.

    /// Duration above which one sidecar write is considered "measurably
    /// delayed" and warned about.
    ///
    /// RATIONALE FOR THE VALUE, so this is not a magic number. The sidecar is a
    /// single QJsonDocument of ~300-500 bytes written through QSaveFile, i.e.
    /// one create + one write + one rename, all against the per-user AppData
    /// directory. On unobstructed storage -- SSD or spinning disk, warm or cold
    /// cache -- that completes in low single-digit milliseconds; the operation
    /// is dominated by filesystem metadata, not by payload size, so it does not
    /// scale with document size and will not drift as documents get larger.
    /// 250 ms is therefore roughly two orders of magnitude above the expected
    /// cost: comfortably beyond scheduler jitter, a busy disk or a virtualised
    /// runner, while still far below the 1 s mark at which a user perceives a
    /// stall. It is a diagnostic floor for "something is inspecting our
    /// writes", not a performance budget, and nothing branches on it except the
    /// warning.
    static constexpr int kSidecarWriteWarnMs = 250;

    /// How many times the latency warning has been emitted this process.
    ///
    /// The warning is emitted AT MOST ONCE per process (see
    /// resetSidecarWriteLatencyProbe). A scanner that delays one write delays
    /// every subsequent one, and autosave ticks on a timer, so warning per
    /// occurrence would emit indefinitely and bury the first, most useful
    /// report. Exposed so a test can assert on the count directly rather than
    /// having to scrape a log.
    static int sidecarWriteWarningCount();

    /// Clear the one-shot latch and the counter. Test-support only; a test that
    /// needs to observe the warning cannot do so if an earlier test in the same
    /// process already consumed it.
    static void resetSidecarWriteLatencyProbe();

    /// Injection seam: @p hook runs INSIDE the timed region of every sidecar
    /// write, so a test can make a write slow deterministically.
    ///
    /// WHY A RUNTIME SEAM RATHER THAN `#ifdef PDFE_TESTING`. GitHub #182
    /// records that `PDFE_TESTING` is never actually set OFF for the shipped
    /// build, so an `#ifdef` on it is not the compile-out it appears to be and
    /// would give a false sense that the seam cannot ship. A default-empty
    /// std::function is honest about what it is: it is always compiled in,
    /// costs one empty-check per autosave tick, changes no behaviour unless
    /// something sets it, and nothing in src/ ever does.
    ///
    /// Passing a default-constructed function removes the hook.
    static void setSidecarWriteProbeHook(std::function<void()> hook);

private:
    void writeSidecar();

    /// The currently-installed probe hook, or an empty function. Returned BY
    /// VALUE under the lock so the hook cannot be destroyed by a concurrent
    /// setSidecarWriteProbeHook() while writeSidecar() is calling it.
    static std::function<void()> sidecarWriteProbeHook();

    /// Feed one measured sidecar-write duration to the probe. Emits the warning
    /// if @p elapsedMs is at or above kSidecarWriteWarnMs and nothing has
    /// warned yet this process.
    static void noteSidecarWriteDuration(long long elapsedMs);

    Document* doc_ = nullptr;
    pdfe::commands::UndoManager* undo_ = nullptr;
    std::filesystem::path sourcePath_;
    std::filesystem::path recoveryDir_;
    std::string sessionId_;
    // Owner stamp written into every sidecar (GitHub #97). Captured once at
    // construction: the pid cannot change, and re-querying the token at write
    // time would only add a failure mode.
    std::uint64_t ownerPid_ = 0;
    std::uint64_t ownerStartToken_ = 0;
    std::filesystem::path recoveryFilePath_;
    std::filesystem::path sidecarPath_;
    QTimer* timer_ = nullptr;

    // "since the last autosave" bookkeeping: the undo-stack size at the last
    // successful autosave. -1 means "never autosaved this session".
    int lastAutosaveStackSize_ = -1;
    // Document::contentVersion() at the last successful autosave (baselined to
    // the doc's version at construction). Lets a non-undo mutation re-trigger a
    // write even when the undo stack never moves.
    uint64_t lastAutosaveContentVersion_ = 0;
    bool wroteRecovery_ = false;

    // Pre-snapshot flush hook (see setPreSnapshotFlush). Empty by default.
    std::function<void()> preSnapshotFlush_;
};

}  // namespace pdfe::core
