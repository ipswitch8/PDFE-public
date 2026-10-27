// -*- coding: utf-8 -*-
//
// Unit tests for pdfe::core::path_security::is_unc_or_remote.
//
// The function is the gate that closes the NetBIOS / LLMNR / mDNS
// broadcast vector observed in the phase-3 corpus runner. We test each
// shape PDFE could receive on its input boundary: bare UNC, slash
// variants, file:// URIs (3-slash local vs 4-slash UNC), network
// schemes, mixed-slash, edge cases (drive letters, localhost,
// extended-length prefixes).
//
// It used to be PURELY LEXICAL, and SEC-50 is what that cost: a drive
// letter mapped to a share (`net use Z: \\server\share`) is a local-
// looking string that reaches the same redirector as the UNC the gate
// refuses. Since phase-8 the check also RESOLVES a leading drive letter;
// `is_lexically_unc_or_remote` preserves the old string-only behaviour so
// the two layers stay separately testable. See MappedDriveResolvesToRemote
// below, which is the case that could not have passed before.

#include <gtest/gtest.h>
#include "core/PathSecurity.h"

#include <filesystem>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace ps = pdfe::core::path_security;

// ---- Local paths must be accepted ----

TEST(PathSecurity, AcceptsWindowsDriveLetter) {
    EXPECT_FALSE(ps::is_unc_or_remote("C:\\Users\\me\\doc.pdf"));
    EXPECT_FALSE(ps::is_unc_or_remote("c:/Users/me/doc.pdf"));
    EXPECT_FALSE(ps::is_unc_or_remote("D:\\file.pdf"));
}

TEST(PathSecurity, AcceptsPosixAbsolute) {
    EXPECT_FALSE(ps::is_unc_or_remote("/home/me/doc.pdf"));
    EXPECT_FALSE(ps::is_unc_or_remote("/tmp/x"));
}

TEST(PathSecurity, AcceptsRelativePath) {
    EXPECT_FALSE(ps::is_unc_or_remote("./doc.pdf"));
    EXPECT_FALSE(ps::is_unc_or_remote("doc.pdf"));
    EXPECT_FALSE(ps::is_unc_or_remote("subdir\\doc.pdf"));
    EXPECT_FALSE(ps::is_unc_or_remote("subdir/doc.pdf"));
}

TEST(PathSecurity, AcceptsFileSchemeLocalThreeSlashes) {
    EXPECT_FALSE(ps::is_unc_or_remote("file:///C:/Users/me/doc.pdf"));
    EXPECT_FALSE(ps::is_unc_or_remote("file:///tmp/doc.pdf"));
    EXPECT_FALSE(ps::is_unc_or_remote("FILE:///C:/x"));  // case-insensitive scheme
}

TEST(PathSecurity, AcceptsFileSchemeOneSlash) {
    // file:/path is a legacy form per RFC 8089 §3 — local absolute.
    EXPECT_FALSE(ps::is_unc_or_remote("file:/C:/x.pdf"));
    EXPECT_FALSE(ps::is_unc_or_remote("file:/tmp/x"));
}

TEST(PathSecurity, AcceptsFileLocalhost) {
    // RFC 8089 §3 — `file://localhost/...` is a documented local-host
    // alias.
    EXPECT_FALSE(ps::is_unc_or_remote("file://localhost/C:/x"));
    EXPECT_FALSE(ps::is_unc_or_remote("file://localhost/tmp/x"));
    EXPECT_FALSE(ps::is_unc_or_remote("file://LOCALHOST/x"));
}

TEST(PathSecurity, AcceptsExtendedLengthLocal) {
    // `\\?\C:\…` is Windows extended-length local — NOT UNC.
    EXPECT_FALSE(ps::is_unc_or_remote("\\\\?\\C:\\very\\long\\path"));
}

TEST(PathSecurity, AcceptsEmptyAndWhitespace) {
    // Empty / whitespace is not classified as remote — Document::open
    // already refuses empty paths upstream.
    EXPECT_FALSE(ps::is_unc_or_remote(""));
    EXPECT_FALSE(ps::is_unc_or_remote("   "));
}

// ---- UNC variants must be rejected ----

TEST(PathSecurity, RejectsBareUncBackslash) {
    EXPECT_TRUE(ps::is_unc_or_remote("\\\\server\\share\\file.pdf"));
}

TEST(PathSecurity, RejectsBareUncForwardSlash) {
    EXPECT_TRUE(ps::is_unc_or_remote("//server/share/file.pdf"));
}

