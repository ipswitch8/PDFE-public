// -*- coding: utf-8 -*-
#include "core/AtomicFileReplace.h"

#include "core/TempFileSafety.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pdfe::core {

namespace {

#if defined(_WIN32)

std::error_code lastWinError() noexcept
{
    return std::error_code(static_cast<int>(::GetLastError()),
                           std::system_category());
}

/// A volume with no security model at all (FAT/exFAT removable media) has no
/// DACL to widen and no way to express one, so the DACL step is vacuous there.
/// Every OTHER read failure is a real one and must fail the replace closed.
bool errorMeansNoSecurityModel(DWORD e) noexcept
{
    return e == ERROR_NOT_SUPPORTED
        || e == ERROR_INVALID_FUNCTION
        || e == ERROR_CALL_NOT_IMPLEMENTED;
}

/// 128 bits from the system CSPRNG rendered as 32 lowercase hex digits.
///
/// DELEGATES to `pdfe::core::csprngHex128` (TempFileSafety.h) rather than
/// keeping its own BCryptGenRandom call. This function used to BE the second
/// copy: phase-60 needed the same primitive for the non-document temp files and
/// the obvious move was to copy these fifteen lines over. Two implementations
/// of a security primitive is two chances to drift and only one of them gets
/// reviewed — so there is now exactly one, and a guard test pins that there is
/// exactly one BCryptGenRandom call site in src/.
bool randomHex128(std::wstring& out) noexcept
{
    return csprngHex128(out);
}

/// Build an ACL holding only the NON-INHERITED ACEs of `src`. Used for the
/// unprotected-destination case: the inherited half is already identical on the
/// staging file (same directory), so copying it too would duplicate every ACE.
/// Returns false only on a genuine structural error; `out` may legitimately end
/// up as an empty-but-present ACL.
bool buildExplicitOnlyAcl(PACL src, std::vector<unsigned char>& storage,
                          PACL& out) noexcept
{
    ACL_SIZE_INFORMATION sizeInfo{};
    if (src && !::GetAclInformation(src, &sizeInfo, sizeof(sizeInfo),
                                    AclSizeInformation)) {
        return false;
    }

    // Worst case: every ACE is explicit, so reserve the source's whole payload.
    const DWORD payload = src ? sizeInfo.AclBytesInUse : 0;
    storage.assign(std::max<DWORD>(payload, sizeof(ACL)) + sizeof(ACL), 0);
    out = reinterpret_cast<PACL>(storage.data());
    if (!::InitializeAcl(out, static_cast<DWORD>(storage.size()), ACL_REVISION)) {
        return false;
    }
    if (!src) {
        return true;
    }
    for (DWORD i = 0; i < sizeInfo.AceCount; ++i) {
        void* ace = nullptr;
        if (!::GetAce(src, i, &ace)) {
            return false;
        }
        auto* hdr = static_cast<ACE_HEADER*>(ace);
        if (hdr->AceFlags & INHERITED_ACE) {
            continue;  // already present on the staging file via the directory
        }
        if (!::AddAce(out, ACL_REVISION, MAXDWORD, ace, hdr->AceSize)) {
            return false;
        }
    }
    return true;
}

/// Shared body of preserveDestinationDacl: read `to`'s DACL, apply it to
/// whichever of (`fromPath`, `fromHandle`) is supplied.
bool preserveDaclImpl(const std::filesystem::path* fromPath,
                      HANDLE fromHandle,
                      const std::filesystem::path& to,
                      std::error_code& ec) noexcept
{
    ec.clear();

    DWORD attrs = ::GetFileAttributesW(to.native().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            return true;  // nothing to preserve; the destination is new
        }
        ec = std::error_code(static_cast<int>(e), std::system_category());
        return false;
    }

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = ::GetNamedSecurityInfoW(
        to.native().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        if (errorMeansNoSecurityModel(rc)) {
            return true;  // FAT/exFAT: no DACL exists, so none can be widened
        }
        ec = std::error_code(static_cast<int>(rc), std::system_category());
        return false;
    }

    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    bool ok = ::GetSecurityDescriptorControl(sd, &control, &revision) != FALSE;
    DWORD applyErr = ok ? ERROR_SUCCESS : ::GetLastError();

    if (ok) {
        const bool destProtected = (control & SE_DACL_PROTECTED) != 0;
        SECURITY_INFORMATION si =
            DACL_SECURITY_INFORMATION
            | (destProtected ? PROTECTED_DACL_SECURITY_INFORMATION
                             : UNPROTECTED_DACL_SECURITY_INFORMATION);

        std::vector<unsigned char> explicitStorage;
        PACL toApply = dacl;
        if (!destProtected) {
            PACL explicitOnly = nullptr;
            if (!buildExplicitOnlyAcl(dacl, explicitStorage, explicitOnly)) {
                ok = false;
                applyErr = ::GetLastError();
            } else {
                toApply = explicitOnly;
            }
        }

        if (ok) {
            if (fromHandle != INVALID_HANDLE_VALUE && fromHandle != nullptr) {
                rc = ::SetSecurityInfo(fromHandle, SE_FILE_OBJECT, si,
                                       nullptr, nullptr, toApply, nullptr);
            } else {
                // SetNamedSecurityInfoW takes a mutable buffer.
                std::wstring mutablePath = fromPath->native();
                rc = ::SetNamedSecurityInfoW(mutablePath.data(), SE_FILE_OBJECT,
                                             si, nullptr, nullptr, toApply,
                                             nullptr);
            }
            if (rc != ERROR_SUCCESS) {
                ok = false;
                applyErr = rc;
            }
        }
    }

    if (sd) {
        ::LocalFree(sd);
    }
    if (!ok) {
        ec = std::error_code(static_cast<int>(applyErr), std::system_category());
    }
    return ok;
}

