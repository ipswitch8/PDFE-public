// -*- coding: utf-8 -*-
//
// Phase-4 SECURITY-ORACLE remediation (R-01): behavioral security oracles.
//
// This header provides *positive-neutralization* observers used by the CVE /
// malicious-PDF suites to prove an attack was NOT merely "handled without a
// throw", but actively neutralized:
//
//   * ProcessSpawnGuard  — snapshots the set of running child-attack process
//                          image names (calc.exe, cmd.exe, notepad.exe, …)
//                          before the operation under test, then asserts NO new
//                          instance of any of them appeared afterwards. A
//                          Launch action / hostile-JS process spawn that
//                          actually fired would create such a process; a
//                          neutralized one leaves the count unchanged.
//
//   * FileWriteGuard    — snapshots a caller-owned scratch tree (and, via a
//                          per-test marker, the shared %TEMP% root) before the
//                          operation under test, then asserts nothing was
//                          created, modified or DELETED. Added for SEC-44: a
//                          hostile-JS payload that writes a file spawns no
//                          process at all, so ProcessSpawnGuard is blind to
//                          it, and EXPECT_NO_THROW is satisfied by a payload
//                          that quietly succeeded. See the class comment for
//                          why %TEMP% is watched through a marker rather than
//                          diffed wholesale.
//
// The guard is Win32-native (CreateToolhelp32Snapshot). On non-Windows builds
// it degrades to a no-op that always reports "no spawn" (the CVE suites run on
// the Windows CI lane, which is where launch-action neutralization matters).
//
// Header-only so no CMake wiring is needed; #include from the test TU.

#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace pdfe_test {

// Attack processes the CVE / malicious-PDF launch-action payloads try to spawn.
// Kept deliberately narrow (interactive binaries a PDF exploit would launch) so
// unrelated CI child processes can't perturb the oracle.
inline const std::vector<std::string>& attackProcessNames() {
    static const std::vector<std::string> names = {
        "calc.exe", "calculator.exe", "cmd.exe", "notepad.exe",
        "powershell.exe", "mshta.exe", "wscript.exe", "cscript.exe",
        "rundll32.exe", "regsvr32.exe",
    };
    return names;
}

#if defined(_WIN32)

// ── PID scoping via a Job Object ────────────────────────────────────────────
// A prior version counted attack-named processes across the ENTIRE machine, so
// any unrelated cmd.exe / powershell.exe (git, an IDE, a co-scheduled CI job)
// made the oracle falsely report a spawn. We instead own the test process in a
// Job Object; children inherit it, so the observation is scoped to OUR process
// tree. If the job is unavailable we fall back to a parent-PID descendant walk —
// either way the count is tree-scoped, never machine-wide.

// Process-wide job owning this test process (and, by inheritance, its children).
// Created once, lazily. Call testProcessJob() early (see test_main) to force
// setup at startup so every spawned child is a member from the beginning.
inline HANDLE testProcessJob() {
    static HANDLE job = []() -> HANDLE {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j && !AssignProcessToJobObject(j, GetCurrentProcess())) {
            // e.g. already in a non-nestable job on a pre-Win8 host, or denied →
            // job scoping unavailable; callers use the descendant-walk fallback.
            CloseHandle(j);
            j = nullptr;
        }
        return j;
    }();
    return job;
}

// PIDs currently in our job (empty if job scoping is unavailable).
inline std::unordered_set<unsigned long> jobProcessIds() {
    std::unordered_set<unsigned long> pids;
    HANDLE job = testProcessJob();
    if (!job) return pids;
    // JOBOBJECT_BASIC_PROCESS_ID_LIST ends in a 1-element flexible array; size the
    // buffer for many PIDs and grow on ERROR_MORE_DATA.
    size_t cap = 512;
    for (int attempt = 0; attempt < 5; ++attempt) {
        const size_t bytes =
            sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + (cap - 1) * sizeof(ULONG_PTR);
        std::vector<unsigned char> buf(bytes, 0);
        auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buf.data());
        DWORD returned = 0;
        if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list,
                                      static_cast<DWORD>(bytes), &returned)) {
            for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i)
                pids.insert(static_cast<unsigned long>(list->ProcessIdList[i]));
            return pids;
        }
        if (GetLastError() != ERROR_MORE_DATA) break;
        cap *= 2;
    }
    return pids;
}

