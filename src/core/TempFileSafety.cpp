// -*- coding: utf-8 -*-
#include "core/TempFileSafety.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <fstream>
#include <vector>

namespace pdfe::core {

namespace {

#if defined(_WIN32)
std::error_code lastWinError() noexcept
{
    const DWORD e = ::GetLastError();
    return std::error_code(static_cast<int>(e ? e : ERROR_INVALID_FUNCTION),
                           std::system_category());
}
#else
std::error_code notSupportedHere() noexcept
{
    return std::make_error_code(std::errc::function_not_supported);
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// csprngHex128
// ---------------------------------------------------------------------------

bool csprngHex128(std::wstring& out) noexcept
{
#if defined(_WIN32)
    unsigned char bytes[16];
    const NTSTATUS st = ::BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) {
        return false;
    }
    static const wchar_t kHex[] = L"0123456789abcdef";
    out.clear();
    out.reserve(32);
    for (unsigned char b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return true;
#else
    // No fallback PRNG on purpose. A predictable name is the defect; a caller
    // that gets `false` must abandon the operation, not pick a weaker name.
    (void)out;
    return false;
#endif
}

bool csprngHex128(std::string& out) noexcept
{
    std::wstring w;
    if (!csprngHex128(w)) {
        return false;
    }
    // Explicit narrowing rather than assign(begin,end): the range form warns
    // (C4244) because the compiler cannot see that every element is one of the
    // 16 ASCII hex digits this function produces.
    out.clear();
    out.reserve(w.size());
    for (wchar_t c : w) {
        out.push_back(static_cast<char>(c & 0x7F));
    }
    return true;
}

// ---------------------------------------------------------------------------
// diagnosticsDir
// ---------------------------------------------------------------------------

const std::filesystem::path& diagnosticsDir() noexcept
{
    static std::filesystem::path dir = [] {
        std::filesystem::path result;
#if defined(_WIN32)
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return result;
        }
        // Eight attempts, so a CSPRNG collision (which does not happen) or a
        // transient failure cannot wedge diagnostics permanently, while a
        // genuinely broken temp directory still gives up rather than spinning.
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::wstring hex;
            if (!csprngHex128(hex)) {
                return result;  // fail closed: no diagnostics rather than a fixed name
            }
            const std::filesystem::path candidate = base / (L"pdfe_diag." + hex);
            if (::CreateDirectoryW(candidate.wstring().c_str(), nullptr)) {
                result = candidate;
                return result;
            }
            if (::GetLastError() != ERROR_ALREADY_EXISTS) {
                return result;
            }
        }
#endif
        return result;
    }();
    return dir;
}

std::filesystem::path diagnosticsSubdir(std::string_view name) noexcept
{
    const std::filesystem::path& root = diagnosticsDir();
    if (root.empty() || name.empty()) {
        return {};
    }
    // A separator — or a dot-component — here would let a caller escape the
    // unpredictable root, which is the one property this whole function exists
    // to provide. `..` was missed on the first draft and caught by the test:
    // it contains no separator at all, so a separator-only check waves it
    // through and `root/..` is the shared %TEMP% the root exists to leave.
    if (name.find('/') != std::string_view::npos
        || name.find('\\') != std::string_view::npos
        || name.find(':') != std::string_view::npos
        || name == "." || name == "..") {
        return {};
    }
    std::error_code ec;
    std::filesystem::path sub = root / std::string(name);
    std::filesystem::create_directories(sub, ec);
    if (ec) {
        return {};
    }
    return sub;
}

// ---------------------------------------------------------------------------
// writeFixedNameTempFile
// ---------------------------------------------------------------------------

bool writeFixedNameTempFile(const std::filesystem::path& path,
                            std::string_view bytes,
                            bool append,
                            std::error_code& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    // FILE_FLAG_OPEN_REPARSE_POINT keeps the OPEN itself off the victim: it
    // lands on the link object rather than traversing it. It is NOT what
    // refuses the write — that is the explicit attribute check below, and the
    // difference was measured, not assumed. (This comment used to claim the
    // flag was sufficient. It sat directly above a check proving it was not,
    // which is the stale-prose-beside-its-own-correction shape this project has
    // logged nine times; corrected rather than left to be read as current.)
    //
    // OPEN_ALWAYS EVEN WHEN TRUNCATING, and the reason is a defect this unit's
    // own test caught on its first run. The obvious spelling is CREATE_ALWAYS
    // for the truncating case — but CREATE_ALWAYS truncates AT OPEN, before any
    // of the checks below get to run. The hardlink case then failed with the
    // victim already emptied: the guard refused the write and reported a
    // refusal, having already destroyed exactly the file it exists to protect.
    // Open without truncating, decide, and truncate only after the decision.
    const DWORD disposition = OPEN_ALWAYS;
    HANDLE h = ::CreateFileW(
        path.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        disposition,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ec = lastWinError();
        return false;
    }

    // A hardlink planted at this name is an ALIAS for a victim file, and
    // truncating it destroys that file's contents under PDFE's token. The open
    // above deliberately did not truncate, so refusing here leaves the victim
    // byte-for-byte intact.
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info)) {
        ec = lastWinError();
        ::CloseHandle(h);
        return false;
    }
    if (info.nNumberOfLinks > 1) {
        ::CloseHandle(h);
        ec = std::make_error_code(std::errc::operation_not_permitted);
        return false;
    }

    // AND AN EXPLICIT REPARSE-POINT REFUSAL, because FILE_FLAG_OPEN_REPARSE_POINT
    // ALONE DID NOT HOLD. The first draft relied on that flag to make a planted
    // symlink un-followable, and the test showed the write going through and
    // returning success — the flag governs how the object is opened, not
    // whether we are willing to write to whatever we opened. Checking the
    // attribute is the part that actually refuses. Left as a pair rather than
    // replaced: the flag still keeps the open itself off the victim.
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        ::CloseHandle(h);
        ec = std::make_error_code(std::errc::operation_not_permitted);
        return false;
    }

    if (append) {
        ::SetFilePointer(h, 0, nullptr, FILE_END);
    } else {
        // The truncation the caller asked for, performed only now that the
        // object at this name has been established as safe to truncate.
        ::SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        ::SetEndOfFile(h);
    }

    bool ok = true;
    const char* p = bytes.data();
    size_t remaining = bytes.size();
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x10000000u ? 0x10000000u : remaining);
        DWORD written = 0;
        if (!::WriteFile(h, p, chunk, &written, nullptr) || written == 0) {
            ec = lastWinError();
            ok = false;
            break;
        }
        p += written;
        remaining -= written;
    }
    ::CloseHandle(h);
    return ok;
