#include "DocumentSearchIndex.h"
#include "../core/Document.h"
#include "../core/Page.h"
#include "../core/TempFileSafety.h"

#include <QCryptographicHash>
#include <QString>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <csignal>
#  include <unistd.h>
#endif

namespace pdfe::search {

namespace {

unsigned long currentProcessId()
{
#ifdef _WIN32
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

/// Produce a stable, filesystem-safe directory name for a document's
/// index. Hashing the absolute path ensures two identical filenames in
/// different folders don't collide.
std::string indexKeyForDocument(const pdfe::core::Document* doc)
{
    if (!doc) return "anonymous";
    const std::string path = doc->filePath().string();
    if (path.empty()) {
        // Anonymous doc — use pointer-based key to keep instances separate.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "anon_%p", static_cast<const void*>(doc));
        return buf;
    }
    QByteArray hash = QCryptographicHash::hash(
        QByteArray::fromStdString(path), QCryptographicHash::Sha1);
    return hash.toHex().left(16).toStdString();
}

/// GitHub #152 — the registry of index directories THIS PROCESS created.
///
/// Deliberately a REGISTRY rather than a disk scan. The shutdown sweep must
/// never consider a directory this process did not write, so the candidate set
/// is built from our own claims and nothing else; a directory enumeration would
/// have to distinguish ours from a concurrent instance's after the fact, which
/// is exactly the check-then-act the age-gated sweep has to work so hard to
/// make safe.
///
/// The value is a LIVE-INSTANCE COUNT, not a flag: two DocumentSearchIndex
/// objects over the same document share one directory, and a shutdown that
/// fired while one was still bound would pull the index out from under it.
struct OwnedIndexRegistry {
    std::mutex mutex;
    std::map<std::filesystem::path, int> liveCount;
};

OwnedIndexRegistry& ownedIndexes()
{
    static OwnedIndexRegistry registry;
    return registry;
}

} // namespace

// PERMISSION GATE (decision taken; supersedes the former "left ungated"
// PERMISSION NOTE at this site).
//
// Unlike pdfe::core::SearchEngine — which is purely in-process and writes
// nothing — this class WRITES every page's plain text into a Tantivy index
// under std::filesystem::temp_directory_path(). For a copy-restricted or
// encrypted document that is a plaintext-searchable copy of the content living
// OUTSIDE the encrypted container, readable by any process with temp-dir access
// for as long as the files survive.
//
// HOW LONG THAT ACTUALLY IS (corrected, GitHub #152). This sentence used to end
// "...until the destructor removes it". THE DESTRUCTOR HAS NEVER REMOVED IT and
// still does not: ~DocumentSearchIndex joins the reindex thread, drops the
// Tantivy handle and closes the lock file, deliberately LEAVING the on-disk
// files so reopening the same document reuses the index. The claim was not a
// small inaccuracy — it was persuasive enough that a data-retention audit read
// it, concluded this surface was already fixed, and stopped three functions
// short of the destructor. The real lifetimes are:
//   * within a session — the files persist across close/reopen, by design;
//   * at process shutdown — sweepOwnIndexesAtShutdown() removes the indexes
//     THIS process owns (GitHub #152); and
//   * anything a shutdown could not reach (a crash, a kill, a pre-fix build) —
//     the age-gated sweepStaleIndexes() at the next construction, 24 h later.
// Same confidentiality class as the
// accessibility-tree bypass closed in src/ui/PdfCanvasAccessible.cpp, so it
// takes the SAME predicate — one extraction-permission rule for the product,
// not two competing ones:
//
//   permitted  <=>  extractForAccessibilityAllowed() || copyAllowed()
//     * ISO 32000-1 bit 10 — extract for accessibility, or
//     * ISO 32000-1 bit  5 — general copy/extract.
//   Only a document denying BOTH is refused. Unencrypted documents grant every
//   bit (Document::permissionAllowed), so nothing changes for them.
//
// DECISION: the on-disk index is REFUSED for such documents; search itself is
// NOT disabled. SearchPanel falls back to the in-process SearchEngine (see
// SearchPanel::onSearchClicked), which never materialises text outside the
// container. The refusal is enforced here as well as at the UI so a FUTURE
// caller cannot reintroduce the leak by using this class directly.
//
// Refusal shape: a silent no-op, never a throw. ensureBuilt() is reached from
// searchPages() on the UI's hot search path (and from build()); throwing there
// would surface as a "Search error: ..." and destabilise a path whose callers
// treat an empty result as normal. So the gate leaves built_ false, creates no
// directory and no index, which makes searchPages() return {} and
// documentCount() return 0; reindexPage() returns early for the same reason.
bool DocumentSearchIndex::extractionAllowed() const
{
    if (!doc_) return false;
    try {
        return doc_->extractForAccessibilityAllowed() || doc_->copyAllowed();
    } catch (...) {
        // Fail closed: if the permission state cannot be determined we must not
        // spill page text to a temp directory.
        return false;
    }
}

// ---------------------------------------------------------------------------
// F24 — stale plaintext-index purge. Rationale and the two-gate safety rule are
// documented on the declarations in DocumentSearchIndex.h.
// ---------------------------------------------------------------------------

std::filesystem::path DocumentSearchIndex::indexRoot()
{
    return std::filesystem::temp_directory_path() / "pdfe_search_indexes";
}

bool DocumentSearchIndex::processIsAlive(unsigned long pid)
{
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                             static_cast<DWORD>(pid));
    if (h == nullptr) {
        // ERROR_INVALID_PARAMETER is Windows' "no such process id". Any other
        // failure (ACCESS_DENIED for a process owned by another user, for
        // instance) means the process EXISTS but is not ours to query — treat
        // it as alive, because the cost of guessing wrong is deleting a
        // running instance's index.
        return ::GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD code = 0;
    const bool alive = (::GetExitCodeProcess(h, &code) != 0)
                       && code == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;   // exists, not ours — fail safe
#endif
}

bool DocumentSearchIndex::isRedirection(const std::filesystem::path& p)
{
#ifdef _WIN32
    // GetFileAttributesW does NOT follow the reparse point, so this reports on
    // the NAME, not on whatever it resolves to. FILE_ATTRIBUTE_REPARSE_POINT
    // covers BOTH IO_REPARSE_TAG_SYMLINK and IO_REPARSE_TAG_MOUNT_POINT (the
    // junction tag MSVC's is_symlink() does not report), which is exactly the
    // gap this exists to close.
    const DWORD attrs = ::GetFileAttributesW(p.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            // Nothing there at all: not a redirection. (A vanished entry is
            // handled by the sweep's own error branches.)
            return false;
        }
        // Anything else — sharing violation, access denied — leaves us unable
        // to classify. Fail safe toward KEEPING.
        return true;
    }
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    const auto st = std::filesystem::symlink_status(p, ec);
    if (ec) {
        if (st.type() == std::filesystem::file_type::not_found) return false;
        return true;   // undecidable => keep
    }
    return std::filesystem::is_symlink(st);
#endif
}

namespace {

/// Depth cap for treeContainsRedirection. An index tree is two or three levels
/// deep in practice; anything deeper than this is either not ours or is being
/// used to exhaust the scan, and either way "undecidable" is the safe answer.
constexpr int kMaxScanDepth = 24;

bool scanForRedirection(const std::filesystem::path& dir, int depth)
{
    if (depth > kMaxScanDepth) return true;   // undecidable => keep

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return true;                      // cannot enumerate => keep

    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) return true;                  // enumeration broke => keep
        const auto& entry = *it;
        if (DocumentSearchIndex::isRedirection(entry.path())) return true;

