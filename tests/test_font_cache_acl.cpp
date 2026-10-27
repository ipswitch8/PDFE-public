// -*- coding: utf-8 -*-
//
// SEC-103 (meta-audit-2026-07 gap register) — the font-cache database's
// location and permissions.
//
// The row's premise is "a local-user-writable SQLite file … opened with no
// flag restrictions", and the trace refutes it: BOTH construction sites in the
// product pass no path — `src/ui/MainWindow.cpp` constructs
// `FontAnalysisCache{}`, and `FontSubstitutionEngine`'s `cachePath` parameter
// defaults to `{}` and is passed by nobody — so `FontAnalysisCache::Impl` maps
// it to ":memory:". There is no file, and therefore no ACL.
//
// That absence is what these tests pin. It is worth pinning for the same
// reason PDF-49 and PDF-58 are worth having: the constructor is public, takes
// a path, and one future call site supplying one would have created exactly
// the described surface with whatever ACL the containing directory granted.
//
// The order below matters. The last case is the one that gives the others
// their meaning: `isOwnerOnlyAccessible` must be able to answer FALSE. Without
// that, every "the file is owner-only" assertion in this file would be
// satisfied by a function that always returns true.

#include <gtest/gtest.h>

#include "core/TempFileSafety.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/FontMetricProfile.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <aclapi.h>
#endif

namespace {

std::filesystem::path freshDir(const char* leaf)
{
    const auto d = std::filesystem::temp_directory_path() / "pdfe_sec103" / leaf;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

#if defined(_WIN32)

// Plant a file that EVERYONE can write — the pre-existing world-writable
// database an attacker leaves for the victim process to adopt.
bool plantWorldWritableFile(const std::filesystem::path& p)
{
    PSID everyone = nullptr;
    SID_IDENTIFIER_AUTHORITY worldAuth = SECURITY_WORLD_SID_AUTHORITY;
    if (!::AllocateAndInitializeSid(&worldAuth, 1, SECURITY_WORLD_RID,
                                    0, 0, 0, 0, 0, 0, 0, &everyone)) {
        return false;
    }

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(everyone);

    PACL dacl = nullptr;
    bool ok = (::SetEntriesInAclW(1, &ea, nullptr, &dacl) == ERROR_SUCCESS && dacl);

    SECURITY_DESCRIPTOR sd{};
    if (ok) {
        ok = ::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)
          && ::SetSecurityDescriptorDacl(&sd, TRUE, dacl, FALSE);
    }

    if (ok) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        HANDLE h = ::CreateFileW(p.wstring().c_str(), GENERIC_WRITE, 0, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            ok = false;
        } else {
            DWORD written = 0;
            ::WriteFile(h, "planted", 7, &written, nullptr);
            ::CloseHandle(h);
        }
    }

    if (dacl) ::LocalFree(dacl);
    if (everyone) ::FreeSid(everyone);
    return ok;
}

#endif // _WIN32

} // namespace

using pdfe::fonts::FontAnalysisCache;

// ── The measurement, pinned ────────────────────────────────────────────────

// The shipping configuration. If this ever goes red, someone has given the
// cache a path, and the ACL question below becomes live rather than
// hypothetical — which is exactly when it should be asked.
TEST(FontAnalysisCacheAcl, TheProductsDefaultCacheHasNoFileAtAll)
{
    FontAnalysisCache cache;
    EXPECT_FALSE(cache.isOnDisk())
        << "the default-constructed cache is backed by a file; SEC-103's "
           "threat-model row and the ACL guarantees now apply to it, and "
           "whoever introduced the path must confirm where it lives";
}

// The cache must still WORK in memory — a hardening that turned the cache off
// would satisfy every assertion in this file and break font substitution.
TEST(FontAnalysisCacheAcl, TheInMemoryCacheStillStoresAndReturnsProfiles)
{
    FontAnalysisCache cache;
    pdfe::fonts::FontMetricProfile p;
    p.postScriptName = "PdfeTest-Regular";
    p.familyName = "PdfeTest";
    p.unitsPerEm = 1000;
    cache.put(p);

    EXPECT_TRUE(cache.contains("PdfeTest-Regular"));
    const auto got = cache.get("PdfeTest-Regular");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->familyName, "PdfeTest");
}

#if defined(_WIN32)

// ── The hardening, when a path IS supplied ─────────────────────────────────

TEST(FontAnalysisCacheAcl, ASuppliedPathIsCreatedOwnerOnlyAndUsed)
{
    const auto dir = freshDir("supplied");
    const auto db = dir / "font-analysis-cache.sqlite";

    FontAnalysisCache cache(db);
    EXPECT_TRUE(cache.isOnDisk())
        << "an owner-only path should have been accepted";
    ASSERT_TRUE(std::filesystem::exists(db));
    EXPECT_TRUE(pdfe::core::isOwnerOnlyAccessible(db))
        << "the database was created with a DACL wider than the owner";
}

