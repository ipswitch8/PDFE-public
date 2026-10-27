// -*- coding: utf-8 -*-
//
// Phase-17 (follow-up phase-70), criterion 2 — the plaintext index directory's
// ACL.
//
// DocumentSearchIndex writes every page's extracted text into a Tantivy index
// under %TEMP%\pdfe_search_indexes. F2 established WHICH documents may be
// materialised there (the extraction-permission gate) and F24 established that
// stale indexes get purged. Neither addressed the directory's permissions: a
// %TEMP% directory carries whatever %TEMP% grants, which on a terminal server
// or a shared build box is a good deal more than "the process that wrote it".
// The content is a plaintext, full-text-searchable copy of the document,
// sitting outside whatever container the document arrived in.
//
// So the root and the per-document directory are created with an owner-only,
// inheritance-broken DACL — supplied AT CREATION, because create-then-secure
// leaves a window in which the directory exists with the inherited ACL, and in
// %TEMP% that window is the exposure.
//
// WHAT MAKES THESE ASSERTIONS REAL. Every case reads the ACL back off the
// filesystem with pdfe::core::isOwnerOnlyAccessible rather than trusting the
// creating call's return value. That distinction has already cost this project
// once: a control that asserts what a function RETURNED about an artifact is
// not asserting anything about the artifact.
//
// Non-Windows builds get the POSIX 0700 equivalent; the DACL-specific cases are
// compiled out rather than weakened, since there is nothing there to assert.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/TempFileSafety.h"
#include "search/DocumentSearchIndex.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

using pdfe::core::createOwnerOnlyDirectory;
using pdfe::core::Document;
using pdfe::core::isOwnerOnlyAccessible;
using pdfe::core::MuPdfContext;
using pdfe::search::DocumentSearchIndex;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path scratchDir(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "pdfe_acl_phase70" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir.parent_path(), ec);
    return dir;
}

#if defined(_WIN32)

/// Create `dir` with a DACL that explicitly grants Everyone full control —
/// i.e. exactly the pre-existing-permissive-directory case the adopt path has
/// to repair rather than accept. Returns false if the ACL could not be set, so
/// a test can refuse to run vacuously.
bool createWorldWritableDir(const std::filesystem::path& dir)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    // WD = Everyone. OICI = inherited by containers and objects. FA = full.
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    const BOOL ok = ::CreateDirectoryW(dir.wstring().c_str(), &sa);
    ::LocalFree(sd);
    return ok != FALSE;
}

/// True if `path`'s OWNER is somebody other than this user, SYSTEM or
/// Administrators. Used to establish a test's precondition from the real
/// filesystem instead of assuming it.
bool ownerIsForeign(const std::filesystem::path& path)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    PSID owner = nullptr;
    if (::GetNamedSecurityInfoW(path.wstring().c_str(), SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION,
                                &owner, nullptr, nullptr, nullptr, &sd)
        != ERROR_SUCCESS) {
        return false;
    }
    bool foreign = true;
    if (owner) {
        if (::IsWellKnownSid(owner, WinLocalSystemSid) ||
            ::IsWellKnownSid(owner, WinBuiltinAdministratorsSid)) {
            foreign = false;
        } else {
            HANDLE token = nullptr;
            if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
                DWORD needed = 0;
                ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
                std::vector<unsigned char> buf(needed ? needed : 1);
                if (needed &&
                    ::GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
                    auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
                    if (tu->User.Sid && ::EqualSid(owner, tu->User.Sid)) foreign = false;
                }
                ::CloseHandle(token);
            }
        }
    }
    if (sd) ::LocalFree(sd);
    return foreign;
}

#endif // _WIN32

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The helper itself
// ─────────────────────────────────────────────────────────────────────────────

TEST(SearchIndexAcl, FreshDirectoryIsOwnerOnly)
{
    const auto dir = scratchDir("fresh");
    std::error_code ec;
    ASSERT_TRUE(createOwnerOnlyDirectory(dir, ec)) << ec.message();
    ASSERT_TRUE(std::filesystem::is_directory(dir));
    EXPECT_TRUE(isOwnerOnlyAccessible(dir))
        << "the directory was created but its ACL is reachable by somebody "
           "else; the DACL passed at creation did not take effect";
}

