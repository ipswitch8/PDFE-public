// -*- coding: utf-8 -*-
#pragma once

// ===========================================================================
// Save-path failure-injection harness  (meta-audit follow-up F10)
//
// Gaps: WIN-64, WIN-69, WIN-76, WIN-77, WIN-81, WIN-84, WIN-85, WIN-87.
//
// WHAT IT INJECTS, AND WHERE
// --------------------------
// PDFE's durable-write sequence has exactly three named points at which the
// world can go wrong. This harness names them and can fail each one:
//
//   SaveInjectionPoint::TempWrite
//       pdf_save_document() writing "<target>.pdfe_tmp".
//       Failure class: the volume has no space left, the media was pulled, the
//       target directory became unwritable.  → ENOSPC-class (WIN-76, WIN-77,
//       WIN-64).
//
//   SaveInjectionPoint::AtomicRename
//       atomicReplaceFileWithRetry() moving the temp over the target.
//       Failure class: an anti-virus scanner / search indexer / backup agent
//       holds the destination open.  → rename lock (WIN-84, WIN-85).
//
//   SaveInjectionPoint::InsideWindow
//       between the two — the moment at which an OS shutdown or a
//       TerminateProcess is maximally destructive.  → process termination
//       (WIN-69, WIN-81).
//
// HOW REAL IS THIS?  (read this before trusting a green result)
// ------------------------------------------------------------
// Everything here is a REAL operating-system failure at the REAL call site.
// There is no mock, no stubbed return value, and — importantly — NO
// PRODUCTION-CODE SEAM: nothing in src/ knows this harness exists. The
// injection works purely by arranging genuinely hostile filesystem state
// around the save, because the FULL-REWRITE save's temp path is deterministic
// ("<target>.pdfe_tmp"), which is what makes that possible.
//
// WHICH SAVE THAT IS, STATED EXACTLY (F58-a, phase-53). PDFE has two staging
// mechanisms, and this harness's three injection points reach ONE of them:
//   * Document::save()'s full-rewrite branch, saveAs-over-existing, redaction,
//     batch and image export stage at the fixed "<target>.pdfe_tmp" — poisonable
//     by name, which is what everything below depends on;
//   * Document::saveIncrementalStaged() (the /DSS and LTV append route) stages
//     at core::StagingFile's "<target>.pdfe_part.<128-bit CSPRNG hex>" behind an
//     exclusive handle held from creation to publication. It is UNPOISONABLE by
//     name on purpose — that unpredictability IS the F58-a hardening — so
//     TempWriteBlocker and RenameLock arm successfully against it and inject
//     NOTHING. Do not read a green /DSS test that used them as evidence; see
//     tests/test_dss_save_fault_injection.cpp, which says so at length, and
//     startAppendGrowthWatcher below, which is the injector that does reach it.
//
//   * TempWrite is failed by making that exact temp path genuinely
//     unwritable — either a DIRECTORY sits on it, or another handle owns it
//     with no sharing. MuPDF's own fopen() then fails for real, inside
//     pdf_save_document, and fz_throw propagates through PDFE's real error
//     path.
//
//   * AtomicRename is failed by a real CreateFileW handle on the destination
//     with dwShareMode == 0 — byte-for-byte the state an A/V scanner creates.
//     MoveFileExW really does return ERROR_SHARING_VIOLATION.
//
//   * ProcessTermination is a real child process really killed with
//     TerminateProcess (no unwinding, no destructors, no flush) — not a
//     simulated drop of C++ objects.
//
// HONEST LIMITATION — the one thing that is NOT literal:
//   A literal ENOSPC (errno 28 / ERROR_DISK_FULL 112) raised PART-WAY through
//   the temp write requires a volume that genuinely runs out of space mid-write.
//   On Windows every way to obtain one — attaching a VHD, setting an NTFS disk
//   quota, mounting a small volume — requires administrative privilege, which a
//   unit-test process must not assume. So this harness fails the temp write at
//   its OPEN rather than at a byte boundary, and the resulting error code is
//   ERROR_ACCESS_DENIED / ERROR_SHARING_VIOLATION rather than ERROR_DISK_FULL.
//
//   Why that is still the right oracle: the save path has NO disk-full-specific
//   handling to differentiate. `grep -rn "ENOSPC\|ERROR_DISK_FULL\|no_space" src/`
//   returns nothing — every temp-write failure, whatever its errno, leaves
//   pdf_save_document by the same fz_throw and takes the same single branch in
//   Document::save()/saveAs(). The property under test — "a failed temp write
//   leaves the ORIGINAL byte-identical and publishes nothing" — is therefore
//   exercised by the identical code path a real disk-full would take. The
//   partial-temp aspect a real ENOSPC adds (a truncated .pdfe_tmp left on disk)
//   is covered separately and literally by stageTruncatedOrphanTemp().
//
//   A literal small-volume run is recorded as a residual manual step in
//   docs/audit/meta-audit-2026-07/followup/phase-10-dispositions.md.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pdfe::testing {