// Fallback when the job is unavailable: PIDs transitively descended from `root`,
// built from the given Toolhelp snapshot handle (parent→children map + BFS).
// Best-effort — Toolhelp parent PIDs can be stale under PID reuse — but still
// tree-scoped rather than machine-wide.
inline std::unordered_set<unsigned long>
descendantPids(HANDLE hSnap, unsigned long root) {
    std::unordered_set<unsigned long> out;
    std::unordered_multimap<unsigned long, unsigned long> children; // parent -> child
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            children.emplace(pe.th32ParentProcessID, pe.th32ProcessID);
        } while (Process32NextW(hSnap, &pe));
    }
    std::vector<unsigned long> stack{root};
    while (!stack.empty()) {
        unsigned long p = stack.back();
        stack.pop_back();
        auto range = children.equal_range(p);
        for (auto it = range.first; it != range.second; ++it) {
            if (out.insert(it->second).second) stack.push_back(it->second);
        }
    }
    return out;
}

// Multiset-ish snapshot: how many live processes IN OUR PROCESS TREE carry each
// attack image name (index-aligned with attackProcessNames()).
struct ProcessSnapshot {
    std::vector<int> counts;

    static ProcessSnapshot take() {
        ProcessSnapshot snap;
        snap.counts.assign(attackProcessNames().size(), 0);

        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return snap;

        // Membership set: prefer the job; else a parent-PID descendant walk. This
        // is what makes the oracle immune to unrelated machine-wide processes.
        std::unordered_set<unsigned long> tree = jobProcessIds();
        if (tree.empty())
            tree = descendantPids(hSnap, GetCurrentProcessId());
        tree.insert(GetCurrentProcessId());  // keep the set well-defined

        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {  // re-walks the same snapshot from start
            do {
                if (tree.find(pe.th32ProcessID) == tree.end())
                    continue;  // not in our tree → ignore (fixes the false positive)
                // Narrow the wide image name to lowercase ASCII for compare.
                std::string img;
                for (const wchar_t* c = pe.szExeFile; *c; ++c) {
                    wchar_t lc = (*c >= L'A' && *c <= L'Z') ? (*c + 32) : *c;
                    img.push_back(static_cast<char>(lc & 0x7F));
                }
                const auto& names = attackProcessNames();
                for (size_t i = 0; i < names.size(); ++i) {
                    if (img == names[i]) { snap.counts[i]++; break; }
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
        return snap;
    }
};

// ── Real-time new-in-job process monitor (closes the fast-exit false negative) ──
// ProcessSnapshot only sees processes ALIVE at the two poll points, so a transient
// in-job spawn (e.g. `cmd.exe /c exit`) that starts and exits between the guard's
// baseline and its single newlySpawned() check is missed. We associate an IO
// completion port with our Job Object; a background thread records every
// JOB_OBJECT_MSG_NEW_PROCESS as a race-free monotonic event (and best-effort
// resolves its image name), so the guard can attribute even an already-exited
// in-job spawn. Still job-scoped → no machine-wide false positives.
//
// Irreducible residual: a spawn brokered ENTIRELY outside our job (created by a
// different process — e.g. an RPC/DCOM broker — so it never joins our job) cannot
// be attributed without reintroducing machine-wide false positives. A naive
// PDF-launch/JS exploit spawns in-process (QProcess / ShellExecute of an EXE),
// which lands in our job and is caught.
class SpawnEventMonitor {
public:
    static SpawnEventMonitor& instance() {
        // Intentionally leaked: a background thread outlives static teardown, so a
        // never-destroyed singleton avoids an exit-time use-after-free.
        static SpawnEventMonitor* m = new SpawnEventMonitor();
        return *m;
    }

    // Monotonic count of new-in-job process events observed (race-free).
    uint64_t sequence() const { return seq_.load(std::memory_order_acquire); }

    // First attack-image-name event with sequence > baseSeq, or "" if none.
    std::string attackSince(uint64_t baseSeq) const {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& e : named_)
            if (e.first > baseSeq) return e.second;
        return "";
    }

private:
    SpawnEventMonitor() {
        HANDLE job = testProcessJob();
        if (!job) return;  // no job → monitor disabled; snapshot path still works
        port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (!port_) return;
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT assoc{};
        assoc.CompletionKey = reinterpret_cast<void*>(kKey);
        assoc.CompletionPort = port_;
        if (!SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation,
                                     &assoc, sizeof(assoc))) {
            CloseHandle(port_);
            port_ = nullptr;
            return;
        }
        std::thread(&SpawnEventMonitor::run, this).detach();
    }

    void run() {
        for (;;) {
            DWORD msg = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED ov = nullptr;
            if (!GetQueuedCompletionStatus(port_, &msg, &key, &ov, INFINITE)) {
                if (GetLastError() == ERROR_ABANDONED_WAIT_0) return;  // port closed
                continue;
            }
            if (key != kKey) continue;
            if (msg == JOB_OBJECT_MSG_NEW_PROCESS) {
                const DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(ov));
                const uint64_t s = seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
                const std::string name = resolveImageName(pid);  // best-effort
                if (!name.empty() && isAttackName(name)) {
                    std::lock_guard<std::mutex> lk(mu_);
                    named_.emplace_back(s, name);
                }
            }
        }
    }

