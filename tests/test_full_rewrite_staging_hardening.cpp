// -*- coding: utf-8 -*-
// ===========================================================================
// WIN-87 — the full-rewrite publish path, phase-56 (follow-up phase 56).
//
// WHAT THIS FILE MEASURES, AND WHY IT IS A DIFFERENT QUESTION FROM F58's.
//
// test_staged_save_hardening.cpp asks whether the STAGED INCREMENTAL APPEND can
// be attacked. This file asks the same question of the other two publish sites —
// Document::save()'s FULL-REWRITE branch (site #1) and Document::saveAs()
// (site #2) — which phase-53 did not migrate and which, until phase-56, resolved
// their staging file BY NAME three separate times:
//
//     1. the write            pdf_save_document(ctx, doc, "<t>.pdfe_tmp", …)
//     2. the durability flush flushFileToStableStorage("<t>.pdfe_tmp")
//                             — CreateFileW/OPEN_EXISTING, by path
//     3. the publish          atomicReplaceFileWithRetry("<t>.pdfe_tmp", t)
//                             — MoveFileExW, by path
//
// Three nameable moments, no handle held across any of them. That is exactly the
// shape phase-52 demonstrated end-to-end against the incremental path: a
// concurrent unprivileged process stamped its own bytes into the staged file and
// PDFE atomically published them over the user's document and reported success.
//
// THE ORACLES ARE THE ATTACKER'S OWN INSTRUMENTS, NOT A PROXY. Every hostile
// test here counts BOTH what the attacker observed and what it managed to do, so
// a green result cannot come from "the window never happened". A test that only
// asserted "the published file is fine" would pass just as loudly against a save
// that never staged, or one whose window closed before the poller looked.
//
// WHY THE ATTACKS ARE SPLIT INTO WRITE AND DELETE/RENAME. FILE_SHARE_READ denies
// both, but for different reasons and with different failure modes: a successful
// WRITE substitutes content silently, while a successful DELETE or RENAME turns
// the publish into either a failure or — with a decoy renamed into place — a
// substitution that a content check alone would catch but a residue check would
// not. Both are the WIN-87 prescribed action ("delete/rename/replace the staging
// file out from under the open handle mid-save").
// ===========================================================================

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/AtomicFileReplace.h"
#include "core/Document.h"

#include "support/SaveFailureInjection.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QRectF>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

// F65 (follow-up phase-67). These helpers were defined AGAIN in this file --
// the fourth copy, which NEITHER the register row (which named two symbols)
// nor the reverification (which corrected it to three) knew about. Found by
// enumerating definitions across the whole tests/ tree rather than reading
// either count. They now have exactly one definition each, in tests/support/.
#if defined(_WIN32)
using pdfe::testing::applyProtectedDacl;
using pdfe::testing::currentUserSidString;
#endif

using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::StagingFile;
using pdfe::core::StagingNameKind;

std::filesystem::path fixtures()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

std::filesystem::path makeWorkDir(const char* tag)
{
    std::error_code ec;
    auto d = std::filesystem::temp_directory_path()
             / (std::string("pdfe_win87_") + tag + "_"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(d, ec);
    return d;
}

/// A source document big enough that the write is not instantaneous — the
/// hostile tests assert the attacker SAW the staging file, so a too-small window
/// fails loudly rather than passing falsely, but there is no reason to make it
/// fight for the observation.
std::filesystem::path sourceFixture()
{
    for (const char* rel : {"realworld/irs_p17.pdf", "realworld/irs_i1040.pdf",
                            "realworld/arxiv_attention.pdf", "test_3page.pdf"}) {
        const auto p = fixtures() / rel;
        if (std::filesystem::exists(p)) {
            return p;
        }
    }
    return {};
}

bool addAnnotation(const std::shared_ptr<MuPdfContext>& ctx, Document* doc)
{
    try {
        AnnotationManager am(ctx);
        auto page = doc->page(0);
        if (!page) return false;
        am.createAnnotation(page.get(), AnnotationType::Square,
                            QRectF(50, 50, 120, 60));
        return true;
    } catch (...) {
        return false;
    }
}

/// Every sibling of `target` that is any kind of PDFE staging artifact.
std::vector<std::filesystem::path> anyStagingResidue(
    const std::filesystem::path& target)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(target.parent_path(), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (pdfe::core::classifyStagingName(target.filename(),
                                            it->path().filename())
            != StagingNameKind::NotStaging) {
            out.push_back(it->path());
        }
    }
    return out;
}

#if defined(_WIN32)

/// The SID of the user running the test, as a string.
std::string currentUserSid()
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
        LPWSTR sidText = nullptr;
        if (::ConvertSidToStringSidW(tu->User.Sid, &sidText)) {
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, sidText, -1, nullptr,
                                                0, nullptr, nullptr);
            out.resize(n > 0 ? n - 1 : 0);
            if (n > 1) {
                ::WideCharToMultiByte(CP_UTF8, 0, sidText, -1, out.data(), n,
                                      nullptr, nullptr);
            }
            ::LocalFree(sidText);
        }
    }
    ::CloseHandle(tok);
    return out;
}