/// The named points in the durable-write sequence at which failure is injected.
enum class SaveInjectionPoint {
    TempWrite,     ///< pdf_save_document writing "<target>.pdfe_tmp"
    AtomicRename,  ///< atomicReplaceFileWithRetry(temp → target)
    InsideWindow   ///< between the two (shutdown / process death)
};

/// Human-readable name, used in test failure messages and parametrized suite
/// names so a red result says WHICH point broke.
const char* injectionPointName(SaveInjectionPoint p);

/// The temp path PDFE's FULL-REWRITE save sequence uses for `target`.
/// Deterministic by design (Document::save()'s full-rewrite branch and
/// Document::saveAs() both append ".pdfe_tmp"), which is what lets the harness
/// pre-poison it without any production seam.
///
/// NOT the staged INCREMENTAL append's path: that is
/// "<target>.pdfe_part.<CSPRNG hex>" and is deliberately unguessable (F58-a).
/// Nothing that poisons this path affects it — see the header note above.
std::filesystem::path saveTempPathFor(const std::filesystem::path& target);

/// How the TempWrite point is made to fail.
enum class TempWriteBlockMode {
    /// A directory occupies "<target>.pdfe_tmp". fopen("wb") on a directory
    /// fails at the OS level. Survives a stray remove() by the code under test.
    DirectoryOnTempPath,
    /// A real file at "<target>.pdfe_tmp" held by a CreateFileW handle with
    /// dwShareMode == 0 — the state a scanner mid-scan produces.
    ExclusiveHandleOnTempPath
};

// ---------------------------------------------------------------------------
// SaveInjectionPoint::TempWrite  — ENOSPC-class temp-write failure.
// ---------------------------------------------------------------------------
class TempWriteBlocker {
public:
    TempWriteBlocker(const std::filesystem::path& target, TempWriteBlockMode mode);
    ~TempWriteBlocker();

    TempWriteBlocker(const TempWriteBlocker&) = delete;
    TempWriteBlocker& operator=(const TempWriteBlocker&) = delete;

    /// True when the hostile state was actually established. A test MUST assert
    /// this — a blocker that silently failed to arm turns the whole oracle into
    /// a happy-path run that passes for the wrong reason.
    bool armed() const { return armed_; }

    /// Release early (e.g. to prove the save works once the obstruction is gone).
    void release();

    const std::filesystem::path& tempPath() const { return tempPath_; }

private:
    std::filesystem::path tempPath_;
    TempWriteBlockMode mode_;
    bool armed_ = false;
    void* handle_ = nullptr;  // HANDLE on Windows; unused elsewhere
};