    static std::string resolveImageName(DWORD pid) {
        std::string out;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return out;  // already exited → unresolved (best-effort)
        wchar_t buf[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            const wchar_t* base = buf;
            for (const wchar_t* c = buf; *c; ++c)
                if (*c == L'\\' || *c == L'/') base = c + 1;
            for (const wchar_t* c = base; *c; ++c) {
                wchar_t lc = (*c >= L'A' && *c <= L'Z') ? (*c + 32) : *c;
                out.push_back(static_cast<char>(lc & 0x7F));
            }
        }
        CloseHandle(h);
        return out;
    }

    static bool isAttackName(const std::string& n) {
        for (const auto& a : attackProcessNames())
            if (n == a) return true;
        return false;
    }

    static constexpr ULONG_PTR kKey = 0x50444645;  // 'PDFE'
    HANDLE port_ = nullptr;
    std::atomic<uint64_t> seq_{0};
    mutable std::mutex mu_;
    std::vector<std::pair<uint64_t, std::string>> named_;
};

// Force the monitor up at startup (call from test_main, after testProcessJob()).
inline void startSpawnEventMonitor() { (void)SpawnEventMonitor::instance(); }

#else // non-Windows: no-op snapshot + monitor (always "no spawn observed").

struct ProcessSnapshot {
    std::vector<int> counts;
    static ProcessSnapshot take() {
        ProcessSnapshot snap;
        snap.counts.assign(attackProcessNames().size(), 0);
        return snap;
    }
};

struct SpawnEventMonitor {
    static SpawnEventMonitor& instance() {
        static SpawnEventMonitor m;
        return m;
    }
    uint64_t sequence() const { return 0; }
    std::string attackSince(uint64_t) const { return ""; }
};
inline void startSpawnEventMonitor() {}

#endif

// RAII-style guard: capture a baseline at construction; newlySpawned() returns
// the image name of any attack process spawned in our process tree since
// construction (empty string if none). The caller asserts newlySpawned().empty().
class ProcessSpawnGuard {
public:
    ProcessSpawnGuard()
        : baseline_(ProcessSnapshot::take()),
          eventBaseline_(SpawnEventMonitor::instance().sequence()) {}

    // Returns the first attack-process name spawned in our tree since baseline, or
    // "" if none. A non-empty return means a child process the exploit wanted was
    // actually spawned — i.e. NOT neutralized. Two complementary sources:
    //   (1) point-in-time snapshot diff — catches a spawn still alive at check;
    //   (2) the Job completion-port event log — catches a transient in-job spawn
    //       that already exited (the fast-exit case the snapshot misses).
    std::string newlySpawned() const {
        ProcessSnapshot now = ProcessSnapshot::take();
        const auto& names = attackProcessNames();
        for (size_t i = 0; i < names.size() && i < now.counts.size(); ++i) {
            if (now.counts[i] > baseline_.counts[i]) return names[i];
        }
        return SpawnEventMonitor::instance().attackSince(eventBaseline_);
    }

    // Race-free: did ANY new process appear in our job since construction (even one
    // that has already exited, and regardless of whether its image name could be
    // resolved)? A stronger signal than newlySpawned() for callers that want to
    // flag unnamed transient in-job spawns too.
    bool observedNewInJobProcess() const {
        return SpawnEventMonitor::instance().sequence() > eventBaseline_;
    }

private:
    ProcessSnapshot baseline_;
    uint64_t eventBaseline_ = 0;
};


// ── FileWriteGuard (SEC-44) ─────────────────────────────────────────────────
//
// ProcessSpawnGuard answers "did the exploit start a program?". It does not
// answer "did the exploit WRITE something?", and a PDF JavaScript payload has
// several documented routes to the filesystem that spawn nothing at all:
// Doc.createDataObject, Doc.exportDataObject, Doc.saveAs, and the /SubmitForm
// and /ImportData action pairs. Every "the sandbox blocked it" claim in the
// hostile-JS suite was, until this guard, resting on EXPECT_NO_THROW — which a
// payload that quietly succeeded would also satisfy.
//
// SCOPING, WHICH IS THE WHOLE DESIGN PROBLEM. The naive oracle diffs %TEMP%
// before and after. That fails here for the same reason ProcessSnapshot's
// first version failed (see the note above it): %TEMP% is shared. This suite
// runs under `run_gtest.py --shards 16`, so fifteen other test processes are
// writing to the same directory throughout, and a machine-wide diff would be
// red on arrival and would then be deleted or weakened by whoever inherited
// it. So attribution is built in rather than hoped for:
//
//   * The SCRATCH directory is watched in full, recursively. The caller owns
//     it and nothing else in the process tree knows its name, so anything
//     that appears, changes, or DISAPPEARS there is ours by construction.
//     Disappearance counts: silent deletion is the data-loss half of the
//     property, and an oracle that only looks for new files would call a
//     wiped canary "clean".
//   * The shared %TEMP% root is watched only for entries whose name contains
//     a per-test MARKER string. The marker is handed to the payload as its
//     target, so a hit is attributable to this test and a concurrent shard's
//     unrelated temp file is not. Absence of a marker hit is weaker evidence
//     than the scratch directory gives, which is exactly why the scratch
//     directory is where the canary lives.
//
// The guard reports the FIRST violation it finds, as a human-readable string,
// so the assertion message names the actual path rather than "expected true".
class FileWriteGuard {
public:
    // `scratch` must exist. `marker` is optional; when empty, %TEMP% is not
    // examined at all (rather than examined with an empty filter, which would
    // match everything and reintroduce the shared-directory problem).
    explicit FileWriteGuard(std::filesystem::path scratch,
                            std::string marker = {})
        : scratch_(std::move(scratch)), marker_(std::move(marker))
    {
        scratchBefore_ = snapshotTree(scratch_);
        if (!marker_.empty()) {
            tempBefore_ = snapshotMarked(tempRoot(), marker_);
        }
    }