/// A directory junction at `link` pointing at `target`. `mklink /J` needs no
/// privilege and no developer mode, which is exactly why it is the realistic
/// plant — and it is the reparse tag MSVC's is_symlink() does NOT report. Same
/// technique tests/test_search_index_reparse_sweep.cpp uses.
bool makeJunction(const std::filesystem::path& link,
                  const std::filesystem::path& target)
{
    const std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \""
                          + target.string() + "\" >nul 2>&1";
    (void)std::system(cmd.c_str());
    const DWORD attrs = ::GetFileAttributesW(link.native().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES
           && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

/// The shared instrument for the "concurrent writer" attacks: poll the
/// directory, try to open every non-victim sibling for WRITING, stamp a marker
/// over offset 0. Records what it saw and what it managed to do.
struct WriteAttacker {
    static constexpr const char* kPayload =
        "PWNED-FULL-REWRITE-0123456789ABCDEF0123456789ABCDEF";

    std::atomic<bool> stop{false};
    std::atomic<int> sawStaging{0};
    std::atomic<int> openedForWrite{0};
    std::atomic<int> tamperedBytes{0};
    std::string firstStagingName;
    std::mutex nameMutex;
    std::thread th;

    void start(const std::filesystem::path& dir,
               const std::filesystem::path& victim)
    {
        th = std::thread([this, dir, victim] {
            while (!stop.load(std::memory_order_relaxed)) {
                std::error_code it_ec;
                for (std::filesystem::directory_iterator it(dir, it_ec), end;
                     !it_ec && it != end; it.increment(it_ec)) {
                    const auto name = it->path().filename();
                    if (name == victim.filename()) {
                        continue;
                    }
                    sawStaging.fetch_add(1, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lk(nameMutex);
                        if (firstStagingName.empty()) {
                            firstStagingName = name.string();
                        }
                    }
                    HANDLE h = ::CreateFileW(
                        it->path().native().c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (h != INVALID_HANDLE_VALUE) {
                        openedForWrite.fetch_add(1, std::memory_order_relaxed);
                        DWORD wrote = 0;
                        ::SetFilePointer(h, 0, nullptr, FILE_BEGIN);
                        if (::WriteFile(h, kPayload,
                                        static_cast<DWORD>(
                                            std::char_traits<char>::length(
                                                kPayload)),
                                        &wrote, nullptr)) {
                            tamperedBytes.fetch_add(static_cast<int>(wrote),
                                                    std::memory_order_relaxed);
                        }
                        ::FlushFileBuffers(h);
                        ::CloseHandle(h);
                    }
                }
            }
        });
    }

    void join()
    {
        stop.store(true, std::memory_order_relaxed);
        if (th.joinable()) th.join();
    }
};

/// The other half of the WIN-87 prescribed action: try to DELETE the staging
/// file, and try to RENAME a decoy over it. A successful rename is the more
/// dangerous of the two — it substitutes a whole file rather than a prefix.
///
/// WHY THERE IS A DELETE-ACCESS PROBE IN FRONT OF BOTH, AND WHY IT IS NOT
/// DECORATION. The first version of this attacker counted a bare
/// MoveFileExW(decoy → stagingName, REPLACE_EXISTING) success as a hit, and it
/// reported one against the FIXED code. It was a false positive, and an
/// instructive one: MoveFileExW with REPLACE_EXISTING onto a name that is
/// ALREADY FREE simply creates the file. The directory listing was captured
/// while the staging file existed; by the time the rename ran, the save had
/// published it and the name was vacant. The "attack" replaced nothing.
///
/// A CreateFileW(..., DELETE, OPEN_EXISTING) probe cannot lie that way: it
/// fails when the name is gone, and it succeeds ONLY if a file is there AND our
/// exclusive handle is not denying DELETE — which is exactly the property being
/// tested, and the necessary precondition for both a delete and a
/// rename-over. The destructive operations still run (they are the prescribed
/// action), but only from inside a branch that has already proved the window
/// was real. An oracle whose green state is reachable by "the thing I attacked
/// wasn't there" is worth nothing.
struct UnlinkAttacker {
    std::atomic<bool> stop{false};
    std::atomic<int> sawStaging{0};
    std::atomic<int> openedForDelete{0};
    std::atomic<int> deleted{0};
    std::atomic<int> renamedOver{0};
    std::string victimNames;   ///< what it actually managed to unlink/replace
    std::mutex namesMutex;
    std::thread th;

    void note(const std::filesystem::path& p, const char* what)
    {
        std::lock_guard<std::mutex> lk(namesMutex);
        victimNames += std::string(what) + ":" + p.filename().string() + " ";
    }

    void start(const std::filesystem::path& dir,
               const std::filesystem::path& victim,
               const std::filesystem::path& decoy)
    {
        th = std::thread([this, dir, victim, decoy] {
            while (!stop.load(std::memory_order_relaxed)) {
                std::error_code it_ec;
                for (std::filesystem::directory_iterator it(dir, it_ec), end;
                     !it_ec && it != end; it.increment(it_ec)) {
                    const auto p = it->path();
                    if (p.filename() == victim.filename()
                        || p.filename() == decoy.filename()) {
                        continue;
                    }
                    sawStaging.fetch_add(1, std::memory_order_relaxed);

                    HANDLE d = ::CreateFileW(
                        p.native().c_str(), DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (d == INVALID_HANDLE_VALUE) {
                        continue;  // gone, or refused — either way, no window
                    }
                    ::CloseHandle(d);
                    openedForDelete.fetch_add(1, std::memory_order_relaxed);
                    note(p, "delete-access");

                    if (::DeleteFileW(p.native().c_str())) {
                        deleted.fetch_add(1, std::memory_order_relaxed);
                        note(p, "deleted");
                        continue;
                    }
                    if (::MoveFileExW(decoy.native().c_str(),
                                      p.native().c_str(),
                                      MOVEFILE_REPLACE_EXISTING)) {
                        renamedOver.fetch_add(1, std::memory_order_relaxed);
                        note(p, "renamed-over");
                        // Put the decoy back so the loop can keep trying.
                        std::error_code cp_ec;
                        std::filesystem::copy_file(
                            p, decoy, std::filesystem::copy_options::overwrite_existing,
                            cp_ec);
                    }
                }
            }
        });
    }

    void join()
    {
        stop.store(true, std::memory_order_relaxed);
        if (th.joinable()) th.join();
    }
};

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

// ===========================================================================
// A. The primitive: StagingFile::createFullRewriteTempFor.
// ===========================================================================

// The name is a CONTRACT, not a detail (F58-d). "<target>.pdfe_tmp" is the
// R2-04 crash-recovery artifact the Document constructor surfaces to the user
// as recoverable unsaved work; "<target>.pdfe_part.<hex>" is internal residue
// that is SWEPT and never surfaced. An implementation that satisfied WIN-87 by
// simply calling createBeside would pass every hostile test in this file and
// silently retire crash recovery for full-rewrite saves. This is the test that
// would notice.
TEST(FullRewriteStagingPrimitive, UsesTheContractualRecoverableNameNotARandomOne)
{
    const auto dir = makeWorkDir("primname");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }

    std::error_code ec;
    auto sf = StagingFile::createFullRewriteTempFor(target, ec);
    ASSERT_NE(sf, nullptr) << ec.message();

    EXPECT_EQ(sf->path(), pdfe::core::fullRewriteTempPathFor(target));
    EXPECT_TRUE(pdfe::core::isFullRewriteTempName(target.filename(),
                                                  sf->path().filename()));
    EXPECT_EQ(pdfe::core::classifyStagingName(target.filename(),
                                              sf->path().filename()),
              StagingNameKind::CompleteFullRewrite)
        << "the full-rewrite staging file must carry the RECOVERABLE meaning. "
           "If this reports PartialIncrementalAppend, the factory has been "
           "swapped for createBeside and R2-04 crash recovery is gone.";

    sf.reset();
    std::filesystem::remove_all(dir, ec);
}

// The three properties, minus the one deliberately given up. Predictability is
// traded away on purpose (see the header); CREATE_NEW and the exclusive handle
// are not, and they are what close the substitution attack.
TEST(FullRewriteStagingPrimitive, HoldsAHandleNoOneElseCanWriteRenameOrDelete)
{
    const auto dir = makeWorkDir("primlock");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }

    std::error_code ec;
    auto sf = StagingFile::createFullRewriteTempFor(target, ec);
    ASSERT_NE(sf, nullptr) << ec.message();
    const auto staged = sf->path();

    HANDLE w = ::CreateFileW(staged.native().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE
                                 | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    EXPECT_EQ(w, INVALID_HANDLE_VALUE)
        << "another process opened the staging file for WRITING — that is the "
           "F58-a substitution attack, on the full-rewrite path";
    if (w != INVALID_HANDLE_VALUE) ::CloseHandle(w);

    EXPECT_FALSE(::DeleteFileW(staged.native().c_str()))
        << "another process DELETED the staging file out from under the save";

    const auto elsewhere = dir / "hijack.pdf";
    { std::ofstream f(elsewhere, std::ios::binary); f << "DECOY"; }
    EXPECT_FALSE(::MoveFileExW(elsewhere.native().c_str(),
                               staged.native().c_str(),
                               MOVEFILE_REPLACE_EXISTING))
        << "another process RENAMED a decoy over the staging file";

    // FILE_SHARE_READ is deliberate, and the test says so rather than leaving
    // it to be tightened by accident later: readers are harmless and denying
    // them breaks A/V, backup and size-polling watchers for no integrity gain.
    // Share EVERYTHING on our side: a reader that itself demanded exclusivity
    // would be refused because of ITS OWN share mode, which would prove nothing
    // about StagingFile's.
    HANDLE r = ::CreateFileW(staged.native().c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE
                                 | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    EXPECT_NE(r, INVALID_HANDLE_VALUE)
        << "the share mode is TIGHTER than FILE_SHARE_READ — intentional? "
           "See the StagingFile header before changing this expectation.";
    if (r != INVALID_HANDLE_VALUE) ::CloseHandle(r);

    sf.reset();
    std::filesystem::remove_all(dir, ec);
}

// Criterion 3, the ordinary half. A crashed prior save leaves residue at exactly
// this name; CREATE_NEW alone would then refuse every subsequent save forever.
// The clear must therefore happen — but it must never ADOPT what it found.
TEST(FullRewriteStagingPrimitive, PrePlantedRegularFileIsClearedNeverAdopted)
{
    const auto dir = makeWorkDir("primplant");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }
    const auto tempName = pdfe::core::fullRewriteTempPathFor(target);
    {
        std::ofstream f(tempName, std::ios::binary);
        f << "PLANTED-BY-ATTACKER-THIS-MUST-NEVER-BE-PUBLISHED";
    }

    std::error_code ec;
    auto sf = StagingFile::createFullRewriteTempFor(target, ec);
    ASSERT_NE(sf, nullptr) << "a stale/planted temp must not make saving "
                              "impossible forever: " << ec.message();

    std::uintmax_t size = 0;
    ASSERT_TRUE(sf->sizeInBytes(size, ec)) << ec.message();
    EXPECT_EQ(size, 0u)
        << "the staging file was ADOPTED rather than created — the planted "
           "bytes are still there and would be published";

    // And publishing really does publish OUR file, not the planted one.
    ASSERT_TRUE(sf->commitOverWithRetry(target, ec)) << ec.message();
    const std::string published = readAll(target);
    EXPECT_EQ(published.find("PLANTED-BY-ATTACKER"), std::string::npos)
        << "attacker bytes were published under the target's name";

    sf.reset();
    std::filesystem::remove_all(dir, ec);
}

// A read-only plant is the cheapest permanent denial of service against a
// CREATE_NEW-only design, and "the save is refused forever" is not an
// acceptable resting state for a file the product itself owns the name of.
TEST(FullRewriteStagingPrimitive, PrePlantedReadOnlyFileIsNotAPermanentDenial)
{
    const auto dir = makeWorkDir("primro");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }
    const auto tempName = pdfe::core::fullRewriteTempPathFor(target);
    { std::ofstream f(tempName, std::ios::binary); f << "PLANTED-READONLY"; }
    ASSERT_TRUE(::SetFileAttributesW(tempName.native().c_str(),
                                     FILE_ATTRIBUTE_READONLY));

    std::error_code ec;
    auto sf = StagingFile::createFullRewriteTempFor(target, ec);
    ASSERT_NE(sf, nullptr) << ec.message();
    std::uintmax_t size = 0;
    ASSERT_TRUE(sf->sizeInBytes(size, ec));
    EXPECT_EQ(size, 0u);

    sf.reset();
    std::filesystem::remove_all(dir, ec);
}

// The clear must REFUSE what it must not delete. A real directory at the temp
// name is not our residue; a clear that recursively removed one would be a
// worse defect than the one it was written to fix.
TEST(FullRewriteStagingPrimitive, ARealDirectoryAtTheTempNameIsRefusedNotDeleted)
{
    const auto dir = makeWorkDir("primdir");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }
    const auto tempName = pdfe::core::fullRewriteTempPathFor(target);
    std::filesystem::create_directories(tempName);
    { std::ofstream f(tempName / "PRECIOUS.txt"); f << "DO-NOT-DELETE-ME"; }

    std::error_code ec;
    auto sf = StagingFile::createFullRewriteTempFor(target, ec);
    EXPECT_EQ(sf, nullptr) << "a real directory at the staging name must be "
                              "refused, not cleared";
    EXPECT_TRUE(static_cast<bool>(ec)) << "the refusal must be diagnosable";
    EXPECT_TRUE(std::filesystem::exists(tempName / "PRECIOUS.txt"))
        << "the stale-name clear DELETED a real directory's contents";

    std::error_code rmec;
    std::filesystem::remove_all(dir, rmec);
}

// Criterion 3, the dangerous half. A reparse point is the plant that REDIRECTS:
// a clear implemented as std::filesystem::remove_all, or a write implemented by
// path, follows it and operates somewhere else entirely. The junction must be
// unlinked as the LINK, and the canary it points at must be untouched.
TEST(FullRewriteStagingPrimitive, PrePlantedJunctionIsUnlinkedWithoutFollowingIt)
{
    const auto dir = makeWorkDir("primjunc");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }

    const auto canary = dir / "canary_dir";
    std::filesystem::create_directories(canary);
    { std::ofstream f(canary / "PRECIOUS.txt"); f << "DO-NOT-DELETE-ME"; }

    const auto tempName = pdfe::core::fullRewriteTempPathFor(target);
    ASSERT_TRUE(makeJunction(tempName, canary))
        << "could not plant an NTFS junction, so the reparse-point defence "
           "cannot be exercised at all. This FAILS rather than skipping "
           "deliberately: `mklink /J` needs no privilege — investigate the "
           "environment, do not disable this test.";

    std::error_code ec;
    auto sf = StagingFile::createFullRewriteTempFor(target, ec);
    ASSERT_NE(sf, nullptr) << ec.message();

    EXPECT_TRUE(std::filesystem::exists(canary / "PRECIOUS.txt"))
        << "the stale-name clear followed the junction and deleted through it";
    std::uintmax_t size = 0;
    ASSERT_TRUE(sf->sizeInBytes(size, ec));
    EXPECT_EQ(size, 0u) << "the junction's target was adopted as the staging "
                           "file — writes would have landed in the canary";

    sf.reset();
    std::error_code rmec;
    std::filesystem::remove(tempName, rmec);
    std::filesystem::remove_all(dir, rmec);
}