// ---------------------------------------------------------------------------
// IN-PLACE (INCREMENTAL) SAVE FAILURE  —  meta-audit follow-up F47 item F5,
// phase-49.
// ---------------------------------------------------------------------------
//
// WHY A FOURTH INJECTION POINT WAS NEEDED, AND WHAT IT REVEALED.
//
// Everything above targets PDFE's temp-then-atomic-rename sequence, and that
// sequence is what makes a failed save SAFE: the original is never touched, so
// "the original is byte-identical afterwards" is a structural guarantee rather
// than a hope.
//
// The LTV /DSS write did not use it. LtvDssWriter::write() mutated the
// document in memory and then called Document::saveIncremental(), which is
// pdf_save_document(do_incremental=1) straight onto the LIVE TARGET FILE.
// There was no temp file, no rename, and therefore NO structural guarantee at
// all — the bytes being appended went on the end of the user's only copy.
// TempWriteBlocker and RenameLock had nothing to bite on: the path they poison
// ("<target>.pdfe_tmp") was never created.
//
// So the DSS/signature save path had never been fault-injected by anything,
// which is exactly why F5 exists.
//
// PAST TENSE ON PURPOSE (F58/F58-a, phases 52–53 — this passage described the
// pre-F58 world for one phase after it stopped being true). LtvDssWriter::write()
// now calls Document::saveIncrementalStaged(), which DOES stage and DOES publish
// by an atomic rename. Two consequences for this harness, and neither cancels
// the paragraph above:
//   * the structural guarantee now exists for the /DSS path too — a failed
//     append can no longer damage the user's copy;
//   * TempWriteBlocker and RenameLock still have nothing to bite on, for a
//     DIFFERENT reason: the staging file is "<target>.pdfe_part.<CSPRNG hex>"
//     behind an exclusive handle, so it is unguessable by design. They arm and
//     inject nothing, which is the blindness DCA 0015 recorded. The injector
//     that reaches the staged append is startAppendGrowthWatcher() below.
//
// HOW IT FAILS THE WRITE. The target is marked read-only at the filesystem
// level (FILE_ATTRIBUTE_READONLY on Windows, mode 0444 elsewhere), so MuPDF's
// own fopen() for update fails for real, inside pdf_save_document, and
// fz_throw propagates through PDFE's real error path. No mock, no stubbed
// return, no production seam — the same honesty constraint the rest of this
// header is written to.
//
// SAME LIMITATION, SAME REASONING as TempWriteBlocker: the failure lands at
// the OPEN rather than at a byte boundary, because a literal mid-write ENOSPC
// needs a volume that genuinely fills up and that needs administrative
// privilege. It is the right oracle for the same reason — the save path has no
// error-code-specific handling to differentiate, so every failure to write
// leaves pdf_save_document by the identical fz_throw.
//
// Note the blocker deliberately works on a file MuPDF already has OPEN for
// reading. That is not incidental: an incremental save always runs against a
// document that is currently open, so any injection that required exclusive
// access (as RenameLock does) could not arm here at all.
class InPlaceWriteBlocker {
public:
    explicit InPlaceWriteBlocker(const std::filesystem::path& target);
    ~InPlaceWriteBlocker();

    InPlaceWriteBlocker(const InPlaceWriteBlocker&) = delete;
    InPlaceWriteBlocker& operator=(const InPlaceWriteBlocker&) = delete;

    /// True when the target really is unwritable now. A test MUST assert this:
    /// an unarmed blocker turns the whole case into a happy-path run that
    /// passes for the wrong reason.
    bool armed() const { return armed_; }

    /// Restore writability early, so the same test can prove the save works
    /// once the obstruction is gone.
    void release();

    const std::filesystem::path& target() const { return target_; }

private:
    std::filesystem::path target_;
    bool armed_ = false;
    unsigned long savedAttributes_ = 0;  // DWORD on Windows; st_mode elsewhere
};

// ---------------------------------------------------------------------------
// SaveInjectionPoint::AtomicRename — A/V-style lock on the rename destination.
// ---------------------------------------------------------------------------
//
// Holds `target` open with GENERIC_READ|GENERIC_WRITE and dwShareMode == 0, so
// MoveFileExW(..., MOVEFILE_REPLACE_EXISTING) onto it fails with
// ERROR_SHARING_VIOLATION for as long as the handle lives. This is the same
// idiom tests/test_save_error_paths.cpp already uses, factored out so the
// transient and permanent variants share one implementation.
class RenameLock {
public:
    /// Permanent lock: held until release()/destruction.
    explicit RenameLock(const std::filesystem::path& target);

    /// Transient lock: a background thread releases the handle after
    /// `holdFor`. Models an A/V scanner that finishes and lets go — the case
    /// the bounded retry in atomicReplaceFileWithRetry exists to survive.
    RenameLock(const std::filesystem::path& target, std::chrono::milliseconds holdFor);

    ~RenameLock();

    RenameLock(const RenameLock&) = delete;
    RenameLock& operator=(const RenameLock&) = delete;

    /// True when the exclusive handle was actually obtained. Assert it.
    bool armed() const;

    /// True once the handle has been closed (by release() or by the timer).
    bool released() const;

    void release();

    /// Implementation detail, public only so the .cpp's shared-state registry
    /// can name it.
    struct State;

private:
    State* state_;  // shared with the timer thread; owned via a shared_ptr inside
};