TEST(SearchIndexAcl, ChildDirectoriesInheritTheOwnerOnlyAcl)
{
    // The per-document index directory is created underneath the root by
    // ordinary create_directories. It is owner-only only if the root's ACE is
    // inheritable — CONTAINER_INHERIT_ACE. Without that this whole control
    // protects an empty parent while the plaintext lands in a child anyone can
    // read.
    const auto root = scratchDir("inherit_root");
    std::error_code ec;
    ASSERT_TRUE(createOwnerOnlyDirectory(root, ec)) << ec.message();

    const auto child = root / "some_document_key";
    ASSERT_TRUE(std::filesystem::create_directory(child, ec)) << ec.message();
    EXPECT_TRUE(isOwnerOnlyAccessible(child))
        << "a child created under the secured root is reachable by others; the "
           "root's ACE is not marked inheritable";

    // AND USABLE BY US. Without this the case is vacuous, which mutation
    // proved: setting NO_INHERITANCE leaves the child with an EMPTY DACL, and
    // isOwnerOnlyAccessible answers "nobody else can reach it" — true, but only
    // because nobody at all can. "Only I have access" and "no one has access"
    // are different states the verifier cannot tell apart, so the test has to.
    const auto probe = child / "writable_probe.txt";
    { std::ofstream f(probe); f << "x"; }
    EXPECT_TRUE(std::filesystem::exists(probe))
        << "the child directory is not writable by its own owner — the ACL is "
           "restrictive rather than owner-only, and the index would break";
}

TEST(SearchIndexAcl, ARepeatedCallOnOurOwnDirectoryStillSucceeds)
{
    // Idempotence matters: this runs on every build, and a second call that
    // failed would fail search closed for the rest of the session.
    const auto dir = scratchDir("idempotent");
    std::error_code ec;
    ASSERT_TRUE(createOwnerOnlyDirectory(dir, ec)) << ec.message();
    ASSERT_TRUE(createOwnerOnlyDirectory(dir, ec)) << ec.message();
    EXPECT_TRUE(isOwnerOnlyAccessible(dir));
}

#if defined(_WIN32)

TEST(SearchIndexAcl, APermissiveDirectoryWeOwnIsRepairedNotAccepted)
{
    // The upgrade case, and the one that decides whether this fix reaches
    // machines that already ran an older build: those left an index root with
    // %TEMP%'s inherited ACL. Adopting it as-is would mean the fix never
    // applies to exactly the users who already have a spill on disk.
    const auto dir = scratchDir("permissive");
    ASSERT_TRUE(createWorldWritableDir(dir))
        << "could not create the world-writable directory this case exists to "
           "repair — the assertion below would pass vacuously";

    ASSERT_FALSE(isOwnerOnlyAccessible(dir))
        << "the world-writable directory reads as owner-only before we touch "
           "it; either the ACL did not apply or the verifier is not looking";

    std::error_code ec;
    ASSERT_TRUE(createOwnerOnlyDirectory(dir, ec)) << ec.message();
    EXPECT_TRUE(isOwnerOnlyAccessible(dir))
        << "an existing world-writable directory was adopted without its DACL "
           "being repaired";
}

TEST(SearchIndexAcl, AFileAtTheDirectoryNameIsRefused)
{
    const auto p = scratchDir("is_a_file");
    std::filesystem::create_directories(p.parent_path());
    { std::ofstream f(p); f << "not a directory"; }
    ASSERT_TRUE(std::filesystem::is_regular_file(p));

    std::error_code ec;
    EXPECT_FALSE(createOwnerOnlyDirectory(p, ec))
        << "a regular file sitting at the index-directory name was accepted";
    EXPECT_TRUE(std::filesystem::is_regular_file(p))
        << "the refusal deleted the file it refused; a guard must not act on "
           "what it declines to accept";
}

TEST(SearchIndexAcl, AReparsePointAtTheDirectoryNameIsRefused)
{
    // %TEMP% is writable by anything running as this user and `mklink /J`
    // needs no privilege, so a junction can be planted at a predictable index
    // path. is_directory() FOLLOWS it, so classifying by status would adopt the
    // junction and then dutifully secure it while the page text lands wherever
    // it points. This is the same rule DocumentSearchIndex's own sweep applies.
    const auto link = scratchDir("junction");
    const auto target = scratchDir("junction_target");
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(target, ec)) << ec.message();
    std::filesystem::create_directories(link.parent_path(), ec);

    // Junction creation via cmd's mklink — no privilege required, unlike a
    // symlink. If it does not succeed the case cannot assert anything, so the
    // failure is surfaced rather than swallowed.
    const std::wstring cmd =
        L"cmd.exe /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>&1";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr,
                                 FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                 &si, &pi))
        << "could not spawn mklink to create the junction";
    ::WaitForSingleObject(pi.hProcess, 30000);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    const DWORD attrs = ::GetFileAttributesW(link.wstring().c_str());
    ASSERT_NE(attrs, INVALID_FILE_ATTRIBUTES) << "the junction was not created";
    ASSERT_TRUE(attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        << "the path exists but is not a reparse point; this case would be "
           "testing an ordinary directory and would pass for the wrong reason";

    EXPECT_FALSE(createOwnerOnlyDirectory(link, ec))
        << "a junction at the index-directory name was adopted as an ordinary "
           "directory";

    std::error_code rmec;
    std::filesystem::remove(link, rmec);
    std::filesystem::remove_all(target, rmec);
}