// ===========================================================================
// B. Site #1 — Document::save(), full-rewrite branch.
// ===========================================================================

// KILLS: reverting the write to pdf_save_document-by-path; reverting the flush
// to flushFileToStableStorage(tempPath); reverting the publish to
// atomicReplaceFileWithRetry. Any ONE of the three re-opens the name, and
// `openedForWrite` becomes non-zero.
TEST(FullRewriteSaveTocttou, ConcurrentWriterCannotTamperWithTheStagedRewrite)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty()) << "no usable source fixture";

    const auto dir = makeWorkDir("savewrite");
    const auto target = dir / "victim.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec) << ec.message();

    WriteAttacker attacker;
    attacker.start(dir, target);

    bool saveThrew = false;
    std::string saveError;
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        // Force the FULL-REWRITE branch explicitly. Relying on the routing
        // heuristics would make this test silently drift onto the incremental
        // path (already covered elsewhere) and stop testing site #1 at all.
        doc->markRequiresFullRewrite();
        try {
            doc->save(target);
        } catch (const std::exception& e) {
            saveThrew = true;
            saveError = e.what();
        }
    }
    attacker.join();

    EXPECT_FALSE(saveThrew) << "the full-rewrite save failed: " << saveError;

    EXPECT_GT(attacker.sawStaging.load(), 0)
        << "the concurrent writer never observed a staging file, so NOTHING "
           "was attacked and a clean result here would prove nothing. Either "
           "the save did not stage, or the window closed before the poller "
           "looked.";
    EXPECT_EQ(attacker.openedForWrite.load(), 0)
        << "a concurrent unprivileged writer OPENED the full-rewrite staging "
           "file for writing. Staging file seen: " << attacker.firstStagingName;
    EXPECT_EQ(attacker.tamperedBytes.load(), 0);

    const std::string after = readAll(target);
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(after.compare(0, 5, "%PDF-"), 0)
        << "the published file is not a PDF";
    EXPECT_EQ(after.find(WriteAttacker::kPayload), std::string::npos)
        << "attacker bytes were published over the user's document";
    EXPECT_TRUE(anyStagingResidue(target).empty())
        << "an uninterrupted full-rewrite save must leave no staging file";

    std::filesystem::remove_all(dir, ec);
}