// THE CASE THE HARDENING EXISTS FOR. A pre-planted world-writable file must be
// REFUSED, not adopted — adopting it is the local-escalation surface — and the
// cache must degrade to in-memory rather than failing the application.
TEST(FontAnalysisCacheAcl, APrePlantedWorldWritableDatabaseIsRefusedNotAdopted)
{
    const auto dir = freshDir("planted");
    const auto db = dir / "font-analysis-cache.sqlite";

    ASSERT_TRUE(plantWorldWritableFile(db))
        << "could not plant the hostile file; the case did not run";
    ASSERT_TRUE(std::filesystem::exists(db));

    // Vacuity guard on the planting itself: if the planted file is NOT
    // actually world-writable, the refusal below proves nothing.
    ASSERT_FALSE(pdfe::core::isOwnerOnlyAccessible(db))
        << "the planted file is already owner-only, so this case would pass "
           "without the product refusing anything";

    FontAnalysisCache cache(db);
    EXPECT_FALSE(cache.isOnDisk())
        << "the cache adopted a world-writable database instead of falling "
           "back to memory";

    // The planted file must be left alone: refusing is not a licence to
    // delete somebody else's file.
    EXPECT_TRUE(std::filesystem::exists(db));

    // And the cache must still function.
    pdfe::fonts::FontMetricProfile p;
    p.postScriptName = "PdfeFallback-Regular";
    p.unitsPerEm = 1000;
    cache.put(p);
    EXPECT_TRUE(cache.contains("PdfeFallback-Regular"))
        << "the in-memory fallback is not usable, so the refusal broke the "
           "feature instead of protecting it";
}

TEST(FontAnalysisCacheAcl, CreateOwnerOnlyFileRefusesAWorldWritablePreexistingFile)
{
    const auto dir = freshDir("primitive");
    const auto f = dir / "state.bin";

    ASSERT_TRUE(plantWorldWritableFile(f));
    std::error_code ec;
    EXPECT_FALSE(pdfe::core::createOwnerOnlyFile(f, ec));
    EXPECT_TRUE(static_cast<bool>(ec)) << "a refusal must report a reason";
}

TEST(FontAnalysisCacheAcl, CreateOwnerOnlyFileMakesParentDirectoriesAndReadsTheAclBack)
{
    const auto dir = freshDir("nested");
    const auto f = dir / "a" / "b" / "state.bin";

    std::error_code ec;
    ASSERT_TRUE(pdfe::core::createOwnerOnlyFile(f, ec)) << ec.message();
    EXPECT_FALSE(static_cast<bool>(ec));
    ASSERT_TRUE(std::filesystem::exists(f));
    EXPECT_TRUE(pdfe::core::isOwnerOnlyAccessible(f));

    // Idempotent: an existing file WE created is adopted without complaint.
    std::error_code ec2;
    EXPECT_TRUE(pdfe::core::createOwnerOnlyFile(f, ec2)) << ec2.message();
}

// ── The case that gives the others their meaning ───────────────────────────
//
// Every assertion above of the form "this file is owner-only" is worthless if
// isOwnerOnlyAccessible cannot return false. It must distinguish the two
// files, and it must also refuse a path that is not there at all rather than
// answering "yes, restrictive" about nothing.
TEST(FontAnalysisCacheAcl, IsOwnerOnlyAccessibleCanActuallySayNo)
{
    const auto dir = freshDir("discriminate");

    const auto ours = dir / "ours.bin";
    std::error_code ec;
    ASSERT_TRUE(pdfe::core::createOwnerOnlyFile(ours, ec)) << ec.message();

    const auto theirs = dir / "theirs.bin";
    ASSERT_TRUE(plantWorldWritableFile(theirs));

    EXPECT_TRUE(pdfe::core::isOwnerOnlyAccessible(ours));
    EXPECT_FALSE(pdfe::core::isOwnerOnlyAccessible(theirs))
        << "the ACL check cannot tell a world-writable file from an "
           "owner-only one, so every other assertion in this file is vacuous";

    EXPECT_FALSE(pdfe::core::isOwnerOnlyAccessible(dir / "not-there.bin"))
        << "a path that does not exist must not read as restrictive";
}

// ── The per-user root ──────────────────────────────────────────────────────

TEST(FontAnalysisCacheAcl, PerUserDataDirIsUnderLocalAppDataAndNamesPdfe)
{
    const auto d = pdfe::core::perUserDataDir();
    ASSERT_FALSE(d.empty())
        << "%LOCALAPPDATA% is unset in this environment";

    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    ASSERT_GT(n, 0u);

    const std::wstring got = d.wstring();
    const std::wstring root(buf);
    EXPECT_EQ(got.compare(0, root.size(), root), 0)
        << "the per-user data dir is not under %LOCALAPPDATA%";
    EXPECT_NE(got.find(L"PDFE"), std::wstring::npos);

    // It must never fall back to the current directory — that is the
    // GlyphAtlas defect phase-60 found, where a relative diagnostics name
    // landed in whatever directory the user happened to be standing in.
    EXPECT_TRUE(d.is_absolute());
}

#endif // _WIN32
