// -*- coding: utf-8 -*-
//
// GitHub #42 / SEC-104 — the autosave recovery directory's ACL posture.
//
// WHAT WAS MISSING AND WHAT THIS IS.
//
// docs/security-policy.md ("The local-machine assumption") records PDFE's
// single-user-workstation assumption and states, deliberately and with the
// reasoning shown, that an explicit owner-only DACL on the recovery directory
// is NOT required — an owner-only DACL on a roaming/redirected AppData is a
// good way to break profile sync and backup tooling, trading a confidentiality
// risk that needs a hostile local account for an availability risk that needs
// no attacker at all.
//
// That document then says the honest thing about itself:
//
//     "it is an assumption asserted in a document, and nothing in the product
//      enforces or checks it ... it is not a control, and it should not be
//      counted as one."
//
// This file is the missing half. It does not change the decision and it does
// not harden anything. It ASSERTS the posture the document describes, against
// the real directory `AutosaveManager::defaultRecoveryDir()` actually creates,
// and fails if that posture ever becomes WEAKER than the documented baseline.
//
// THE BASELINE, STATED AS A PREDICATE.
//
//   Expected trustees on the recovery directory:  the owning user, SYSTEM,
//   and Administrators — i.e. exactly what the per-user app-data root grants
//   and the directory inherits, because defaultRecoveryDir() calls
//   std::filesystem::create_directories() with no security descriptor. That
//   root is %APPDATA% (roaming), NOT %LOCALAPPDATA%: Qt 6 maps
//   QStandardPaths::AppDataLocation to the roaming profile. The policy document
//   said "local" and was wrong; see
//   TheRecoveryDirectoryRootIsTheOnePolicyDescribes below.
//
//   NOT required:  a protected (inheritance-broken) owner-only DACL. The
//   directory is expected to carry an INHERITED DACL, and
//   TheBaselineToleratesAnInheritedNonProtectedDacl asserts exactly that, so
//   this file cannot quietly drift into demanding the hardening the policy
//   document declined.
//
//   Weaker than baseline (a FAILURE):  any ALLOW ace for a principal outside
//   that set — `Everyone`, `Authenticated Users`, `Users`, a second local
//   account, a domain group. The named examples in GitHub #42 are Everyone and
//   Authenticated Users gaining write; the predicate is broader than that on
//   purpose, because a READ ace for Everyone is the same confidentiality
//   exposure (recovery files are full copies of the document being edited,
//   written without the user having saved).
//
// PRIOR ART, REUSED RATHER THAN REINVENTED. `pdfe::core::isOwnerOnlyAccessible`
// (src/core/TempFileSafety.h, SEC-103) is already precisely this predicate: it
// reads the DACL back off the filesystem and returns false if any ALLOW ace
// names anyone but the calling user, SYSTEM or Administrators. Its NAME says
// "owner only", which reads as stricter than this baseline — it is not: SYSTEM
// and Administrators are explicitly tolerated (they own the machine and cannot
// meaningfully be excluded), and it says nothing about whether the DACL is
// protected. That is the documented baseline exactly. tests/test_font_cache_acl
// and tests/test_search_index_acl_phase70 use the same helper for the same
// reason: an assertion that reads the real ACL is worth more than one that
// trusts a creating call's return value.
//
// WHY THIS IS NOT A VACUOUS ORACLE. Three separate hazards, each answered:
//
//   1. "Permanently true." Two POSITIVE CONTROLS build directories that ARE
//      weaker — one granting Everyone full control, one granting Authenticated
//      Users full control, the two principals GitHub #42 names — and require
//      the predicate to say NO. If the predicate could not say no, those fail.
//   2. "The measurement never happened." Every case that asserts on the real
//      directory first establishes, from the filesystem, that the DACL was
//      readable and non-empty (`readDaclShape`), and fails naming that if not.
//      A NULL or unreadable DACL is a failure, not a pass.
//   3. "It asserts about some directory, not the product's." The path under
//      test is the one AutosaveManager itself resolves: the static
//      defaultRecoveryDir(), cross-checked against the instance-side
//      recoveryDir() an AutosaveManager built with an empty recoveryDir lands
//      on, which is what actually receives the recovery copies.
//
// POSIX: the baseline is the 0700-equivalent check isOwnerOnlyAccessible
// already implements there. The DACL-shape cases are compiled out rather than
// weakened — there is no DACL to read, so there is nothing to assert.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "core/AutosaveManager.h"
#include "core/TempFileSafety.h"

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