// The WIN-87 prescribed action in its own right: delete/rename/replace the
// staging file out from under the open handle mid-save. A diagnosable outcome,
// no crash, and above all no silent adoption of attacker-controlled bytes.
TEST(FullRewriteSaveTocttou, HostileDeleteAndRenameCannotSubstituteThePublished)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());

    const auto dir = makeWorkDir("savekill");
    const auto target = dir / "victim.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec) << ec.message();

    // The decoy lives in the same directory so a MoveFileExW onto the staging
    // name is a same-volume rename, i.e. the strongest form of the attack.
    const auto decoy = dir / "decoy.bin";
    {
        std::ofstream f(decoy, std::ios::binary);
        f << "%PDF-1.7\nDECOY-SUBSTITUTED-FILE-0123456789\n";
    }

    UnlinkAttacker attacker;
    attacker.start(dir, target, decoy);

    bool saveThrew = false;
    std::string saveError;
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        doc->markRequiresFullRewrite();
        try {
            doc->save(target);
        } catch (const std::exception& e) {
            saveThrew = true;
            saveError = e.what();
        }
    }
    attacker.join();

    EXPECT_GT(attacker.sawStaging.load(), 0)
        << "the attacker never observed a staging file, so nothing was attacked";
    EXPECT_EQ(attacker.openedForDelete.load(), 0)
        << "another process obtained DELETE access to the staging file while "
           "the save held it — the precondition for both unlinking it and "
           "renaming a decoy over it. Observed: " << attacker.victimNames;
    EXPECT_EQ(attacker.deleted.load(), 0)
        << "the staging file was DELETED out from under the open handle."
           " Unlinked: " << attacker.victimNames;
    EXPECT_EQ(attacker.renamedOver.load(), 0)
        << "a decoy was RENAMED over the staging file — whatever is published "
           "next is the attacker's whole file, not ours. Unlinked: "
        << attacker.victimNames;

    // Either outcome is acceptable; publishing the decoy is not.
    const std::string after = readAll(target);
    ASSERT_FALSE(after.empty()) << "the target was destroyed (save threw: "
                               << saveError << ")";
    EXPECT_EQ(after.compare(0, 5, "%PDF-"), 0);
    EXPECT_EQ(after.find("DECOY-SUBSTITUTED-FILE"), std::string::npos)
        << "the decoy was published under the target's name";
    if (saveThrew) {
        EXPECT_FALSE(saveError.empty()) << "the failure must be diagnosable";
    }

    std::filesystem::remove_all(dir, ec);
}