/// F4 (phase-56 security review) — the extended-length form of an absolute
/// path, when and only when it is needed.
///
/// THE REGRESSION THIS FIXES. Moving `saveAs`-to-a-NEW-destination onto staging
/// appends `kFullRewriteTempSuffix` (9 characters) to the user's chosen path and
/// then creates it with CreateFileW. A destination of 251-259 characters used to
/// succeed — MuPDF wrote straight to it — and started failing with "the filename
/// or extension is too long", because the staging name crosses MAX_PATH (260)
/// even though the destination does not. There is no `longPathAware` manifest in
/// this repo, so the Win32 path limit is live.
///
/// Applied only above a threshold, deliberately: the `\\?\` form disables path
/// normalisation (no `.`/`..` collapsing, no forward-slash translation), so
/// using it unconditionally would silently change how ordinary paths resolve.
/// UNC paths take the `\\?\UNC\server\share` spelling.
std::wstring win32Path(const std::filesystem::path& p)
{
    const std::wstring native = p.native();
    if (native.size() < 240 || native.rfind(LR"(\\?\)", 0) == 0
        || !p.is_absolute()) {
        return native;
    }
    if (native.rfind(LR"(\\)", 0) == 0) {
        return LR"(\\?\UNC\)" + native.substr(2);
    }
    return LR"(\\?\)" + native;
}

/// Create `candidate` EXCLUSIVELY — the one CreateFileW call that gives a
/// StagingFile all three of its properties, factored out so the two factories
/// cannot drift apart in access mask, share mode or disposition.
///
/// Returns INVALID_HANDLE_VALUE on failure. `nameTaken` distinguishes the one
/// error a caller may reasonably act on (the name already exists — retry with
/// another name, or clear it) from every other error, which is terminal.
HANDLE createExclusiveNew(const std::filesystem::path& candidate,
                          bool& nameTaken, std::error_code& ec) noexcept
{
    nameTaken = false;
    // GENERIC_READ|GENERIC_WRITE|DELETE|WRITE_DAC. DELETE is what
    // SetFileInformationByHandle(FileRenameInfoEx) needs, and acquiring it
    // HERE — before any DACL is applied to the staging file — is what makes
    // the DACL step safe: a destination DACL that does not grant us delete
    // cannot lock us out of publishing (or of UNLINKING — see ~StagingFile)
    // our own staging file, because the access was already granted on this
    // handle. WRITE_DAC is required for that DACL write itself and is NOT
    // implied by GENERIC_WRITE (which is FILE_GENERIC_WRITE: READ_CONTROL,
    // but no WRITE_DAC).
    //
    // FILE_SHARE_READ (not 0): readers are harmless, writers and deleters
    // are the attack, and denying FILE_READ_ATTRIBUTES would break A/V,
    // backup agents and size-polling watchers for no integrity gain.
    //
    // CREATE_NEW is load-bearing twice over: it never adopts a file it did not
    // create, and — because it fails on any EXISTING NAME — it never follows a
    // pre-planted reparse point to create somewhere else.
    const std::wstring wide = win32Path(candidate);
    HANDLE h = ::CreateFileW(
        wide.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE | WRITE_DAC,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        return h;
    }
    const DWORD e = ::GetLastError();
    // ERROR_ACCESS_DENIED is in this set DELIBERATELY, and it is not obvious.
    // CreateFileW(CREATE_NEW) on a name occupied by a DIRECTORY — including a
    // junction, which is the reparse point an unprivileged attacker can
    // actually plant — reports ERROR_ACCESS_DENIED, NOT ERROR_FILE_EXISTS,
    // because a directory cannot be opened at all without
    // FILE_FLAG_BACKUP_SEMANTICS. Reading it as a terminal permission error is
    // how a stale-name clear silently never runs against the one plant it most
    // needs to handle. (Measured: the junction test failed exactly this way
    // before this line existed.) The caller still has to prove the name is
    // really occupied before acting on it — see createFullRewriteTempFor.
    if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS
        || e == ERROR_ACCESS_DENIED) {
        nameTaken = true;
    }
    ec = std::error_code(static_cast<int>(e), std::system_category());
    return INVALID_HANDLE_VALUE;
}

#ifndef ERROR_DIRECTORY_NOT_SUPPORTED
#define ERROR_DIRECTORY_NOT_SUPPORTED 336L
#endif