using pdfe::core::AutosaveManager;
using pdfe::core::isOwnerOnlyAccessible;

namespace {

std::filesystem::path scratchDir(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "pdfe_autosave_acl" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir.parent_path(), ec);
    return dir;
}

#if defined(_WIN32)

/// What a real read of `path`'s DACL found. `read` false means the security
/// descriptor could not be fetched at all; `present` false means a NULL DACL
/// (which grants everyone everything). Both are failures for a directory that
/// is supposed to be protected by an inherited ACL — and, just as importantly,
/// both are the shapes under which a naive "no bad ace found" loop would
/// report success having examined nothing.
struct DaclShape {
    bool read = false;
    bool present = false;
    bool protectedDacl = false;   ///< SE_DACL_PROTECTED: inheritance broken
    DWORD aceCount = 0;
    std::vector<std::wstring> foreignTrustees;  ///< allow-aces outside baseline
};

std::vector<unsigned char> currentUserSidBytes()
{
    std::vector<unsigned char> out;
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return out;
    }
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed) {
        std::vector<unsigned char> buf(needed);
        if (::GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
            auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
            if (tu->User.Sid && ::IsValidSid(tu->User.Sid)) {
                const DWORD len = ::GetLengthSid(tu->User.Sid);
                out.resize(len);
                ::CopySid(len, out.data(), tu->User.Sid);
            }
        }
    }
    ::CloseHandle(token);
    return out;
}

std::wstring sidToString(PSID sid)
{
    LPWSTR s = nullptr;
    if (!::ConvertSidToStringSidW(sid, &s)) {
        return L"<unprintable-sid>";
    }
    std::wstring out(s);
    ::LocalFree(s);
    // Resolve to account\name where possible — a bare S-1-5-11 in a failure
    // message costs the reader a lookup at exactly the wrong moment.
    WCHAR name[256] = {};
    WCHAR dom[256] = {};
    DWORD nlen = 256, dlen = 256;
    SID_NAME_USE use{};
    if (::LookupAccountSidW(nullptr, sid, name, &nlen, dom, &dlen, &use)) {
        out += L" (";
        out += dom;
        out += L"\\";
        out += name;
        out += L")";
    }
    return out;
}

std::string narrow(const std::wstring& w)
{
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) {
        out.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return out;
}

/// Read `path`'s DACL and classify every ALLOW ace against the documented
/// baseline trustee set (this user + SYSTEM + Administrators). Never writes.
DaclShape readDaclShape(const std::filesystem::path& path)
{
    DaclShape shape;
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    if (::GetNamedSecurityInfoW(path.wstring().c_str(), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &dacl, nullptr, &sd)
        != ERROR_SUCCESS) {
        return shape;
    }
    shape.read = true;

    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (::GetSecurityDescriptorControl(sd, &control, &revision)) {
        shape.protectedDacl = (control & SE_DACL_PROTECTED) != 0;
    }

    if (dacl) {
        shape.present = true;
        shape.aceCount = dacl->AceCount;
        const std::vector<unsigned char> me = currentUserSidBytes();
        for (DWORD i = 0; i < dacl->AceCount; ++i) {
            void* raw = nullptr;
            if (!::GetAce(dacl, i, &raw)) {
                continue;
            }
            auto* hdr = static_cast<ACE_HEADER*>(raw);
            if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) {
                continue;  // a DENY ace can only narrow access
            }
            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            PSID sid = reinterpret_cast<PSID>(&ace->SidStart);
            if (!me.empty()
                && ::EqualSid(sid, const_cast<unsigned char*>(me.data()))) {
                continue;
            }
            if (::IsWellKnownSid(sid, WinLocalSystemSid)
                || ::IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) {
                continue;
            }
            shape.foreignTrustees.push_back(sidToString(sid));
        }
    }
    if (sd) {
        ::LocalFree(sd);
    }
    return shape;
}