// Criterion 3, end to end through the product rather than the primitive: a
// reparse point planted at the name save() is ABOUT to use. The pre-phase-56
// code had no stale-name check at all here, so pdf_save_document's fopen("wb")
// would have followed it.
TEST(FullRewriteSaveTocttou, PrePlantedJunctionAtTheSaveTempIsNeverFollowed)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());

    const auto dir = makeWorkDir("savejunc");
    const auto target = dir / "victim.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec) << ec.message();
    const std::string before = readAll(target);

    const auto canary = dir / "canary_dir";
    std::filesystem::create_directories(canary);
    { std::ofstream f(canary / "PRECIOUS.txt"); f << "DO-NOT-DELETE-ME"; }

    const auto tempName = pdfe::core::fullRewriteTempPathFor(target);
    ASSERT_TRUE(makeJunction(tempName, canary))
        << "could not plant an NTFS junction — see the primitive-level test's "
           "note; this FAILS rather than skips deliberately";

    bool saveThrew = false;
    std::string saveError;
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        doc->markRequiresFullRewrite();
        try {
            doc->save(target);
        } catch (const std::exception& e) {
            saveThrew = true;
            saveError = e.what();
        }
    }

    EXPECT_TRUE(std::filesystem::exists(canary / "PRECIOUS.txt"))
        << "the save wrote or deleted THROUGH the planted junction";
    const std::string after = readAll(target);
    ASSERT_FALSE(after.empty())
        << "the target was destroyed (save threw: " << saveError << ")";
    EXPECT_EQ(after.compare(0, 5, "%PDF-"), 0);
    EXPECT_EQ(after.find("DO-NOT-DELETE-ME"), std::string::npos)
        << "content reached the document from the planted reparse point";

    // THE STRONGER OF THE TWO OUTCOMES THE CRITERION ALLOWS, PINNED
    // DELIBERATELY. WIN-87 criterion 3 accepts either "refuses it" or
    // "publishes only content from the handle it created". PDFE implements the
    // second: createFullRewriteTempFor unlinks the reparse point as a LINK and
    // creates its own file, so the save SUCCEEDS. Accepting a refusal here as
    // well would have made this test non-discriminating — measured, not
    // assumed: against the pre-phase-56 path-based code the save throws
    // (fopen("wb") on a name occupied by a directory junction fails), the
    // canary survives for a reason that has nothing to do with any defence,
    // and a test that tolerated that outcome would have gone green on the
    // vulnerable build. The permanent denial of service is also a real defect
    // in its own right: a planted junction no user can see makes the document
    // unsaveable forever.
    EXPECT_FALSE(saveThrew)
        << "the save was DENIED by a planted junction rather than clearing it: "
        << saveError;
    EXPECT_NE(after, before)
        << "the save reported success but published nothing, so the checks "
           "above measured the unmodified original";

    std::error_code rmec;
    std::filesystem::remove(tempName, rmec);
    std::filesystem::remove_all(dir, rmec);
}

// ===========================================================================
// C. Site #2 — Document::saveAs().
// ===========================================================================

TEST(SaveAsStagingTocttou, ConcurrentWriterCannotTamperWithTheStagedSaveAs)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());

    const auto dir = makeWorkDir("saveaswrite");
    // The SOURCE document lives in a different directory from the one under
    // attack. Deliberately: the attacker stamps bytes into every sibling it can
    // open, and MuPDF's read handle on the source does not deny writers — a
    // corrupted source would fail the save for a reason that has nothing to do
    // with the staging window this test exists to measure.
    const auto srcDir = makeWorkDir("saveaswrite_src");
    const auto source = srcDir / "source.pdf";
    const auto target = dir / "victim.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, source, ec);
    ASSERT_FALSE(ec) << ec.message();
    // saveAs ONTO AN EXISTING FILE — the case that has always staged.
    {
        std::ofstream f(target, std::ios::binary);
        f << "%PDF-1.7\nPRE-EXISTING-TARGET-CONTENT\n";
    }

    WriteAttacker attacker;
    attacker.start(dir, target);

    bool saveThrew = false;
    std::string saveError;
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, source);
        ASSERT_NE(doc, nullptr);
        (void)addAnnotation(ctx, doc.get());
        try {
            doc->saveAs(target);
        } catch (const std::exception& e) {
            saveThrew = true;
            saveError = e.what();
        }
    }
    attacker.join();

    EXPECT_FALSE(saveThrew) << "the staged saveAs failed: " << saveError;
    EXPECT_GT(attacker.sawStaging.load(), 0);

    const std::string after = readAll(target);
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(after.compare(0, 5, "%PDF-"), 0);
    EXPECT_EQ(after.find("PRE-EXISTING-TARGET-CONTENT"), std::string::npos)
        << "saveAs did not actually replace the target, so nothing here is "
           "measuring the publish";
    EXPECT_EQ(after.find(WriteAttacker::kPayload), std::string::npos)
        << "attacker bytes were published over the saveAs target";
    EXPECT_TRUE(anyStagingResidue(target).empty());

    std::filesystem::remove_all(dir, ec);
}

