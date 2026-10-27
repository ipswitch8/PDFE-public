// -*- coding: utf-8 -*-
#pragma once

// Shared atomic file-replacement helper (meta-audit follow-up F10, WIN-84/WIN-85).
//
// WHY THIS EXISTS
// ---------------
// Document::save() has always had the CORRECT replace-the-original-atomically
// design (MoveFileExW + MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH, plus a
// bounded retry loop that tolerates a transient A/V or indexer handle on the
// destination). Four other rename sites in the product did NOT:
//
//   src/batch/BatchEngine.cpp        — throwing std::filesystem::rename, no retry
//   src/exports/ImageExporter.cpp    — rename, then a DESTRUCTIVE remove+rename
//                                      fallback (deletes the user's existing file
//                                      and can then fail → data loss)
//   src/redaction/RedactionManager.cpp — throwing std::filesystem::rename, no retry
//   src/core/Document.cpp (POSIX arm) — bare std::filesystem::rename, no retry
//
// WIN-85's remediation is to factor the one good implementation into a shared
// helper and route every site through it. This header is that helper.
//
// CONTRACT
// --------
//  * On success `to` becomes `from`; `from` no longer exists.
//  * On ANY failure `to` is left COMPLETELY UNTOUCHED — there is no window in
//    which the destination is absent or truncated. The destination is never
//    removed first.
//  * Failure is reported through `ec`; nothing throws.
//  * (phase-53, F58-b HIGH) When `to` already exists, its DACL is carried onto
//    the incoming file BEFORE the rename, so publishing a staged copy can never
//    WIDEN the permissions of the file it replaces. See preserveDestinationDacl.
//
// STAGING (phase-53, F58-a CRITICAL)
// ----------------------------------
// `StagingFile` is the other half: it creates the file that will later be
// published, at an UNPREDICTABLE name, with an exclusive handle held for the
// entire window between creation and publication. Before it existed, every
// stage-and-rename site created its temp at a fully predictable path with no
// handle held across the write→rename gap, so an unprivileged process with
// write access to the containing directory could stamp bytes into the staged
// file and have PDFE atomically publish them over the user's document — an
// attack that was demonstrated end-to-end against Document::saveIncrementalStaged.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>