std::string describeForeign(const DaclShape& shape)
{
    std::string s;
    for (const auto& t : shape.foreignTrustees) {
        if (!s.empty()) {
            s += ", ";
        }
        s += narrow(t);
    }
    return s;
}

/// Create `dir` with an explicit DACL granting `sddlTrustee` full control, so a
/// positive control can prove the baseline predicate is capable of saying NO.
/// Returns false if the directory could not be created that way, which a test
/// must treat as "cannot run" rather than as a pass.
bool createDirGrantingFullControlTo(const std::filesystem::path& dir,
                                    const wchar_t* sddl)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    std::error_code ec;
    std::filesystem::create_directories(dir.parent_path(), ec);
    const BOOL ok = ::CreateDirectoryW(dir.wstring().c_str(), &sa);
    ::LocalFree(sd);
    return ok != FALSE;
}

#endif  // _WIN32

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The directory under test really is the product's
// ─────────────────────────────────────────────────────────────────────────────

TEST(AutosaveRecoveryDirAcl, TheDefaultRecoveryDirectoryExistsAndIsADirectory)
{
    const std::filesystem::path dir = AutosaveManager::defaultRecoveryDir();
    ASSERT_FALSE(dir.empty())
        << "defaultRecoveryDir() returned an empty path; there is nothing to "
           "assert an ACL on";
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(dir, ec))
        << "defaultRecoveryDir() is documented as 'Created if missing' but "
        << dir.string() << " does not exist";
    EXPECT_TRUE(std::filesystem::is_directory(dir, ec))
        << dir.string() << " exists but is not a directory";
}

TEST(AutosaveRecoveryDirAcl, TheDirectoryUnderTestIsTheOneAutosaveWritesInto)
{
    // The instance path and the static must agree, or this whole file would be
    // asserting the ACL of a directory nothing writes to. AutosaveManager's
    // constructor maps an empty recoveryDir onto defaultRecoveryDir(); assert
    // that mapping rather than assume it.
    const std::filesystem::path expected = AutosaveManager::defaultRecoveryDir();
    AutosaveManager mgr(nullptr, nullptr, std::filesystem::path{},
                        std::filesystem::path{});
    EXPECT_EQ(mgr.recoveryDir(), expected)
        << "an AutosaveManager constructed with an empty recoveryDir did not "
           "land on defaultRecoveryDir(); the ACL asserted below would be the "
           "wrong directory's";
}

TEST(AutosaveRecoveryDirAcl, TheRecoveryDirectoryIsUnderTheUserAppDataLocation)
{
    // The documented baseline is INHERITED from the per-user app-data root. If
    // the directory has landed somewhere else — the QDir::tempPath() fallback
    // defaultRecoveryDir() takes when AppDataLocation is empty, or a machine
    // whose AppData is redirected to a share — then the parent whose ACL is
    // being inherited is NOT the one docs/security-policy.md reasons about, and
    // the reader must be told that rather than shown a green tick.
    const std::filesystem::path dir = AutosaveManager::defaultRecoveryDir();
    const QString appData =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    ASSERT_FALSE(appData.isEmpty())
        << "QStandardPaths::AppDataLocation is empty, so defaultRecoveryDir() "
           "fell back to the temp directory — the inherited-ACL reasoning in "
           "docs/security-policy.md does not apply to that location";

    const std::filesystem::path base(appData.toStdU16String());
    std::error_code ec;
    const auto rel = std::filesystem::relative(dir, base, ec);
    EXPECT_FALSE(ec) << "could not relate " << dir.string() << " to "
                     << base.string() << ": " << ec.message();
    EXPECT_FALSE(rel.empty());
    EXPECT_NE(rel.native().rfind(std::filesystem::path("..").native(), 0), 0u)
        << "recovery dir " << dir.string() << " is not under the per-user "
        << "app-data location " << base.string();
}

#if defined(_WIN32)