    // "" when nothing was written, changed or removed. Otherwise a
    // description naming the offending path and what happened to it.
    [[nodiscard]] std::string violation() const {
        const auto scratchNow = snapshotTree(scratch_);
        if (auto v = diff(scratchBefore_, scratchNow, "scratch"); !v.empty()) {
            return v;
        }
        if (!marker_.empty()) {
            const auto tempNow = snapshotMarked(tempRoot(), marker_);
            if (auto v = diff(tempBefore_, tempNow, "%TEMP%"); !v.empty()) {
                return v;
            }
        }
        return "";
    }

    [[nodiscard]] const std::filesystem::path& scratch() const noexcept {
        return scratch_;
    }
    [[nodiscard]] const std::string& marker() const noexcept { return marker_; }

    // The absolute path a payload should be pointed at so that a successful
    // write is unambiguous. Deliberately inside the watched scratch tree.
    [[nodiscard]] std::filesystem::path target(const char* leaf) const {
        return scratch_ / leaf;
    }

private:
    struct Entry {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type mtime{};
        bool operator!=(const Entry& o) const noexcept {
            return size != o.size || mtime != o.mtime;
        }
    };
    using Map = std::map<std::string, Entry>;

    static std::filesystem::path tempRoot() {
        std::error_code ec;
        auto p = std::filesystem::temp_directory_path(ec);
        return ec ? std::filesystem::path{} : p;
    }

    static Entry entryOf(const std::filesystem::directory_entry& de) {
        std::error_code ec;
        Entry e;
        if (de.is_regular_file(ec)) {
            e.size = de.file_size(ec);
            e.mtime = de.last_write_time(ec);
        }
        return e;
    }

    // Everything under `root`, recursively. Errors are skipped rather than
    // thrown: a directory that vanishes mid-walk is not the property under
    // test, and an exception here would mask the real assertion.
    static Map snapshotTree(const std::filesystem::path& root) {
        Map m;
        std::error_code ec;
        if (root.empty() || !std::filesystem::exists(root, ec)) return m;
        for (std::filesystem::recursive_directory_iterator
                 it(root, std::filesystem::directory_options::skip_permission_denied, ec),
                 end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            m[it->path().string()] = entryOf(*it);
        }
        return m;
    }

    // Only the immediate children of `root` whose filename contains `marker`.
    // Non-recursive on purpose: %TEMP% subtrees can be enormous and are not
    // ours to walk.
    static Map snapshotMarked(const std::filesystem::path& root,
                              const std::string& marker) {
        Map m;
        std::error_code ec;
        if (root.empty() || marker.empty()) return m;
        for (std::filesystem::directory_iterator
                 it(root, std::filesystem::directory_options::skip_permission_denied, ec),
                 end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const std::string name = it->path().filename().string();
            if (name.find(marker) == std::string::npos) continue;
            m[it->path().string()] = entryOf(*it);
        }
        return m;
    }

    static std::string diff(const Map& before, const Map& after,
                            const char* where) {
        for (const auto& [path, e] : after) {
            auto it = before.find(path);
            if (it == before.end()) {
                return std::string("a file APPEARED in ") + where + ": " + path;
            }
            if (it->second != e) {
                return std::string("a file was MODIFIED in ") + where + ": " + path;
            }
        }
        for (const auto& [path, e] : before) {
            (void)e;
            if (after.find(path) == after.end()) {
                return std::string("a file was DELETED from ") + where + ": " + path;
            }
        }
        return "";
    }

    std::filesystem::path scratch_;
    std::string marker_;
    Map scratchBefore_;
    Map tempBefore_;
};

} // namespace pdfe_test