// ---------------------------------------------------------------------------
// SaveInjectionPoint::InsideWindow — real process termination.
// ---------------------------------------------------------------------------
//
// Re-executes THIS test binary as a child in "save victim" mode: the child
// opens `sourcePdf`, makes a real structural edit, then saves to `target`. The
// parent waits for the child to signal (via a marker file) that it is inside
// the durable-write window and then TerminateProcess()es it — no unwinding, no
// destructors, no buffer flush, exactly like a power loss or a
// WM_ENDSESSION-driven kill.
//
// The child mode is selected by the environment variable below; test_main.cpp
// needs no changes because the child is launched with a --gtest_filter that
// runs only the victim test.
constexpr const char* kSaveVictimEnvVar = "PDFE_F10_SAVE_VICTIM";
constexpr const char* kSaveVictimTargetEnvVar = "PDFE_F10_SAVE_VICTIM_TARGET";
constexpr const char* kSaveVictimSourceEnvVar = "PDFE_F10_SAVE_VICTIM_SOURCE";
constexpr const char* kSaveVictimMarkerEnvVar = "PDFE_F10_SAVE_VICTIM_MARKER";

/// The code TerminateProcess stamps on a victim it actually terminated.
///
/// GitHub #291. Windows applies this ONLY if the process was still running; a
/// child that already exited keeps its own exit code. So this constant is what
/// separates "the kill interrupted a write" from "the kill arrived after the
/// write finished" -- a distinction the oracle needs and `childKilled` cannot
/// provide, because that flag is TerminateProcess's return value and says
/// nothing about timing.
///
/// The premise is MEASURED, not assumed, by
/// MidAppendKillClassification.TerminatingAnAlreadyExitedChildKeepsItsExitCode.
constexpr unsigned long kKilledWhileRunningExitCode = 0xF10DEAD;

struct KilledSaveResult {
    bool childLaunched = false;   ///< CreateProcessW succeeded
    bool childReachedWindow = false;  ///< the marker file appeared
    bool childKilled = false;     ///< TerminateProcess succeeded
    unsigned long exitCode = 0;   ///< child's exit code after the kill
    std::string diagnostic;       ///< why it failed, when it did

    /// True when the kill actually interrupted a RUNNING child (GitHub #291).
    ///
    /// Distinct from `childKilled`: that one only says the API call succeeded,
    /// and TerminateProcess succeeds on an already-dead process too. Without
    /// this, a kill that landed too late is indistinguishable from one that
    /// interrupted an append, and the oracle reports a completed write as a
    /// destroyed signature.
    bool killLandedWhileRunning = false;
};

/// What the post-kill on-disk state actually demonstrates (GitHub #291).
enum class MidAppendVerdict {
    /// The kill interrupted a running append and the document is unchanged --
    /// the property the control exists to prove.
    KillLandedAndDocumentIntact,
    /// The kill interrupted a running append and the document MOVED. The
    /// alarming message is correct here.
    DataLoss,
    /// The child finished its write before the kill arrived. NOTHING was
    /// injected mid-append, so the on-disk state proves nothing either way --
    /// and a grown file is the expected result of a completed /DSS append
    /// rather than evidence of loss.
    KillLandedLate,
    /// The child died of something that was neither our kill nor a clean exit.
    /// Not data loss, not a late kill, and collapsing it into either would hide
    /// a victim that is itself broken.
    ChildDiedUnexpectedly,
};

/// Classify a mid-append kill from the three facts the parent can observe.
///
/// Pure on purpose: reproducing a late kill on demand is precisely what nobody
/// can do reliably -- which is why GitHub #291 survived as a "transient flake"
/// -- so the DECISION is testable even though the RACE is not. See
/// tests/test_dss_mid_append_kill_classification.cpp.
///
/// Ordering note: the exit code is consulted FIRST. If the child was not
/// interrupted, the file's state is not evidence about interruption at all, so
/// no combination of size/bytes may promote a late kill to either a pass or a
/// data-loss claim.
constexpr MidAppendVerdict classifyMidAppendKill(unsigned long exitCode,
                                                 bool sizeChanged,
                                                 bool byteIdentical)
{
    if (exitCode != kKilledWhileRunningExitCode) {
        return exitCode == 0 ? MidAppendVerdict::KillLandedLate
                             : MidAppendVerdict::ChildDiedUnexpectedly;
    }
    // The kill landed. Now the on-disk state means something.
    if (sizeChanged || !byteIdentical) {
        return MidAppendVerdict::DataLoss;
    }
    return MidAppendVerdict::KillLandedAndDocumentIntact;
}