TEST(AutosaveRecoveryDirAcl, TheRecoveryDirectoryRootIsTheOnePolicyDescribes)
{
    // MEASURED, NOT ASSUMED — and the measurement corrected the document.
    //
    // docs/security-policy.md used to state the recovery directory's parent was
    // %LOCALAPPDATA%. It is not: Qt 6 maps QStandardPaths::AppDataLocation to
    // the ROAMING profile (%APPDATA%); the local one is AppLocalDataLocation.
    // Measured 2026-09-12 the directory is
    // C:\Users\<user>\AppData\Roaming\PDFE\PDFE\recovery.
    //
    // The distinction is not cosmetic: it decides whether the policy's
    // "redirected or roaming AppData" caveat is an edge case or the norm. On a
    // domain-joined machine with roaming profiles it is the norm — which is why
    // the decision NOT to break profile synchronisation with an owner-only DACL
    // is better justified than the document originally realised.
    //
    // So this pins the root the policy now names. It fails if the directory
    // moves to the local root, to the temp fallback, or anywhere else, which is
    // the signal that the ACL reasoning above needs re-deriving rather than
    // re-asserting.
    const std::filesystem::path dir = AutosaveManager::defaultRecoveryDir();
    const QString roaming = qEnvironmentVariable("APPDATA");
    ASSERT_FALSE(roaming.isEmpty()) << "%APPDATA% is unset on this machine";

    const std::filesystem::path roamingRoot(roaming.toStdU16String());
    std::error_code ec;
    const auto rel = std::filesystem::relative(dir, roamingRoot, ec);
    const bool underRoaming =
        !ec && !rel.empty()
        && rel.native().rfind(std::filesystem::path("..").native(), 0) != 0u;

    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    EXPECT_TRUE(underRoaming)
        << "recovery dir " << dir.string() << " is no longer under %APPDATA% ("
        << roamingRoot.string() << "). docs/security-policy.md derives the "
           "expected trustee set from that root's inherited ACL; if the "
           "location has moved (to %LOCALAPPDATA% = "
        << local.toStdString()
        << ", to the temp fallback, or to a redirected share) that derivation "
           "must be redone, not merely re-asserted.";
}

#endif  // _WIN32

// ─────────────────────────────────────────────────────────────────────────────
// The baseline assertion itself
// ─────────────────────────────────────────────────────────────────────────────

TEST(AutosaveRecoveryDirAcl, EffectiveAclMatchesTheDocumentedBaseline)
{
    const std::filesystem::path dir = AutosaveManager::defaultRecoveryDir();
    ASSERT_TRUE(std::filesystem::is_directory(dir));

#if defined(_WIN32)
    const DaclShape shape = readDaclShape(dir);
    // Establish that the measurement HAPPENED before believing its verdict.
    ASSERT_TRUE(shape.read)
        << "could not read the DACL of " << dir.string()
        << " (GetNamedSecurityInfoW failed) — this assertion measured nothing";
    ASSERT_TRUE(shape.present)
        << dir.string() << " has a NULL DACL, which grants every principal "
           "full control — the weakest possible posture, not the baseline";
    ASSERT_GT(shape.aceCount, 0u)
        << dir.string() << " has an empty DACL; nothing was examined";

    EXPECT_TRUE(shape.foreignTrustees.empty())
        << "recovery directory " << dir.string()
        << " is WEAKER than the documented single-user-workstation baseline "
           "(docs/security-policy.md, 'The local-machine assumption'). "
           "Recovery files are full copies of the document being edited, "
           "written without the user saving. Principals outside {this user, "
           "SYSTEM, Administrators} hold an allow-ace on it: "
        << describeForeign(shape);
#endif

    // The same verdict through the shared helper the other ACL suites use, so
    // this file and src/core agree on what the baseline means rather than
    // carrying two drifting definitions of it.
    EXPECT_TRUE(isOwnerOnlyAccessible(dir))
        << "pdfe::core::isOwnerOnlyAccessible says " << dir.string()
        << " is reachable by a principal outside {this user, SYSTEM, "
           "Administrators}";
}

#if defined(_WIN32)