TEST(SearchIndexAcl, AForeignOwnedDirectoryIsClassifiedAsForeign)
{
    // WHY THE OWNER MATTERS AND NOT JUST THE DACL: an owner holds WRITE_DAC
    // implicitly. Re-securing a directory somebody else created is theatre —
    // they can rewrite the ACL back the moment we finish. Refusal is the only
    // answer that holds.
    //
    // WHY THIS CALLS THE INSPECTOR AND NOT createOwnerOnlyDirectory. The first
    // version of this case pointed the *writing* function at %SystemRoot% and
    // argued it was safe because an ASSERT established the foreign owner first,
    // so the write was unreachable. That reasoning covered the unmutated code
    // only — and a mutation harness exists to delete exactly the check the
    // argument rests on. When the "foreign-owner refusal is removed" mutation
    // ran, the call proceeded to SetNamedSecurityInfoW against C:\Windows and
    // was stopped by Windows, not by anything I wrote. (Verified afterwards:
    // C:\Windows was untouched — owner TrustedInstaller, 13 ACEs.)
    //
    // The refusal is therefore decided by a READ-ONLY predicate that cannot
    // write under any mutation, and this case asserts the predicate. That is
    // also why the mutation survived: the old case never exercised the check.
    wchar_t sysroot[MAX_PATH] = {};
    const UINT n = ::GetWindowsDirectoryW(sysroot, MAX_PATH);
    ASSERT_GT(n, 0u);
    const std::filesystem::path win(sysroot);

    ASSERT_TRUE(ownerIsForeign(win))
        << "%SystemRoot% is owned by this user, SYSTEM or Administrators on "
           "this machine, so it cannot stand in for a foreign-owned directory. "
           "Do NOT relax this into a skip — find another foreign-owned path.";

    EXPECT_EQ(pdfe::core::inspectDirectoryForAdoption(win),
              pdfe::core::DirAdoptionVerdict::ForeignOwner)
        << "a directory owned by another principal was classified as adoptable";

    // A directory we DO own must classify as Ok, or the case above would pass
    // for a predicate that answers ForeignOwner unconditionally.
    const auto mine = scratchDir("owned_by_me");
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(mine, ec)) << ec.message();
    EXPECT_EQ(pdfe::core::inspectDirectoryForAdoption(mine),
              pdfe::core::DirAdoptionVerdict::Ok);
}

TEST(SearchIndexAcl, TheInspectorClassifiesEveryRefusalReasonDistinctly)
{
    // createOwnerOnlyDirectory refuses on the inspector's verdict, so the
    // verdicts are the guard. Each is asserted separately: a predicate that
    // collapsed them all to one refusal would still satisfy a boolean test.
    std::error_code ec;

    const auto missing = scratchDir("inspect_missing");
    EXPECT_EQ(pdfe::core::inspectDirectoryForAdoption(missing),
              pdfe::core::DirAdoptionVerdict::DoesNotExist);

    const auto file = scratchDir("inspect_file");
    std::filesystem::create_directories(file.parent_path(), ec);
    { std::ofstream f(file); f << "x"; }
    EXPECT_EQ(pdfe::core::inspectDirectoryForAdoption(file),
              pdfe::core::DirAdoptionVerdict::NotADirectory);

    const auto dir = scratchDir("inspect_dir");
    ASSERT_TRUE(std::filesystem::create_directories(dir, ec)) << ec.message();
    EXPECT_EQ(pdfe::core::inspectDirectoryForAdoption(dir),
              pdfe::core::DirAdoptionVerdict::Ok);
}

#endif // _WIN32

// ─────────────────────────────────────────────────────────────────────────────
// The real call site
// ─────────────────────────────────────────────────────────────────────────────

TEST(SearchIndexAcl, BuiltIndexDirectoryAndRootAreOwnerOnly)
{
    // The helper being correct is worth nothing if DocumentSearchIndex does not
    // use it. This asserts the ACL on the directories that actually hold page
    // plaintext, after a real build.
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturesDir() / "test_3page.pdf");
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->copyAllowed())
        << "fixture must be permitted to build an on-disk index, or this case "
           "asserts the ACL of a directory that was never created";

    DocumentSearchIndex idx(doc.get());
    idx.invalidate();
    auto hits = idx.searchPages("title", 100);
    ASSERT_FALSE(hits.empty())
        << "no hits — the index was not built, so there is no directory whose "
           "ACL this case could be asserting";

    ASSERT_TRUE(std::filesystem::is_directory(idx.indexDir()));
    EXPECT_TRUE(isOwnerOnlyAccessible(idx.indexDir()))
        << "the directory holding this document's plaintext is reachable by "
           "principals other than the current user";
    EXPECT_TRUE(isOwnerOnlyAccessible(DocumentSearchIndex::indexRoot()))
        << "the index ROOT is world-reachable; the per-document directory's "
           "ACL does not help if the root can be enumerated and traversed";
}