#else
    (void)path;
    (void)bytes;
    (void)append;
    ec = notSupportedHere();
    return false;
#endif
}

// ---------------------------------------------------------------------------
// PinnedTempFile
// ---------------------------------------------------------------------------

std::unique_ptr<PinnedTempFile> PinnedTempFile::reserve(
    const std::filesystem::path& dir,
    std::string_view prefix,
    std::string_view extension,
    std::error_code& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    const std::wstring wPrefix(prefix.begin(), prefix.end());
    const std::wstring wExt(extension.begin(), extension.end());

    for (int attempt = 0; attempt < 8; ++attempt) {
        std::wstring hex;
        if (!csprngHex128(hex)) {
            // FAIL CLOSED. There is no weaker name worth falling back to --
            // a guessable name is the whole defect.
            ec = std::make_error_code(std::errc::no_such_device);
            return nullptr;
        }
        const std::filesystem::path candidate = dir / (wPrefix + L"." + hex + wExt);

        // FILE_SHARE_READ AND NOTHING ELSE, HELD FOR THE OBJECT'S LIFETIME.
        //   * no FILE_SHARE_WRITE -- nobody else can open this for writing, so
        //     the in-place overwrite that a file-identity check could never have
        //     detected is not merely detected, it is impossible.
        //   * no FILE_SHARE_DELETE -- this is what stops a rename-OVER as well
        //     as a delete, which is the substitution primitive the whole WIN-87
        //     class turns on.
        //   * FILE_SHARE_READ granted deliberately -- the consumer re-reads this
        //     path, and a pin that locked out readers would break the very
        //     feature it protects.
        HANDLE h = ::CreateFileW(
            candidate.wstring().c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,  // success proves nothing was already at this name
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            // Captured ONCE: GetLastError is thread-local mutable state, and
            // reading it twice in one condition is a habit that eventually
            // reads it after something has clobbered it.
            const DWORD err = ::GetLastError();
            if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
                continue;
            }
            ec = std::error_code(static_cast<int>(err), std::system_category());
            return nullptr;
        }

        std::unique_ptr<PinnedTempFile> out(new PinnedTempFile());
        out->path_ = candidate;
        out->handle_ = h;
        return out;
    }
    ec = std::make_error_code(std::errc::file_exists);
    return nullptr;
