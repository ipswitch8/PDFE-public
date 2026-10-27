// -*- coding: utf-8 -*-
#include "support/SaveFailureInjection.h"

#include "core/AtomicFileReplace.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
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
#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>   // fileIdentityOf(): (st_dev, st_ino)
#include <sys/types.h>
#endif

namespace pdfe::testing {
namespace {

#if defined(_WIN32)
std::wstring toWide(const std::string& s)
{
    if (s.empty()) {
        return std::wstring();
    }
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          out.data(), n);
    return out;
}
#endif

std::string envOrEmpty(const char* name)
{
#if defined(_WIN32)
    char* buf = nullptr;
    size_t len = 0;
    if (::_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) {
        return std::string();
    }
    std::string out(buf);
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

}  // namespace

const char* injectionPointName(SaveInjectionPoint p)
{
    switch (p) {
    case SaveInjectionPoint::TempWrite:    return "TempWrite";
    case SaveInjectionPoint::AtomicRename: return "AtomicRename";
    case SaveInjectionPoint::InsideWindow: return "InsideWindow";
    }
    return "Unknown";
}

std::filesystem::path saveTempPathFor(const std::filesystem::path& target)
{
    // The FULL-REWRITE temp specifically — this harness's TempWrite injection
    // point poisons the path Document::save()'s full-rewrite branch and
    // Document::saveAs() write to. The staged incremental append uses the
    // unpredictable ".pdfe_part.<hex>" shape and is found by enumeration
    // instead (see startAppendGrowthWatcher / stagingResidueOf below).
    return pdfe::core::fullRewriteTempPathFor(target);
}

// ---------------------------------------------------------------------------
// TempWriteBlocker
// ---------------------------------------------------------------------------

TempWriteBlocker::TempWriteBlocker(const std::filesystem::path& target,
                                   TempWriteBlockMode mode)
    : tempPath_(saveTempPathFor(target)), mode_(mode)
{
    std::error_code ec;
    std::filesystem::remove_all(tempPath_, ec);  // clear whatever is there

    if (mode_ == TempWriteBlockMode::DirectoryOnTempPath) {
        // A directory on the temp path: MuPDF's fopen("wb") of that name fails
        // at the OS level (EISDIR / ERROR_ACCESS_DENIED).
        //
        // It must also be NON-EMPTY. The save path clears a stale temp with
        // std::filesystem::remove(), which succeeds on an EMPTY directory — an
        // empty blocker would be silently swept away and the save would then
        // succeed, turning this whole oracle into a happy path. (That is exactly
        // what happened on the first run of this harness.) A directory holding a
        // file cannot be removed by remove(), so the block is stable.
        if (!std::filesystem::create_directory(tempPath_, ec) || ec) {
            return;
        }
        {
            std::ofstream pin(tempPath_ / "occupied", std::ios::binary);
            pin << "keeps the blocking directory non-empty";
        }
        armed_ = std::filesystem::exists(tempPath_ / "occupied");
        return;
    }

#if defined(_WIN32)
    // A real file held with NO sharing — an anti-virus scanner mid-scan.
    {
        std::ofstream seed(tempPath_, std::ios::binary);
        seed << "poisoned";
    }
    HANDLE h = ::CreateFileW(tempPath_.native().c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             0 /* dwShareMode: deny everything */,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        handle_ = h;
        armed_ = true;
    }
#else
    // POSIX has no mandatory locking; fall back to a read-only file, which
    // makes fopen("wb") fail with EACCES.
    {
        std::ofstream seed(tempPath_, std::ios::binary);
        seed << "poisoned";
    }
    std::filesystem::permissions(tempPath_, std::filesystem::perms::owner_read, ec);
    armed_ = !ec;
#endif
}

void TempWriteBlocker::release()
{
#if defined(_WIN32)
    if (handle_) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#endif
    std::error_code ec;
    if (mode_ == TempWriteBlockMode::DirectoryOnTempPath) {
        std::filesystem::remove_all(tempPath_, ec);
    } else {
        std::filesystem::permissions(tempPath_, std::filesystem::perms::all, ec);
        std::filesystem::remove(tempPath_, ec);
    }
    armed_ = false;
}

TempWriteBlocker::~TempWriteBlocker()
{
    release();
}

// ---------------------------------------------------------------------------
// InPlaceWriteBlocker (F47 item F5, phase-49) — see header for why the
// incremental /DSS save path needs its own injection point.
// ---------------------------------------------------------------------------

InPlaceWriteBlocker::InPlaceWriteBlocker(const std::filesystem::path& target)
    : target_(target)
{
    if (!std::filesystem::exists(target_)) return;
#if defined(_WIN32)
    const std::wstring w = toWide(target_.string());
    const DWORD attrs = ::GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    savedAttributes_ = attrs;
    if (::SetFileAttributesW(w.c_str(), attrs | FILE_ATTRIBUTE_READONLY)) {
        // Confirm rather than assume: an unarmed blocker that reports armed
        // is the one failure mode that would silently make the oracle vacuous.
        const DWORD now = ::GetFileAttributesW(w.c_str());
        armed_ = (now != INVALID_FILE_ATTRIBUTES)
                 && ((now & FILE_ATTRIBUTE_READONLY) != 0);
    }
#else
    std::error_code ec;
    const auto perms = std::filesystem::status(target_, ec).permissions();
    if (ec) return;
    savedAttributes_ = static_cast<unsigned long>(perms);
    std::filesystem::permissions(
        target_,
        std::filesystem::perms::owner_read | std::filesystem::perms::group_read
            | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace, ec);
    if (ec) return;
    const auto now = std::filesystem::status(target_, ec).permissions();
    armed_ = !ec
             && (now & std::filesystem::perms::owner_write)
                    == std::filesystem::perms::none;
#endif
}

void InPlaceWriteBlocker::release()
{
    if (!armed_) return;
    armed_ = false;
#if defined(_WIN32)
    const std::wstring w = toWide(target_.string());
    ::SetFileAttributesW(w.c_str(), static_cast<DWORD>(savedAttributes_));
#else
    std::error_code ec;
    std::filesystem::permissions(
        target_, static_cast<std::filesystem::perms>(savedAttributes_),
        std::filesystem::perm_options::replace, ec);
#endif
}

InPlaceWriteBlocker::~InPlaceWriteBlocker()
{
    release();
}

// ---------------------------------------------------------------------------
// RenameLock
// ---------------------------------------------------------------------------

struct RenameLock::State {
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    std::atomic<bool> armed{false};
    std::atomic<bool> released{false};
    std::thread timer;