#if defined(_WIN32)

TEST(SearchIndexAcl, TheSweepHardensTheRootBeforeItWalksIt)
{
    // ORDERING, asserted rather than commented.
    //
    // The stale-index sweep runs from DocumentSearchIndex's constructor;
    // buildFromScratch() runs only on the first search. Hardening the root
    // solely in buildFromScratch() would therefore leave it carrying %TEMP%'s
    // inherited ACL for exactly the window in which the sweep is walking and
    // deleting inside it — which is the window its own check-then-act residual
    // is about. So the no-arg sweepStaleIndexes() applies the DACL first.
    //
    // This case is deterministic in a shared process precisely because it calls
    // the sweep directly instead of relying on the constructor's call_once,
    // which fires at most once per process and may already have run.
    const auto root = DocumentSearchIndex::indexRoot();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ASSERT_TRUE(createWorldWritableDir(root))
        << "could not stage a permissive root; the assertion below would pass "
           "on a root that was already hardened and prove nothing";
    ASSERT_FALSE(isOwnerOnlyAccessible(root))
        << "the staged root already reads as owner-only";

    (void)DocumentSearchIndex::sweepStaleIndexes();

    EXPECT_TRUE(isOwnerOnlyAccessible(root))
        << "the sweep walked and deleted inside a root it had not secured; the "
           "ACL is being applied only on the build path, which runs later";
}

TEST(SearchIndexAcl, AnUnprotectableIndexDirectoryFailsClosedAndWritesNothing)
{
    // The refusal has to be a refusal. If buildFromScratch() returns false but
    // ensureBuilt() sets built_ anyway, the result is a "built" index with
    // nothing behind it — and, worse, the question of whether page text got
    // written somewhere first becomes a matter of luck.
    //
    // A junction at the per-document index path makes createOwnerOnlyDirectory
    // refuse, which is also the realistic attack: %TEMP% is writable by this
    // user and the index path is derived from the document, so it is
    // predictable enough to pre-plant.
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturesDir() / "test_3page.pdf");
    ASSERT_NE(doc, nullptr);

    DocumentSearchIndex idx(doc.get());
    const auto dir = idx.indexDir();
    idx.invalidate();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto target = scratchDir("failclosed_target");
    ASSERT_TRUE(std::filesystem::create_directories(target, ec)) << ec.message();
    std::filesystem::create_directories(dir.parent_path(), ec);

    const std::wstring cmd =
        L"cmd.exe /c mklink /J \"" + dir.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>&1";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr,
                                 FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                 &si, &pi));
    ::WaitForSingleObject(pi.hProcess, 30000);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    const DWORD attrs = ::GetFileAttributesW(dir.wstring().c_str());
    ASSERT_NE(attrs, INVALID_FILE_ATTRIBUTES);
    ASSERT_TRUE(attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        << "could not plant the junction, so nothing below is being tested";

    EXPECT_TRUE(idx.searchPages("title", 100).empty())
        << "search returned hits through an index directory that could not be "
           "secured; the refusal did not stop the build";
    EXPECT_EQ(idx.documentCount(), 0)
        << "built_ was set despite buildFromScratch() refusing";

    // The part that matters most: nothing was written THROUGH the junction.
    bool wroteThrough = false;
    for (const auto& e : std::filesystem::directory_iterator(target, ec)) {
        (void)e;
        wroteThrough = true;
        break;
    }
    EXPECT_FALSE(wroteThrough)
        << "page text was written through the junction into " << target.string()
        << " — the refusal came too late to matter";

    std::error_code rmec;
    std::filesystem::remove(dir, rmec);
    std::filesystem::remove_all(target, rmec);
}

#endif // _WIN32

TEST(SearchIndexAcl, SearchStillWorksWithTheHardenedAcl)
{
    // The control. A fix that locks the directory down so thoroughly that the
    // product can no longer read its own index would satisfy every assertion
    // above.
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturesDir() / "test_3page.pdf");
    ASSERT_NE(doc, nullptr);

    DocumentSearchIndex idx(doc.get());
    idx.invalidate();

    // searchPages() FIRST: documentCount() reports the current state and does
    // not trigger a build, so asserting it before any search would read 0 for a
    // reason having nothing to do with the ACL.
    EXPECT_FALSE(idx.searchPages("title", 100).empty())
        << "the hardened ACL prevented the process from building or reading "
           "its own index";
    EXPECT_GT(idx.documentCount(), 0);
}