TEST(SaveAsStagingTocttou, HostileDeleteAndRenameCannotSubstituteTheSaveAs)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());

    const auto dir = makeWorkDir("saveaskill");
    const auto srcDir = makeWorkDir("saveaskill_src");  // see the test above
    const auto source = srcDir / "source.pdf";
    const auto target = dir / "victim.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, source, ec);
    ASSERT_FALSE(ec) << ec.message();
    {
        std::ofstream f(target, std::ios::binary);
        f << "%PDF-1.7\nPRE-EXISTING-TARGET-CONTENT\n";
    }
    const auto decoy = dir / "decoy.bin";
    {
        std::ofstream f(decoy, std::ios::binary);
        f << "%PDF-1.7\nDECOY-SUBSTITUTED-FILE-0123456789\n";
    }

    UnlinkAttacker attacker;
    attacker.start(dir, target, decoy);

    bool saveThrew = false;
    std::string saveError;
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, source);
        ASSERT_NE(doc, nullptr);
        try {
            doc->saveAs(target);
        } catch (const std::exception& e) {
            saveThrew = true;
            saveError = e.what();
        }
    }
    attacker.join();

    EXPECT_GT(attacker.sawStaging.load(), 0)
        << "the attacker never observed a staging file, so nothing was attacked";
    EXPECT_EQ(attacker.openedForDelete.load(), 0)
        << "another process obtained DELETE access to the saveAs staging file "
           "while the save held it. Observed: " << attacker.victimNames;
    EXPECT_EQ(attacker.deleted.load(), 0)
        << "the saveAs staging file was DELETED out from under the open handle."
           " Unlinked: " << attacker.victimNames;
    EXPECT_EQ(attacker.renamedOver.load(), 0)
        << "a decoy was RENAMED over the saveAs staging file."
           " Unlinked: " << attacker.victimNames;
    const std::string after = readAll(target);
    ASSERT_FALSE(after.empty()) << "the target was destroyed (save threw: "
                                << saveError << ")";
    EXPECT_EQ(after.find("DECOY-SUBSTITUTED-FILE"), std::string::npos)
        << "the decoy was published under the saveAs target's name";

    std::filesystem::remove_all(dir, ec);
}

// ===========================================================================
// D. The four defects the non-gate security-code-reviewer found in this fix.
// ===========================================================================

// F1 — THE FIX'S OWN FAILURE PATH WAS THE BUG IT REMOVES.
//
// ~StagingFile() used to CloseHandle() and then remove() BY NAME. With
// createBeside's 128-bit random name that was tolerable; with the contractual,
// PREDICTABLE full-rewrite name it is the WIN-87 shape, and worse than the
// hostile version because it is self-inflicted: commitOver copies the
// DESTINATION's DACL onto the staging file, so a destination whose protected
// ACL does not grant this user DELETE leaves the by-name remove() with
// ACCESS_DENIED. The orphan then sits at the contractual name forever, the
// stale-name clear fails closed on the same check, and EVERY subsequent save of
// that document fails permanently.
//
// This test reproduces that state exactly. The handle acquired DELETE at
// CREATION, before any DACL existed, so a disposition-through-the-handle unlink
// still works and a by-name one cannot.
//
// KILLS: reverting the destructor to CloseHandle-then-remove-by-name.
TEST(FullRewriteStagingPrimitive, AbandonedStagingIsUnlinkedThroughTheHandle)
{
    const auto dir = makeWorkDir("f1dacl");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }

    const auto sid = currentUserSid();
    ASSERT_FALSE(sid.empty()) << "could not resolve the current user's SID";

    std::error_code ec;
    const auto staged = pdfe::core::fullRewriteTempPathFor(target);
    const std::wstring me(sid.begin(), sid.end());

    // BOTH halves are required, and the first draft of this test had only one —
    // which is why it passed against the unfixed destructor and proved nothing.
    // On Windows a delete is authorised by DELETE on the FILE **or** by
    // FILE_DELETE_CHILD on the containing DIRECTORY, so stripping DELETE from
    // the file alone leaves a by-name unlink working through the parent. The
    // directory therefore gets everything EXCEPT FILE_DELETE_CHILD
    // (0x1F01FF & ~0x40), and the file gets read-only.
    ASSERT_TRUE(applyProtectedDacl(dir, L"D:P(A;OICI;0x1F01BF;;;" + me + L")"))
        << "could not remove FILE_DELETE_CHILD from the working directory";

    {
        auto sf = StagingFile::createFullRewriteTempFor(target, ec);
        ASSERT_NE(sf, nullptr) << ec.message();
        ASSERT_EQ(sf->path(), staged);

        // Read-only, protected: exactly what commitOver would copy across from
        // an owner-only destination that does not grant DELETE.
        ASSERT_TRUE(applyProtectedDacl(staged, L"D:P(A;;FR;;;" + me + L")"))
            << "could not apply the read-only DACL the scenario needs";
    }  // destructor runs here: must unlink through the handle

    // GetFileAttributesW + GetLastError, NOT std::filesystem::exists: exists()
    // returns FALSE on a permission error too, so it would report "gone" for a
    // file that is merely unreadable — the exact false green this test is here
    // to avoid. Only ERROR_FILE_NOT_FOUND means gone.
    const DWORD attrs = ::GetFileAttributesW(staged.native().c_str());
    const DWORD attrErr = ::GetLastError();
    EXPECT_TRUE(attrs == INVALID_FILE_ATTRIBUTES
                && (attrErr == ERROR_FILE_NOT_FOUND
                    || attrErr == ERROR_PATH_NOT_FOUND))
        << "the abandoned staging file survived at the CONTRACTUAL name (attrs="
        << attrs << ", err=" << attrErr << "). Every subsequent save of this "
           "document now fails closed, and the R2-04 scan offers an unreadable "
           "recovery prompt on every open.";

    // The independent half: with the orphan gone the document is saveable
    // again. This is what the user actually loses, and it fails against a
    // by-name unlink even if the check above were somehow satisfied.
    auto again = StagingFile::createFullRewriteTempFor(target, ec);
    EXPECT_NE(again, nullptr)
        << "the document is permanently unsaveable: " << ec.message();
    again.reset();

    // Restore a deletable DACL so the fixture can be cleaned up.
    applyProtectedDacl(staged, L"D:P(A;;FA;;;" + me + L")");
    applyProtectedDacl(dir, L"D:P(A;OICI;FA;;;" + me + L")");
    std::filesystem::remove_all(dir, ec);
}