namespace pdfe::core {

/// Single-shot atomic replace. Windows: MoveFileExW with MOVEFILE_REPLACE_EXISTING
/// (single directory operation — the original is swapped in place, never
/// deleted-then-recreated) plus MOVEFILE_WRITE_THROUGH (the metadata change is
/// flushed before returning). POSIX: rename(), which atomically replaces an
/// existing destination on the same filesystem.
bool atomicReplaceFile(const std::filesystem::path& from,
                       const std::filesystem::path& to,
                       std::error_code& ec) noexcept;

// DELETED, NOT MERELY UNUSED — `atomicReplaceFileWithRetry(from, to, ...)`,
// WIN-87 phase-58.
//
// This was the shared publish helper WIN-85 migrated seven ad-hoc
// remove()+rename() sites onto. It closed the DATA-LOSS hazard at all seven and
// left the SUBSTITUTION hazard open at all seven, for one reason: IT TAKES
// PATHS. A caller holding no handle re-resolves the staging name at rename
// time, so anything that can win the race between the last write and the rename
// gets its bytes published as the user's document. Phases 56-58 migrated all
// seven sites onto StagingFile, and this overload's last caller went with them.
//
// It is REMOVED rather than left in place for a future caller, because "already
// hardened" was precisely why nobody looked at those seven sites again for a
// year. An unused-but-present helper is one #include away from re-creating the
// class. If you are about to reintroduce it: the thing you want is
// StagingFile::createBeside() + commitOverWithRetry(), which carries the SAME
// bounded 10 x 100 ms anti-A/V calibration and never resolves the name twice.
//
// The retry CONTRACT it documented (clamp attempts to >= 1, no sleep on the
// happy path, survives a transient destination hold, fails safe on exhaustion
// leaving the destination intact) is unchanged and still tested — the four cases
// in tests/test_core_atomic_file_replace.cpp were re-pointed onto
// commitOverWithRetry rather than deleted with the function, because the
// behaviour did not go with it.
//
// `atomicReplaceFile()` (single attempt, no retry) SURVIVES: StagingFile's own
// POSIX arm uses it, and it is not a publish-by-path API on Windows.

// ---------------------------------------------------------------------------
// DACL preservation — meta-audit follow-up F58-b (HIGH), phase-53.
// ---------------------------------------------------------------------------
//
// THE DEFECT. std::filesystem::copy_file is CopyFileW, which does NOT copy the
// security descriptor: the staged copy is a NEW file and therefore receives the
// CONTAINING DIRECTORY's inheritable ACEs. MoveFileExW then carries the source's
// DACL to the destination. So a signed document locked to owner-only, sitting in
// a folder that grants `Users:(OI)(CI)(M)`, came out of ONE staged save as
// `Users:(I)(M)` — permanently, and silently. Measured, not theorised:
//
//     === ORIGINAL DACL ===   EXAMPLECORP\dev:(F)
//     === PUBLISHED DACL ===  BUILTIN\Users:(I)(M)
//
// It also downgraded the staging-file attack above from "needs directory write"
// to "needs nothing at all, on the second save".
//
// THE FIX. Before the rename, read the DESTINATION's DACL and re-apply it to the
// incoming file:
//
//   * destination DACL is PROTECTED (inheritance broken — which is exactly how a
//     file becomes owner-only inside a permissive folder): re-apply the whole
//     DACL to `from` with PROTECTED_DACL_SECURITY_INFORMATION, reproducing the
//     effective permissions exactly.
//   * destination DACL is UNPROTECTED (it inherits): the inherited ACEs come
//     from the same directory the staging file is in, so they are already
//     identical. Only the destination's EXPLICIT (non-inherited) ACEs are copied
//     across, with UNPROTECTED_DACL_SECURITY_INFORMATION, so the result is
//     explicit-plus-inherited exactly as before and future directory ACL changes
//     still propagate.
//
// FAILURE POLICY — FAIL CLOSED, with one principled exemption. If the
// destination's DACL cannot be read, or the copy onto `from` fails, the replace
// is ABORTED and reported through `ec`. Publishing anyway would be a silent
// permission widening, and aborting preserves the helper's headline contract
// (the destination is left completely untouched). The single exemption is a
// filesystem with no security model at all — GetNamedSecurityInfoW returning
// ERROR_NOT_SUPPORTED / ERROR_INVALID_FUNCTION / ERROR_ACCESS_DENIED-on-FAT,
// i.e. FAT/exFAT removable media — where there is no DACL to widen and no way to
// express one; there the replace proceeds. On POSIX this is a no-op: rename()
// preserves the source inode's mode, and the caller (StagingFile) already
// chmod()s the staged file to the destination's mode.
//
/// Carry `to`'s DACL onto `from`. Returns true when the permissions of the
/// eventual published file are guaranteed no wider than `to`'s are now — which
/// includes the "`to` does not exist" and "no security model on this volume"
/// cases. Returns false, with `ec` set, when that guarantee cannot be made.
bool preserveDestinationDacl(const std::filesystem::path& from,
                             const std::filesystem::path& to,
                             std::error_code& ec) noexcept;

// ---------------------------------------------------------------------------
// StagingFile — meta-audit follow-up F58-a (CRITICAL), phase-53.
// ---------------------------------------------------------------------------
//
// THE EXPLOIT THIS CLOSES. Document::saveIncrementalStaged() staged at the fully
// predictable path "<target>.pdfe_tmp", created it with CopyFileW (no CREATE_NEW,
// no restrictive share mode) and held NO handle on it between the append
// finishing and the rename. A separate unprivileged process polled for that path
// and stamped 64 bytes over offset 0 — a region MuPDF had already passed, since
// an incremental append seeks to EOF. PDFE then atomically published the
// attacker's bytes over the user's signed document and reported SUCCESS. The
// published file was no longer even a PDF.
//
// THE THREE PROPERTIES THAT CLOSE IT, all of which this class provides:
//
//   1. UNPREDICTABLE NAME. "<target>.pdfe_part.<32 hex>" — 128 bits from
//      BCryptGenRandom (BCRYPT_USE_SYSTEM_PREFERRED_RNG), i.e. a CSPRNG, not a
//      std::random_device-seeded PRNG. An attacker cannot poll for a path it
//      cannot name.
//   2. CREATE_NEW. If the name somehow IS guessed and pre-created, creation
//      fails and the save aborts with the original intact — it never adopts a
//      file it did not create. This also defeats a pre-planted reparse point,
//      which CREATE_NEW rejects as an existing name.
//   3. AN EXCLUSIVE HANDLE HELD FOR THE ENTIRE WINDOW. dwShareMode is
//      FILE_SHARE_READ: no other opener can obtain write or delete access from
//      creation until publication, and publication happens THROUGH that same
//      handle (SetFileInformationByHandle / FileRenameInfoEx), so there is no
//      instant at which the file is nameable-and-writable by anyone else.
//
//      FILE_SHARE_READ rather than 0 is deliberate: denying FILE_READ_ATTRIBUTES
//      breaks size-polling watchers, A/V and backup agents for no integrity
//      gain. Write and delete — the only access the demonstrated attack needed
//      — are denied either way.
//
//      THE OLD JUSTIFICATION FOR THAT WAS WRONG, AND IT IS WORTH KEEPING THE
//      CORRECTION VISIBLE (F71). This block used to say readers are harmless
//      "(the staged bytes are a copy of a file the reader could already read)".
//      That does not follow. Until F71 the staging file was created with NO
//      security descriptor, so it wore the CONTAINING DIRECTORY's inherited
//      ACEs — not the destination's. For an owner-only document inside a
//      folder granting Users:(OI)(CI)(M), the reader emphatically could NOT
//      already read it, and a full plaintext copy sat there readable for the
//      whole append. The premise was the error, not the conclusion.
//
//      What makes readers harmless NOW is that both factories apply the
//      DESTINATION's DACL immediately after CreateFileW, before any bytes are
//      written, so "whoever may read the staging file" is by construction
//      "whoever may read the destination". FILE_SHARE_READ is then genuinely
//      a no-op on confidentiality. Do not restore the old parenthetical.
//
// THE SUFFIX IS ALSO A CONTRACT (F58-d). ".pdfe_part.<hex>" is deliberately NOT
// ".pdfe_tmp". A ".pdfe_tmp" is a COMPLETE pdf_save_document-finished file that
// never got renamed — the R2-04 crash-recovery artifact Document's constructor
// surfaces. A ".pdfe_part.<hex>" is a POSSIBLY-TRUNCATED incremental append and
// must never be offered to a user as a recoverable document.
class StagingFile {
public:
    /// Suffix marker for staged-append temps. Recovery code must treat a file
    /// whose name is "<something>" + this + 32 hex digits as internal residue,
    /// never as a recoverable document.
    static constexpr const char* kSuffixMarker = ".pdfe_part.";