        // symlink_status, NOT status: classify the NAME, never the target.
        // (Belt to the attribute check's braces — if a future platform grows a
        // redirection kind the attribute check misses, this still refuses to
        // descend through it.)
        std::error_code sec;
        const auto st = std::filesystem::symlink_status(entry.path(), sec);
        if (sec) return true;                 // undecidable => keep
        if (std::filesystem::is_symlink(st)) return true;
        if (std::filesystem::is_directory(st)) {
            if (scanForRedirection(entry.path(), depth + 1)) return true;
        }
    }
    return false;
}

} // namespace

bool DocumentSearchIndex::treeContainsRedirection(
    const std::filesystem::path& dir)
{
    if (isRedirection(dir)) return true;
    return scanForRedirection(dir, 0);
}

#ifdef _WIN32
namespace {

// ---------------------------------------------------------------------------
// HANDLE-BASED RECURSIVE DELETE — closing the check-then-act window for real.
//
// The previous shape was: walk the tree with std::filesystem looking for
// reparse points, then call remove_all(). That is a check-then-act, and the
// in-code comment said so: an attacker who plants a junction between the scan
// and the delete redirects a recursive delete at a directory of their choosing.
// Narrowing WHO can plant it (phase-70's owner-only ACL on the root) reduced the
// consequence but left the window open, and "cannot be raced" is what the
// criterion asks for.
//
// What makes this version unraceable is not that it looks before it acts —
// everything looks before it acts — but that IT ACTS ON THE OBJECT IT LOOKED AT:
//
//   * every entry is opened with FILE_FLAG_OPEN_REPARSE_POINT, so the open
//     lands on the link itself and never traverses it;
//   * the entry is classified from the HANDLE (GetFileInformationByHandle),
//     not from the path, so the answer describes the object we hold open;
//   * the object's FILE ID is compared against the id the directory
//     enumeration reported. If anything swapped the name in between, the ids
//     differ and we refuse rather than delete;
//   * deletion is by handle (FileDispositionInfo), not by name, so there is no
//     second name resolution for an attacker to win.
//
// A junction that survives all that is deleted AS A LINK — its target is never
// entered, because we never traverse it.
//
// Returns false on the first thing it cannot verify. Refusing to delete leaves
// a stale index directory on disk, which is the safe direction: the alternative
// is deleting something else.

constexpr int kMaxDeleteDepth = 8;

struct ScopedHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~ScopedHandle() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE v) : h(v) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    bool valid() const { return h != INVALID_HANDLE_VALUE; }
};