// F2 — PDFE SQUATTING ON ITSELF. A predictable name plus CREATE_NEW plus
// FILE_SHARE_READ means two writers of the same target contend. A sharing
// violation means "someone is actively using it", NOT stale residue, and
// failing on sight would be a regression: before this phase both saves
// completed (by interleaving into one shared unheld temp — corruption wearing a
// success return, but not a visible failure).
//
// KILLS: treating ERROR_SHARING_VIOLATION as an unclearable squatter and
// returning immediately (the shape this phase shipped first).
TEST(FullRewriteStagingPrimitive, ASecondWriterWaitsForTheFirstRatherThanFailing)
{
    const auto dir = makeWorkDir("f2wait");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }

    std::error_code ec;
    auto first = StagingFile::createFullRewriteTempFor(target, ec);
    ASSERT_NE(first, nullptr) << ec.message();

    // The first writer finishes shortly after the second STARTS ASKING — and
    // "starts asking" is a synchronised fact here, not a hoped-for one.
    //
    // THIS USED TO BE AN UNCONDITIONAL sleep_for(400ms) BEFORE THE RELEASE, and
    // it flaked: measured 2026-08-13 under a 16-shard run, where this case
    // failed and then passed on a fresh rerun. The failure is starvation of THIS
    // thread, not a defect in the product — if the main thread is descheduled
    // past the 400 ms mark, the name is already free when it finally calls
    // create, the call returns instantly, and the ">= 200 ms, so it really
    // waited" VACUITY guard fires. A real red for a fake reason.
    //
    // Fixing it by relaxing that guard would have been the wrong repair: the
    // guard is what stops this case passing when the first handle never held
    // the name at all. So the ORDERING is made explicit instead — the releaser
    // waits until the second create is genuinely in flight, then holds the name
    // for its measured interval. The product side has ample room either way:
    // the in-use path retries 12 times with backoff capped at 800 ms (~6.4 s),
    // against a 400 ms hold.
    std::atomic<bool> secondIsAsking{false};
    std::thread releaser([&] {
        while (!secondIsAsking.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        first.reset();
    });

    const auto t0 = std::chrono::steady_clock::now();
    secondIsAsking.store(true, std::memory_order_release);
    std::error_code secondEc;
    auto second = StagingFile::createFullRewriteTempFor(target, secondEc);
    const auto waited = std::chrono::steady_clock::now() - t0;
    releaser.join();

    ASSERT_NE(second, nullptr)
        << "a second writer of the same target FAILED instead of waiting for "
           "the first to finish — that is the regression a predictable staging "
           "name introduced: " << secondEc.message();
    // It must actually have WAITED; succeeding instantly would mean the first
    // handle never held the name and the test is vacuous.
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(waited)
                  .count(),
              200)
        << "the second create returned before the first writer released the "
           "name, so nothing was contended";

    second.reset();
    std::filesystem::remove_all(dir, ec);
}

// …and the give-up is DIAGNOSABLE rather than generic: a caller must be able to
// say "another save of this document is in progress" instead of guessing.
TEST(FullRewriteStagingPrimitive, AContendedNameEventuallyReportsASharingViolation)
{
    const auto dir = makeWorkDir("f2give");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }

    std::error_code ec;
    auto first = StagingFile::createFullRewriteTempFor(target, ec);
    ASSERT_NE(first, nullptr) << ec.message();

    const auto t0 = std::chrono::steady_clock::now();
    std::error_code secondEc;
    auto second = StagingFile::createFullRewriteTempFor(target, secondEc);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    EXPECT_EQ(second, nullptr) << "the name is held; creation cannot succeed";
    EXPECT_EQ(secondEc.value(), static_cast<int>(ERROR_SHARING_VIOLATION))
        << "the contended case must be distinguishable from 'a squatter planted "
           "a file here' — got: " << secondEc.message();

    // THE ASSERTION THAT MAKES THIS DISCRIMINATING, AND WHY IT WAS ADDED LATE.
    // The error code alone cannot tell the fixed code from the unfixed one:
    // clearStaleStagingName sets `ec` from the failed DeleteFileW, so the
    // PRE-fix "give up on the first refusal" path returns exactly the same
    // ERROR_SHARING_VIOLATION. Measured against the mutation, this case passed
    // — it pinned a contract but witnessed nothing. What is unique to the fix
    // is that it WAITS the whole backoff budget before reporting that error;
    // the unfixed path returns in ~3 ms. 2000 ms is well inside the ~6.4 s
    // budget (25 ms doubling to an 800 ms cap over 12 rounds) and well outside
    // any plausible immediate return.
    EXPECT_GE(waited, 2000)
        << "creation gave up after " << waited << " ms — it reported the right "
           "error without ever waiting for the other writer, which is the "
           "pre-fix behaviour wearing the post-fix error code";

    first.reset();
    std::filesystem::remove_all(dir, ec);
}