TEST(PathSecurity, RejectsUncMixedSlashes) {
    EXPECT_TRUE(ps::is_unc_or_remote("\\/server\\share/file.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote("/\\server/share\\file.pdf"));
}

TEST(PathSecurity, RejectsExtendedLengthUnc) {
    // `\\?\UNC\server\share\…` is the extended-length UNC form.
    EXPECT_TRUE(ps::is_unc_or_remote("\\\\?\\UNC\\server\\share\\x"));
    EXPECT_TRUE(ps::is_unc_or_remote("\\\\?\\unc\\server\\share\\x"));  // case-insensitive
}

TEST(PathSecurity, RejectsDeviceNamespace) {
    // `\\.\PhysicalDriveN` is the device namespace; not a UNC strictly,
    // but it routes through the kernel resolver so we treat it the
    // same.
    EXPECT_TRUE(ps::is_unc_or_remote("\\\\.\\PhysicalDrive0"));
}

TEST(PathSecurity, RejectsLeadingWhitespaceUnc) {
    // Some malicious PDFs prefix UNC with whitespace to evade naive
    // checks. The Windows kernel still resolves these.
    EXPECT_TRUE(ps::is_unc_or_remote("  \\\\server\\share\\x.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote("\t//server/share/x.pdf"));
}

// ---- file:// scheme UNC variants must be rejected ----

TEST(PathSecurity, RejectsFileSchemeUncTwoSlashes) {
    // `file://host/path` — host non-empty → UNC.
    EXPECT_TRUE(ps::is_unc_or_remote("file://attacker.example/share/x.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote("file://192.168.0.1/share/x"));
}

TEST(PathSecurity, RejectsFileSchemeUncFourSlashes) {
    // `file:////host/share/path` — 4-slash UNC encoding inside file URI.
    EXPECT_TRUE(ps::is_unc_or_remote("file:////attacker.example/share/x.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote("file://///attacker.example/share/x"));  // 5 slashes
}

TEST(PathSecurity, RejectsFileSchemeNoSlash) {
    // `file:server/share` — RFC-reserved no-slash form. Refuse.
    EXPECT_TRUE(ps::is_unc_or_remote("file:server/share"));
}

// ---- Other network schemes must be rejected ----

TEST(PathSecurity, RejectsHttp) {
    EXPECT_TRUE(ps::is_unc_or_remote("http://attacker.example/x.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote("https://attacker.example/x.pdf"));
}

TEST(PathSecurity, RejectsFtp) {
    EXPECT_TRUE(ps::is_unc_or_remote("ftp://attacker.example/x.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote("ftps://attacker.example/x.pdf"));
}

TEST(PathSecurity, RejectsSmbAndCifs) {
    EXPECT_TRUE(ps::is_unc_or_remote("smb://attacker.example/share/x.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote("cifs://attacker.example/share/x"));
}

TEST(PathSecurity, RejectsNfs) {
    EXPECT_TRUE(ps::is_unc_or_remote("nfs://attacker.example/share/x.pdf"));
}

TEST(PathSecurity, RejectsWebdav) {
    EXPECT_TRUE(ps::is_unc_or_remote("webdav://attacker.example/x"));
    EXPECT_TRUE(ps::is_unc_or_remote("webdavs://attacker.example/x"));
    EXPECT_TRUE(ps::is_unc_or_remote("dav://attacker.example/x"));
    EXPECT_TRUE(ps::is_unc_or_remote("davs://attacker.example/x"));
}

TEST(PathSecurity, RejectsSshSftp) {
    EXPECT_TRUE(ps::is_unc_or_remote("ssh://attacker.example/x"));
    EXPECT_TRUE(ps::is_unc_or_remote("sftp://attacker.example/x"));
}

TEST(PathSecurity, RejectsLdap) {
    EXPECT_TRUE(ps::is_unc_or_remote("ldap://attacker.example/dc=x"));
    EXPECT_TRUE(ps::is_unc_or_remote("ldaps://attacker.example/dc=x"));
}

TEST(PathSecurity, RejectsCaseInsensitiveSchemes) {
    EXPECT_TRUE(ps::is_unc_or_remote("HTTP://attacker.example/x.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote("Http://attacker.example/x.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote("FILE://attacker.example/share/x"));
}

// ---- Wide-string overload ----

TEST(PathSecurity, WideStringUnc) {
    EXPECT_TRUE(ps::is_unc_or_remote(std::wstring_view{L"\\\\server\\share\\x"}));
    EXPECT_TRUE(ps::is_unc_or_remote(std::wstring_view{L"http://x.example/y"}));
    EXPECT_FALSE(ps::is_unc_or_remote(std::wstring_view{L"C:\\Users\\me"}));
}

// std::filesystem::path callers should `.string()` first; verify the
// conversion path works for the lexical check.
TEST(PathSecurity, FilesystemPathString) {
    EXPECT_TRUE(ps::is_unc_or_remote(std::filesystem::path("\\\\server\\share\\x.pdf").string()));
    EXPECT_FALSE(ps::is_unc_or_remote(std::filesystem::path("C:\\Users\\me\\x.pdf").string()));
}

// ---- is_unsafe_link_target: narrower predicate for /URI follow-ups ----
//
// This is the function used by MainWindow::dispatchLinkAction's Uri
// case. Browser-safe schemes (http, https, mailto, tel, ...) MUST NOT
// be classified as unsafe — refusing them would block every clickable
// web link in every PDF, contradicting the design intent.

TEST(PathSecurity, LinkTargetAllowsHttp) {
    EXPECT_FALSE(ps::is_unsafe_link_target("http://example.com/page"));
    EXPECT_FALSE(ps::is_unsafe_link_target("https://example.com/page"));
    EXPECT_FALSE(ps::is_unsafe_link_target("HTTPS://EXAMPLE.COM/PAGE"));  // case
}

TEST(PathSecurity, LinkTargetAllowsMailtoTelSms) {
    EXPECT_FALSE(ps::is_unsafe_link_target("mailto:user@example.com"));
    EXPECT_FALSE(ps::is_unsafe_link_target("tel:+15551234"));
    EXPECT_FALSE(ps::is_unsafe_link_target("sms:+15551234"));
}

TEST(PathSecurity, LinkTargetRefusesUnc) {
    EXPECT_TRUE(ps::is_unsafe_link_target("\\\\attacker.example\\share\\x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("//attacker.example/share/x"));
}

TEST(PathSecurity, LinkTargetRefusesFileScheme) {
    EXPECT_TRUE(ps::is_unsafe_link_target("file://attacker.example/share/x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("file:////attacker.example/share/x"));
}

TEST(PathSecurity, LinkTargetRefusesFileShareSchemes) {
    EXPECT_TRUE(ps::is_unsafe_link_target("smb://attacker.example/share/x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("cifs://attacker.example/share/x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("nfs://attacker.example/share/x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("webdav://attacker.example/x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("dav://attacker.example/x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("ftp://attacker.example/x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("ssh://attacker.example/x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("sftp://attacker.example/x"));
    EXPECT_TRUE(ps::is_unsafe_link_target("ldap://attacker.example/x"));
}

TEST(PathSecurity, LinkTargetEmptyOrWhitespace) {
    EXPECT_FALSE(ps::is_unsafe_link_target(""));
    EXPECT_FALSE(ps::is_unsafe_link_target("   "));
}

// Call-site regression: the URI dispatcher in MainWindow.cpp passes
// every link's URI through is_unsafe_link_target(); this is a table
// summary of the policy decision that the dispatcher implements. If a
// future refactor changes the dispatcher to call is_unc_or_remote()
// instead, these table rows will diverge from the URL-class
// expectation and the test will fail.
TEST(PathSecurity, LinkDispatcherPolicyTable) {
    struct Row { const char* uri; bool expectRefused; const char* why; };
    const Row rows[] = {
        // Browser-safe URLs: must NOT be refused.
        {"http://example.com",                 false, "web link"},
        {"https://example.com/path?x=1",       false, "secure web link"},
        {"mailto:user@example.com",            false, "email link"},
        {"tel:+15551234",                      false, "phone link"},
        // UNC and file-share schemes: MUST be refused.
        {"\\\\attacker.example\\share\\x",     true,  "bare UNC"},
        {"//attacker.example/share/x",         true,  "POSIX-slash UNC"},
        {"file://attacker.example/share/x",    true,  "file:// to UNC"},
        {"smb://attacker.example/share/x",     true,  "smb file share"},
        {"ftp://attacker.example/x",           true,  "ftp share"},
        {"webdav://attacker.example/x",        true,  "webdav share"},
        {"ldap://attacker.example/dc=x",       true,  "ldap fetch"},
    };
    for (const auto& r : rows) {
        EXPECT_EQ(ps::is_unsafe_link_target(r.uri), r.expectRefused)
            << "policy mismatch for [" << r.uri << "] (" << r.why << ")";
    }
}

// ============================================================================
// SEC-50 / SEC-51 — the drive letter that is a share
// ============================================================================
//
// SEC-50: `path_security::is_unc_or_remote` was purely lexical, so
// `net use Z: \\server\share` produced a path — `Z:\evil.exe` — that failed
// the `isSep(s[0]) && isSep(s[1])` test and was classified LOCAL on both the
// /Launch refusal (src/ui/MainWindow.cpp) and the Document::open boundary.
// The gate was bypassable by one command, and the bypass was invisible in
// the string.
//
// SEC-51 is the other half: `AcceptsWindowsDriveLetter` (above) deliberately
// accepts any drive letter, which is the decision that made SEC-50 possible.
// That test stays exactly as it was — accepting a genuinely local drive is
// correct and must not regress — and the mapped-drive REFUSAL now sits
// alongside it, so the pair states the whole policy rather than half of it.
//
// HOW THIS IS TESTED WITHOUT A SERVER. `DefineDosDeviceW(DDD_RAW_TARGET_PATH)`
// writes the DOS device table directly — the same table `subst` writes — so a
// drive letter can be pointed at `\??\UNC\server\share` with no network stack
// involved, no share that has to exist, and no administrator rights. Nothing
// is ever opened through it; only the classification is under test. That also
// means the test cannot accidentally emit the very broadcast the gate exists
// to prevent.

#if defined(_WIN32)

namespace {

// Defines a DOS drive letter for the lifetime of the object and removes
// exactly that definition afterwards. `letter()` is 0 if no free letter was
// available, which the tests treat as "cannot run here" rather than as a
// pass — a fixture that silently degrades to a no-op is how a security test
// becomes decoration.
class ScopedDosDevice {
public:
    // `rawTarget` is an NT-namespace path, e.g. L"\\??\\UNC\\srv\\share".
    explicit ScopedDosDevice(const wchar_t* rawTarget) {
        const DWORD used = ::GetLogicalDrives();
        // Walk down from Z: — high letters are the least likely to collide
        // with a real mapping on a developer box or a CI runner.
        for (int i = 25; i >= 3; --i) {
            if (used & (1u << i)) continue;
            const wchar_t name[3] = {
                static_cast<wchar_t>(L'A' + i), L':', L'\0' };
            if (::DefineDosDeviceW(DDD_RAW_TARGET_PATH, name, rawTarget)) {
                letter_ = static_cast<char>('A' + i);
                target_ = rawTarget;
                break;
            }
        }
    }

    ~ScopedDosDevice() {
        if (!letter_) return;
        const wchar_t name[3] = {
            static_cast<wchar_t>(letter_), L':', L'\0' };
        ::DefineDosDeviceW(
            DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH
                | DDD_EXACT_MATCH_ON_REMOVE,
            name, target_.c_str());
    }

    ScopedDosDevice(const ScopedDosDevice&) = delete;
    ScopedDosDevice& operator=(const ScopedDosDevice&) = delete;

    [[nodiscard]] char letter() const noexcept { return letter_; }

    // "Q:\\x.pdf" for the letter that was actually claimed.
    [[nodiscard]] std::string path(const char* tail) const {
        return std::string(1, letter_) + ":" + tail;
    }

private:
    char letter_ = 0;
    std::wstring target_;
};

} // namespace

// THE SEC-50 CASE. The same string, asked twice: the lexical layer says
// local (correctly — nothing in "Q:\x.pdf" says otherwise) and the gate says
// remote. Before this phase there was only the first answer, and it was the
// one every call site got.
TEST(PathSecurity, MappedDriveResolvesToRemote) {
    ScopedDosDevice dev(L"\\??\\UNC\\pdfe-test-server\\pdfe-test-share");
    ASSERT_NE(dev.letter(), 0)
        << "no free drive letter to define; the case did not run";

    const std::string doc = dev.path("\\x.pdf");

    EXPECT_FALSE(ps::is_lexically_unc_or_remote(doc))
        << "vacuity guard: if the LEXICAL layer already refused [" << doc
        << "] then this case proves nothing about the resolution step";

    EXPECT_TRUE(ps::is_unc_or_remote(doc))
        << "[" << doc << "] resolves to a UNC target and must be refused";

    // Every spelling of the same drive that reaches a caller.
    EXPECT_TRUE(ps::is_unc_or_remote(dev.path("/x.pdf")));
    EXPECT_TRUE(ps::is_unc_or_remote(dev.path("\\sub\\dir\\x.pdf")));
    EXPECT_TRUE(ps::is_unc_or_remote(
        std::wstring(1, static_cast<wchar_t>(dev.letter())) + L":\\x.pdf"));

    // The extended-length prefix is a local-path spelling, not a UNC one,
    // so it must not be allowed to launder the mapped drive past the check.
    EXPECT_TRUE(ps::is_unc_or_remote("\\\\?\\" + dev.path("\\x.pdf")));

    // And the URI spelling, which reaches is_unsafe_link_target from the
    // /URI dispatcher.
    EXPECT_TRUE(ps::is_unc_or_remote("file:///" + dev.path("/x.pdf")));
}

// The /Launch reproduction from the gap report, end to end at this layer:
// `/A << /S /Launch /F (Z:\evil.exe) >>` must now hit the UNC refusal.
TEST(PathSecurity, MappedDriveLaunchTargetIsAnUnsafeLinkTarget) {
    ScopedDosDevice dev(L"\\??\\UNC\\pdfe-test-server\\pdfe-test-share");
    ASSERT_NE(dev.letter(), 0)
        << "no free drive letter to define; the case did not run";

    const std::string exe = dev.path("\\evil.exe");
    EXPECT_FALSE(ps::is_lexically_unc_or_remote(exe));
    EXPECT_TRUE(ps::is_unsafe_link_target(exe))
        << "a /Launch target on a share-mapped drive reaches the same "
           "redirector as the UNC spelling and must be refused the same way";
}

// THE OTHER HALF, and the reason this is a resolution step rather than a
// blanket "refuse anything substituted". A `subst` onto a LOCAL directory is
// an ordinary local path and must still be accepted — if it were not, the
// check would be passing the case above for the wrong reason and would break
// every developer who uses subst.
TEST(PathSecurity, SubstOntoALocalDirectoryIsStillLocal) {
    wchar_t windir[MAX_PATH] = {};
    ASSERT_NE(::GetWindowsDirectoryW(windir, MAX_PATH), 0u);
    const std::wstring rawLocal = std::wstring(L"\\??\\") + windir;

    ScopedDosDevice dev(rawLocal.c_str());
    ASSERT_NE(dev.letter(), 0)
        << "no free drive letter to define; the case did not run";

    EXPECT_FALSE(ps::is_unc_or_remote(dev.path("\\notepad.exe")))
        << "a subst onto a local directory is local; refusing it would make "
           "the mapped-drive case pass for the wrong reason";
}

// The resolution step must not answer "remote" for a letter that names
// nothing, and must not answer it for the drive the tests themselves live on.
TEST(PathSecurity, DriveResolutionIsNarrow) {
    // The volume this test binary was built on is local by construction.
    const std::filesystem::path here = std::filesystem::current_path();
    const std::string root = here.root_name().string();   // e.g. "C:"
    ASSERT_FALSE(root.empty());
    EXPECT_FALSE(ps::is_unc_or_remote(root + "\\some\\file.pdf"));
    EXPECT_FALSE(ps::drive_letter_resolves_to_remote(root[0]));

    // Non-letters and letters that are not drives answer "not remote":
    // the step only ever WIDENS the refusal set.
    EXPECT_FALSE(ps::drive_letter_resolves_to_remote('1'));
    EXPECT_FALSE(ps::drive_letter_resolves_to_remote('\0'));
    EXPECT_FALSE(ps::drive_letter_resolves_to_remote('['));
}

// A caller that explicitly opts DOWN to the lexical layer gets the old
// behaviour, unchanged — this is what makes the weak check a deliberate
// choice rather than an accident of which overload was in scope.
TEST(PathSecurity, LexicalVariantStillAnswersTheStringOnly) {
    EXPECT_TRUE(ps::is_lexically_unc_or_remote("\\\\server\\share\\x.pdf"));
    EXPECT_FALSE(ps::is_lexically_unc_or_remote("C:\\Users\\me\\x.pdf"));
    EXPECT_TRUE(ps::is_lexically_unc_or_remote(L"\\\\server\\share\\x.pdf"));
    EXPECT_FALSE(ps::is_lexically_unc_or_remote(L"C:\\Users\\me\\x.pdf"));
}

#endif // _WIN32
