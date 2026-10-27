// -*- coding: utf-8 -*-
#pragma once

// =============================================================================
// SaveSpacePrecheck — warn BEFORE a save that the volume cannot hold the result
// (GitHub #100 / meta-audit WIN-78).
// =============================================================================
//
// WHAT WAS WRONG
// --------------
// `grep -rl GetDiskFreeSpace src` returned zero files. Every save path stages a
// full copy of the document next to the target and then renames it over the
// original (AtomicFileReplace::StagingFile). That design is what makes an
// out-of-space save SAFE — `EnospcDuringTempWriteLeavesOriginalByteIdentical`
// proves the user's file survives byte-for-byte — but the user still learns
// about it as a thrown error after waiting through a multi-minute write.
//
// WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT
// ---------------------------------------------
// It is a WARNING, never a refusal. `warnIfInsufficientSpaceForSave` reports
// and returns; the save then proceeds exactly as before and fails, if it fails,
// through the existing hardened path.
//
// That is not timidity, it is a correctness requirement. `std::filesystem::space`
// is a point-in-time reading of a shared, racing resource: another process can
// free a gigabyte a millisecond later, a quota can differ from the volume
// figure, and a sparse/compressed/deduplicated target can need far less than
// the estimate. Turning any of those into "PDFE refused to save your document"
// converts a recoverable, already-safe failure into a lost edit. It would also
// short-circuit the very path
// `TempWriteBlockModes/TempWriteFailureTest.EnospcDuringTempWriteLeavesOriginalByteIdentical`
// exercises, which must keep reaching the temp write and keep proving the
// original is untouched.
//
// THE ESTIMATE
// ------------
// Output size is estimated as the SOURCE file's size. A PDF save is a rewrite
// of the same content — garbage collection shrinks it, added annotations and
// embedded fonts grow it — so the source size is the only figure available
// before serialisation and is the right order of magnitude. Because the staged
// copy lives beside the target while the original is still present, "free space
// below one source size" really is the point at which the operation is at risk.
//
// Deliberately no fudge factor: a multiplier would be a made-up number, and the
// message quotes both figures so the user can judge a marginal case themselves.

#include <cstdint>
#include <filesystem>
#include <functional>

namespace pdfe::core {

/// The outcome of one pre-check. `determined == false` means the question could
/// not be answered (unknown source size, unreadable volume) — callers must treat
/// that as "no opinion", never as "fine".
struct SaveSpaceEstimate {
    bool determined = false;
    bool insufficient = false;
    std::uintmax_t estimatedBytes = 0;
    std::uintmax_t freeBytes = 0;
};

/// Bytes the save is expected to produce, from the document currently on disk.
/// Returns 0 when the path is empty or unreadable (i.e. "no estimate").
std::uintmax_t estimateSaveOutputBytes(const std::filesystem::path& sourcePath) noexcept;

/// Free space AVAILABLE TO THIS USER on the volume holding `target`, compared
/// against `estimatedBytes`. Never throws; `determined` is false if either
/// figure is unavailable.
///
/// `std::filesystem::space().available` is the portable spelling of
/// GetDiskFreeSpaceExW's lpFreeBytesAvailableToCaller — the quota-aware figure,
/// which is the one that matters. `capacity`/`free` would over-report on a
/// quota'd share.
SaveSpaceEstimate checkFreeSpaceForSave(const std::filesystem::path& target,
                                        std::uintmax_t estimatedBytes) noexcept;

// -----------------------------------------------------------------------
// THE SEAM AT THE VOLUME BOUNDARY (GitHub #100, second half)
// -----------------------------------------------------------------------
//
// The pre-check above is unobservable on a healthy machine: there is always
// enough disk, so `insufficient` is never true, the handler never fires, and
// nothing downstream of it — the warning dialog most of all — is ever
// exercised. #100 was closed once on the pre-check alone and reopened for
// exactly that reason. A test that cannot make the volume look full can only
// assert that the arithmetic runs.
//
// So the ONE call to the OS is behind a replaceable probe. This is the
// `GetDiskFreeSpaceEx` boundary itself: `std::filesystem::space().available`
// is the portable spelling of `lpFreeBytesAvailableToCaller`, and the probe
// substitutes for that single reading and nothing else. Everything above it —
// the estimate, the comparison, the record, the handler dispatch, the GUI
// dialog — runs unmodified against the simulated figure, which is what makes
// the dialog assertion meaningful rather than a test of a test.
//
// It is deliberately NOT a way to fake `insufficient`: the probe reports a
// byte count, exactly as the OS does, and the comparison that decides is the
// production one.

/// Reports bytes available to the caller on the volume holding `probePath`.
/// Return false for "cannot answer" — the same contract the OS call has, and
/// the reason `SaveSpaceEstimate::determined` exists.
using FreeSpaceProbe =
    std::function<bool(const std::filesystem::path& probePath,
                       std::uintmax_t& availableBytesOut)>;

/// Install a probe used INSTEAD OF the OS reading. Passing an empty function
/// restores the real `std::filesystem::space`. Test-only; nothing in the
/// product installs one except the PDFE_TEST_PIPES-gated simulation pipe.
void setFreeSpaceProbeForTesting(FreeSpaceProbe probe);

/// True when a probe is installed. Lets a test prove its own simulation took
/// effect rather than inferring it from a green result.
bool freeSpaceProbeInstalled();

/// Where a warning goes. `target` is passed separately so a UI handler can name
/// the file; the default handler logs only the FILENAME (see the .cpp for why a
/// full path must not reach pdfe.log).
using SaveSpaceWarningHandler =
    std::function<void(const SaveSpaceEstimate&, const std::filesystem::path& target)>;

/// Install the process-wide handler (the GUI installs one that shows a dialog).
/// Passing an empty function restores the default log-only handler.
void setSaveSpaceWarningHandler(SaveSpaceWarningHandler handler);

/// Run the pre-check for `target` with an explicit estimate, and fire the
/// handler if free space is below it. Returns what it found. Never throws,
/// never blocks the save.
SaveSpaceEstimate warnIfInsufficientSpaceForSave(const std::filesystem::path& target,
                                                 std::uintmax_t estimatedBytes) noexcept;

/// As above, taking the estimate from the document currently at `sourcePath`.
SaveSpaceEstimate warnIfInsufficientSpaceForSave(const std::filesystem::path& target,
                                                 const std::filesystem::path& sourcePath) noexcept;

/// The most recent pre-check performed anywhere in the process.
///
/// This exists so the WIRING is assertable. Whether the check RUNS before a
/// save cannot be observed from its result on a healthy machine — there is
/// always enough disk, so nothing is warned about and the save behaves
/// identically either way. A test that only exercised checkFreeSpaceForSave()
/// directly would keep passing after someone deleted the call from
/// Document::save, which is the regression that matters.
struct SaveSpacePrecheckRecord {
    /// Increments on every pre-check. 0 means none has run.
    unsigned long long sequence = 0;
    std::filesystem::path target;
    SaveSpaceEstimate estimate;
};

SaveSpacePrecheckRecord lastSaveSpacePrecheck();
void resetSaveSpacePrecheckRecord();

}  // namespace pdfe::core