/// Full path of the currently running test executable.
std::filesystem::path currentExecutablePath();

/// Spawn a child of this test binary that opens `sourcePdf`, edits it and saves
/// over `target`; kill the child once it reports being inside the save window.
///
/// `gtestFilterForVictim` is the --gtest_filter naming the victim test in the
/// child (so the child runs that one test and nothing else).
KilledSaveResult killChildInsideSaveWindow(
    const std::filesystem::path& sourcePdf,
    const std::filesystem::path& target,
    const std::string& gtestFilterForVictim,
    std::chrono::milliseconds waitForWindow = std::chrono::seconds(30));

/// True when this process IS the child victim (the env var is set).
bool runningAsSaveVictim();

/// Child-side helpers: the paths the parent handed down.
std::filesystem::path saveVictimTarget();
std::filesystem::path saveVictimSource();
std::filesystem::path saveVictimMarker();

/// Child-side: announce "I am now inside the durable-write window" by creating
/// the marker file the parent polls for.
void announceInsideSaveWindow();

/// Child-side, and the reason this injection point is genuinely INSIDE the
/// window rather than merely near it: start a watcher thread that polls for
/// "<target>.pdfe_tmp" and calls announceInsideSaveWindow() the instant it
/// appears. The temp existing means pdf_save_document has created it and the
/// atomic rename has NOT yet happened — precisely the interval an OS shutdown
/// or a TerminateProcess is most destructive in. No production seam is needed
/// because that temp path is deterministic.
///
/// FULL-REWRITE SAVES ONLY. The staged INCREMENTAL append never creates
/// "<target>.pdfe_tmp" (it uses "<target>.pdfe_part.<CSPRNG hex>"), so this
/// watcher never fires for it and a test that relies on it there would hang out
/// its timeout rather than fail loudly. Use startAppendGrowthWatcher() instead.
///
/// The returned thread must be joined (or the process killed, which is the
/// expected outcome). Pass `stop` to make it exit when the save finished.
void startTempAppearanceWatcher(const std::filesystem::path& target);

// ---------------------------------------------------------------------------
// MID-APPEND process death  —  meta-audit follow-up F58, phase-52.
// ---------------------------------------------------------------------------
//
// THE LIMIT THIS INJECTOR EXISTS TO PASS.
//
// Everything above this line fails a save at an OPEN boundary.
// InPlaceWriteBlocker says so itself ("the failure lands at the OPEN rather
// than at a byte boundary"), and that limit is not cosmetic — it is the
// difference between the two cases the /DSS save path can be in:
//
//   * failure AT the open  → not one byte was written, so "the original is
//     unchanged" is the trivially expected outcome and holds for the unstaged
//     in-place append just as well as for a staged one. It discriminates
//     nothing about staging.
//   * failure MID-WRITE    → bytes are already on disk. An in-place append has
//     put them on the END OF THE USER'S ONLY COPY: a partial revision, a
//     broken trailing xref, and for a signed document a /ByteRange that no
//     longer resolves. A staged append has put them in the staging file, which
//     is thrown away. THIS is the case that tells the two designs apart, and
//     before F58 nothing in the harness could reach it.
//
// HOW IT REACHES MID-WRITE WITHOUT ADMINISTRATIVE PRIVILEGE.
// The honest limitation recorded at the top of this header still stands: a
// literal ENOSPC part-way through a write needs a volume that genuinely fills
// up, and every way to get one on Windows needs admin. So this injector uses
// the other real mid-write failure — the one the InsideWindow point already
// uses and which needs no privilege at all: a REAL TerminateProcess of a child
// that is genuinely part-way through the append. No unwinding, no destructors,
// no CRT flush. That is a power loss, and it is literal rather than simulated.
//
// HOW IT KNOWS IT IS MID-WRITE (and not merely near it).
// startTempAppearanceWatcher fires on the mere EXISTENCE of the staging file,
// which is useless here: the staged save CREATES it as a full-size byte copy of
// the original before appending a single byte, and the unstaged save never
// creates one at all. So this watcher triggers on GROWTH PAST A BASELINE
// instead — the size the target had before the save began:
//
//   size(target) > baseline                → an in-place append is part-way
//                                            through the user's file (the
//                                            pre-F58 shape);
//   a staging sibling > baseline           → a staged append is part-way
//                                            through the staging copy (the
//                                            post-F58 shape).
//
// Either condition means bytes of the new revision are committed to disk and
// the write has NOT finished. One watcher therefore covers both code paths
// with no branch on which one is compiled in, which is what makes the
// revert-and-observe control an apples-to-apples comparison.
//
// THE STAGED ARM CANNOT NAME THE FILE IT WAITS ON (F58-a, phase-53 — this
// header's text was stale for one phase and is corrected here). The staged
// incremental append no longer uses the fixed, predictable "<target>.pdfe_tmp";
// core::StagingFile draws the suffix from a CSPRNG
// ("<target>.pdfe_part.<32 hex>"), which is precisely the hardening that closed
// the CRITICAL staging-file-hijack defect. So the implementation ENUMERATES the
// target's directory and matches candidates with
// StagingFile::isStagingResidueName() rather than stat'ing a known path.
// "<target>.pdfe_tmp" is still checked as well, because Document::save()'s
// FULL-REWRITE branch does still use it — the two suffixes name two different
// mechanisms, and neither one superseded the other.
//
// PROVING IT FIRED. The condition is a genuine observation of on-disk state,
// so it cannot report a trigger that did not happen; and
// KilledSaveResult::childReachedWindow is only set when the child's marker
// file actually appears. A test MUST assert it — an injector that arms and
// then no-ops is the exact blindness DCA 0015 recorded against
// TempWriteBlocker on this path.
//
/// Child-side: start a detached watcher that announces the moment the append
/// has committed bytes beyond `baselineBytes` — to the target itself
/// (unstaged), to a "<target>.pdfe_part.<hex>" staging sibling found by
/// directory enumeration (staged incremental, F58-a), or to "<target>.pdfe_tmp"
/// (Document::save()'s full-rewrite temp, which still uses that fixed name).
/// Call it BEFORE starting the save, with the target's pre-save size.
void startAppendGrowthWatcher(const std::filesystem::path& target,
                              std::uintmax_t baselineBytes);