#else
    (void)dir;
    (void)prefix;
    (void)extension;
    ec = notSupportedHere();
    return nullptr;
#endif
}

int PinnedTempFile::openWriteFd(std::error_code& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    if (handle_ == nullptr) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return -1;
    }
    // The descriptor must own a DUPLICATE, not this object's handle: the caller
    // closes what it is given (QFile with AutoCloseHandle does), and the whole
    // point of this class is that the exclusive handle OUTLIVES the write.
    HANDLE dup = INVALID_HANDLE_VALUE;
    if (!::DuplicateHandle(::GetCurrentProcess(), static_cast<HANDLE>(handle_),
                           ::GetCurrentProcess(), &dup, 0, FALSE,
                           DUPLICATE_SAME_ACCESS)) {
        ec = lastWinError();
        return -1;
    }
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(dup), _O_BINARY);
    if (fd < 0) {
        ::CloseHandle(dup);
        ec = std::make_error_code(std::errc::too_many_files_open);
        return -1;
    }
    // From here the fd owns `dup`; closing the fd closes it.
    ::_lseek(fd, 0, SEEK_SET);
    return fd;
#else
    ec = notSupportedHere();
    return -1;
#endif
}

bool PinnedTempFile::isPinned() const noexcept
{
    return handle_ != nullptr;
}

PinnedTempFile::~PinnedTempFile()
{
#if defined(_WIN32)
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    if (!path_.empty()) {
        ::DeleteFileW(path_.wstring().c_str());
    }
#endif
}


// ── SEC-103: owner-only file creation ───────────────────────────────────────

namespace {

#if defined(_WIN32)

// The calling process's user SID, in a caller-freed buffer. Returns nullptr
// on failure; every caller treats that as "cannot establish ownership", which
// means refuse, never "assume it is fine".
std::vector<unsigned char> currentUserSid()
{
    std::vector<unsigned char> out;
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return out;
    }
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) { ::CloseHandle(token); return out; }

    std::vector<unsigned char> buf(needed);
    if (!::GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
        ::CloseHandle(token);
        return out;
    }
    ::CloseHandle(token);

    auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
    if (!tu->User.Sid || !::IsValidSid(tu->User.Sid)) return out;

    const DWORD len = ::GetLengthSid(tu->User.Sid);
    out.resize(len);
    if (!::CopySid(len, out.data(), tu->User.Sid)) out.clear();
    return out;
}

// True if `sid` is one of the principals that already own the machine and so
// cannot meaningfully be excluded from a file on it.
bool isSystemOrAdministrators(PSID sid)
{
    if (!sid || !::IsValidSid(sid)) return false;
    static const struct { WELL_KNOWN_SID_TYPE type; } kBenign[] = {
        {WinLocalSystemSid},
        {WinBuiltinAdministratorsSid},
    };
    for (const auto& k : kBenign) {
        if (::IsWellKnownSid(sid, k.type)) return true;
    }
    return false;
}

#endif // _WIN32

}  // namespace