/// Unlink whatever occupies `p`, WITHOUT ever traversing it. See the
/// createFullRewriteTempFor comment in the header for the reasoning; this is
/// only the mechanism.
///
/// Returns true when the name is (or has become) free. Returns false with `ec`
/// set when it is occupied by something this must not remove — a real
/// directory — or when the removal failed.
///
/// `removedSomething` says whether the name was actually occupied and has now
/// been unlinked. It is what stops "the name was never taken" from being read
/// as "the clear worked" — the caller must not then retry a creation that will
/// fail identically, and must report the ORIGINAL error rather than a
/// misleading "file exists".
///
/// `inUseByAnotherWriter` separates the one refusal that is not a squatter at
/// all — see the F2 note below.
bool clearStaleStagingName(const std::filesystem::path& p,
                           bool& removedSomething,
                           bool& inUseByAnotherWriter,
                           std::error_code& ec) noexcept
{
    removedSomething = false;
    inUseByAnotherWriter = false;
    const std::wstring wide = win32Path(p);
    const DWORD attrs = ::GetFileAttributesW(wide.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            return true;  // already gone — someone else lost the race
        }
        ec = std::error_code(static_cast<int>(e), std::system_category());
        return false;
    }

    const bool isReparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (isDirectory) {
        if (!isReparse) {
            // A directory that is not a reparse point. Not our residue, and not
            // ours to delete. (This classify-then-act pair is NOT atomic — see
            // the F3-a note in the header, which states the limit rather than
            // overclaiming the guarantee.)
            ec = std::error_code(static_cast<int>(ERROR_DIRECTORY_NOT_SUPPORTED),
                                 std::system_category());
            return false;
        }
        // A junction or a directory symlink. RemoveDirectoryW removes the
        // REPARSE POINT, never the directory it names.
        if (!::RemoveDirectoryW(wide.c_str())) {
            ec = lastWinError();
            return false;
        }
        removedSomething = true;
        return true;
    }

    if ((attrs & FILE_ATTRIBUTE_READONLY) != 0) {
        // Otherwise a planted read-only file denies every future save.
        ::SetFileAttributesW(wide.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
    // DeleteFileW on a file symlink removes the LINK, not its target.
    if (!::DeleteFileW(wide.c_str())) {
        ec = lastWinError();
        // F2 (phase-56 security review) — A SHARING VIOLATION IS NOT STALE
        // RESIDUE. It is the one answer that means "a file is there and
        // somebody is actively holding it", which for this name means ANOTHER
        // Document is mid-save on the same target: its StagingFile holds the
        // name with FILE_SHARE_READ, so our delete is refused. Treating that as
        // an unclearable squatter and failing immediately turned two concurrent
        // saves of one document from "both complete" into "the second fails" —
        // a regression this phase introduced by making the name predictable.
        // The caller backs off and retries on this signal instead.
        if (ec.value() == ERROR_SHARING_VIOLATION) {
            inUseByAnotherWriter = true;
        }
        return false;
    }
    removedSomething = true;
    return true;
}

#endif  // _WIN32

}  // namespace

