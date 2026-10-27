// -*- coding: utf-8 -*-
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

// =============================================================================
// TempFileSafety — the temp files that are NOT documents, and are still attack
// surface (WIN-87 wider class, phase-60).
// =============================================================================
//
// AtomicFileReplace/StagingFile covers the PUBLISH paths: a finished document
// that must land on the user's chosen path atomically. This unit covers the
// other half of the same class, which the WIN-87 enumeration did not reach
// because none of these paths ends in a rename:
//
//   * a WORKING file that a by-name producer writes and something else later
//     re-reads BY PATH (clipboard image paste, image-to-PDF wrapping in the
//     combine service). The re-read is the vulnerable moment: whatever is at
//     that path at that instant is what enters the user's document.
//   * a FIXED-NAME %TEMP% write, which is not a substitution target but a
//     PLANT-AND-REDIRECT target: an attacker who pre-creates the name as a
//     hardlink or a reparse point makes PDFE's own write land somewhere it
//     never intended, under PDFE's token.
//
// The two need different answers, and the difference is worth stating because
// applying the wrong one is how a hardened path stays exploitable:
//
//   UNPREDICTABLE names defeat AIMING. They do nothing about a name the
//   attacker does not have to guess, and nothing about a file that is already
//   there.
//   HARDENED OPENS defeat PLANTING. They do nothing about a name the attacker
//   can predict and race AFTER the write.
//
// PinnedTempFile below is the first answer plus EXCLUSIVE POSSESSION, because
// for these paths unpredictability alone leaves a real interval between
// "producer finished writing" and "consumer reads it". It closes that interval
// by never opening one — see the class comment for why an earlier design that
// merely WATCHED the interval was rejected.
//
namespace pdfe::core {

/// 128 bits from the system CSPRNG rendered as 32 lowercase hex digits.
///
/// THE SINGLE IMPLEMENTATION IN THE PRODUCT. `BCryptGenRandom` with
/// `BCRYPT_USE_SYSTEM_PREFERRED_RNG`, never `std::random_device`: on some
/// MSVC/MinGW configurations random_device is a deterministic PRNG, and a
/// predictable staging name IS the defect this whole family exists to close.
/// AtomicFileReplace.cpp delegates here rather than keeping a second copy —
/// two implementations of a security primitive is two chances to drift, and
/// only one of them gets reviewed.
///
/// Returns false (and leaves `out` unspecified) if the CSPRNG failed. Every
/// caller must FAIL CLOSED on false; there is no acceptable fallback, because
/// the fallback would be exactly the predictable name.
bool csprngHex128(std::wstring& out) noexcept;
bool csprngHex128(std::string& out) noexcept;

/// The per-process diagnostics directory: `%TEMP%/pdfe_diag.<32 hex>/`.
///
/// Created ONCE per process, exclusively (CreateDirectoryW fails if the name
/// is taken, and the name is 128 CSPRNG bits, so success means this process
/// created it). Every diagnostic dump in the TRE — glyph atlases, font-match
/// traces, ranking logs — writes inside here instead of at a fixed, guessable
/// path, which makes all of them uninteresting to plant-and-redirect at once
/// rather than one hardened call site at a time.
///
/// Returns an EMPTY path if the directory could not be created. That is not an
/// error to report to the user and not a reason to fall back to a fixed name:
/// callers skip the diagnostic write. A debug log is never worth an arbitrary
/// file write.
const std::filesystem::path& diagnosticsDir() noexcept;

/// `diagnosticsDir()/name`, created on demand.
///
/// The subsystems that dump diagnostics used to each pick their OWN fixed
/// `%TEMP%` name ("pdfe_glyph_debug", "pdfe_cnn_debug", "pdfe_match_diag.txt",
/// and one that was not in %TEMP% at all — GlyphAtlas wrote "debug_log.txt"
/// relative to the PROCESS CWD, which for a shell-launched PDFE is whatever
/// directory the user happened to be in). Nesting all of them inside one
/// unpredictable per-process root makes every one of them uninteresting to
/// plant-and-redirect at once, rather than hardening call sites one at a time
/// and missing the next one somebody adds.
///
/// Returns an EMPTY path if unavailable; callers skip the diagnostic write.
/// `name` must be a single path component with no separators.
std::filesystem::path diagnosticsSubdir(std::string_view name) noexcept;

/// Write `bytes` to a FIXED — therefore guessable — temp path, refusing to
/// write through anything an attacker could have planted there first.
///
/// WHY A FIXED NAME SURVIVES AT ALL. `%TEMP%/pdfe_ui_status.txt` is the
/// sentinel eight Python UI tests poll by name; randomising it would retire
/// those tests rather than harden anything. So the name stays and the OPEN is
/// hardened instead:
///
///   * `FILE_FLAG_OPEN_REPARSE_POINT` — opens the reparse point ITSELF rather
///     than following it, so a planted symlink/junction cannot redirect the
///     write to another volume or another user's file.
///   * `nNumberOfLinks > 1` is refused — a hardlink planted at this name is an
///     alias for a victim file, and truncating it destroys that file's
///     contents under PDFE's token.
///
/// Returns false with `ec` set on refusal or failure. A refusal is a security
/// event, not a transient error: the caller writes nothing and carries on.
bool writeFixedNameTempFile(const std::filesystem::path& path,
                            std::string_view bytes,
                            bool append,
                            std::error_code& ec) noexcept;

/// A working file with an UNPREDICTABLE name that is EXCLUSIVELY HELD from the
/// moment it is created until this object dies — so nothing can substitute it
/// before, during, or after the producer writes it.
///
/// THE SHAPE THIS EXISTS FOR. Clipboard image paste materialises the clipboard
/// to a PNG, and `InsertImageCommand::redo()` re-reads that PNG BY PATH — on
/// the original insert and again after every undo/redo, arbitrarily far in the
/// future. A predictable name (the old code used a counter that restarted at 0
/// each process) means an attacker who loses the first race simply waits for
/// the user to press Ctrl+Z, Ctrl+Y. Unpredictability alone does not close
/// that, because the path is on disk for the rest of the session.
///
/// WHY THERE IS NO release-then-verify STEP, AND WHY THERE USED TO BE.
/// The first version of this class reserved the name with `CREATE_NEW`, CLOSED
/// the handle so a by-name producer (`QImage::save`, `QPdfWriter`) could open
/// the path itself, and then re-opened and compared the volume serial + file
/// index to detect substitution. **The phase-7 `karen` gate rejected that, and
/// was right to.** A file-IDENTITY check does not see an attacker who opens the
/// SAME file object and overwrites it IN PLACE — which is precisely the
/// operation the legitimate producer performs, so it could not have been
/// distinguished. The window was real and the guard could not close it; worse,
/// release-then-re-acquire is structurally the fallback shape phase-55 deleted
/// under F62 with a "do not add them back" tombstone.
///
/// So the window is REMOVED rather than watched. The handle is opened
/// `GENERIC_READ | GENERIC_WRITE` with `FILE_SHARE_READ` and never let go, and
/// producers write THROUGH it via `openWriteFd()`. Three properties, all
/// load-bearing and none of them probabilistic:
///
///   1. UNPREDICTABLE — 128 CSPRNG bits, so the name cannot be pre-planted or
///      aimed at.
///   2. CREATED BY US — `CREATE_NEW`, so success proves nothing was already
///      sitting at that name. A plain open would happily adopt an attacker's
///      file.
///   3. HELD — for the object's whole lifetime there is no writer, no deleter
///      and no renamer other than us. Withholding `FILE_SHARE_DELETE` is what
///      stops a rename-over, not just a delete. `FILE_SHARE_READ` is granted
///      deliberately, because the consumer re-reads the path and a pin that
///      locked out readers would break the feature it protects.
class PinnedTempFile {
public:
    /// Create `dir`/`prefix`.`<32 hex>``extension`, exclusively, and HOLD it.
    ///
    /// `extension` must include its dot (".png"), or be empty.
    static std::unique_ptr<PinnedTempFile> reserve(
        const std::filesystem::path& dir,
        std::string_view prefix,
        std::string_view extension,
        std::error_code& ec) noexcept;