    /// True when `name` is a staging residue of `targetFileName` — i.e. exactly
    /// "<targetFileName>.pdfe_part.<32 hex digits>".
    static bool isStagingResidueName(const std::filesystem::path& targetFileName,
                                     const std::filesystem::path& name) noexcept;

    /// An UNPREDICTABLE sibling PATH for `target`, of exactly the shape
    /// `isStagingResidueName` recognises, WITHOUT creating or holding anything.
    ///
    /// WHY A NAME-ONLY FORM EXISTS AT ALL, since holding the handle is the whole
    /// point of this class: some producers cannot be handed a handle. The batch
    /// engine chains actions that each open their OUTPUT BY NAME (Document::saveAs,
    /// QPDF), so a `createBeside` handle would lock them out of the file they are
    /// supposed to write — the self-inflicted-lock class this codebase has already
    /// hit three times. For those producers the achievable property is
    /// UNPREDICTABILITY, not exclusivity: an attacker cannot target 128 bits of
    /// CSPRNG output, even though the artifact is briefly unheld.
    ///
    /// THIS IS WEAKER THAN `createBeside` AND MUST NOT BE USED WHERE THAT WOULD
    /// WORK. If you can write through a handle, do; the interval this leaves open
    /// is real, just unaimable. Callers must also publish through a StagingFile,
    /// not by handing this path to a by-path replace.
    static std::filesystem::path unpredictableSiblingPath(
        const std::filesystem::path& target, std::error_code& ec) noexcept;