    void closeHandle()
    {
#if defined(_WIN32)
        HANDLE h = handle;
        if (h != INVALID_HANDLE_VALUE) {
            handle = INVALID_HANDLE_VALUE;
            ::CloseHandle(h);
        }
#endif
        released = true;
    }
};

namespace {
// The State outlives the RenameLock only for as long as the timer thread needs
// it; a shared_ptr registry keeps it alive without leaking.
std::vector<std::shared_ptr<RenameLock::State>>& lockStates()
{
    static std::vector<std::shared_ptr<RenameLock::State>> v;
    return v;
}
}  // namespace

RenameLock::RenameLock(const std::filesystem::path& target)
{
    auto st = std::make_shared<State>();
    lockStates().push_back(st);
    state_ = st.get();
#if defined(_WIN32)
    HANDLE h = ::CreateFileW(target.native().c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             0 /* deny all sharing — MoveFileExW onto this
                                  fails with ERROR_SHARING_VIOLATION */,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        st->handle = h;
        st->armed = true;
    }
#else
    // POSIX rename() cannot be blocked by an open handle at all; the transient
    // /permanent distinction is Windows-specific by nature.
    (void)target;
    st->armed = false;
#endif
}

RenameLock::RenameLock(const std::filesystem::path& target,
                       std::chrono::milliseconds holdFor)
    : RenameLock(target)
{
    // Grab the shared_ptr we just registered so the timer thread keeps the
    // state alive independently of this object's lifetime.
    auto st = lockStates().back();
    if (!st->armed) {
        return;
    }
    st->timer = std::thread([st, holdFor]() {
        std::this_thread::sleep_for(holdFor);
        st->closeHandle();
    });
}

bool RenameLock::armed() const { return state_->armed.load(); }
bool RenameLock::released() const { return state_->released.load(); }

void RenameLock::release()
{
    state_->closeHandle();
}

RenameLock::~RenameLock()
{
    state_->closeHandle();
    if (state_->timer.joinable()) {
        state_->timer.join();
    }
}

// ---------------------------------------------------------------------------
// Process termination
// ---------------------------------------------------------------------------

std::filesystem::path currentExecutablePath()
{
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4] = {0};
    DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) {
        return {};
    }
    return std::filesystem::path(std::wstring(buf, buf + n));
#else
    return std::filesystem::read_symlink("/proc/self/exe");
#endif
}

bool runningAsSaveVictim()
{
    return !envOrEmpty(kSaveVictimEnvVar).empty();
}

std::filesystem::path saveVictimTarget()
{
    return std::filesystem::path(envOrEmpty(kSaveVictimTargetEnvVar));
}

