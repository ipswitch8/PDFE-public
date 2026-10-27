// -*- coding: utf-8 -*-
#pragma once

#include <filesystem>
#include <optional>

namespace pdfe::core {

/// Out-of-process crash-isolation check for opening a document.
///
/// Spawns the `pdfe_preflight` helper (located next to the running
/// executable) as a child process that attempts to open `file` with MuPDF.
/// A hostile document can drive MuPDF's parser/repair into unbounded
/// (effectively cyclic) recursion that exhausts the stack — an
/// unrecoverable crash that cannot be safely trapped in-process. By
/// attempting the open in a disposable child first, such a crash terminates
/// only the helper; this function observes the abnormal exit and returns
/// false, so the caller can refuse the document while staying healthy.
///
/// Returns true ONLY when the helper opened and walked the document cleanly
/// (exit code 0). Any other outcome — an ordinary MuPDF "malformed file"
/// error, an abnormal/crash termination, or a timeout — returns false, on
/// the conservative principle that a document the isolated opener could not
/// cleanly handle should not be opened in-process.
///
/// `timeoutMs` bounds a document that hangs the helper (e.g. a pathological
/// loop) rather than crashing it.
///
/// Failure modes distinguish two cases:
///   * If the running executable's own directory CANNOT be resolved (so we
///     have no idea where the helper would live), this returns false —
///     fail CLOSED. Refusing to open the document is the safe outcome: an
///     unresolvable executable location means the sandbox is effectively
///     disabled, and silently treating every document as safe would defeat
///     the security control. This case previously fail-opened on macOS
///     because executableDir() had no Apple branch (it does now).
///   * If the directory resolves but the helper binary is simply absent
///     there (a deployment/packaging gap), this returns true — fail OPEN —
///     so a missing helper does not make the app unable to open any file;
///     the in-process open then applies its normal error handling.
bool isDocumentOpenSafe(const std::filesystem::path& file, int timeoutMs = 15000);

/// How much the last helper launch was actually restricted (GitHub #59,
/// SEC-61).
///
/// The helper parses attacker-controlled bytes, so it is created with a
/// LOW-INTEGRITY primary token: a low-integrity process cannot write to
/// medium-integrity objects (the user's profile, the registry hives the app
/// uses, or the parent process), so a code-execution bug in MuPDF's parser is
/// contained rather than merely relocated to another process.
///
/// This exists because the degradation would otherwise be INVISIBLE. If token
/// duplication or `CreateProcessAsUser` fails, the code falls back to an
/// ordinary launch — the helper still provides crash isolation, so failing the
/// open would be a worse trade — but that fallback silently removes a security
/// property. An effect that matters and cannot be observed cannot be tested
/// or audited, so it is reported here instead.
///
/// Values are per-process and reflect the MOST RECENT launch only.
enum class HelperIsolation {
    NotLaunched,    ///< no helper launch has happened in this process
    LowIntegrity,   ///< created with a low-integrity primary token (intended)
    Unrestricted    ///< fell back to an ordinary launch (degraded)
};

/// Isolation actually achieved by the most recent helper launch.
/// Always `NotLaunched` on non-Windows, where no such token model exists.
HelperIsolation lastHelperIsolation();

namespace detail {

/// Test seam. Overrides the directory that isDocumentOpenSafe() treats as the
/// running executable's location (where it looks for the pdfe_preflight
/// helper). Engaging it with an EMPTY path simulates an UNRESOLVABLE
/// executable location, exercising the fail-closed branch on any host —
/// including hosts where the real resolution always succeeds. Passing
/// std::nullopt restores normal OS-based resolution. For tests only; not part
/// of the shipping contract.
void setExecutableDirForTesting(std::optional<std::filesystem::path> dir);

} // namespace detail

} // namespace pdfe::core