TEST(AutosaveRecoveryDirAcl, TheBaselineToleratesAnInheritedNonProtectedDacl)
{
    // The policy document declines to require an owner-only DACL, so this
    // suite must not smuggle that requirement in. The recovery directory is
    // created by std::filesystem::create_directories() with NO security
    // descriptor, so its DACL is INHERITED and SE_DACL_PROTECTED is clear —
    // and it still satisfies the baseline. Asserting that combination
    // explicitly is what stops a later edit turning this file into a
    // hardening mandate the policy deliberately rejected.
    const std::filesystem::path dir = AutosaveManager::defaultRecoveryDir();
    const DaclShape shape = readDaclShape(dir);
    ASSERT_TRUE(shape.read && shape.present);

    EXPECT_FALSE(shape.protectedDacl)
        << dir.string() << " carries a PROTECTED (inheritance-broken) DACL. "
           "That is not a security failure, but docs/security-policy.md "
           "records that PDFE deliberately does NOT set one here — so either "
           "the product started hardening this directory and the policy "
           "document is now stale, or something outside PDFE altered it.";
    EXPECT_TRUE(isOwnerOnlyAccessible(dir))
        << "an inherited DACL is expected, but it must still name no "
           "principal outside the baseline set";
}

// ─────────────────────────────────────────────────────────────────────────────
// Positive controls — the predicate can say NO, for the two principals
// GitHub #42 names by name.
// ─────────────────────────────────────────────────────────────────────────────

TEST(AutosaveRecoveryDirAcl, AnEveryoneWritableDirectoryIsWeakerThanTheBaseline)
{
    const auto dir = scratchDir("everyone");
    // WD = Everyone, OICI = inherited by containers and objects, FA = full.
    ASSERT_TRUE(createDirGrantingFullControlTo(dir, L"D:P(A;OICI;FA;;;WD)"))
        << "could not create a Everyone-full-control directory, so this "
           "control proves nothing — refusing to pass vacuously";

    const DaclShape shape = readDaclShape(dir);
    ASSERT_TRUE(shape.read && shape.present);
    EXPECT_FALSE(shape.foreignTrustees.empty())
        << "an Everyone(FA) DACL was not classified as naming a foreign "
           "trustee — the classifier used on the real directory cannot detect "
           "the exposure it exists to detect";
    EXPECT_FALSE(isOwnerOnlyAccessible(dir))
        << "isOwnerOnlyAccessible accepted a directory granting Everyone full "
           "control; the baseline assertion above is therefore vacuous";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(AutosaveRecoveryDirAcl,
     AnAuthenticatedUsersWritableDirectoryIsWeakerThanTheBaseline)
{
    const auto dir = scratchDir("authusers");
    // AU = Authenticated Users. Named explicitly because it is the case that
    // looks harmless — every logged-in account on the box, which on a
    // terminal server is precisely the co-user the policy assumes away.
    ASSERT_TRUE(createDirGrantingFullControlTo(dir, L"D:P(A;OICI;FA;;;AU)"))
        << "could not create an Authenticated-Users-full-control directory, "
           "so this control proves nothing — refusing to pass vacuously";

    const DaclShape shape = readDaclShape(dir);
    ASSERT_TRUE(shape.read && shape.present);
    EXPECT_FALSE(shape.foreignTrustees.empty())
        << "an Authenticated Users(FA) DACL was not classified as naming a "
           "foreign trustee";
    EXPECT_FALSE(isOwnerOnlyAccessible(dir))
        << "isOwnerOnlyAccessible accepted a directory granting Authenticated "
           "Users full control";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(AutosaveRecoveryDirAcl, ADirectoryWithOnlyBaselineTrusteesIsAccepted)
{
    // The other half of the control pair: the classifier must also be able to
    // say YES, or "no foreign trustees" on the real directory would be
    // indistinguishable from a classifier that rejects everything.
    const auto dir = scratchDir("baseline");
    std::error_code ec;
    ASSERT_TRUE(pdfe::core::createOwnerOnlyDirectory(dir, ec)) << ec.message();

    const DaclShape shape = readDaclShape(dir);
    ASSERT_TRUE(shape.read && shape.present);
    EXPECT_TRUE(shape.foreignTrustees.empty())
        << "a directory built with only baseline trustees was reported as "
           "carrying foreign ones: " << describeForeign(shape);
    EXPECT_TRUE(isOwnerOnlyAccessible(dir));

    std::filesystem::remove_all(dir, ec);
}

#endif  // _WIN32