    /// Create a new staging file beside `target` (same directory ⇒ same volume
    /// ⇒ the eventual rename really is a single atomic directory operation).
    /// Returns nullptr with `ec` set on failure.
    ///
    /// (This comment was ORPHANED by phase-58, which inserted
    /// `unpredictableSiblingPath` between it and the declaration it describes —
    /// so the STRONGER factory read as undocumented while the deliberately
    /// weaker one appeared to promise "creates a staging file". Restored
    /// phase-59.)
    static std::unique_ptr<StagingFile> createBeside(
        const std::filesystem::path& target, std::error_code& ec) noexcept;

    // -----------------------------------------------------------------------
    // The FULL-REWRITE staging file — WIN-87 sites #1/#2, phase-56.
    // -----------------------------------------------------------------------
    //
    // WHY A SECOND FACTORY RATHER THAN createBeside. createBeside picks an
    // UNPREDICTABLE name, which is the right answer for an artifact nothing
    // else may ever look at. The full-rewrite temp is not that:
    // "<target>.pdfe_tmp" is a CONTRACT (F58-d, see the block below) — it is the
    // R2-04 crash-recovery artifact the Document constructor surfaces to the
    // user as recoverable unsaved work. Randomising it would silently retire
    // that recovery path, because a ".pdfe_part.<hex>" is SWEPT, never
    // surfaced. So this factory keeps the contractual name and buys back the
    // security properties the other way.
    //
    // WHAT A PREDICTABLE NAME COSTS, STATED PLAINLY. Of StagingFile's three
    // properties, only #1 (unpredictability) is given up. #2 (CREATE_NEW) and
    // #3 (an exclusive handle held from creation THROUGH publication) still
    // hold in full, and those two are what close the substitution attack: an
    // attacker who knows the name still cannot open it for writing or deletion
    // at any instant, and cannot have PDFE adopt one it planted. What is left
    // is SQUATTING — keeping the name occupied to deny the save. That is a
    // denial of service whose failure mode is "save refused, original
    // byte-intact", which is strictly better than the substitution it replaces.
    //
    // AND THE SQUATTER IS USUALLY PDFE (F2, phase-56 security review). The
    // paragraph above framed squatting as something an attacker does, which
    // missed the common case: a predictable name plus CREATE_NEW plus
    // FILE_SHARE_READ means TWO Document instances saving the SAME target
    // contend for one name. B's create gets FILE_EXISTS, and B's stale-name
    // clear gets ERROR_SHARING_VIOLATION against A's share mode. Before this
    // phase both saves "completed" — by interleaving writes into one shared,
    // unheld ".pdfe_tmp", which is corruption wearing a success return, not a
    // property worth keeping. But turning it into an immediate hard failure
    // would be a visible regression, so a sharing violation is classified
    // SEPARATELY from residue: it means "someone is actively using this",
    // which is a wait condition, not a squatter. createFullRewriteTempFor backs
    // off (25 ms doubling to 800 ms, ~12 rounds) and only then gives up, with
    // ERROR_SHARING_VIOLATION so the caller can say "another save of this
    // document is in progress" instead of a generic failure. Autosave is
    // unaffected either way: writeRecoveryCopy() writes a sidecar at its own
    // distinct path and never touches this name.
    //
    // THE STALE-NAME CLEAR (WIN-87 criterion 3), AND WHY IT IS NOT A remove().
    // A crashed prior save leaves residue at exactly this name, and CREATE_NEW
    // alone would then refuse every subsequent save forever. So an existing
    // entry is cleared first — but CLASSIFIED BY ATTRIBUTE FIRST, and never
    // opened, written or deleted THROUGH:
    //
    //   * a reparse point (symlink OR junction — junctions need no privilege to
    //     plant, so they are the realistic shape) is unlinked as the LINK:
    //     DeleteFileW and RemoveDirectoryW both operate on the reparse point
    //     itself, never on its target. GetFileAttributesW reports
    //     FILE_ATTRIBUTE_REPARSE_POINT for the link rather than following it,
    //     which is what makes the classification trustworthy.
    //   * a read-only regular file has the attribute cleared and is deleted —
    //     otherwise planting a read-only file is a permanent save denial.
    //   * a directory that is NOT a reparse point is REFUSED. It is not our
    //     residue, and recursively deleting a directory at a guessable name is
    //     how a stale-name clear becomes the attack it was meant to prevent.
    //
    // THE DIRECTORY REFUSAL IS NOT ATOMIC, AND SAYING OTHERWISE WOULD BE THE
    // PROJECT'S RECURRING DEFECT (F3-a, phase-56 security review). This used to
    // read "a REAL directory is REFUSED outright". Classification is
    // GetFileAttributesW and the action is a separate RemoveDirectoryW, so an
    // attacker who swaps a junction for a real empty directory between the two
    // gets the real one removed. Exploit value is close to nil — the window is
    // microseconds, it must be swapped at a name only PDFE writes, and
    // RemoveDirectoryW still refuses a NON-EMPTY directory, so nothing with
    // contents can be destroyed. The claim is corrected rather than the code
    // hardened because a claim whose evidence cannot establish it is exactly
    // the species this programme keeps finding.
    //
    // THE BOUNDED RETRY IS FOR TERMINATION, NOT FOR WINNING (F3-b, same
    // review). This used to say a re-planting process "loses rather than wins".
    // It wins: there is no backoff between the clear attempts and no way to
    // clear-and-create atomically, so a tight re-plant loop takes every round.
    // What the bound actually guarantees is that PDFE stops rather than
    // livelocks, and that the only thing the re-planter can achieve is a
    // REFUSED SAVE with the target untouched — never an adopted file, because
    // CREATE_NEW is atomic. It wins the race and loses the war.
    // The clear can never destroy user data that the recovery contract still
    // needs: the R2-04 artifact is surfaced at OPEN time, long before a save
    // runs, and the code this replaces already destroyed it unconditionally
    // (pdf_save_document fopen("wb")s the temp; saveAs remove()d it first).
    static std::unique_ptr<StagingFile> createFullRewriteTempFor(
        const std::filesystem::path& target, std::error_code& ec) noexcept;

    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;