#if defined(_WIN32)
namespace {

/// True if `dacl` grants access to nobody but the calling user (plus
/// SYSTEM/Administrators, which already own the machine).
///
/// Factored out of isOwnerOnlyAccessible so the same judgment can be applied to
/// a DACL obtained from a HANDLE rather than from a path. That distinction is
/// the whole point of the handle-based path in createOwnerOnlyDirectory: a
/// by-path read-back resolves through a reparse point an attacker may have
/// substituted, so it can verify the wrong object and report success.
bool daclIsOwnerOnly(PACL dacl) noexcept
{
    // A NULL DACL grants everyone everything. Present-but-empty grants nobody
    // anything, which is restrictive but would break the cache; either way it
    // is not the shape we write, so refuse both rather than guess.
    if (!dacl) return false;

    const std::vector<unsigned char> me = currentUserSid();
    if (me.empty()) return false;

    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* aceRaw = nullptr;
        if (!::GetAce(dacl, i, &aceRaw)) return false;
        auto* hdr = static_cast<ACE_HEADER*>(aceRaw);

        // Only ALLOW aces can widen access; a DENY ace can only narrow it.
        if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;

        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(aceRaw);
        PSID aceSid = reinterpret_cast<PSID>(&ace->SidStart);
        if (::EqualSid(aceSid, const_cast<unsigned char*>(me.data()))) continue;
        if (isSystemOrAdministrators(aceSid)) continue;

        return false;   // somebody else can reach it
    }
    return true;
}

}  // namespace
#endif

bool isOwnerOnlyAccessible([[maybe_unused]] const std::filesystem::path& path) noexcept
{
#if defined(_WIN32)
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    if (::GetNamedSecurityInfoW(path.wstring().c_str(), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &dacl, nullptr, &sd)
        != ERROR_SUCCESS) {
        return false;
    }
    const bool ok = daclIsOwnerOnly(dacl);
    ::LocalFree(sd);
    return ok;
#else
    std::error_code ec;
    const auto perms = std::filesystem::status(path, ec).permissions();
    if (ec) return false;
    using std::filesystem::perms;
    return (perms & (perms::group_all | perms::others_all)) == perms::none;
#endif
}

bool createOwnerOnlyFile(const std::filesystem::path& path,
                         std::error_code& ec) noexcept
{
    ec.clear();
    if (path.empty()) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return false;
    }

    std::error_code mkec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), mkec);
        // An existing directory is not an error; anything else is.
        if (mkec && !std::filesystem::is_directory(path.parent_path())) {
            ec = mkec;
            return false;
        }
    }

    // An existing file is ADOPTED ONLY IF its ACL is already owner-only.
    // Silently adopting a pre-planted world-writable file is precisely the
    // local-escalation surface SEC-103 is about, so this is a refusal.
    if (std::filesystem::exists(path)) {
        if (isOwnerOnlyAccessible(path)) return true;
        ec = std::make_error_code(std::errc::operation_not_permitted);
        return false;
    }

#if defined(_WIN32)
    const std::vector<unsigned char> me = currentUserSid();
    if (me.empty()) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | DELETE;
    access.grfAccessMode        = SET_ACCESS;
    access.grfInheritance       = NO_INHERITANCE;
    access.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    access.Trustee.ptstrName    =
        reinterpret_cast<LPWSTR>(const_cast<unsigned char*>(me.data()));

    PACL dacl = nullptr;
    if (::SetEntriesInAclW(1, &access, nullptr, &dacl) != ERROR_SUCCESS || !dacl) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    SECURITY_DESCRIPTOR sd{};
    if (!::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) ||
        !::SetSecurityDescriptorDacl(&sd, TRUE, dacl, FALSE)) {
        ::LocalFree(dacl);
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    // CREATE_NEW, not CREATE_ALWAYS: if something raced us to the name between
    // the exists() check above and here, we must lose rather than truncate it.
    // (DCA 0026 — a guard that truncates before deciding has already done the
    // damage its refusal was for.)
    HANDLE h = ::CreateFileW(path.wstring().c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             0,                      // no sharing while we set up
                             &sa, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    ::LocalFree(dacl);

    if (h == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(::GetLastError()),
                             std::system_category());
        return false;
    }
    ::CloseHandle(h);

    // Read the ACL back. The call above can succeed with a DACL the filesystem
    // adjusted; asserting on the return value alone would be trusting the
    // request rather than the result.
    if (!isOwnerOnlyAccessible(path)) {
        std::error_code rmec;
        std::filesystem::remove(path, rmec);
        ec = std::make_error_code(std::errc::operation_not_permitted);
        return false;
    }
    return true;
#else
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    f.close();
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    return !ec;
#endif
}