HANDLE openNoFollow(const std::wstring& path, DWORD access)
{
    return ::CreateFileW(
        path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
}

bool deleteByHandle(HANDLE h)
{
    FILE_DISPOSITION_INFO info{};
    info.DeleteFile = TRUE;
    return ::SetFileInformationByHandle(h, FileDispositionInfo,
                                        &info, sizeof(info)) != FALSE;
}

unsigned long long fileIdOf(const BY_HANDLE_FILE_INFORMATION& bhfi)
{
    return (static_cast<unsigned long long>(bhfi.nFileIndexHigh) << 32) |
           bhfi.nFileIndexLow;
}

bool removeTreeByHandle(const std::wstring& path, int depth);

/// Delete the children of an already-open, already-verified directory handle.
bool removeChildren(HANDLE dirHandle, const std::wstring& dirPath, int depth)
{
    // FileIdBothDirectoryInfo carries the FILE ID alongside the name, which is
    // what lets the open below prove it got the same object the enumeration
    // described. A plain FindFirstFile walk cannot do that.
    std::vector<unsigned char> buf(64 * 1024);
    BOOL more = ::GetFileInformationByHandleEx(
        dirHandle, FileIdBothDirectoryInfo, buf.data(),
        static_cast<DWORD>(buf.size()));
    while (more) {
        const unsigned char* p = buf.data();
        for (;;) {
            const auto* info =
                reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p);
            const std::wstring name(
                info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                const std::wstring child = dirPath + L"\\" + name;
                const unsigned long long expectedId =
                    static_cast<unsigned long long>(info->FileId.QuadPart);

                ScopedHandle ch(openNoFollow(child, DELETE | FILE_LIST_DIRECTORY));
                if (!ch.valid()) return false;

                BY_HANDLE_FILE_INFORMATION bhfi{};
                if (!::GetFileInformationByHandle(ch.h, &bhfi)) return false;

                // THE RACE CHECK. If the name now refers to a different object
                // than the one enumerated, something swapped it underneath us.
                if (fileIdOf(bhfi) != expectedId) return false;

                const bool isReparse =
                    (bhfi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                const bool isDir =
                    (bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                if (isDir && !isReparse) {
                    // Recurse into a REAL directory. The handle is closed first
                    // so the child can re-open it with the access it needs; the
                    // id check is repeated there, so closing does not reopen the
                    // window.
                    ::CloseHandle(ch.h);
                    ch.h = INVALID_HANDLE_VALUE;
                    if (!removeTreeByHandle(child, depth + 1)) return false;
                } else {
                    // A file, or a reparse point of any kind. Deleting by
                    // handle removes THIS object — for a junction that is the
                    // link, never what it points at, because the handle was
                    // opened with FILE_FLAG_OPEN_REPARSE_POINT.
                    if (!deleteByHandle(ch.h)) return false;
                }
            }
            if (info->NextEntryOffset == 0) break;
            p += info->NextEntryOffset;
        }
        more = ::GetFileInformationByHandleEx(
            dirHandle, FileIdBothDirectoryInfo, buf.data(),
            static_cast<DWORD>(buf.size()));
    }
    // ERROR_NO_MORE_FILES is the normal terminator; anything else means the
    // enumeration failed partway and we must not claim the tree is empty.
    return ::GetLastError() == ERROR_NO_MORE_FILES;
}

bool removeTreeByHandle(const std::wstring& path, int depth)
{
    if (depth > kMaxDeleteDepth) return false;

    ScopedHandle dir(openNoFollow(path, DELETE | FILE_LIST_DIRECTORY));
    if (!dir.valid()) return false;

    BY_HANDLE_FILE_INFORMATION bhfi{};
    if (!::GetFileInformationByHandle(dir.h, &bhfi)) return false;

    // Refuse a reparse point at the ROOT of the walk outright rather than
    // deleting it here: the caller enumerated it as a candidate index
    // directory, and a candidate that is really a link is a finding, not a
    // routine cleanup.
    if (bhfi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    if (!(bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;

    if (!removeChildren(dir.h, path, depth)) return false;
    return deleteByHandle(dir.h);
}

} // namespace
#endif // _WIN32

bool DocumentSearchIndex::removeTreeUnraceable(const std::filesystem::path& dir)
{
#ifdef _WIN32
    return removeTreeByHandle(dir.wstring(), 0);
#else
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return !ec;
#endif
}

std::filesystem::path DocumentSearchIndex::lockFilePath() const
{
    std::filesystem::path p = indexDir_;
    p += kLockSuffix;
    return p;
}

void DocumentSearchIndex::claimIndexDir()
{
    if (lockFile_.is_open()) return;
    std::error_code ec;
    std::filesystem::create_directories(indexDir_.parent_path(), ec);
    lockFile_.open(lockFilePath(), std::ios::out | std::ios::trunc);
    if (!lockFile_) return;   // best effort; the age gate still applies
    lockFile_ << currentProcessId() << "\n";
    lockFile_.flush();
    // Deliberately NOT closed: an open handle is a second, OS-level liveness
    // signal on Windows (a file open without FILE_SHARE_DELETE cannot be
    // removed), and closing it would throw that away.

    // GitHub #152 — record the directory as OURS so the shutdown sweep has a
    // candidate set it can trust. Registered only on the path where the lock
    // was actually written with OUR pid, which is the same fact the sweep
    // re-verifies from disk before deleting anything.
    if (!registeredOwned_) {
        auto& registry = ownedIndexes();
        std::lock_guard<std::mutex> g(registry.mutex);
        ++registry.liveCount[indexDir_];
        registeredOwned_ = true;
    }
}

StaleIndexSweepReport DocumentSearchIndex::sweepStaleIndexes(
    const std::filesystem::path& root,
    std::chrono::seconds staleAfter)
{
    StaleIndexSweepReport report;

    // Safety rail: this function deletes directory trees. It runs only against
    // a directory that IS a PDFE index root, never an arbitrary path handed in
    // by a future caller who thought it was passing a document directory.
    if (root.filename() != "pdfe_search_indexes") {
        report.refusedRoot = true;
        return report;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return report;

    const auto now = std::filesystem::file_time_type::clock::now();

    std::vector<std::filesystem::path> candidates;
    std::vector<std::filesystem::path> redirections;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        // --- Gate (0): REDIRECTION. ---
        //
        // Classify the NAME, never the target. `entry.is_directory()` FOLLOWS a
        // reparse point, so a junction planted here — %TEMP% is writable by
        // anything running as the same user, and `mklink /J` needs no privilege
        // — used to be enumerated as an ordinary index directory. With no
        // adjacent lock file and an old mtime it then passed BOTH remaining
        // gates, and remove_all() was aimed at whatever it pointed to.
        //
        // Recorded, not silently dropped: a redirection under the index root is
        // not a normal condition, and the report is the only thing that can say
        // so.
        if (isRedirection(entry.path())) {
            redirections.push_back(entry.path());
            continue;
        }
        std::error_code sec;
        const auto st = std::filesystem::symlink_status(entry.path(), sec);
        if (sec) {
            // Cannot classify => cannot delete. Same policy as the age gate's
            // undecidable branch.
            redirections.push_back(entry.path());
            continue;
        }
        if (std::filesystem::is_directory(st)) candidates.push_back(entry.path());
    }
    for (auto& p : redirections) {
        report.kept.push_back(p);
        report.skippedRedirection.push_back(std::move(p));
    }

    for (const auto& dir : candidates) {
        std::filesystem::path lock = dir;
        lock += kLockSuffix;

        // The LOCK FILE is read (its pid is parsed) and then deleted. A lock
        // that is itself a reparse point would have both of those aimed
        // elsewhere, so it disqualifies the directory outright rather than
        // being followed.
        if (isRedirection(lock)) {
            report.kept.push_back(dir);
            report.skippedRedirection.push_back(dir);
            continue;
        }

        // --- Gate (a): ownership. ---
        bool owned = false;
        if (std::filesystem::exists(lock, ec)) {
            unsigned long pid = 0;
            std::ifstream in(lock);
            if (in >> pid) {
                owned = processIsAlive(pid);
            } else {
                // A lock file we cannot parse is a lock file we must respect.
                owned = true;
            }
        }
        if (owned) {
            report.kept.push_back(dir);
            continue;
        }

        // --- Gate (b): age. ---
        const auto mtime = std::filesystem::last_write_time(dir, ec);
        if (ec) {
            // Cannot establish age => cannot establish staleness => keep.
            report.kept.push_back(dir);
            ec.clear();
            continue;
        }
        if (now - mtime <= staleAfter) {
            report.kept.push_back(dir);
            continue;
        }

        // --- Gate (0) again, DEEP, immediately before the delete. ---
        //
        // remove_all() RECURSES, so a junction nested one level inside an
        // otherwise ordinary index directory is the same hazard as one planted
        // at the top. The per-candidate check above cannot see it. Walk the
        // tree without following anything and refuse the whole candidate if a
        // redirection — or an entry we cannot classify — is anywhere in it.
        //
        // THE RESIDUAL THIS COMMENT USED TO DESCRIBE IS CLOSED (phase-70).
        //
        // It said: this is a check-then-act, an attacker could plant a junction
        // between the scan and remove_all(), and closing it properly needs
        // handle-based traversal rather than std::filesystem. That was correct,
        // and the traversal is now written — see removeTreeByHandle() above,
        // which is what performs the delete below.
        //
        // Two defences were considered and only one of them closes it:
        //   * the owner-only ACL now applied to the index root (phase-70,
        //     criterion 2) narrows WHO can plant a junction here to processes
        //     already running as this user. That reduces the consequence but
        //     leaves the window open, and "narrowed" is not "cannot be raced".
        //   * removeTreeByHandle acts on the object it inspected — opened with
        //     FILE_FLAG_OPEN_REPARSE_POINT, classified from the handle, FILE ID
        //     matched against the enumeration, deleted by handle. That closes
        //     it.
        //
        // This scan is KEPT, but its job has changed: it is now a cheap early
        // refusal that reports a candidate containing a redirection as a
        // finding (skippedRedirection) instead of silently cleaning it up. It
        // is no longer load-bearing for safety, so a race against it can no
        // longer redirect a delete.
        if (treeContainsRedirection(dir)) {
            report.kept.push_back(dir);
            report.skippedRedirection.push_back(dir);
            continue;
        }

#ifdef _WIN32
        // THE DELETE ITSELF IS NOW UNRACEABLE, which is what closes the
        // residual the scan above could only narrow. removeTreeByHandle opens
        // every entry with FILE_FLAG_OPEN_REPARSE_POINT, classifies it from the
        // HANDLE, checks the object's FILE ID against the id the directory
        // enumeration reported, and deletes BY HANDLE. A junction swapped in
        // after the scan fails the id check, and even if it did not it would be
        // unlinked as a link rather than descended.
        //
        // The scan above is retained as a cheap early refusal: it reports a
        // candidate containing a redirection as a FINDING (skippedRedirection)
        // rather than silently cleaning it, which is diagnostic information the
        // delete path does not produce.
        //
        // Refusing beats deleting the wrong thing: any failure to verify leaves
        // the directory in place and it is reported as kept.
        if (!removeTreeUnraceable(dir)) {
            report.kept.push_back(dir);
            continue;
        }
#else
        std::filesystem::remove_all(dir, ec);
        if (ec) {
            report.kept.push_back(dir);
            ec.clear();
            continue;
        }
#endif
        std::filesystem::remove(lock, ec);
        ec.clear();
        report.removed.push_back(dir);
    }

    return report;
}

StaleIndexSweepReport DocumentSearchIndex::sweepStaleIndexes()
{
    // ORDER IS LOAD-BEARING, and getting it wrong would be silent.
    //
    // The root's owner-only DACL is applied HERE, before the sweep walks and
    // deletes inside it — not only in buildFromScratch(). The sweep runs from
    // the constructor and buildFromScratch() only on the first search, so
    // hardening solely there would leave the root carrying %TEMP%'s inherited
    // ACL for exactly the window in which this function is racing an attacker.
    //
    // And that is what narrows the check-then-act residual documented in the
    // two-argument overload. Nobody outside this account can create anything
    // under a protected, inheritance-broken owner-only root, so the junction
    // has to be planted by a process ALREADY running as this user — which can
    // read the plaintext index directly and gains nothing by racing the sweep.
    // The residual is not closed in the abstract, but the principals who could
    // exploit it no longer need to.
    //
    // A failure to harden is deliberately NOT fatal to the sweep: an
    // unhardenable root is a reason to keep purging stale plaintext, not to
    // stop. buildFromScratch() is where the same failure refuses to WRITE.
    std::error_code aclEc;
    (void)pdfe::core::createOwnerOnlyDirectory(indexRoot(), aclEc);

    return sweepStaleIndexes(indexRoot(),
                             std::chrono::duration_cast<std::chrono::seconds>(
                                 kDefaultStaleAfter));
}

DocumentSearchIndex::DocumentSearchIndex(pdfe::core::Document* doc)
    : doc_(doc)
{
    if (!doc_) throw std::invalid_argument("DocumentSearchIndex: null document");
    indexDir_ = indexRoot() / indexKeyForDocument(doc_);

    // First-index sweep. The purge has to happen SOMEWHERE that always runs;
    // hanging it off the first index construction in the process means it runs
    // for the UI, for a headless caller and for a test alike, without every
    // entry point having to remember to call it.
    static std::once_flag sweepOnce;
    std::call_once(sweepOnce, []() {
        // GitHub #152 — arm the SHUTDOWN sweep, and do it before anything else
        // in this block.
        //
        // ORDER IS LOAD-BEARING AND THE FAILURE WOULD BE A CRASH, NOT A MISS.
        // std::atexit handlers and function-local static destructors run in one
        // combined LIFO order. Touching ownedIndexes() here forces the registry
        // to be constructed BEFORE our handler is registered, which guarantees
        // it is destroyed AFTER the handler runs. Left to claimIndexDir() to
        // construct it lazily — which happens later, on the first index build —
        // the registry would be destroyed FIRST and the handler would read a
        // dead object.
        (void)ownedIndexes();
        (void)std::atexit([]() {
            try {
                (void)sweepOwnIndexesAtShutdown();
            } catch (...) {
                // Never let cleanup turn a clean exit into a crash.
            }
        });

        try {
            // The no-arg overload hardens the root's ACL before it sweeps —
            // see the comment there for why the order is load-bearing.
            (void)sweepStaleIndexes();
        } catch (...) {
            // A failed purge must never stop the product from starting.
        }
    });
}

StaleIndexSweepReport DocumentSearchIndex::sweepOwnIndexesAtShutdown()
{
    StaleIndexSweepReport report;

    // Snapshot under the lock; the deletes below are slow and must not hold it.
    std::vector<std::filesystem::path> candidates;
    {
        auto& registry = ownedIndexes();
        std::lock_guard<std::mutex> g(registry.mutex);
        for (const auto& entry : registry.liveCount) {
            if (entry.second > 0) {
                // Gate 3: a DocumentSearchIndex is still bound to it.
                report.kept.push_back(entry.first);
                continue;
            }
            candidates.push_back(entry.first);
        }
    }

    const std::filesystem::path root = indexRoot();
    const unsigned long self = currentProcessId();

    for (const auto& dir : candidates) {
        // Safety rail, same spirit as sweepStaleIndexes' root check: nothing
        // outside the index root is deletable through this path, whatever a
        // future caller manages to get into the registry.
        if (dir.parent_path() != root) {
            report.kept.push_back(dir);
            continue;
        }

        std::filesystem::path lock = dir;
        lock += kLockSuffix;

        // Redirection gates — identical to the age-gated sweep's, and for the
        // identical reason: a junction at either name aims the delete elsewhere.
        if (isRedirection(dir) || isRedirection(lock) || treeContainsRedirection(dir)) {
            report.kept.push_back(dir);
            report.skippedRedirection.push_back(dir);
            continue;
        }

        // Gate 2: the lock must still name US. If a second instance opened the
        // same document it overwrote this file with its own pid, and that
        // instance is the live owner — deleting its index here would be the
        // cross-instance data-loss bug, not a cleanup.
        std::error_code ec;
        if (!std::filesystem::exists(lock, ec) || ec) {
            report.kept.push_back(dir);
            continue;
        }
        unsigned long recordedPid = 0;
        {
            std::ifstream in(lock);
            if (!(in >> recordedPid)) {
                report.kept.push_back(dir);
                continue;
            }
        }
        if (recordedPid != self) {
            report.kept.push_back(dir);
            continue;
        }

        if (!removeTreeUnraceable(dir)) {
            report.kept.push_back(dir);
            continue;
        }
        ec.clear();
        std::filesystem::remove(lock, ec);
        report.removed.push_back(dir);

        auto& registry = ownedIndexes();
        std::lock_guard<std::mutex> g(registry.mutex);
        registry.liveCount.erase(dir);
    }

    return report;
}

DocumentSearchIndex::~DocumentSearchIndex()
{
    // Wait for any in-flight background reindex before tearing down
    // the index handle — the worker captures index_.get() by value
    // and must not see a freed pointer.
    joinReindexThread();
    // Drop the Tantivy handle first so it closes cleanly.
    std::lock_guard<std::mutex> g(indexMutex_);
    index_.reset();
    // Leave on-disk index files. THIS IS STILL DELIBERATE and GitHub #152 did
    // not change it: reopening the same document later in the SAME session
    // reuses the index instead of re-extracting every page. What #152 changed
    // is what happens after the session — sweepOwnIndexesAtShutdown() removes
    // the directories this process owns at process exit, so the amortisation
    // window is now the process lifetime rather than the 24-hour age gate.
    //
    // F24 — release the ownership lock. The FILE stays: it now records a dead
    // pid, which is exactly the signal a later sweep needs to classify this
    // directory as unowned and let the age gate decide.
    if (lockFile_.is_open()) lockFile_.close();

    // GitHub #152 — this object no longer holds the directory. The entry stays
    // in the registry with a count of zero: it is still OURS to clean up at
    // shutdown, it is merely no longer in use.
    if (registeredOwned_) {
        auto& registry = ownedIndexes();
        std::lock_guard<std::mutex> g(registry.mutex);
        auto it = registry.liveCount.find(indexDir_);
        if (it != registry.liveCount.end() && it->second > 0) {
            --it->second;
        }
        registeredOwned_ = false;
    }
}

void DocumentSearchIndex::joinReindexThread()
{
    if (reindexThread_.joinable()) {
        reindexThread_.join();
    }
}

void DocumentSearchIndex::waitForPendingReindex()
{
    joinReindexThread();
}

void DocumentSearchIndex::ensureBuilt()
{
    std::lock_guard<std::mutex> g(indexMutex_);
    if (built_) return;
    // PERMISSION GATE (see the block comment above extractionAllowed()):
    // refuse to materialise page text on disk for a document that denies both
    // accessibility-extraction (bit 10) and copy (bit 5). No throw, no
    // directory, no index — built_ stays false so searchPages() yields {} and
    // callers fall back to the in-process SearchEngine.
    if (!extractionAllowed()) return;
    // built_ follows the RESULT. buildFromScratch() refuses when the index
    // directory cannot be made owner-only; setting built_ regardless would
    // leave a "built" index with no index_ behind it, and would convert a
    // fail-closed refusal into a silently broken success.
    if (!buildFromScratch()) return;
    built_ = true;
}

bool DocumentSearchIndex::buildFromScratch()
{
    // CONFIDENTIALITY GATE, second of two. extractionAllowed() decides whether
    // this document's text may be materialised on disk at all; this decides
    // whether the place it would land can be made private.
    //
    // Everything below writes every page's plain text into %TEMP%, whose
    // default ACL lets any process running as this user read it — and on a
    // terminal server or shared build box "this user" is not a boundary anyone
    // chose. So the root and the per-document directory are created with an
    // owner-only, inheritance-broken DACL, verified by reading the ACL back.
    //
    // FAIL CLOSED, and the fallback is already built: refusing here leaves
    // built_ false, so searchPages() returns {} and SearchPanel drops to the
    // in-process SearchEngine, which never materialises text outside the
    // container. That is the same refusal shape the permission gate uses — a
    // silent no-op, never a throw, because ensureBuilt() sits on the UI's hot
    // search path. An unprotectable index directory is worse than no index.
    std::error_code aclEc;
    if (!pdfe::core::createOwnerOnlyDirectory(indexRoot(), aclEc)) {
        return false;
    }
    if (!pdfe::core::createOwnerOnlyDirectory(indexDir_, aclEc)) {
        return false;
    }

    // Fresh index each time we build from scratch — guarantees we don't
    // inherit stale entries from a previous run with different content.
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(indexDir_, ec)) {
        std::filesystem::remove_all(entry.path(), ec);
    }
    // F24 — mark this directory as owned by a LIVE process before a single byte
    // of page text lands in it, so a sweep running concurrently in another
    // instance can never mistake it for a pre-fix leak. The lock is a SIBLING
    // file, not a member of the index directory, so Tantivy never sees it.
    claimIndexDir();

    // make_unique never returns null (throws bad_alloc on OOM), so
    // the constructed object is always present here; only the runtime
    // valid() check matters.
    index_ = std::make_unique<TantivyIndex>(indexDir_);
    if (!index_->valid()) {
        index_.reset();
        throw std::runtime_error(
            "DocumentSearchIndex: failed to open Tantivy index at "
            + indexDir_.string());
    }

    // The extraction-permission gate that authorises this write lives in
    // ensureBuilt() (and reindexPage()) — see the extractionAllowed() comment.
    // buildFromScratch() is private and reached only through those, so by the
    // time control gets here the document has been cleared for on-disk
    // materialisation of its page text.
    const int total = doc_->pageCount();
    for (int i = 0; i < total; ++i) {
        auto page = doc_->page(i);
        if (!page) continue;
        const std::string text = page->extractPlainText();
        index_->upsertPage(i, text);
    }
    index_->commit();
    return true;
}

std::vector<SearchHit> DocumentSearchIndex::searchPages(
    const std::string& query, int maxHits)
{
    ensureBuilt();
    // Block on any pending reindex so search results reflect the
    // latest committed index state.
    joinReindexThread();
    std::lock_guard<std::mutex> g(indexMutex_);
    if (!index_) return {};
    return index_->search(query, maxHits);
}

void DocumentSearchIndex::reindexPage(int pageIndex)
{
    // Page-text extraction stays on the calling thread because Page
    // objects hold a back-pointer to Document and MuPDF context that
    // is owned by this thread. Tantivy upsert+commit moves to a
    // worker thread to keep the UI responsive on disk I/O.
    // PERMISSION GATE — second enforcement point. built_ can only be true if
    // ensureBuilt() already cleared the document, but a permission state can
    // change under us (reopen/decrypt) and this method is also reachable via
    // the public invalidatePage() spec adapter, so re-check rather than trust.
    if (!extractionAllowed()) return;
    std::string text;
    {
        std::lock_guard<std::mutex> g(indexMutex_);
        if (!built_ || !index_) return;
    }
    auto page = doc_->page(pageIndex);
    if (!page) return;
    text = page->extractPlainText();

    // Wait for any prior in-flight reindex to finish so we serialize
    // commits in the order callers requested them.
    joinReindexThread();

    reindexThread_ = std::thread(
        [this, pageIndex, text = std::move(text)]() {
            std::lock_guard<std::mutex> g(indexMutex_);
            if (!index_) return;
            index_->upsertPage(pageIndex, text);
            index_->commit();
        });
}

void DocumentSearchIndex::invalidate()
{
    joinReindexThread();
    std::lock_guard<std::mutex> g(indexMutex_);
    index_.reset();
    built_ = false;
    std::error_code ec;
    std::filesystem::remove_all(indexDir_, ec);
    // F24 — the directory is gone, so its ownership marker must go too;
    // otherwise a lock file with no directory accumulates in the root.
    if (lockFile_.is_open()) lockFile_.close();
    std::filesystem::remove(lockFilePath(), ec);
}

std::int64_t DocumentSearchIndex::documentCount() const
{
    std::lock_guard<std::mutex> g(indexMutex_);
    if (!index_) return 0;
    return index_->documentCount();
}

// Pipeline G phase-8 spec'd interface (AC #1).

void DocumentSearchIndex::build(pdfe::core::Document* doc)
{
    if (doc != doc_) return;  // Bound to the constructor's document.
    invalidate();             // Drop cached index + on-disk files.
    ensureBuilt();            // Rebuild from scratch.
}

std::vector<int> DocumentSearchIndex::query(const std::string& q)
{
    auto hits = searchPages(q, 500);
    std::vector<int> out;
    out.reserve(hits.size());
    for (const auto& h : hits) out.push_back(h.pageIndex);
    return out;
}

void DocumentSearchIndex::invalidatePage(int pageIndex)
{
    reindexPage(pageIndex);
}

} // namespace pdfe::search