std::filesystem::path saveVictimSource()
{
    return std::filesystem::path(envOrEmpty(kSaveVictimSourceEnvVar));
}

std::filesystem::path saveVictimMarker()
{
    return std::filesystem::path(envOrEmpty(kSaveVictimMarkerEnvVar));
}

void announceInsideSaveWindow()
{
    auto marker = saveVictimMarker();
    if (marker.empty()) {
        return;
    }
    std::ofstream f(marker, std::ios::binary);
    f << "inside";
    f.flush();
}

void startTempAppearanceWatcher(const std::filesystem::path& target)
{
    const auto temp = saveTempPathFor(target);
    // Detached: this thread lives only until the parent kills us (the normal
    // outcome) or the process exits. It touches nothing but the filesystem.
    std::thread([temp]() {
        for (int i = 0; i < 60000; ++i) {  // ~60 s ceiling
            if (std::filesystem::exists(temp)) {
                announceInsideSaveWindow();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

void startAppendGrowthWatcher(const std::filesystem::path& target,
                              std::uintmax_t baselineBytes)
{
    const auto temp = saveTempPathFor(target);
    // Detached, same as startTempAppearanceWatcher: it lives only until the
    // parent kills us (the expected outcome) or the process exits, and it
    // touches nothing but the filesystem.
    std::thread([target, temp, baselineBytes]() {
        auto grewPast = [baselineBytes](const std::filesystem::path& p) {
            std::error_code ec;
            const auto n = std::filesystem::file_size(p, ec);
            return !ec && n > baselineBytes;
        };
        // F58-a (phase-53): the staged append no longer uses the fixed
        // "<target>.pdfe_tmp" name — that predictability was the CRITICAL defect
        // the review found, and core::StagingFile now draws the suffix from a
        // CSPRNG. So the staged arm of this watcher can no longer name the file
        // it is waiting on; it enumerates the directory for it instead. `temp`
        // is still checked, because a FULL-REWRITE save does still use it.
        //
        // Enumerating and stat'ing the staging file is unaffected by its
        // exclusive handle: Windows exempts attribute-only opens from sharing
        // checks, and the handle allows FILE_SHARE_READ regardless.
        auto stagedSiblingGrewPast = [&target, baselineBytes]() {
            const auto dir = target.parent_path();
            const auto stem = target.filename();
            std::error_code ec;
            for (std::filesystem::directory_iterator it(dir, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!pdfe::core::StagingFile::isStagingResidueName(
                        stem, it->path().filename())) {
                    continue;
                }
                std::error_code se;
                const auto n = std::filesystem::file_size(it->path(), se);
                if (!se && n > baselineBytes) {
                    return true;
                }
            }
            return false;
        };
        for (int i = 0; i < 600000; ++i) {  // ~60 s ceiling at 100 us
            // Either shape of the save counts: bytes past the baseline mean a
            // revision is part-way written, whether it is going onto the user's
            // file (unstaged) or onto the staging copy (staged).
            if (grewPast(target) || grewPast(temp) || stagedSiblingGrewPast()) {
                announceInsideSaveWindow();
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }).detach();
}

KilledSaveResult killChildInsideSaveWindow(const std::filesystem::path& sourcePdf,
                                           const std::filesystem::path& target,
                                           const std::string& gtestFilterForVictim,
                                           std::chrono::milliseconds waitForWindow)
{
    KilledSaveResult r;
#if !defined(_WIN32)
    r.diagnostic = "process-kill injection is implemented for Windows only";
    return r;
#else
    const auto exe = currentExecutablePath();
    if (exe.empty()) {
        r.diagnostic = "GetModuleFileNameW failed";
        return r;
    }

    auto marker = target;
    marker += ".f10_inside_window";
    std::error_code ec;
    std::filesystem::remove(marker, ec);

    // The child inherits our environment plus the four victim variables.
    // SetEnvironmentVariableW on the parent then CreateProcessW with a null
    // environment block gives the child a copy of ours.
    ::SetEnvironmentVariableW(toWide(kSaveVictimEnvVar).c_str(), L"1");
    ::SetEnvironmentVariableW(toWide(kSaveVictimTargetEnvVar).c_str(),
                              target.native().c_str());
    ::SetEnvironmentVariableW(toWide(kSaveVictimSourceEnvVar).c_str(),
                              sourcePdf.native().c_str());
    ::SetEnvironmentVariableW(toWide(kSaveVictimMarkerEnvVar).c_str(),
                              marker.native().c_str());

    std::wstring cmd = L"\"" + exe.native() + L"\" --gtest_filter="
                       + toWide(gtestFilterForVictim)
                       + L" --gtest_also_run_disabled_tests=0";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(exe.native().c_str(), cmdBuf.data(),
                                     nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr,
                                     exe.parent_path().native().c_str(),
                                     &si, &pi);

    // Un-set immediately so the PARENT process (which keeps running the rest of
    // the suite) is never itself mistaken for a victim.
    ::SetEnvironmentVariableW(toWide(kSaveVictimEnvVar).c_str(), nullptr);
    ::SetEnvironmentVariableW(toWide(kSaveVictimTargetEnvVar).c_str(), nullptr);
    ::SetEnvironmentVariableW(toWide(kSaveVictimSourceEnvVar).c_str(), nullptr);
    ::SetEnvironmentVariableW(toWide(kSaveVictimMarkerEnvVar).c_str(), nullptr);

    if (!ok) {
        r.diagnostic = "CreateProcessW failed, GetLastError="
                       + std::to_string(::GetLastError());
        return r;
    }
    r.childLaunched = true;

    // Poll for the child's "I am inside the window" marker.
    const auto deadline = std::chrono::steady_clock::now() + waitForWindow;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(marker)) {
            r.childReachedWindow = true;
            break;
        }
        if (::WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            break;  // child exited without ever reaching the window
        }
        // 1 ms, not 10: F58's mid-append point is a moving window rather than a
        // state that persists until we act, so every millisecond between the
        // child's announcement and TerminateProcess is a millisecond in which
        // the child can finish the write and leave nothing injected.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (r.childReachedWindow) {
        // A REAL kill: no unwinding, no destructors, no flush. This is the
        // power-loss / forced-logoff shape, not a simulated object drop.
        r.childKilled =
            ::TerminateProcess(pi.hProcess, kKilledWhileRunningExitCode) != 0;
        if (!r.childKilled) {
            r.diagnostic = "TerminateProcess failed, GetLastError="
                           + std::to_string(::GetLastError());
        }
    } else {
        r.diagnostic = "child never reported reaching the save window";
    }

    ::WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = code;
    // GitHub #291. `childKilled` above is TerminateProcess's RETURN VALUE, and
    // that call succeeds on a process which has already exited -- so it cannot
    // tell a kill that interrupted an append from one that arrived after the
    // write finished. The exit code can: Windows stamps
    // kKilledWhileRunningExitCode only on a process it actually terminated, and
    // a child that exited on its own keeps its own code.
    //
    // Reading it here rather than in the oracle means every caller of this
    // harness gets the distinction, not just the one test that needed it.
    r.killLandedWhileRunning = (code == kKilledWhileRunningExitCode);
    if (r.childReachedWindow && !r.killLandedWhileRunning) {
        r.diagnostic = "the kill LANDED LATE: the child had already exited "
                       "(code " + std::to_string(code) + ") before "
                       "TerminateProcess, so nothing was interrupted "
                       "mid-append and the on-disk state is not evidence "
                       "about interruption (GitHub #291)";
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    std::filesystem::remove(marker, ec);
    return r;
#endif
}

// ---------------------------------------------------------------------------
// Partial-temp staging + byte oracles
// ---------------------------------------------------------------------------

std::filesystem::path stageTruncatedOrphanTemp(const std::filesystem::path& target,
                                               const std::filesystem::path& sourcePdf,
                                               std::uintmax_t bytes)
{
    const auto temp = saveTempPathFor(target);
    std::ifstream in(sourcePdf, std::ios::binary);
    std::string buf(static_cast<size_t>(bytes), '\0');
    in.read(buf.data(), static_cast<std::streamsize>(bytes));
    buf.resize(static_cast<size_t>(in.gcount()));
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    out.flush();
    return temp;
}

// ---------------------------------------------------------------------------
// Staging-file observation and permission flips — see the header for why these
// live in the shared harness rather than in the test file that first needed
// them (DCA 0015).
// ---------------------------------------------------------------------------

std::vector<std::filesystem::path> stagingResidueOf(
    const std::filesystem::path& target)
{
    std::vector<std::filesystem::path> found;

    // The full-rewrite branch's fixed temp.
    const auto fixed = saveTempPathFor(target);
    std::error_code fe;
    if (std::filesystem::exists(fixed, fe) && !fe) {
        found.push_back(fixed);
    }

    // The staged incremental append's unguessable sibling — enumerated, because
    // its suffix is drawn from a CSPRNG and cannot be named (F58-a).
    std::error_code ec;
    const auto dir = target.parent_path();
    const auto stem = target.filename();
    for (std::filesystem::directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (pdfe::core::StagingFile::isStagingResidueName(stem,
                                                          it->path().filename())) {
            found.push_back(it->path());
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// File identity — "did a rename publish this?"  (DCA 0015, phase-10)
// See the header for why a residue check cannot answer that question and why
// this must not be repurposed into an overwrite detector.
// ---------------------------------------------------------------------------

FileIdentity fileIdentityOf(const std::filesystem::path& p)
{
    FileIdentity id;
#if defined(_WIN32)
    // FILE_FLAG_BACKUP_SEMANTICS so a directory can be identified too, and
    // FILE_SHARE_* everything so this never perturbs a save under observation.
    HANDLE h = ::CreateFileW(p.wstring().c_str(), 0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return id;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(h, &info)) {
        id.valid = true;
        id.volume = info.dwVolumeSerialNumber;
        id.index = (static_cast<unsigned long long>(info.nFileIndexHigh) << 32)
                   | info.nFileIndexLow;
    }
    ::CloseHandle(h);
#else
    struct stat st {};
    if (::stat(p.string().c_str(), &st) == 0) {
        id.valid = true;
        id.volume = static_cast<unsigned long long>(st.st_dev);
        id.index = static_cast<unsigned long long>(st.st_ino);
    }
#endif
    return id;
}

#if defined(_WIN32)

std::string currentUserSidString()
{
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        return {};
    }
    DWORD need = 0;
    ::GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    std::vector<unsigned char> buf(need);
    std::string out;
    if (need && ::GetTokenInformation(tok, TokenUser, buf.data(), need, &need)) {
        auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
        LPWSTR s = nullptr;
        if (::ConvertSidToStringSidW(tu->User.Sid, &s)) {
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0,
                                                nullptr, nullptr);
            out.resize(n > 0 ? static_cast<size_t>(n - 1) : 0);
            if (n > 1) {
                ::WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr,
                                      nullptr);
            }
            ::LocalFree(s);
        }
    }
    ::CloseHandle(tok);
    return out;
}

std::string daclSddl(const std::filesystem::path& p)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    const DWORD rc = ::GetNamedSecurityInfoW(
        p.native().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        return "<GetNamedSecurityInfoW failed: " + std::to_string(rc) + ">";
    }
    LPWSTR text = nullptr;
    std::string result;
    if (::ConvertSecurityDescriptorToStringSecurityDescriptorW(
            sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr)) {
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                            nullptr, nullptr);
        result.resize(n > 0 ? n - 1 : 0);
        if (n > 1) {
            ::WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), n,
                                  nullptr, nullptr);
        }
        ::LocalFree(text);
    }
    ::LocalFree(sd);
    return result;
}

bool applyProtectedDacl(const std::filesystem::path& p, const std::wstring& sddl)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    bool ok = false;
    if (::GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present) {
        std::wstring mutablePath = p.native();
        ok = ::SetNamedSecurityInfoW(
                 mutablePath.data(), SE_FILE_OBJECT,
                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                 nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    }
    ::LocalFree(sd);
    return ok;
}

void denyReadOnTargetOnceStagingGrows(const std::filesystem::path& target,
                                      std::uintmax_t baselineBytes,
                                      const std::string& userSid,
                                      std::atomic<bool>& stop,
                                      std::atomic<bool>& firedOut)
{
    const std::wstring me(userSid.begin(), userSid.end());
    while (!stop.load(std::memory_order_relaxed)) {
        for (const auto& s : stagingResidueOf(target)) {
            std::error_code se;
            const auto sz = std::filesystem::file_size(s, se);
            if (se || sz <= baselineBytes) {
                continue;
            }
            // Deny read, allow everything else, inheritance broken. The DENY
            // ace is written first, which is the order Windows evaluates.
            // Windows always grants the OWNER READ_CONTROL and WRITE_DAC
            // regardless of the DACL, so the DACL can still be read for
            // preservation and restored for cleanup while FILE_READ_DATA
            // stays denied.
            if (applyProtectedDacl(target,
                                   L"D:(D;;FR;;;" + me + L")(A;;FA;;;" + me + L")")) {
                firedOut.store(true, std::memory_order_relaxed);
            }
            return;
        }
    }
}

void restoreFullControl(const std::filesystem::path& p,
                        const std::string& userSid)
{
    const std::wstring me(userSid.begin(), userSid.end());
    applyProtectedDacl(p, L"D:(A;;FA;;;" + me + L")");
}

#endif  // _WIN32

std::string readAllBytes(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::string();
    }
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

bool fileIsByteIdenticalTo(const std::filesystem::path& p, const std::string& snapshot)
{
    return readAllBytes(p) == snapshot;
}

}  // namespace pdfe::testing