// The product-level half of F2: two Documents saving the same target. Whatever
// each save returns, the published file must be a valid PDF and no residue may
// be left. Before this phase both writers scribbled into one shared, unheld
// ".pdfe_tmp".
TEST(FullRewriteSaveTocttou, TwoDocumentsSavingTheSameTargetDoNotCorruptIt)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());

    const auto dir = makeWorkDir("twodocs");
    const auto target = dir / "shared.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec) << ec.message();

    std::atomic<int> ok{0};
    std::string errA, errB;
    auto saver = [&](std::string& err) {
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            auto doc = Document::open(ctx, target);
            if (!doc) { err = "open failed"; return; }
            doc->markRequiresFullRewrite();
            doc->save(target);
            ok.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            err = e.what();
        }
    };
    std::thread a([&] { saver(errA); });
    std::thread b([&] { saver(errB); });
    a.join();
    b.join();

    // LABELLED, NOT COUNTED: THIS CASE IS A CORRUPTION GUARD, NOT A
    // DISCRIMINATION WITNESS FOR F2 — and the attempt to make it one is the
    // finding worth recording.
    //
    // Measured against the mutation that removes the wait-and-retry, this case
    // PASSES: the loser fails immediately, the winner still publishes a valid
    // PDF, and every assertion below holds. The obvious strengthening is to
    // require BOTH saves to complete, which is what F2 sounds like it should
    // deliver. That was tried, and it FAILS 5 runs out of 5 against the FIXED
    // code — with the loser failing at the PUBLISH step, not at staging:
    //
    //     "Failed to atomically replace original file (original left intact):
    //      Access is denied."
    //
    // The cause is documented in Document::saveIncrementalStaged's header and
    // is nothing to do with F2: after the winner publishes, save() REOPENS the
    // target, and a CRT "rb" handle held IN THIS PROCESS blocks a
    // replace-existing rename over it (FileRenameInfoEx's POSIX semantics
    // relaxes that for OTHER processes' handles, not for one the same rename
    // must supersede). Two in-process Documents saving one target therefore
    // cannot both complete, with or without F2, and asserting that they do
    // would pin a property the system does not have.
    //
    // What F2 restores is contention handling at the STAGING NAME, and
    // .ASecondWriterWaitsForTheFirstRatherThanFailing is the discriminating
    // witness for it. This case keeps the assertions that are true and that
    // nothing else covers: whatever each save returns, the published document
    // is a valid PDF and no residue is left. The loser's failure is
    // diagnosable and leaves the original intact, which is also asserted.
    EXPECT_GE(ok.load(), 1)
        << "BOTH concurrent saves failed; A=" << errA << " B=" << errB;
    // GitHub #273: the diagnosability check was gated on `ok.load() < 2`, so
    // when both saves succeeded nothing was asserted about the error strings.
    // Both outcomes are specifiable: a loser must say why it lost, and a run
    // where nobody lost must not be reporting errors.
    if (ok.load() < 2) {
        EXPECT_FALSE(errA.empty() && errB.empty())
            << "a save failed silently — the loser must report why";
    } else {
        EXPECT_TRUE(errA.empty() && errB.empty())
            << "both concurrent saves reported success, so neither should "
               "carry an error string; A=" << errA << " B=" << errB;
    }
    const std::string after = readAll(target);
    ASSERT_FALSE(after.empty()) << "the shared target was destroyed";
    EXPECT_EQ(after.compare(0, 5, "%PDF-"), 0)
        << "two concurrent saves left a file that is not a PDF — the shared "
           "staging file was written by both";
    EXPECT_TRUE(anyStagingResidue(target).empty())
        << "a contended save left staging residue behind";

    std::filesystem::remove_all(dir, ec);
}

// F4 — a real regression this phase introduced. Moving saveAs-to-a-NEW
// destination onto staging appends 9 characters to the user's path, so a
// destination of 251-259 characters — which succeeded before, because MuPDF
// wrote straight to it — started failing with "the filename or extension is too
// long". There is no longPathAware manifest in this repo, so MAX_PATH is live
// and only the STAGING name crosses it.
//
// KILLS: dropping the extended-length form from the staging create.
TEST(SaveAsStagingTocttou, ADestinationOnlyTheStagingNameOverflowsStillSaves)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());

    const auto srcDir = makeWorkDir("f4src");
    const auto source = srcDir / "source.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, source, ec);
    ASSERT_FALSE(ec) << ec.message();

    // A destination whose own length is under MAX_PATH but whose staging name
    // (+9) is over it. Nested short components, so every mkdir is legal.
    auto deep = makeWorkDir("f4dst");
    while ((deep.native().size() + 11 + 8) < 245) {
        deep = deep / "0123456789";
        std::filesystem::create_directories(deep, ec);
        ASSERT_FALSE(ec) << ec.message();
    }
    std::string leaf = "d.pdf";
    while ((deep.native().size() + 1 + leaf.size()) < 251) {
        leaf.insert(0, "x");
    }
    const std::filesystem::path dest = deep / leaf;
    ASSERT_GE(dest.native().size(), 251u);
    ASSERT_LT(dest.native().size(), 260u)
        << "the DESTINATION itself must fit in MAX_PATH — otherwise this test "
           "measures a limitation that predates the phase";
    ASSERT_GE(dest.native().size() + 9, 260u)
        << "the STAGING name must be the thing that overflows, or the test is "
           "not exercising the regression";

    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, source);
        ASSERT_NE(doc, nullptr);
        ASSERT_NO_THROW(doc->saveAs(dest))
            << "a Save As to a 251-259 character destination failed; before "
               "this phase it succeeded, because only the staging name crosses "
               "MAX_PATH";
    }
    EXPECT_TRUE(std::filesystem::exists(dest, ec));
    const std::string written = readAll(dest);
    ASSERT_FALSE(written.empty());
    EXPECT_EQ(written.compare(0, 5, "%PDF-"), 0);

    std::filesystem::remove_all(deep, ec);
    std::filesystem::remove_all(srcDir, ec);
}

#endif  // _WIN32
