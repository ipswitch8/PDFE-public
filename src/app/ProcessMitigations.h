#pragma once

namespace pdfe::app {

/// Apply Windows process mitigation policies at startup.
///
/// Each policy is best-effort: per-policy failures log a Warning via
/// the Qt logging handler and the function continues. None of the
/// policies abort startup. On non-Windows builds this is a no-op.
///
/// What is applied:
///   * DataExecutionPrevention — DEP, with disable-thunk-emulation
///     and permanent flag (already on by default for x64 binaries
///     but pinning makes it explicit + irrevocable per process).
///   * MandatoryASLR + BottomUpASLR — relocate images even if no
///     /DYNAMICBASE flag, and randomize bottom-up allocations.
///   * HighEntropyASLR — 64-bit-wide ASLR randomization (more bits
///     of entropy on x64, harder to brute-force).
///   * ImageLoadPolicy: PreferSystem32 + NoRemoteImages + NoLowLabel.
///     Refuses to load DLLs from a remote share or a low-IL location,
///     and prefers System32 over the application directory for any
///     ambiguous load. Closes the most common DLL-search-path attacks.
///   * StrictHandleCheckPolicy — abort on invalid HANDLE use, which
///     surfaces use-after-close bugs as crashes instead of silent
///     UB. Cheap and safe.
///
/// What is INTENTIONALLY NOT applied (and why):
///   * MicrosoftSignedOnly (BinarySignaturePolicy) — would block
///     PDFE's own non-Microsoft-signed binary from loading its own
///     plugins; useless for self-defense.
///   * ProhibitDynamicCode (DynamicCodePolicy) — incompatible with
///     Qt6's QML/JS engines and with MuJS (the JS engine bundled in
///     MuPDF for PDF form scripts when JS is opted-in via
///     AppSettings).
///   * BlockNonMicrosoftBinaries — same reason as MicrosoftSignedOnly.
///   * StrictControlFlowGuard — only applies to processes compiled
///     with /guard:cf and is automatically active when that flag is
///     set; querying GetProcessMitigationPolicy lets us *log* whether
///     CFG is on but applying it again is a no-op.
///
/// CFG status is queried via GetProcessMitigationPolicy and the result
/// logged so we have evidence in the logs that /guard:cf is active.
///
/// Call this from main() before QApplication's event loop starts,
/// preferably before QApplication is even constructed (some policies
/// like ImageLoadPolicy:NoRemoteImages should be active before Qt
/// starts probing for plugins).
void applyProcessMitigations() noexcept;

} // namespace pdfe::app