    /// A CRT file descriptor over a DUPLICATE of the held handle, positioned at
    /// offset 0, for a producer that needs a stream rather than a path.
    ///
    /// The caller OWNS the descriptor and must close it (`QFile::open` with
    /// `QFileDevice::AutoCloseHandle` does; so does `_close`). Closing it does
    /// NOT release this object's handle, so the exclusive window survives the
    /// write — the same duplicate-handle pattern `StagingFile::openDuplicateStream`
    /// uses for MuPDF, and for the same reason.
    ///
    /// Returns -1 with `ec` set on failure.
    int openWriteFd(std::error_code& ec) noexcept;

    /// True once the file is created and held. False only after a failed
    /// reserve, which returns nullptr anyway — kept for callers that want to
    /// assert the invariant.
    bool isPinned() const noexcept;

    /// The held path. Safe to hand to a by-path CONSUMER at any time: readers
    /// are permitted, writers and deleters are not.
    const std::filesystem::path& path() const noexcept { return path_; }

    /// Releases the handle and DELETES the file.
    ~PinnedTempFile();

    PinnedTempFile(const PinnedTempFile&) = delete;
    PinnedTempFile& operator=(const PinnedTempFile&) = delete;

private:
    PinnedTempFile() = default;

    std::filesystem::path path_;
    void* handle_ = nullptr;  // HANDLE; nullptr = not held
};

/// Creates `path` (and any missing parent directories) with a DACL granting
/// FULL CONTROL to the calling user's SID and to nobody else — no Users, no
/// Everyone, no inherited entries. Added for SEC-103.
///
/// The reason this is not "create the file and set permissions afterwards":
/// between those two steps the file exists with the directory's inherited
/// ACL, and on a shared machine that window is the whole vulnerability. The
/// DACL is supplied at creation, in the SECURITY_ATTRIBUTES, so there is no
/// moment at which the file is readable by anyone else.
///
/// Returns true if the file exists afterwards with that DACL — INCLUDING the
/// case where it already existed, in which case its DACL is verified and the
/// call FAILS if the file is reachable by anyone but the owner. A cache that
/// silently adopts a pre-planted world-writable file is the surface this
/// function exists to remove, so adoption has to be a refusal, not a shrug.
///
/// On non-Windows this creates the file with 0600 and reports accordingly.
bool createOwnerOnlyFile(const std::filesystem::path& path,
                         std::error_code& ec) noexcept;

/// Why `createOwnerOnlyDirectory` would refuse an EXISTING path — decided
/// without modifying anything.
///
/// Split out from the create/secure call deliberately. The decision can then be
/// asserted against real system state (a directory genuinely owned by another
/// principal) with no risk that a future edit — or a mutation-testing harness,
/// which exists precisely to break the guard — turns the test into a write
/// against that directory. A read-only predicate is safe to point anywhere.
enum class DirAdoptionVerdict {
    Ok,             ///< exists, is a plain directory, and we own it
    DoesNotExist,   ///< nothing there; the caller should create it
    NotADirectory,  ///< a file (or other object) occupies the name
    ReparsePoint,   ///< a junction/symlink — securing it would secure the target
    ForeignOwner,   ///< owned by another principal, who can rewrite the DACL
    Indeterminate,  ///< the owner could not be read; treated as a refusal
};

/// Classify `path` for adoption without touching it. Never writes.
DirAdoptionVerdict inspectDirectoryForAdoption(
    const std::filesystem::path& path) noexcept;

/// Creates `path` as a directory whose DACL grants full control to the calling
/// user and to nobody else, with inheritance BROKEN so the parent's entries do
/// not leak in and with the ACE marked inheritable so anything created inside
/// is owner-only from the moment it exists.
///
/// Added for the plaintext search index (phase-17/70). That index writes every
/// page's extracted text into a directory under %TEMP%, which by default is
/// readable by any process running as the same user — a plaintext-searchable
/// copy of a document's content sitting outside whatever container the document
/// came in.
///
/// The DACL is supplied at creation, not applied afterwards, for the reason
/// given on createOwnerOnlyFile: between create and chmod the directory exists
/// with the inherited ACL, and that window is the exposure.
///
/// An EXISTING directory is adopted and re-secured, but ONLY if the current
/// user (or SYSTEM/Administrators) owns it. Re-applying a DACL to a directory
/// somebody else created is theatre: an owner holds WRITE_DAC implicitly and
/// can undo it at will. A foreign-owned directory, a reparse point, or a
/// non-directory at that name is refused outright.
///
/// Returns true only after the resulting ACL has been READ BACK and verified;
/// a successful return from the setting call is not accepted as evidence.
/// Callers must treat false as "do not write here", not as a warning.
bool createOwnerOnlyDirectory(const std::filesystem::path& path,
                              std::error_code& ec) noexcept;

/// True if `path` exists and its DACL grants access to nobody but the calling
/// user (and SYSTEM/Administrators, which cannot meaningfully be excluded and
/// already own the machine). Used by createOwnerOnlyFile and directly by
/// tests, which is why it is public: an assertion that reads the real ACL is
/// worth more than one that trusts the creating call's return value.
bool isOwnerOnlyAccessible(const std::filesystem::path& path) noexcept;

/// Create `path` and write `bytes` to it, failing if ANYTHING already exists
/// at that name. For extracting document-controlled content into a directory
/// the user chose (attachment extraction, phase-71).
///
/// THE SHAPE THIS REPLACES. The caller used to pick a free name with
/// `std::filesystem::exists()` and then open it with an ordinary truncating
/// `ofstream`. Between those two steps anything with write access to the
/// directory can place an object at that name — a junction pointing elsewhere,
/// a hardlink aliasing a victim file, or just a file — and the open adopts it,
/// writing document-controlled bytes through the attacker's choice of target
/// under PDFE's token. Phase-70 was gate-failed for this exact shape in
/// `src/search`; this is the same defect one component over, which is why the
/// fix here is the same discipline rather than a narrower mitigation.
///
///   * `CREATE_NEW` — there is no check-then-act left to race. The create IS
///     the existence test, performed by the kernel atomically, and success
///     proves nothing was there and that WE made it. A caller wanting
///     "name (1).txt" retries on `std::errc::file_exists` rather than asking
///     `exists()` first.
///   * `FILE_FLAG_OPEN_REPARSE_POINT` plus an explicit
///     `FILE_ATTRIBUTE_REPARSE_POINT` refusal — the flag governs how the object
///     is opened, not whether we are willing to write to it. Kept as a pair for
///     the reason measured in `writeFixedNameTempFile`, where the flag alone
///     did not hold.
///   * `nNumberOfLinks > 1` refused. `CREATE_NEW` makes this close to
///     unreachable, and it is checked anyway: the cost is one call and the
///     failure it guards against is destroying a user's file.
///
/// Nothing is left behind on refusal or on a partial write — a truncated file
/// at the target name is worse than either outcome, because it looks like a
/// successful extraction.
///
/// Returns false with `ec` set. `ec == std::errc::file_exists` specifically
/// means the name was taken and the caller may try another.
bool writeNewFileExclusive(const std::filesystem::path& path,
                           const void* data,
                           std::size_t len,
                           std::error_code& ec) noexcept;

/// `%LOCALAPPDATA%\PDFE\` — the per-user, non-roaming location for caches and
/// other derived state that must not be shared between accounts. Empty if the
/// environment does not name one, which callers must treat as "no on-disk
/// cache", never as "use the current directory".
std::filesystem::path perUserDataDir() noexcept;

}  // namespace pdfe::core
