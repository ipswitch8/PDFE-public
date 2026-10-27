#pragma once
// -*- coding: utf-8 -*-
//
// DirectoryAttacker — the shared TOCTTOU attacker for publish-path hardening.
//
// GitHub #275. This struct was DEFINED TWICE, in
// test_publish_handle_hardening_phase58.cpp and _phase59.cpp, and between them the
// two copies backed 20 TOCTTOU publish tests across 9 suites.
//
// THE COST WAS MEASURED, NOT HYPOTHESISED. A poll-race fix went into the phase58
// copy; all 20 TOCTTOU cases went green; a stability run under deliberate load then
// failed 9 of 12, entirely in phase59, whose copy still had the bug. One fix, half
// applied, looking complete. UNLOADED both copies passed 12/12, which is why the
// race survived review in the first place. The silver lining was a free controlled
// A/B — same load, same harness, two copies differing only by the fix:
//
//     3 spinners on 4 cores, 12 repeats per arm
//       phase58, WITH the rendezvous     12/12 pass,  0 vacuity-guard trips
//       phase59, WITHOUT it               3/12 pass,  9 trips
//     unloaded, BOTH copies              12/12 pass
//     after fixing both                  24/24 pass, 0 trips, same load
//
// MERGING THIS WAS NOT A MOVE. The copies had drifted four ways, and one of them is
// load-bearing:
//
//   1. the counter was `renamedOver` in phase58 and `substituted` in phase59.
//      `substituted` wins: it is what win() already labels and what both
//      assertions already say.
//   2. only phase58 had `skipExt`, with the rationale now below.
//   3. only phase59 had the full rendezvous documentation (the A/B above and the
//      quarantine cross-reference). The duplication had split the REASONING as well
//      as the code — neither copy alone told the whole story.
//   4. the attack PAYLOAD differed: "PWNED-WIN87-PHASE58-…" vs "…PHASE59-…". So the
//      payload is a PARAMETER here rather than a file-local constant. Hardcoding
//      either one would have made the shared helper lie to one of the two suites.
//
// `readAll` is implemented here rather than borrowed, because both copies relied on
// a file-local helper of that name that a shared header cannot see.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace pdfe::testing {

/// The byte pattern the attacker writes into anything it can open.
///
/// A DISTINCT TYPE, and deliberately `explicit`, for a reason worth stating: the
/// pre-merge `start()` took `(dir, decoy, skip, skipExt)` and one caller passed
/// `".png"` in that fourth position. Adding `payload` as a plain `std::string`
/// fourth parameter would have left that call COMPILING, silently using ".png" as
/// the attack payload and an empty `skipExt` — a test that still passes while
/// attacking with the wrong bytes and no extension exemption. Wrapping it turns
/// every un-updated call site into a compile error instead.
struct AttackPayload {
    explicit AttackPayload(std::string b) : bytes(std::move(b)) {}
    std::string bytes;
};

/// Enumerates `dir` and attacks every artifact that is not in `skip`.
///
/// It attacks by NAME DISCOVERY, not by knowing the name: that is the whole point
/// after phase-58, since the staging names are now 128 bits of CSPRNG. Three
/// distinct attacks are counted separately because they mean different things —
/// writable, substitutable, and deletable are three different failures of the same
/// property.
struct DirectoryAttacker {
    std::atomic<bool> stop{false};
    std::atomic<bool> inOperation{false};
    std::atomic<int> saw{0};
    /// Completed passes over the watched directory. The rendezvous in
    /// beginOperation() waits on this so the operation cannot finish before the
    /// poller has looked even once.
    std::atomic<int> sweeps{0};
    std::atomic<int> openedForWrite{0};
    /// Full-file substitutions that LANDED. Named `renamedOver` in the phase58 copy
    /// and `substituted` in phase59; this is the phase59 name because it matches
    /// both the win() label and the assertion text in every caller.
    std::atomic<int> substituted{0};
    std::atomic<int> deleted{0};
    std::set<std::string> seenSet;
    std::string wins;
    std::mutex mtx;
    std::thread th;

    static std::string readAll(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }

    void note(const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(mtx);
        seenSet.insert(p.filename().string());
    }
    void win(const char* what, const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(mtx);
        wins += std::string(what) + ":" + p.filename().string() + " ";
    }
    std::set<std::string> observed()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return seenSet;
    }
    std::string seenNames()
    {
        std::lock_guard<std::mutex> lk(mtx);
        std::string s;
        for (const auto& n : seenSet) { s += n; s += ' '; }
        return s;
    }

    /// `payload` is a PARAMETER, not a constant: the two original copies used
    /// different markers ("…PHASE58-…" / "…PHASE59-…") and their suites assert on
    /// their own, so a shared constant would have broken one of them silently.
    /// See AttackPayload above for why it is a distinct type.
    ///
    /// `skipExt`, when non-empty, exempts FINISHED outputs by extension.
    ///
    /// That is not a loophole, and leaving it out produced a false failure on the
    /// first run: the attacker kept attacking "page_1.png" AFTER it had been
    /// published, then the case failed because "the published file is not a PNG" —
    /// of course it was not, the test had just overwritten it. PDFE cannot stop a
    /// process that already has write access to your output directory from
    /// scribbling on a finished export, and it has never claimed to. The property
    /// under test is that the STAGING artifact cannot be substituted so that PDFE
    /// PUBLISHES attacker bytes, which is a different claim and the one asserted.
    void start(const std::filesystem::path& dir,
               const std::filesystem::path& decoy,
               std::set<std::string> skip,
               AttackPayload payload,
               std::string skipExt = {})
    {
        namespace fs = std::filesystem;
        const std::string payloadBytes = payload.bytes;
        th = std::thread([this, dir, decoy, skip, payloadBytes, skipExt] {
            while (!stop.load(std::memory_order_relaxed)) {
                std::error_code ec;
                for (fs::directory_iterator it(dir, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    const auto p = it->path();
                    const auto name = p.filename().string();
                    if (name == decoy.filename().string()) continue;
                    if (skip.count(name)) continue;
                    if (!skipExt.empty() && p.extension() == skipExt) continue;
                    saw.fetch_add(1, std::memory_order_relaxed);
                    note(p);
                    if (!inOperation.load(std::memory_order_relaxed)) continue;

#if defined(_WIN32)
                    HANDLE w = ::CreateFileW(
                        p.native().c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (w != INVALID_HANDLE_VALUE) {
                        openedForWrite.fetch_add(1, std::memory_order_relaxed);
                        win("wrote", p);
                        DWORD wrote = 0;
                        ::SetFilePointer(w, 0, nullptr, FILE_BEGIN);
                        ::WriteFile(w, payloadBytes.data(),
                                    static_cast<DWORD>(payloadBytes.size()),
                                    &wrote, nullptr);
                        ::FlushFileBuffers(w);
                        ::CloseHandle(w);
                    }

                    if (!inOperation.load(std::memory_order_relaxed)) continue;
                    // FULL-FILE SUBSTITUTION, AND IT MUST BE ATTEMPTED THROUGH AN
                    // OPEN_EXISTING HANDLE — not std::filesystem::copy_file.
                    //
                    // MEASURED, and it produced a false failure on the first run:
                    // copy_file(overwrite_existing) CREATES the destination when it
                    // is absent, so after the publish renamed the staging file away,
                    // the attacker "substituted" a name that no longer belonged to
                    // anything. That is an attacker littering an empty directory
                    // entry, not PDFE publishing attacker bytes, and counting it
                    // would have reported a live defect where there is none.
                    // OPEN_EXISTING cannot lie about this: the file either exists
                    // and is obtainable, or the attack did not land.
                    HANDLE sub = ::CreateFileW(
                        p.native().c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (sub != INVALID_HANDLE_VALUE) {
                        const auto body = readAll(decoy);
                        DWORD wrote = 0;
                        ::SetFilePointer(sub, 0, nullptr, FILE_BEGIN);
                        ::WriteFile(sub, body.data(),
                                    static_cast<DWORD>(body.size()), &wrote,
                                    nullptr);
                        ::SetEndOfFile(sub);
                        ::FlushFileBuffers(sub);
                        ::CloseHandle(sub);
                        substituted.fetch_add(1, std::memory_order_relaxed);
                        win("substituted", p);
                    }
#endif  // _WIN32

                    if (!inOperation.load(std::memory_order_relaxed)) continue;
                    std::error_code dec;
                    if (fs::remove(p, dec) && !dec) {
                        deleted.fetch_add(1, std::memory_order_relaxed);
                        win("deleted", p);
                    }
                }
                sweeps.fetch_add(1, std::memory_order_release);
            }
        });
    }

    /// Open the attack window only once the poller is demonstrably running.
    ///
    /// MEASURED. `saw > 0` is the vacuity guard in every caller: a green run in
    /// which the attacker observed nothing proves nothing. `beginOperation()` used
    /// to only store a flag, with no rendezvous against the thread created in
    /// `start()`, so on a loaded machine the operation could finish before the
    /// poller was ever scheduled — and the guard then fired `0 vs 0`, reporting a
    /// product defect where there was only a scheduling accident.
    ///
    /// `ComplianceConverterPublishTocttou.PoisoningAnyStagedPdfUAArtifact…` is
    /// carried in tools/gate/flaky_quarantine.json for exactly that, and the entry
    /// names this fix in preference to a wider timeout: "gate the conversion on the
    /// attacker thread having sampled at least once".
    ///
    /// A/B UNDER IDENTICAL LOAD (3 spinners on 4 cores, 12 repeats each),
    /// exploiting the fact that this struct was duplicated and only one copy had
    /// been fixed:
    ///
    ///     phase58 (with rendezvous)    12/12 pass,  0 vacuity trips
    ///     phase59 (without)             3/12 pass,  9 trips
    ///     unloaded, both               12/12 pass   <- why it survived review
    ///     after fixing both            24/24 pass,  0 trips
    /// Waiting for a COMPLETED sweep rather than merely for the thread to start: a
    /// thread that has begun but not yet iterated the directory can still be
    /// descheduled before its first look, which is the same race one step later.
    ///
    /// Bounded, and deliberately does NOT assert on the bound. If the poller cannot
    /// complete a sweep in two seconds the machine is pathologically loaded;
    /// proceeding lets the caller's `saw > 0` guard fail and say so, which is more
    /// useful than a second failure mode here. The wait is ~instant in practice:
    /// the loop has no sleep in it.
    ///
    /// ORDER IS LOAD-BEARING. The rendezvous happens BEFORE `inOperation` is set.
    /// Setting the flag first would open the attack window during the wait, which is
    /// the very interval this exists to close.
    void beginOperation()
    {
        const int before = sweeps.load(std::memory_order_acquire);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (sweeps.load(std::memory_order_acquire) <= before
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        inOperation.store(true);
    }
    void endOperation() { inOperation.store(false); }
    void join()
    {
        stop.store(true);
        if (th.joinable()) th.join();
    }
    /// The destructor joins, so an early ASSERT_* return in a test body cannot leave
    /// the poller running against a directory the fixture is about to delete.
    ~DirectoryAttacker() { join(); }
};

}  // namespace pdfe::testing