    /// Closes the exclusive handle and DELETES the staging file, unless
    /// commitOver() already published it.
    ~StagingFile();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Byte-copy `source` into the staging file THROUGH the exclusive handle
    /// (never CopyFileW, which would also drag FILE_ATTRIBUTE_READONLY across
    /// and would need a second, unprotected open). Verifies the full length
    /// arrived: a short copy would append onto a truncated prefix and publish a
    /// corrupt document that still opened.
    bool copyFrom(const std::filesystem::path& source, std::error_code& ec) noexcept;

    /// A stdio stream over a DUPLICATE of the exclusive handle, positioned at
    /// offset 0. The caller owns the FILE* and must fclose() it (MuPDF's
    /// fz_drop_output does exactly that); closing it does NOT release this
    /// object's handle, so the exclusive window survives the write.
    /// Returns nullptr with `ec` set on failure.
    std::FILE* openDuplicateStream(std::error_code& ec) noexcept;

    // openDuplicateReadStream() WAS HERE, and is deleted with its caller
    // (WIN-87 phase-57). It handed out a READ-ONLY duplicate of the exclusive
    // handle so a caller could keep reading a file after commitOver published
    // it — the idea being that a duplicate predating the rename is a reader no
    // attacker ever had a chance to open for writing.
    //
    // MEASURED, IT DOES NOT DELIVER THAT, AND THE MEASUREMENT IS THE REASON
    // THIS TOMBSTONE EXISTS RATHER THAN A QUIET DELETION. commitOver closes
    // the ORIGINAL handle as part of publishing. With only the duplicate left,
    // a concurrent unprivileged process still obtained GENERIC_WRITE on the
    // published file: the hostile test's attacker recorded
    // "wrote:work.pdf.redact_tmp" against a build using exactly this. So the
    // share reservation the original open made is not something a surviving
    // duplicate can be relied on to carry.
    //
    // Two smaller facts learned on the way, both worth keeping:
    //   * A DUPLICATE_SAME_ACCESS copy carries the staging handle's DELETE
    //     access, and a later CRT fopen("rb") — which is what MuPDF's
    //     fz_open_document does — requests FILE_SHARE_READ | FILE_SHARE_WRITE
    //     and NOT FILE_SHARE_DELETE. Holding one across Document::save()'s
    //     reopen therefore fails that reopen with a sharing violation, not
    //     with anything that names the real cause.
    //   * Reducing the duplicate to GENERIC_READ fixes that sharing violation
    //     and does NOT fix the first problem, which is how the first problem
    //     was isolated.
    //
    // If you need to read a file you are about to republish, use
    // openDenyWriteReadStream() below: it makes its own open, and its
    // guarantee is tested directly rather than inherited from another handle.