// ---------------------------------------------------------------------------
// Literal partial-temp staging — the on-disk residue a real ENOSPC leaves.
// ---------------------------------------------------------------------------
/// Write a genuinely TRUNCATED "<target>.pdfe_tmp" holding the first `bytes` of
/// a valid PDF: byte-for-byte the state a disk-full crash between the temp
/// write and the rename leaves behind. Returns the temp path.
std::filesystem::path stageTruncatedOrphanTemp(const std::filesystem::path& target,
                                               const std::filesystem::path& sourcePdf,
                                               std::uintmax_t bytes);

// ---------------------------------------------------------------------------
// STAGING-FILE OBSERVATION AND PERMISSION FLIPS  (Windows only)
// ---------------------------------------------------------------------------
//
// These exist so a test can make a save fail AFTER the staged bytes are
// published but BEFORE the document can be re-opened — the F58-f case, and the
// only way to drive LtvDssWriter's "published but re-open failed" branch
// (phase-53 review, finding 6). The technique is a legitimate concurrent
// permissions change, OBSERVED rather than timed: wait until a staging sibling
// has actually grown past the original's size, then deny the current user read
// on the target. commitOver() carries that DACL onto the staging file (F58-b)
// and publishes it, so the revision is on disk and correct — and
// fz_open_document() on it then fails with access denied, exactly as an A/V
// scanner or the search indexer produces on a just-renamed file.
//
// They live HERE, not in one test file, for the reason DCA 0015 records about
// InPlaceWriteBlocker: an injection technique open-coded in the file that first
// needed it is rediscovered (or, worse, re-derived slightly differently) by the
// next one. NOTE: tests/test_staged_save_hardening.cpp carries file-local
// copies that predate this extraction and should migrate to these.

/// Every staging sibling of `target` currently on disk: the fixed
/// "<target>.pdfe_tmp" (full-rewrite) plus any "<target>.pdfe_part.<hex>"
/// (staged incremental) found by enumerating the containing directory.
std::vector<std::filesystem::path> stagingResidueOf(
    const std::filesystem::path& target);