bool preserveDestinationDacl(const std::filesystem::path& from,
                             const std::filesystem::path& to,
                             std::error_code& ec) noexcept
{
#if defined(_WIN32)
    return preserveDaclImpl(&from, INVALID_HANDLE_VALUE, to, ec);
#else
    // POSIX: rename() keeps the source inode's mode, so "preserve" means "give
    // the incoming file the destination's mode before the rename".
    ec.clear();
    struct stat st{};
    if (::stat(to.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    if (::chmod(from.c_str(), st.st_mode & 07777) != 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    return true;
#endif
}

bool atomicReplaceFile(const std::filesystem::path& from,
                       const std::filesystem::path& to,
                       std::error_code& ec) noexcept
{
    ec.clear();

    // F58-b (phase-53): the incoming file is a NEW file, so it carries the
    // CONTAINING DIRECTORY's inheritable ACEs, and the rename below carries
    // those onto the destination. Every stage-and-rename site in the product
    // routes through here, so doing this once here fixes save(), saveAs()
    // over-existing, redaction, batch and image export at the same time.
    // Fail-closed: a DACL we cannot reproduce aborts the replace with the
    // destination untouched, which is strictly better than publishing with
    // silently widened permissions.
    if (!preserveDestinationDacl(from, to, ec)) {
        return false;
    }

#if defined(_WIN32)
    // MoveFileExW with MOVEFILE_REPLACE_EXISTING swaps `to` for `from` as a single
    // directory operation — the original is replaced in place, never
    // deleted-then-recreated (so a crash or a failure here can never leave only
    // the temp). MOVEFILE_WRITE_THROUGH flushes the metadata change to disk before
    // returning. Both paths are UTF-16 native, so Unicode file names round-trip
    // losslessly (unlike a narrow ANSI path — see mupdfPathUtf8).
    if (::MoveFileExW(from.native().c_str(), to.native().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    ec = lastWinError();
    return false;
#else
    // POSIX rename() atomically replaces an existing destination on the same
    // filesystem — there is never a window in which `to` is absent.
    std::filesystem::rename(from, to, ec);
    return !ec;
#endif
}

// ===========================================================================
// StagingFile
// ===========================================================================

std::filesystem::path StagingFile::unpredictableSiblingPath(
    const std::filesystem::path& target, std::error_code& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    std::wstring hex;
    if (!randomHex128(hex)) {
        // FAIL CLOSED. A predictable staging name is the whole defect, so a
        // CSPRNG failure must not silently degrade to a counter or a timestamp.
        ec = std::make_error_code(std::errc::resource_unavailable_try_again);
        return {};
    }
    std::filesystem::path p = target;
    p += std::filesystem::path(kSuffixMarker).native() + hex;
    return p;
#else
    // POSIX: no mandatory share mode anywhere in this file, so the WIN-87
    // property does not hold on this arm either way. Refuse rather than hand
    // back a name that reads as hardened.
    (void)target;
    ec = std::make_error_code(std::errc::function_not_supported);
    return {};
#endif
}

bool StagingFile::isStagingResidueName(
    const std::filesystem::path& targetFileName,
    const std::filesystem::path& name) noexcept
{
    using S = std::filesystem::path::string_type;
    const S prefix =
        targetFileName.native() + std::filesystem::path(kSuffixMarker).native();
    const S cand = name.native();
    if (cand.size() != prefix.size() + 32) {
        return false;
    }
    if (cand.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    for (std::size_t i = prefix.size(); i < cand.size(); ++i) {
        const auto c = cand[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Full-rewrite temp name (F58-d contract, product side). See the header for why
// the two shapes must never be answered by one predicate.
// ---------------------------------------------------------------------------

std::filesystem::path fullRewriteTempPathFor(const std::filesystem::path& target)
{
    auto p = target;
    p += kFullRewriteTempSuffix;  // append to the WHOLE path, never replace_extension
    return p;
}

bool isFullRewriteTempName(const std::filesystem::path& targetFileName,
                           const std::filesystem::path& name) noexcept
{
    // Exact equality on the native strings — same comparison discipline
    // isStagingResidueName uses, so the two predicates agree about what
    // "<targetFileName>" means on a platform whose paths are wide.
    using S = std::filesystem::path::string_type;
    const S expected = targetFileName.native()
                       + std::filesystem::path(kFullRewriteTempSuffix).native();
    return name.native() == expected;
}

StagingNameKind classifyStagingName(const std::filesystem::path& targetFileName,
                                    const std::filesystem::path& name) noexcept
{
    // Order is irrelevant to correctness — the two predicates cannot both be
    // true (a name ending in the fixed ".pdfe_tmp" cannot also end in
    // ".pdfe_part." + 32 hex digits) — but the full-rewrite test is first
    // because it is the cheaper one and by far the more common answer.
    if (isFullRewriteTempName(targetFileName, name)) {
        return StagingNameKind::CompleteFullRewrite;
    }
    if (StagingFile::isStagingResidueName(targetFileName, name)) {
        return StagingNameKind::PartialIncrementalAppend;
    }
    return StagingNameKind::NotStaging;
}

StagingFile::~StagingFile()
{
#if defined(_WIN32)
    HANDLE h = static_cast<HANDLE>(handle_);
    if (h && h != INVALID_HANDLE_VALUE) {
        // F1 (phase-56 security review) — DELETE THROUGH THE HANDLE, BEFORE
        // CLOSING IT. This used to CloseHandle() and then remove() BY NAME, and
        // that is the WIN-87 shape sitting on the failure path of the very fix
        // that removes it: a by-name unlink re-resolves a now-PREDICTABLE name
        // and re-runs the access check.
        //
        // It is not only a substitution window, it is a self-inflicted lockout,
        // and the reasoning is already written down in createExclusiveNew:
        // DELETE is acquired AT CREATION precisely so a later DACL write cannot
        // revoke it. commitOver then copies the DESTINATION's DACL onto the
        // staging file — and a destination whose protected ACL does not grant
        // this user DELETE (an owner-only document in a Modify folder: exactly
        // the F58-b scenario) leaves a by-name remove() with ACCESS_DENIED. The
        // orphan then survives at the contractual name, and because the
        // stale-name clear fails closed on the same check, EVERY subsequent
        // save of that document fails permanently while the R2-04 scan offers
        // an unreadable recovery prompt on every open. One transient rename
        // failure — the ordinary A/V case the retry loop exists for — reaches
        // it.
        //
        // FileDispositionInfo marks the file for deletion using the access
        // already granted on THIS handle, so no name is resolved and no ACL is
        // re-evaluated. The unlink happens when the last handle closes, i.e. at
        // the CloseHandle below. The by-name remove() survives only as a
        // best-effort backstop for the case where the disposition call itself
        // failed (and on POSIX, where the name is not the security boundary).
        FILE_DISPOSITION_INFO disp{};
        disp.DeleteFile = TRUE;
        const bool markedForDelete =
            !committed_
            && ::SetFileInformationByHandle(h, FileDispositionInfo, &disp,
                                            sizeof(disp)) != FALSE;
        ::CloseHandle(h);
        handle_ = nullptr;
        if (markedForDelete) {
            return;  // already gone; never touch the name
        }
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    if (!committed_ && !path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);  // best-effort; never throws
    }
}

std::unique_ptr<StagingFile> StagingFile::createBeside(
    const std::filesystem::path& target, std::error_code& ec) noexcept
{
    ec.clear();
    std::unique_ptr<StagingFile> sf(new (std::nothrow) StagingFile());
    if (!sf) {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return nullptr;
    }

#if defined(_WIN32)
    // Up to 8 attempts: a CREATE_NEW collision on a 128-bit random name is
    // astronomically unlikely, but retrying costs nothing and turns a
    // pre-created squat on a guessed name into a survivable condition rather
    // than a hard save failure.
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::wstring hex;
        if (!randomHex128(hex)) {
            ec = std::error_code(static_cast<int>(NTE_FAIL),
                                 std::system_category());
            return nullptr;
        }
        std::filesystem::path candidate = target;
        candidate += std::filesystem::path(L".pdfe_part." + hex);

        bool nameTaken = false;
        HANDLE h = createExclusiveNew(candidate, nameTaken, ec);
        if (h != INVALID_HANDLE_VALUE) {
            ec.clear();
            sf->handle_ = h;
            sf->path_ = candidate;

            // F71 -- CARRY THE DESTINATION'S DACL NOW, NOT ONLY AT PUBLISH.
            //
            // commitOver() applies it too, and used to be the ONLY place that
            // did. That closed the window at publication and left it open for
            // the whole append: CREATE_NEW with no security descriptor gives
            // the staging file the CONTAINING DIRECTORY's inherited ACEs, so a
            // document whose own ACL is restrictive -- the F58-b shape, an
            // owner-only file inside a folder granting Users:(OI)(CI)(M) --
            // had a full plaintext copy of itself readable by every principal
            // that directory admits, for as long as the save took. On a large
            // document that is seconds, and FILE_SHARE_READ is deliberate (see
            // createExclusiveNew), so it is readable BY DESIGN.
            //
            // INTEGRITY was never the gap and is not what this changes: the
            // exclusive handle denies write and delete to everyone else from
            // creation through publication.
            //
            // THIS NARROWS THE CONFIDENTIALITY WINDOW; IT DOES NOT CLOSE IT.
            // Saying otherwise would repeat the exact mistake F71 is about, so:
            // CreateFileW returns BEFORE SetSecurityInfo runs, and for those few
            // instructions the file exists wearing the DIRECTORY's inherited
            // ACEs. Windows does NOT revoke an already-open handle when a DACL
            // later tightens, so a reader who opens in that instant keeps read
            // access for the file's whole life -- including every byte written
            // afterwards.
            //
            // The exposure therefore went from "the entire append" to "a few
            // instructions inside one function". That is a large reduction and
            // it is not zero. Winning that race needs the name IN ADVANCE, so
            // it does not apply to this factory (128 bits of CSPRNG) -- see
            // createFullRewriteTempFor, whose name is a CONTRACT and where the
            // race consequently has an address to aim at. Closing it outright
            // means passing CreateFileW an explicit SECURITY_ATTRIBUTES rather
            // than nullptr, which is a larger change than this one.
            //
            // The ORDERING is what makes it safe, and it is not incidental:
            // createExclusiveNew already took DELETE and WRITE_DAC on this
            // handle BEFORE any DACL existed on the file, precisely so a
            // restrictive destination ACL cannot lock us out of publishing --
            // or of unlinking our own staging file, which ~StagingFile does
            // THROUGH the handle for exactly that reason.
            //
            // Fails CLOSED, consistently with commitOver(): a destination
            // whose DACL cannot be read is one whose confidentiality cannot be
            // honoured, and proceeding would reintroduce the exposure this
            // removes. preserveDaclImpl already returns true for both benign
            // cases -- a destination that does not exist yet (Save As to a new
            // name: nothing to inherit) and a volume with no security model
            // (FAT/exFAT: no DACL to widen) -- so this rejects only a real
            // failure. Returning nullptr lets ~StagingFile close and unlink
            // what was just created.
            if (!preserveDaclImpl(nullptr, h, target, ec)) {
                return nullptr;
            }
            return sf;
        }
        // Only a genuine name COLLISION is worth another random name here.
        // createExclusiveNew also reports ERROR_ACCESS_DENIED as "name taken"
        // for the benefit of createFullRewriteTempFor's stale-name clear (a
        // directory occupying the name); on a 128-bit random name that error
        // means the DIRECTORY is not writable, and retrying it 7 more times
        // would only replace a precise diagnosis with a misleading one.
        (void)nameTaken;
        if (ec.value() != ERROR_FILE_EXISTS
            && ec.value() != ERROR_ALREADY_EXISTS) {
            return nullptr;  // `ec` already carries the terminal error
        }
    }
    ec = std::error_code(static_cast<int>(ERROR_FILE_EXISTS),
                         std::system_category());
    return nullptr;
#else
    for (int attempt = 0; attempt < 8; ++attempt) {
        unsigned char bytes[16];
        {
            int rf = ::open("/dev/urandom", O_RDONLY);
            if (rf < 0) {
                ec = std::error_code(errno, std::generic_category());
                return nullptr;
            }
            ssize_t got = ::read(rf, bytes, sizeof(bytes));
            ::close(rf);
            if (got != static_cast<ssize_t>(sizeof(bytes))) {
                ec = std::make_error_code(std::errc::io_error);
                return nullptr;
            }
        }
        static const char kHex[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(32);
        for (unsigned char b : bytes) {
            hex.push_back(kHex[(b >> 4) & 0x0F]);
            hex.push_back(kHex[b & 0x0F]);
        }
        std::filesystem::path candidate = target;
        candidate += (".pdfe_part." + hex);
        // O_EXCL is the POSIX equivalent of CREATE_NEW, and mode 0600 is the
        // equivalent of the exclusive share mode: no other user can open it at
        // all. (POSIX has no mandatory locking, so the unpredictable name plus
        // O_EXCL plus 0600 is the strongest available form.)
        int fd = ::open(candidate.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            sf->fd_ = fd;
            sf->path_ = candidate;
            return sf;
        }
        if (errno != EEXIST) {
            ec = std::error_code(errno, std::generic_category());
            return nullptr;
        }
    }
    ec = std::make_error_code(std::errc::file_exists);
    return nullptr;
#endif
}

std::unique_ptr<StagingFile> StagingFile::createFullRewriteTempFor(
    const std::filesystem::path& target, std::error_code& ec) noexcept
{
    ec.clear();
    std::unique_ptr<StagingFile> sf(new (std::nothrow) StagingFile());
    if (!sf) {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return nullptr;
    }

    std::filesystem::path candidate;
    try {
        candidate = fullRewriteTempPathFor(target);
    } catch (...) {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return nullptr;
    }

    // TWO LOOPS, BECAUSE THERE ARE TWO DIFFERENT SITUATIONS AND ONLY ONE OF
    // THEM IS ADVERSARIAL (F2/F3-b, phase-56 security review).
    //
    //  * CLEARABLE RESIDUE — a crashed prior save, or a plant. Four attempts,
    //    no delay. What the bound is actually for is stated below; it is NOT
    //    that "a re-planter loses".
    //  * IN USE BY ANOTHER WRITER — ERROR_SHARING_VIOLATION from the delete,
    //    which for this name means another Document is mid-save on the same
    //    target and its StagingFile holds it. That is not residue and must not
    //    be treated as a failure on sight: before this phase the two saves
    //    interleaved on one shared, unheld ".pdfe_tmp" (which "worked" only in
    //    the sense that neither returned an error), and failing immediately
    //    would be a visible regression. So back off and retry — the other save
    //    finishes and frees the name — and only after the whole budget give up
    //    with ERROR_SHARING_VIOLATION, which the caller reports as "another
    //    save of this document is in progress" rather than as a generic error.
    //
    // THE BOUND IS NOT A RACE THIS WINS. An attacker re-planting in a tight
    // loop wins EVERY round, because there is no delay to escape and the
    // filesystem offers no atomic clear-and-create. What the bound buys is
    // termination with the target untouched: the re-planter wins the race and
    // loses the war, because CREATE_NEW means the only thing it can achieve is
    // a refused save, never an adopted file.
    static constexpr int kClearAttempts = 4;
    static constexpr int kInUseAttempts = 12;
    static constexpr auto kInUseBackoffCap = std::chrono::milliseconds(800);
#if defined(_WIN32)
    // GitHub #282 -- Windows' DELETE-PENDING window, measured 2026-10-10.
    //
    // `FullRewriteStagingPrimitive.ASecondWriterWaitsForTheFirstRatherThanFailing`
    // failed 3 times in 200 fresh processes (and once in a 6,519-test shard,
    // which is how it was reported). Every failure landed at 412-434 ms against
    // the test's 400 ms hold -- i.e. exactly as the first writer's handle is
    // released, not spread through the wait -- reporting either
    // "The system cannot find the file specified" or "Access is denied". Which
    // of the two appears is not stable across sweeps, so neither code is the
    // signature; the TIMING is.
    //
    // Both are the same transient: the previous holder's file object outlives
    // its directory entry, so for a few milliseconds the name can be neither
    // created nor cleared. Neither code is ERROR_SHARING_VIOLATION, which is
    // the only contention the loop below was built for.
    //
    // This budget is deliberately SEPARATE from kInUseAttempts, whose bound
    // carries a security argument about an adversarial re-planter. Nothing
    // adversarial is implied here and the two must not borrow each other's
    // reasoning.
    //
    // THE VERDICT IS PRESERVED, ONLY DEFERRED. On exhaustion each site returns
    // the error it already had, so a planted directory, an unwritable
    // directory, or a genuinely unremovable squatter still fails with that
    // error -- just later. This widens WHEN we give up, never WHETHER.
    //
    // ERROR_PATH_NOT_FOUND is excluded: a missing PARENT directory is permanent,
    // and retrying it would turn a caller's bad path into a multi-second stall
    // before the same failure. FILE_NOT_FOUND and PATH_NOT_FOUND are different
    // codes for a reason and only the first is transient here.
    static constexpr int kTransientAttempts = 10;
    const auto isDeletePendingShape = [](const std::error_code& e) noexcept {
        return (e.value() == static_cast<int>(ERROR_ACCESS_DENIED)
                || e.value() == static_cast<int>(ERROR_FILE_NOT_FOUND))
               && e.value() != static_cast<int>(ERROR_PATH_NOT_FOUND);
    };

    int clearAttempts = 0;
    int inUseAttempts = 0;
    int transientAttempts = 0;
    auto backoff = std::chrono::milliseconds(25);
    for (;;) {
        bool nameTaken = false;
        HANDLE h = createExclusiveNew(candidate, nameTaken, ec);
        if (h != INVALID_HANDLE_VALUE) {
            ec.clear();
            sf->handle_ = h;
            sf->path_ = candidate;

            // F71, same as createBeside -- the full reasoning is there.
            // This factory needs it just as much, and arguably more:
            // "<target>.pdfe_tmp" is a full plaintext copy of the document,
            // and being the CONTRACTUAL crash-recovery name makes it MORE
            // exposed, not less -- it is predictable by design, and it
            // deliberately SURVIVES a crash. Inheriting the directory's ACEs
            // would leave it readable by every principal that directory admits
            // until the save publishes, or indefinitely if the process dies
            // first, which is precisely when it is meant to still be there.
            if (!preserveDaclImpl(nullptr, h, target, ec)) {
                return nullptr;
            }
            return sf;
        }
        if (!nameTaken) {
            return nullptr;
        }
        const std::error_code createEc = ec;
        bool removedSomething = false;
        bool inUseByAnotherWriter = false;
        if (!clearStaleStagingName(candidate, removedSomething,
                                   inUseByAnotherWriter, ec)) {
            if (!inUseByAnotherWriter) {
                // GitHub #282, exit 1 of 2. "An unremovable squatter" is the
                // right reading for a planted directory and the WRONG one for
                // Windows' DELETE-PENDING window: when the previous holder's
                // handle closes, the file object outlives the directory entry
                // briefly, and a delete against the name in that instant
                // reports ERROR_ACCESS_DENIED -- indistinguishable here from a
                // squatter we genuinely cannot clear.
                if (isDeletePendingShape(ec)
                    && ++transientAttempts <= kTransientAttempts) {
                    std::this_thread::sleep_for(backoff);
                    backoff = std::min(backoff * 2, kInUseBackoffCap);
                    continue;
                }
                return nullptr;  // a real directory, or an unremovable squatter
            }
            if (++inUseAttempts > kInUseAttempts) {
                ec = std::error_code(static_cast<int>(ERROR_SHARING_VIOLATION),
                                     std::system_category());
                return nullptr;
            }
            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, kInUseBackoffCap);
            continue;
        }
        if (!removedSomething) {
            // The name was not occupied at all, so the creation failure was
            // about something else (an unwritable directory, most likely).
            // Report THAT, not a retry-exhausted "file exists".
            //
            // GitHub #282, exit 2 of 2, and the reason three earlier attempts
            // appeared to do nothing. BOTH this exit and the squatter exit
            // above fire, about equally often -- so patching either one alone
            // halves a 3-in-200 rate, which is indistinguishable from no change
            // at that sample size. They were located together by instrumenting
            // all thirteen `return nullptr` sites in this function with
            // distinct error codes in one throwaway build: 6 failures over 250
            // processes, 3 carrying this site's code and 3 the other's.
            //
            // "Not occupied at all" is also literally true in the
            // delete-pending window: the directory entry is already gone, so
            // the clear finds nothing to remove, while the create still cannot
            // have the name.
            if (isDeletePendingShape(createEc)
                && ++transientAttempts <= kTransientAttempts) {
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, kInUseBackoffCap);
                continue;
            }
            ec = createEc;
            return nullptr;
        }
        if (++clearAttempts >= kClearAttempts) {
            ec = std::error_code(static_cast<int>(ERROR_FILE_EXISTS),
                                 std::system_category());
            return nullptr;
        }
    }
#else
    for (int attempt = 0; attempt < kClearAttempts; ++attempt) {
        int fd = ::open(candidate.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            ec.clear();
            sf->fd_ = fd;
            sf->path_ = candidate;
            return sf;
        }
        if (errno != EEXIST) {
            ec = std::error_code(errno, std::generic_category());
            return nullptr;
        }
        // lstat, never stat: classify the LINK, never what it points at.
        struct stat st{};
        if (::lstat(candidate.c_str(), &st) != 0) {
            if (errno == ENOENT) {
                continue;  // already gone — someone else lost the race
            }
            ec = std::error_code(errno, std::generic_category());
            return nullptr;
        }
        if (S_ISDIR(st.st_mode)) {
            ec = std::make_error_code(std::errc::is_a_directory);
            return nullptr;
        }
        if (::unlink(candidate.c_str()) != 0 && errno != ENOENT) {
            ec = std::error_code(errno, std::generic_category());
            return nullptr;
        }
    }
    ec = std::make_error_code(std::errc::file_exists);
    return nullptr;
#endif
}

bool StagingFile::copyFrom(const std::filesystem::path& source,
                           std::error_code& ec) noexcept
{
    ec.clear();

    std::ifstream in(source, std::ios::binary);
    if (!in) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }

    std::error_code sizeEc;
    const auto expected = std::filesystem::file_size(source, sizeEc);
    if (sizeEc) {
        ec = sizeEc;
        return false;
    }

    std::vector<char> buf(1u << 20);
    std::uintmax_t written = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
#if defined(_WIN32)
        const char* p = buf.data();
        DWORD remaining = static_cast<DWORD>(got);
        while (remaining > 0) {
            DWORD wrote = 0;
            if (!::WriteFile(static_cast<HANDLE>(handle_), p, remaining, &wrote,
                             nullptr)
                || wrote == 0) {
                ec = lastWinError();
                return false;
            }
            p += wrote;
            remaining -= wrote;
        }
#else
        const char* p = buf.data();
        std::size_t remaining = static_cast<std::size_t>(got);
        while (remaining > 0) {
            const ssize_t wrote = ::write(fd_, p, remaining);
            if (wrote <= 0) {
                ec = std::error_code(errno, std::generic_category());
                return false;
            }
            p += wrote;
            remaining -= static_cast<std::size_t>(wrote);
        }
#endif
        written += static_cast<std::uintmax_t>(got);
    }

    if (written != expected) {
        // A short copy would append onto a truncated prefix and publish a
        // corrupt document that still opened — the worst possible outcome.
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    return true;
}

std::FILE* StagingFile::openDuplicateStream(std::error_code& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    // The stdio stream must own a DUPLICATE, not this object's handle: MuPDF's
    // fz_drop_output fcloses the FILE*, and the whole point of this class is
    // that the exclusive handle outlives the write and carries through to the
    // rename.
    HANDLE dup = INVALID_HANDLE_VALUE;
    if (!::DuplicateHandle(::GetCurrentProcess(), static_cast<HANDLE>(handle_),
                           ::GetCurrentProcess(), &dup, 0, FALSE,
                           DUPLICATE_SAME_ACCESS)) {
        ec = lastWinError();
        return nullptr;
    }
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(dup), _O_BINARY);
    if (fd < 0) {
        ::CloseHandle(dup);
        ec = std::make_error_code(std::errc::too_many_files_open);
        return nullptr;
    }
    // From here the fd owns `dup`; closing the fd closes it.
    std::FILE* fp = ::_fdopen(fd, "r+b");
    if (!fp) {
        ::_close(fd);
        ec = std::make_error_code(std::errc::io_error);
        return nullptr;
    }
    std::rewind(fp);
    return fp;
#else
    const int dup = ::dup(fd_);
    if (dup < 0) {
        ec = std::error_code(errno, std::generic_category());
        return nullptr;
    }
    std::FILE* fp = ::fdopen(dup, "r+b");
    if (!fp) {
        ::close(dup);
        ec = std::error_code(errno, std::generic_category());
        return nullptr;
    }
    std::rewind(fp);
    return fp;
#endif
}

bool StagingFile::sizeInBytes(std::uintmax_t& out,
                              std::error_code& ec) const noexcept
{
    ec.clear();
#if defined(_WIN32)
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(static_cast<HANDLE>(handle_), &li)) {
        ec = lastWinError();
        return false;
    }
    out = static_cast<std::uintmax_t>(li.QuadPart);
    return true;
#else
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    out = static_cast<std::uintmax_t>(st.st_size);
    return true;
#endif
}

bool StagingFile::flushToStableStorage(std::error_code& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    if (!::FlushFileBuffers(static_cast<HANDLE>(handle_))) {
        ec = lastWinError();
        return false;
    }
    return true;
#else
    if (::fsync(fd_) != 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    return true;
#endif
}

bool StagingFile::commitOver(const std::filesystem::path& target,
                             std::error_code& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    HANDLE h = static_cast<HANDLE>(handle_);
    if (!h || h == INVALID_HANDLE_VALUE) {
        ec = std::make_error_code(std::errc::bad_file_descriptor);
        return false;
    }

    // F58-b: carry the destination's DACL onto the staging file BEFORE it is
    // published, applied through the handle so it cannot revoke the DELETE
    // access the rename below needs.
    if (!preserveDaclImpl(nullptr, h, target, ec)) {
        return false;
    }

    // Rename THROUGH the exclusive handle. There is no instant between the last
    // staged byte and publication at which the file is openable for writing by
    // anyone else — which is the property MoveFileExW-by-path cannot provide,
    // because it re-resolves the name.
    const std::wstring dest = target.native();
    const size_t nameBytes = (dest.size() + 1) * sizeof(wchar_t);

    // Declared locally rather than relying on the SDK's version-gated
    // FILE_RENAME_INFO union member: the layout is fixed ABI, and this compiles
    // identically whatever _WIN32_WINNT the build happens to target.
    struct PdfeRenameInfo {
        DWORD  Flags;            // union { BOOLEAN ReplaceIfExists; DWORD Flags; }
        HANDLE RootDirectory;
        DWORD  FileNameLength;
        WCHAR  FileName[1];
    };
    constexpr DWORD kReplaceIfExists = 0x00000001;  // FILE_RENAME_FLAG_REPLACE_IF_EXISTS
    constexpr DWORD kPosixSemantics  = 0x00000002;  // FILE_RENAME_FLAG_POSIX_SEMANTICS
    constexpr auto  kFileRenameInfoEx =
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);  // FileRenameInfoEx

    // FileRenameInfoEx (Win10 1709+/Server 2019+) with POSIX semantics: the
    // destination's own open handles stop resolving to it immediately instead of
    // the rename failing, which removes the "A/V still has the target open"
    // failure mode entirely.
    {
        std::vector<unsigned char> buf(sizeof(PdfeRenameInfo) + nameBytes, 0);
        auto* fri = reinterpret_cast<PdfeRenameInfo*>(buf.data());
        fri->Flags = kReplaceIfExists | kPosixSemantics;
        fri->RootDirectory = nullptr;
        fri->FileNameLength = static_cast<DWORD>(dest.size() * sizeof(wchar_t));
        std::memcpy(fri->FileName, dest.c_str(), nameBytes);
        if (::SetFileInformationByHandle(h, kFileRenameInfoEx, buf.data(),
                                         static_cast<DWORD>(buf.size()))) {
            committed_ = true;
            ::CloseHandle(h);
            handle_ = nullptr;
            return true;
        }
    }

    // Fall back to plain FileRenameInfo where FileRenameInfoEx is unavailable
    // (older kernels, or a filesystem that rejects POSIX semantics). Still a
    // rename through the handle, so the window stays closed.
    {
        std::vector<unsigned char> buf(sizeof(FILE_RENAME_INFO) + nameBytes, 0);
        auto* fri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
        fri->ReplaceIfExists = TRUE;
        fri->RootDirectory = nullptr;
        fri->FileNameLength = static_cast<DWORD>(dest.size() * sizeof(wchar_t));
        std::memcpy(fri->FileName, dest.c_str(), nameBytes);
        if (::SetFileInformationByHandle(h, FileRenameInfo, buf.data(),
                                         static_cast<DWORD>(buf.size()))) {
            committed_ = true;
            ::CloseHandle(h);
            handle_ = nullptr;
            return true;
        }
        ec = lastWinError();
    }
    return false;
#else
    if (!preserveDestinationDacl(path_, target, ec)) {
        return false;
    }
    std::filesystem::rename(path_, target, ec);
    if (ec) {
        return false;
    }
    committed_ = true;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    return true;
#endif
}

bool StagingFile::commitOverWithRetry(const std::filesystem::path& target,
                                      std::error_code& ec,
                                      int attempts,
                                      std::chrono::milliseconds delay) noexcept
{
    if (attempts < 1) {
        attempts = 1;
    }
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (commitOver(target, ec)) {
            return true;
        }
#if defined(_WIN32)
        if (attempt + 1 < attempts) {
            std::this_thread::sleep_for(delay);
        }
#else
        break;  // POSIX rename does not suffer transient lock holdovers
#endif
    }
    return false;
}

// ---------------------------------------------------------------------------
// openDenyWriteReadStream — WIN-87 phase-57, the READ side. See the header for
// why a read handle is a security boundary here and why it must not outlive
// the read.
// ---------------------------------------------------------------------------
std::FILE* openDenyWriteReadStream(const std::filesystem::path& path,
                                   std::error_code& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    // FILE_SHARE_READ only: readers stay welcome, writers and deleters are
    // refused for the lifetime of this handle. OPEN_EXISTING (never
    // OPEN_ALWAYS) so a vanished file is an error rather than a fresh empty
    // one. FILE_FLAG_OPEN_REPARSE_POINT is deliberately NOT set — the caller
    // is opening a document the user named, and following a legitimate
    // symlink to it is correct behaviour; what must not happen is the name
    // being re-resolved LATER, which is exactly what this removes.
    HANDLE h = ::CreateFileW(path.native().c_str(), GENERIC_READ,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ec = lastWinError();
        return nullptr;
    }
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY | _O_RDONLY);
    if (fd < 0) {
        ::CloseHandle(h);
        ec = std::make_error_code(std::errc::too_many_files_open);
        return nullptr;
    }
    // From here the fd owns `h`; fclose on the FILE* closes both.
    std::FILE* fp = ::_fdopen(fd, "rb");
    if (!fp) {
        ::_close(fd);
        ec = std::make_error_code(std::errc::io_error);
        return nullptr;
    }
    std::rewind(fp);
    return fp;
#else
    // POSIX has no mandatory share mode, so the deny-write half of the
    // contract simply does not exist here — the same carve-out StagingFile's
    // commitOver records for its own POSIX arm. Written down rather than left
    // to inference: on POSIX this returns an ordinary read stream and the
    // WIN-87 property does not hold.
    std::FILE* fp = std::fopen(path.string().c_str(), "rb");
    if (!fp) {
        ec = std::error_code(errno, std::generic_category());
        return nullptr;
    }
    return fp;
#endif
}

}  // namespace pdfe::core