    // ---------------------------------------------------------------------
    // DELETED, DELIBERATELY — do not add them back (F62, phase-55).
    //
    // `releaseHandleForExternalWrite()` / `reacquireExclusiveHandle()` used to
    // live here, as "the mandated fallback route" for an external API that can
    // only open by path — MuPDF's pdf_save_document, in
    // Document::saveIncrementalStaged's `bias != 0` case. The contract they
    // documented was that the caller would close the window afterwards by
    // re-verifying the staged bytes through the re-acquired handle.
    //
    // The caller could not, and did not. The re-verification available to it
    // (stagedPrefixMatchesOriginal) covers the pre-existing PREFIX; the bytes at
    // risk are the appended TAIL, which the external writer had just produced
    // and which nothing looked at. A forged, LARGER tail passed every check and
    // would have been published over the user's document. Hashing the tail "as
    // written" was not available either — that needs an output-side tee, and a
    // by-path write has no fz_output to wrap.
    //
    // So the window was removed instead of verified: saveIncrementalStaged()
    // now appends through openDuplicateStream() for EVERY document, applying the
    // header-preamble offset correction itself. These two methods have no
    // remaining caller and are gone rather than left unused, so a future caller
    // cannot reopen the window by reaching for a convenience that is still
    // sitting here. If you need one again, read the F62 reasoning in
    // Document.cpp first — the conclusion there is that a staging file which can
    // be opened for writing by anyone else, for any interval, cannot be vouched
    // for afterwards.
    // ---------------------------------------------------------------------

    /// Current size of the staging file, read through the exclusive handle.
    bool sizeInBytes(std::uintmax_t& out, std::error_code& ec) const noexcept;

    // readAt() WAS HERE, and is deleted for the same reason (F-4, phase-55).
    // Its only caller was stagedPrefixMatchesOriginal(), the other half of the
    // routine above. Leaving a read-through-the-handle convenience sitting here
    // with no caller invites exactly the reconstruction the deletion exists to
    // prevent: the way back to a by-path write is "release, let something else
    // write, re-acquire, read it back and check" — and this was the "read it
    // back" half. If a future caller genuinely needs to read the staged bytes,
    // adding it back is three lines and should be a deliberate decision with the
    // F62 reasoning in view, not a discovery that it was already available.

    /// Push the staged bytes toward stable storage (FlushFileBuffers / fsync on
    /// the exclusive handle). Best-effort; a failure is reported but is not by
    /// itself a reason to abandon the save.
    bool flushToStableStorage(std::error_code& ec) noexcept;

    /// Publish over `target`: preserve `target`'s DACL onto the staging file,
    /// then rename THROUGH the exclusive handle. On success the staging file no
    /// longer exists under its own name and the destructor will not remove it.
    /// On ANY failure `target` is completely untouched.
    ///
    /// WINDOWS ONLY, AND THE CARVE-OUT IS DELIBERATE (phase-56 security
    /// review). The "never re-resolves the name" property this class exists for
    /// holds on the Windows arm alone. The POSIX arm publishes with a
    /// path-based `std::filesystem::rename` and preserves permissions with a
    /// path-based `stat`/`chmod`, so the staging name IS resolved again there —
    /// the WIN-87 property simply does not hold on POSIX today. That is
    /// harmless while the product is Windows-only (see
    /// platforms_windows_only_for_now), and it is written down HERE rather than
    /// left to a reader's inference, because the contract text above otherwise
    /// reads as a platform-independent guarantee. Closing it needs renameat2 /
    /// RENAME_EXCHANGE and fchmod, and belongs with the port, not with this
    /// phase.
    bool commitOver(const std::filesystem::path& target,
                    std::error_code& ec) noexcept;