// ---------------------------------------------------------------------------
// DID THE SAVE PUBLISH BY RENAME, OR APPEND IN PLACE?  (DCA 0015, phase-10)
// ---------------------------------------------------------------------------
//
// WHY A RESIDUE CHECK CANNOT ANSWER THIS, which is the whole reason this exists.
// "No staging file was left behind" is satisfied two ways: the save staged and
// then cleaned up, or the save NEVER STAGED AT ALL. Those are the two branches
// a revert-and-observe has to tell apart, and the residue assertion scores them
// identically — which is exactly how DCA 0015's class-level test survived being
// pointed at a filename its own code path had stopped producing. Asserting the
// ABSENCE of litter can never establish the PRESENCE of the mechanism.
//
// A concurrent observer (poll for a staging sibling to appear mid-write) would
// answer it, but only by racing a window measured in milliseconds, and a
// control that flakes is worse than one that is merely weak — it teaches people
// to re-run it.
//
// File identity is the durable, race-free answer, read AFTER the save. A staged
// save publishes with MoveFileExW, so the target's directory entry ends up
// pointing at the STAGING file's index — a different one. An in-place
// incremental append writes through the existing entry and preserves it. So:
//
//     identity changed  → the save published by rename (staged)
//     identity same     → the save wrote in place (unstaged)
//
// SCOPE, stated because the opposite claim has been made in this repo and was
// wrong: this does NOT detect that a file's CONTENTS were overwritten, and must
// never be used for that. Identity survives an in-place overwrite by design.
// It answers one question — "did a rename publish this?" — and the /DSS write's
// staged-vs-unstaged shape is precisely that question.
//
// On non-Windows the identity is (st_dev, st_ino), which has the same property.

/// Opaque, comparable identity of a file on disk. Equality means "the same file
/// object", not "the same path" and not "the same bytes". Default-constructed
/// (or a failed read) compares unequal to everything INCLUDING itself, so a
/// test that fails to read an identity cannot silently pass a comparison.
struct FileIdentity {
    bool valid = false;
    unsigned long long volume = 0;
    unsigned long long index = 0;

    friend bool operator==(const FileIdentity& a, const FileIdentity& b)
    {
        return a.valid && b.valid && a.volume == b.volume && a.index == b.index;
    }
    friend bool operator!=(const FileIdentity& a, const FileIdentity& b)
    {
        return !(a == b);
    }
};

/// Identity of `p`, or an invalid FileIdentity if it cannot be read.
FileIdentity fileIdentityOf(const std::filesystem::path& p);

#if defined(_WIN32)
/// The SID of the user running the test, as a string. Empty on failure.
std::string currentUserSidString();

/// The DACL of `p` as an SDDL string — the same thing `icacls` renders, in a
/// form that can be compared for equality. Returns a `<...failed: N>` marker
/// rather than an empty string when the DACL cannot be read, so a test that
/// compares two snapshots cannot mistake "unreadable" for "unchanged".
std::string daclSddl(const std::filesystem::path& p);

/// Apply an SDDL DACL to `p`, PROTECTED (inheritance broken) — the state a user
/// reaches with `icacls <f> /inheritance:r /grant <me>:F`.
bool applyProtectedDacl(const std::filesystem::path& p, const std::wstring& sddl);

/// Deny the current user READ on `target` the moment a staging sibling has
/// grown past `baselineBytes`, then return. Runs on the CALLING thread, so
/// start it on a std::thread and join it. `firedOut` is set true iff the DACL
/// was actually applied — a test MUST assert it, or a run in which the flip
/// never happened is indistinguishable from one where the guard worked.
void denyReadOnTargetOnceStagingGrows(const std::filesystem::path& target,
                                      std::uintmax_t baselineBytes,
                                      const std::string& userSid,
                                      std::atomic<bool>& stop,
                                      std::atomic<bool>& firedOut);

/// Undo denyReadOnTargetOnceStagingGrows(): give the user full control back so
/// the temp directory can be cleaned up. Best-effort.
void restoreFullControl(const std::filesystem::path& p,
                        const std::string& userSid);
#endif  // _WIN32

// ---------------------------------------------------------------------------
// Shared oracles.
// ---------------------------------------------------------------------------
/// Read a whole file as bytes ("" when absent).
std::string readAllBytes(const std::filesystem::path& p);

/// A byte-for-byte comparison of a file against a previously captured snapshot.
/// This is THE data-loss oracle: "the save failed" is not enough — the user's
/// original must be unchanged down to the last byte.
bool fileIsByteIdenticalTo(const std::filesystem::path& p, const std::string& snapshot);

}  // namespace pdfe::testing