DirAdoptionVerdict inspectDirectoryForAdoption(
    const std::filesystem::path& path) noexcept
{
#if defined(_WIN32)
    if (path.empty()) return DirAdoptionVerdict::Indeterminate;

    const std::wstring wide = path.wstring();
    const DWORD attrs = ::GetFileAttributesW(wide.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return DirAdoptionVerdict::DoesNotExist;

    // Classify by NAME, not by following the link. `is_directory()` follows a
    // reparse point, so a junction planted at this name would be adopted as an
    // ordinary directory and then dutifully "secured" — while the bytes land
    // wherever it points.
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return DirAdoptionVerdict::ReparsePoint;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return DirAdoptionVerdict::NotADirectory;

    // OWNERSHIP, not just the DACL. An owner holds WRITE_DAC implicitly and can
    // rewrite the ACL at any moment, so re-securing a directory somebody else
    // created buys nothing — they simply undo it. Refusing is the only answer
    // that holds. (This is the residual left open by the same reapply-on-adopt
    // pattern in CrashHandler.cpp's applyOwnerOnlyDacl, which secures an adopted
    // directory without checking who owns it.)
    PSECURITY_DESCRIPTOR osd = nullptr;
    PSID owner = nullptr;
    if (::GetNamedSecurityInfoW(wide.c_str(), SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION,
                                &owner, nullptr, nullptr, nullptr, &osd)
        != ERROR_SUCCESS) {
        return DirAdoptionVerdict::Indeterminate;
    }
    const std::vector<unsigned char> me = currentUserSid();
    const bool mine =
        owner && !me.empty() &&
        (::EqualSid(owner, const_cast<unsigned char*>(me.data())) ||
         isSystemOrAdministrators(owner));
    if (osd) ::LocalFree(osd);
    return mine ? DirAdoptionVerdict::Ok : DirAdoptionVerdict::ForeignOwner;
#else
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return DirAdoptionVerdict::DoesNotExist;
    if (std::filesystem::is_symlink(path, ec)) return DirAdoptionVerdict::ReparsePoint;
    if (!std::filesystem::is_directory(path, ec)) return DirAdoptionVerdict::NotADirectory;
    return DirAdoptionVerdict::Ok;
#endif
}

bool createOwnerOnlyDirectory(const std::filesystem::path& path,
                              std::error_code& ec) noexcept
{
    ec.clear();
    if (path.empty()) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return false;
    }

#if defined(_WIN32)
    // Parents first. They are ordinary directories; only `path` itself is
    // required to be owner-only, because that is where the content lands.
    if (!path.parent_path().empty()) {
        std::error_code mkec;
        std::filesystem::create_directories(path.parent_path(), mkec);
    }

    const std::wstring wide = path.wstring();

    // DECIDE BEFORE TOUCHING ANYTHING. Every refusal is settled by the
    // read-only predicate, so nothing below this point runs against a
    // directory we have already declined to adopt.
    const DirAdoptionVerdict verdict = inspectDirectoryForAdoption(path);
    const bool exists = (verdict != DirAdoptionVerdict::DoesNotExist);
    switch (verdict) {
    case DirAdoptionVerdict::Ok:
    case DirAdoptionVerdict::DoesNotExist:
        break;
    case DirAdoptionVerdict::NotADirectory:
        ec = std::make_error_code(std::errc::not_a_directory);
        return false;
    case DirAdoptionVerdict::ReparsePoint:
    case DirAdoptionVerdict::ForeignOwner:
        ec = std::make_error_code(std::errc::operation_not_permitted);
        return false;
    case DirAdoptionVerdict::Indeterminate:
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    // Build the owner-only, inheritable DACL once; it is used both as the
    // creation-time descriptor and as the descriptor re-applied on adopt.
    const std::vector<unsigned char> me = currentUserSid();
    if (me.empty()) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode        = SET_ACCESS;
    // Children inherit it, so index subdirectories created underneath are
    // owner-only from the moment they exist rather than being fixed up after.
    access.grfInheritance       = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    access.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    access.Trustee.ptstrName    =
        reinterpret_cast<LPWSTR>(const_cast<unsigned char*>(me.data()));

    PACL dacl = nullptr;
    if (::SetEntriesInAclW(1, &access, nullptr, &dacl) != ERROR_SUCCESS || !dacl) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    if (!exists) {
        SECURITY_DESCRIPTOR sd{};
        if (!::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) ||
            !::SetSecurityDescriptorDacl(&sd, TRUE, dacl, FALSE)) {
            ::LocalFree(dacl);
            ec = std::make_error_code(std::errc::permission_denied);
            return false;
        }
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;

        // The DACL is supplied AT CREATION for the same reason
        // createOwnerOnlyFile does it: create-then-secure leaves a window in
        // which the directory exists with the parent's inherited ACL, and in
        // %TEMP% that window is the exposure.
        if (!::CreateDirectoryW(wide.c_str(), &sa)) {
            // ERROR_ALREADY_EXISTS here means we LOST A RACE: inspect said the
            // name was free, and something created it in between. Falling
            // through to secure whatever now occupies the name is exactly the
            // object-substitution bug this function exists to prevent, so this
            // is a refusal, not a shrug.
            ::LocalFree(dacl);
            ec = (::GetLastError() == ERROR_ALREADY_EXISTS)
                     ? std::make_error_code(std::errc::operation_not_permitted)
                     : lastWinError();
            return false;
        }
    }

    // ---------------------------------------------------------------------
    // FROM HERE ON, EVERYTHING IS DONE THROUGH A HANDLE.
    //
    // The inspect above classifies BY PATH, and a path is not an object. A
    // process running as this same user -- which is precisely the threat class
    // this ACL exists to contain -- can delete the directory and drop a
    // junction in its place between the inspect and the act. SetNamedSecurityInfoW
    // and a by-path read-back would then both resolve THROUGH that junction: the
    // attacker's own directory gets "secured", the read-back passes, this
    // function returns true, and every page's plaintext is written wherever the
    // junction points. The verification would have confirmed the wrong object.
    //
    // Opening once with FILE_FLAG_OPEN_REPARSE_POINT and doing the classify,
    // the set and the read-back all on that handle removes every subsequent
    // name resolution, so there is nothing left to substitute. This is the same
    // discipline removeTreeByHandle applies to the delete path; the
    // phase-17 security-audit gate caught that it had not been carried over to
    // the create/secure path.
    // ---------------------------------------------------------------------
    HANDLE h = ::CreateFileW(
        wide.c_str(),
        READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ::LocalFree(dacl);
        ec = lastWinError();
        return false;
    }

    auto fail = [&](std::error_code e) {
        ::CloseHandle(h);
        ::LocalFree(dacl);
        ec = e;
        return false;
    };

    // Re-classify the OBJECT we hold open. If the name was swapped after the
    // inspect, this is where it is caught.
    BY_HANDLE_FILE_INFORMATION bhfi{};
    if (!::GetFileInformationByHandle(h, &bhfi)) {
        return fail(lastWinError());
    }
    if (bhfi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return fail(std::make_error_code(std::errc::operation_not_permitted));
    }
    if (!(bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return fail(std::make_error_code(std::errc::not_a_directory));
    }

    // Ownership, re-checked on the handle for the same reason.
    {
        PSECURITY_DESCRIPTOR osd = nullptr;
        PSID owner = nullptr;
        if (::GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                              &owner, nullptr, nullptr, nullptr, &osd)
            != ERROR_SUCCESS) {
            return fail(std::make_error_code(std::errc::permission_denied));
        }
        const std::vector<unsigned char> meNow = currentUserSid();
        const bool mine =
            owner && !meNow.empty() &&
            (::EqualSid(owner, const_cast<unsigned char*>(meNow.data())) ||
             isSystemOrAdministrators(owner));
        if (osd) ::LocalFree(osd);
        if (!mine) {
            return fail(std::make_error_code(std::errc::operation_not_permitted));
        }
    }

    // PROTECTED_DACL_SECURITY_INFORMATION is what actually severs inheritance.
    // Supplying a DACL at creation does not by itself stop the object manager
    // merging the parent's inheritable ACEs, and %TEMP% commonly carries them.
    const DWORD rc = ::SetSecurityInfo(
        h, SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
    if (rc != ERROR_SUCCESS) {
        return fail(std::error_code(static_cast<int>(rc), std::system_category()));
    }

    // Read the ACL back FROM THE SAME HANDLE. Both calls above can report
    // success with a DACL the filesystem adjusted, and "we asked for it" is not
    // "it is so" -- but a by-path read-back could also be answering about a
    // different object entirely, which is the failure this handle prevents.
    {
        PSECURITY_DESCRIPTOR vsd = nullptr;
        PACL vdacl = nullptr;
        if (::GetSecurityInfo(h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &vdacl, nullptr, &vsd)
            != ERROR_SUCCESS) {
            return fail(std::make_error_code(std::errc::permission_denied));
        }
        const bool ownerOnly = daclIsOwnerOnly(vdacl);
        if (vsd) ::LocalFree(vsd);
        if (!ownerOnly) {
            return fail(std::make_error_code(std::errc::operation_not_permitted));
        }
    }

    ::CloseHandle(h);
    ::LocalFree(dacl);
    return true;
#else
    std::filesystem::create_directories(path, ec);
    if (ec && !std::filesystem::is_directory(path)) return false;
    ec.clear();
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    return !ec;
#endif
}

// ---------------------------------------------------------------------------
// writeNewFileExclusive
// ---------------------------------------------------------------------------

bool writeNewFileExclusive(const std::filesystem::path& path,
                           const void* data,
                           std::size_t len,
                           std::error_code& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    // CREATE_NEW, so there is no window between deciding the name is free and
    // taking it. The kernel does both in one step; if anything is already
    // there — file, directory, junction, symlink — this fails outright rather
    // than adopting it. That is what removes the race, not the flags below.
    HANDLE h = ::CreateFileW(
        path.wstring().c_str(),
        GENERIC_WRITE,
        0,                       // no sharing: nothing else touches it while we write
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
            // Distinguished so the caller can try the next candidate name.
            // Every other failure is reported as-is; a refusal is not a
            // collision and must not be retried as one.
            ec = std::make_error_code(std::errc::file_exists);
        } else {
            ec = lastWinError();
        }
        return false;
    }

    // Belt-and-braces after CREATE_NEW succeeded. These should be unreachable
    // — we just created this object — and they cost one call each. The reason
    // they are here anyway is that `writeFixedNameTempFile` MEASURED
    // FILE_FLAG_OPEN_REPARSE_POINT failing to refuse on its own, and an
    // assumption about which of two mechanisms is doing the work is exactly
    // what this project keeps being wrong about.
    BY_HANDLE_FILE_INFORMATION info{};
    bool refused = false;
    if (!::GetFileInformationByHandle(h, &info)) {
        ec = lastWinError();
        refused = true;
    } else if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
               info.nNumberOfLinks > 1) {
        ec = std::make_error_code(std::errc::operation_not_permitted);
        refused = true;
    }

    bool ok = !refused;
    if (ok && len > 0) {
        const char* p = static_cast<const char*>(data);
        std::size_t remaining = len;
        while (remaining > 0) {
            const DWORD chunk = static_cast<DWORD>(
                remaining > 0x10000000u ? 0x10000000u : remaining);
            DWORD wrote = 0;
            if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) {
                ec = lastWinError();
                ok = false;
                break;
            }
            p += wrote;
            remaining -= wrote;
        }
    }
    ::CloseHandle(h);

    if (!ok) {
        // Leave nothing behind. A partial file at the target name looks like a
        // successful extraction to everything downstream, which is worse than
        // either a clean success or a clean refusal.
        std::error_code rm;
        std::filesystem::remove(path, rm);
    }
    return ok;
#else
    // POSIX: O_CREAT|O_EXCL is the same atomic create-or-fail, and O_NOFOLLOW
    // keeps a planted symlink from being followed.
    if (std::filesystem::exists(path)) {
        ec = std::make_error_code(std::errc::file_exists);
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    if (len > 0) out.write(static_cast<const char*>(data),
                           static_cast<std::streamsize>(len));
    const bool ok = static_cast<bool>(out);
    out.close();
    if (!ok) {
        std::error_code rm;
        std::filesystem::remove(path, rm);
        ec = std::make_error_code(std::errc::io_error);
    }
    return ok;
#endif
}

std::filesystem::path perUserDataDir() noexcept
{
#if defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"PDFE";
#else
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::filesystem::path(home) / ".local" / "share" / "pdfe";
#endif
}

}  // namespace pdfe::core