    /// commitOver with the bounded retry atomicReplaceFileWithRetry documents —
    /// the anti-A/V window (F10/WIN-84). Publishing through the handle with
    /// POSIX semantics removes most of the "something else holds the
    /// destination open" failure mode, but not all of it: the FileRenameInfo
    /// fallback (older kernels, filesystems that reject POSIX semantics) still
    /// behaves like MoveFileExW, and Document::save has retried this exact
    /// operation 10×100 ms since R2-04. Migrating the callers onto the handle
    /// must not quietly drop that calibration.
    ///
    /// Safe to retry: a failed commitOver leaves the exclusive handle open and
    /// the staging file exactly as it was, so attempt N+1 is attempt N.
    bool commitOverWithRetry(const std::filesystem::path& target,
                             std::error_code& ec,
                             int attempts = 10,
                             std::chrono::milliseconds delay =
                                 std::chrono::milliseconds(100)) noexcept;

private:
    StagingFile() = default;

    std::filesystem::path path_;
#if defined(_WIN32)
    void* handle_ = nullptr;  // HANDLE; INVALID_HANDLE_VALUE when unset
#else
    int fd_ = -1;
#endif
    bool committed_ = false;
};

// ---------------------------------------------------------------------------
// The READ side of WIN-87 — phase-57 (sites #3/#4/#5).
// ---------------------------------------------------------------------------
//
// StagingFile closes the window on the file PDFE WRITES. It says nothing about
// the file PDFE READS, and two of the three sites migrated in phase-57 are
// attacked there rather than at the staging artifact:
//
//   * MainWindow's QPDF redaction rewrite deliberately calls closeDocument()
//     to release every lock on the user's document, and then re-resolves that
//     same path BY NAME to read it back. An attacker who renames a valid decoy
//     PDF over the name in that interval has QPDF read attacker bytes and
//     republish them as "the redacted document". No staging file is involved
//     in the attack at all.
//   * MainWindow's alignment flush runs under the same deliberately-closed
//     document, reading the file the user has just saved.
//
// The fix is symmetric with the write side: resolve the name ONCE, while the
// bytes are still known-good, and do every subsequent read through that handle.
// dwShareMode = FILE_SHARE_READ denies every other opener WRITE and DELETE for
// as long as the stream lives, so neither an in-place overwrite nor a
// rename-over can land.
//
// TWO THINGS THIS DELIBERATELY DOES NOT DO.
//   * It does not deny other READERS. A reader cannot change what we parse, and
//     denying FILE_READ_ATTRIBUTES breaks A/V, backup agents and size-polling
//     watchers for no integrity gain — the same reasoning StagingFile records
//     for its own share mode.
//   * It does not outlive the read. A handle this process holds on a file
//     BLOCKS a later replace-existing rename onto it (FileRenameInfoEx's POSIX
//     semantics relaxes that for OTHER processes' handles only — measured, see
//     DCA 0021's closing note). Every caller here publishes over the file it
//     just read, so the stream MUST be closed before the commit. Callers scope
//     it accordingly; a leaked stream turns a working publish into a
//     diagnosable failure rather than into a security hole, but it is still a
//     bug.
//
/// Open `path` for reading with an exclusive-enough share mode: other openers
/// may READ it, but none may obtain write or delete access while the returned
/// stream is open. The stream is binary, seekable, positioned at offset 0, and
/// owned by the caller (fclose it). Returns nullptr with `ec` set on failure —
/// including when someone else already holds the file in a way that denies us
/// this guarantee, which is a refusal, never a silent downgrade.
std::FILE* openDenyWriteReadStream(const std::filesystem::path& path,
                                   std::error_code& ec) noexcept;

// ---------------------------------------------------------------------------
// The full-rewrite temp name — the OTHER half of the F58-d contract.
// ---------------------------------------------------------------------------
//
// WHY THIS BLOCK EXISTS. The ".pdfe_part.<hex>" shape above has had exactly one
// definition since phase-53 (StagingFile::kSuffixMarker), so it cannot drift.
// Its counterpart — the full-rewrite temp — did not: the literal ".pdfe_tmp"
// was spelled independently in three product call sites and in eight test files.
// That is not a cosmetic problem. F58-d is a contract about WHICH NAME MEANS
// WHICH THING, and a contract whose terms are re-typed at every site is one
// keystroke away from being silently renegotiated. Changing what Document::save
// names its staging artifact must be a one-line change here, and every consumer
// must move with it or fail to compile.
//
// THE TWO MEANINGS, RESTATED — DO NOT CONFLATE THEM.
//
//   "<target>.pdfe_tmp"
//       A COMPLETE, pdf_save_document-finished full rewrite that never got
//       renamed over the original. Produced by Document::save()'s full-rewrite
//       branch and by Document::saveAs(). This IS the R2-04 crash-recovery
//       artifact: the Document constructor surfaces it (after validating that
//       it is structurally finished) and the user is offered it as recoverable
//       unsaved work.
//
//   "<target>.pdfe_part.<32 hex>"
//       A POSSIBLY TRUNCATED incremental append behind an exclusive handle,
//       produced by StagingFile. It is internal residue. It is SWEPT, never
//       surfaced. Offering one to a user as "your unsaved changes" is precisely
//       the trap F58-d closed: a real mid-append TerminateProcess leaves a file
//       whose only `startxref` is the ORIGINAL's, so it opens as the PRE-save
//       document with megabytes of unsigned junk past the last `%%EOF` — the
//       classic incremental-update tampering shape, for a signed document.
//
// A SINGLE "is this any kind of staging residue?" PREDICATE APPLIED EVERYWHERE
// WOULD COLLAPSE THAT DISTINCTION AND REINTRODUCE THE DEFECT. That is why there
// are two predicates below and not one, why the classifier names the two kinds
// rather than returning a bool, and why callers are expected to pick the one
// that matches what they actually mean. `classifyStagingName` exists for the
// callers whose question genuinely is "which of the two is this?" (the recovery
// scan) — not as a general-purpose replacement for either predicate.

/// The full-rewrite temp suffix. THE canonical definition — no other file in
/// src/ or tests/ may spell this literal as an executable constant. A file
/// named "<target>" + this suffix carries the COMPLETE-full-rewrite meaning
/// documented above (recoverable), never the partial-append meaning.
inline constexpr const char* kFullRewriteTempSuffix = ".pdfe_tmp";

/// The path Document::save()'s full-rewrite branch and Document::saveAs() write
/// their new version to before the atomic replace: `target` with
/// kFullRewriteTempSuffix appended (appended to the whole path, NOT substituted
/// for the extension — "doc.pdf" ⇒ "doc.pdf.pdfe_tmp"). Same directory as
/// `target`, so the eventual rename is a single atomic directory operation.
std::filesystem::path fullRewriteTempPathFor(const std::filesystem::path& target);

/// True when `name` is EXACTLY "<targetFileName>" + kFullRewriteTempSuffix —
/// i.e. the COMPLETE-full-rewrite / R2-04-recoverable shape, for the target
/// file name (not path) `targetFileName`.
///
/// Exact match, deliberately: "doc.pdf.pdfe_tmp2" and "other.pdf.pdfe_tmp" are
/// NOT ours, and a suffix-only `ends_with` test would claim both. Nothing that
/// decides whether to offer a file to the user as their recovered document may
/// be that loose.
bool isFullRewriteTempName(const std::filesystem::path& targetFileName,
                           const std::filesystem::path& name) noexcept;

/// The two staging shapes, plus "neither". The enumerator NAMES carry the
/// semantics, so a caller cannot treat the two as interchangeable without
/// writing down that it is doing so:
///
///   * CompleteFullRewrite      — recoverable. Surface it (after validating it
///                                is structurally finished).
///   * PartialIncrementalAppend — internal residue. Sweep it. NEVER surface it.
enum class StagingNameKind {
    NotStaging,                ///< not a staging artifact of this target at all
    CompleteFullRewrite,       ///< "<target>.pdfe_tmp"        — see above
    PartialIncrementalAppend,  ///< "<target>.pdfe_part.<hex>" — see above
};

/// Answer the question the R2-04 recovery scan actually asks of each sibling:
/// which of the two staging meanings — if either — does this name carry?
///
/// Defined strictly in terms of isFullRewriteTempName and
/// StagingFile::isStagingResidueName, so it cannot disagree with them. The two
/// shapes are mutually exclusive by construction (one is a fixed-length exact
/// match, the other requires a different marker plus 32 hex digits); the
/// contract test pins that as a property rather than leaving it to inspection.
StagingNameKind classifyStagingName(const std::filesystem::path& targetFileName,
                                    const std::filesystem::path& name) noexcept;

}  // namespace pdfe::core
