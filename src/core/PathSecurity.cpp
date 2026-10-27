#include "PathSecurity.h"

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winnetwk.h>
#  pragma comment(lib, "mpr.lib")
#endif

namespace pdfe::core::path_security {

namespace {

// Lower-case ASCII without locale dependency.
constexpr char asciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// True if `s` starts with `pfx` case-insensitively (ASCII only).
bool startsWithCi(std::string_view s, std::string_view pfx) noexcept {
    if (s.size() < pfx.size()) return false;
    for (std::size_t i = 0; i < pfx.size(); ++i) {
        if (asciiLower(s[i]) != asciiLower(pfx[i])) return false;
    }
    return true;
}

// Strip one leading scheme like "file:" if present and record whether
// it was found. The output points at characters after the colon.
bool stripScheme(std::string_view& s, std::string_view scheme) noexcept {
    if (!startsWithCi(s, scheme)) return false;
    s.remove_prefix(scheme.size());
    return true;
}

// True if `c` is a path separator on Windows or POSIX.
constexpr bool isSep(char c) noexcept {
    return c == '/' || c == '\\';
}

// Count leading separators.
std::size_t countLeadingSep(std::string_view s) noexcept {
    std::size_t n = 0;
    while (n < s.size() && isSep(s[n])) ++n;
    return n;
}

// True if after stripping the leading separators we have a non-empty
// non-separator hostname-ish token. We require at least one character
// before the next separator, otherwise it's just `\\\\` or `////`
// trash.
bool hasHostname(std::string_view rest) noexcept {
    if (rest.empty() || isSep(rest.front())) return false;
    // Disallow drive letter as "hostname" — `\C:\…` is malformed but
    // shouldn't be classified as UNC.
    if (rest.size() >= 2 && rest[1] == ':') return false;
    return true;
}

// Heuristic: classify an *un-schemed* path string. Returns true if it
// looks like a UNC.
bool unschemedLooksLikeUnc(std::string_view s) noexcept {
    // `\\?\UNC\server\share\…` — Windows extended-length UNC.
    // `\\?\server\share\…`     — same with normalized slashes (rare).
    // `\\.\…`                  — device namespace; treat as UNC-equivalent
    //                             since it routes through the kernel.
    if (s.size() >= 4 && isSep(s[0]) && isSep(s[1])) {
        if ((s[2] == '?' || s[2] == '.') && isSep(s[3])) {
            // `\\?\C:\…` is local extended-length, not UNC.
            // `\\?\UNC\…` is UNC.
            // `\\.\PhysicalDriveN` is device, treat as remote-equivalent.
            std::string_view tail = s.substr(4);
            if (s[2] == '.') return true;            // device namespace
            if (startsWithCi(tail, "UNC\\") ||
                startsWithCi(tail, "UNC/")) {
                return true;
            }
            return false;                            // `\\?\C:\…` local
        }
        // Plain UNC `\\server\share\…`.
        return hasHostname(s.substr(2));
    }
    return false;
}

bool checkUtf8(std::string_view path) noexcept {
    // Strip surrounding whitespace; some PDFs embed paths with leading
    // spaces that the OS still resolves.
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t' ||
                             path.front() == '\r' || path.front() == '\n')) {
        path.remove_prefix(1);
    }
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t' ||
                             path.back() == '\r' || path.back() == '\n')) {
        path.remove_suffix(1);
    }
    if (path.empty()) return false;

    // Network-implying URI schemes — fetch from elsewhere on the wire.
    static constexpr std::string_view kRemoteSchemes[] = {
        "http://",   "https://",
        "ftp://",    "ftps://",
        "smb://",    "cifs://",
        "nfs://",
        "webdav://", "webdavs://", "dav://", "davs://",
        "ssh://",    "sftp://",
        "tftp://",
        "ldap://",   "ldaps://",
    };
    if (std::any_of(std::begin(kRemoteSchemes), std::end(kRemoteSchemes),
            [path](auto sch) { return startsWithCi(path, sch); })) {
        return true;
    }

    // file:// URIs need shape analysis. ISO 8089 / RFC 8089 canonical
    // form is `file://<host>/<path>`; `file:///<path>` (empty host)
    // means local. `file:////server/share/…` is the de-facto encoding
    // for a UNC inside a file URI on Windows.
    {
        std::string_view rest = path;
        if (stripScheme(rest, "file:")) {
            const std::size_t leading = countLeadingSep(rest);
            // file:/foo or file://foo … (one or two slashes)
            //   file://host/path → host is non-empty → UNC
            //   file:///path     → host empty → local
            //   file:////host/sh → embedded UNC → UNC
            //   file:\\\\host\sh → backslash variant → UNC
            if (leading == 0) {
                // `file:foo` — RFC permits no slashes for relative, but
                // PDFE doesn't accept these from PDFs at all. Treat as
                // remote/suspicious and refuse at the boundary.
                return true;
            }
            if (leading == 1) {
                // `file:/path` — local absolute path.
                return false;
            }
            if (leading == 2) {
                // `file://host/path` — if there's a host token before
                // the next separator, it's UNC.
                std::string_view tail = rest.substr(2);
                // `file://localhost/...` is a documented local-host
                // alias per RFC 8089.
                if (startsWithCi(tail, "localhost/") ||
                    startsWithCi(tail, "localhost\\") ||
                    tail == "localhost") {
                    return false;
                }
                // No host segment at all → `file:///…` (3rd slash present
                // but countLeadingSep would have caught it above; here
                // we only have 2 slashes total, so a host MUST follow).
                return hasHostname(tail);
            }
            // 3+ leading slashes: `file:///path` (3) is local;
            // `file:////host/share` (4+) is embedded UNC.
            if (leading >= 4) return true;
            // leading == 3 → local.
            return false;
        }
    }

    // No scheme: classify as raw path.
    return unschemedLooksLikeUnc(path);
}

// ── SEC-50: the part of "is this remote?" that the string cannot answer ──
//
// `Z:\x.pdf` and `\\server\share\x.pdf` can name the same bytes on the same
// wire. Only the first one looks local. Everything below exists to close
// that, and it is deliberately the DEFAULT path of is_unc_or_remote rather
// than an opt-in sibling — see the header for why.

// Extract the drive letter of a path that begins `X:`, else '\0'.
// Handles the `\\?\X:\…` extended-length prefix, which is a local path in
// UNC clothing and would otherwise be missed.
char leadingDriveLetter(std::string_view s) noexcept {
    // `file:///Z:/x.pdf` names the mapped drive just as surely as `Z:\x.pdf`
    // does, so the scheme is stripped here too — otherwise the URI spelling
    // would be a free bypass of the resolution step.
    if (startsWithCi(s, "file:")) {
        s.remove_prefix(5);
        // `file:///Z:/…`, `file://Z:/…`, `file:Z:/…` — drop any separators
        // between the scheme and the drive letter. (`file://server/share`
        // is already refused lexically and never reaches here.)
        s.remove_prefix(countLeadingSep(s));
    }
    // `\\?\X:` / `//?/X:` — skip the extended-length prefix.
    if (s.size() >= 6 && isSep(s[0]) && isSep(s[1]) && s[2] == '?' && isSep(s[3])) {
        s.remove_prefix(4);
    }
    if (s.size() < 2 || s[1] != ':') return '\0';
    const char c = asciiLower(s[0]);
    if (c < 'a' || c > 'z') return '\0';
    return c;
}

#if defined(_WIN32)

// True if a QueryDosDeviceW target string names the network redirector.
bool dosTargetIsRemote(const wchar_t* target) noexcept {
    static const wchar_t* const kRemotePrefixes[] = {
        L"\\??\\UNC\\",
        L"\\Device\\Mup\\",
        L"\\Device\\LanmanRedirector\\",
        L"\\??\\\\\\",           // subst onto a raw `\\server\share`
    };
    for (const wchar_t* pfx : kRemotePrefixes) {
        const std::size_t n = std::wcslen(pfx);
        if (::_wcsnicmp(target, pfx, n) == 0) return true;
    }
    return false;
}

#endif // _WIN32

} // namespace

bool drive_letter_resolves_to_remote([[maybe_unused]] char driveLetter) noexcept {
#if defined(_WIN32)
    const char c = asciiLower(driveLetter);
    if (c < 'a' || c > 'z') return false;

    wchar_t letter = static_cast<wchar_t>(L'A' + (c - 'a'));

    // Follow at most this many `subst X: Y:\…` indirections. Two hops is
    // already beyond anything real; the bound is here so a deliberately
    // circular DOS device table cannot spin this function.
    for (int hop = 0; hop < 4; ++hop) {
        // (1) The cheap, and by far the most common, case: `net use`.
        const wchar_t root[4] = { letter, L':', L'\\', L'\0' };
        if (::GetDriveTypeW(root) == DRIVE_REMOTE) return true;

        const wchar_t name[3] = { letter, L':', L'\0' };

        // (2) A redirected drive whose type query was inconclusive.
        {
            wchar_t remote[MAX_PATH + 1] = {};
            DWORD len = MAX_PATH;
            if (::WNetGetConnectionW(name, remote, &len) == NO_ERROR) return true;
        }

        // (3) `subst Z: \\server\share` is not a network connection, so
        //     neither call above sees it. The DOS device table does.
        wchar_t target[1024] = {};
        if (::QueryDosDeviceW(name, target, 1024) == 0) return false;
        if (dosTargetIsRemote(target)) return true;

        // A subst onto another drive letter — `\??\Y:\sub` — is the only
        // case worth another hop. Anything else is a local device.
        if (::_wcsnicmp(target, L"\\??\\", 4) != 0) return false;
        const wchar_t* rest = target + 4;
        if (!rest[0] || rest[1] != L':') return false;
        const wchar_t next = static_cast<wchar_t>(::towupper(rest[0]));
        if (next < L'A' || next > L'Z') return false;
        if (next == letter) return false;   // self-referential; stop.
        letter = next;
    }
#endif
    return false;
}

bool is_lexically_unc_or_remote(std::string_view path) noexcept {
    return checkUtf8(path);
}

bool is_unc_or_remote(std::string_view path) noexcept {
    if (checkUtf8(path)) return true;

    // Not lexically remote. It may still BE remote: ask the drive table.
    const char drive = leadingDriveLetter(path);
    if (drive == '\0') return false;
    return drive_letter_resolves_to_remote(drive);
}

bool is_unsafe_link_target(std::string_view uri) noexcept {
    // Strip surrounding whitespace.
    auto trimmed = uri;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' ||
                                trimmed.front() == '\r' || trimmed.front() == '\n')) {
        trimmed.remove_prefix(1);
    }
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' ||
                                trimmed.back() == '\r' || trimmed.back() == '\n')) {
        trimmed.remove_suffix(1);
    }
    if (trimmed.empty()) return false;

    // Whitelist: schemes that ALWAYS go through a registered browser /
    // mail handler and have their own DNS path. Refusing these would
    // break every clickable web link in every PDF. ShellExecute on
    // these schemes does NOT trigger NetBIOS / LLMNR / mDNS broadcasts
    // in PDFE's process — the registered handler's HTTP stack does its
    // own resolution.
    static constexpr std::string_view kBrowserSafeSchemes[] = {
        "http://",   "https://",
        "mailto:",   "tel:",       "sms:",
        "news:",     "nntp://",
        "gopher://",
    };
    if (std::any_of(std::begin(kBrowserSafeSchemes),
                    std::end(kBrowserSafeSchemes),
            [trimmed](auto sch) { return startsWithCi(trimmed, sch); })) {
        return false;
    }

    // Anything else falls through to the strict input-boundary check:
    // UNC, file://-to-UNC, smb/cifs/nfs/webdav/ssh/sftp/ftp/tftp/ldap —
    // and, since SEC-50, a drive letter that resolves remote. A /Launch or
    // /URI target spelled `Z:\evil.exe` is exactly the reproduction the gap
    // was filed with, so it must go through the resolving check, not the
    // lexical one.
    return is_unc_or_remote(trimmed);
}

bool is_lexically_unc_or_remote(std::wstring_view path) noexcept {
    std::string narrow;
    narrow.resize(path.size());
    std::transform(path.begin(), path.end(), narrow.begin(),
        [](wchar_t wc) {
            return static_cast<char>(wc < 0x80 ? wc : '?');
        });
    return checkUtf8(narrow);
}

bool is_unc_or_remote(std::wstring_view path) noexcept {
    // Narrow the wide string to ASCII for the lexical check. UNC and
    // URI scheme tokens are pure ASCII, so a lossy narrow is fine — we
    // never compare on non-ASCII bytes. Use a manual conversion to
    // avoid std::wstring_convert (deprecated in C++17).
    std::string narrow;
    narrow.resize(path.size());
    std::transform(path.begin(), path.end(), narrow.begin(),
        [](wchar_t wc) {
            return static_cast<char>(wc < 0x80 ? wc : '?');
        });
    // Narrowing is lossy above U+007F, but a drive letter and every UNC /
    // scheme token is ASCII, so the resolving check sees exactly what the
    // wide string said where it matters. Delegating (rather than repeating
    // the two-step here) is what stops the two overloads from drifting —
    // SEC-50 existed because one gate was weaker than it looked.
    return is_unc_or_remote(std::string_view{narrow});
}

// ---------------------------------------------------------------------------
// GitHub #240 — same-share scoping for the opt-in resource setting.
// ---------------------------------------------------------------------------

namespace {

/// Lowercase, forward-to-backslash normalised copy.
std::wstring normaliseSeparators(std::wstring_view in) {
    std::wstring out(in);
    for (wchar_t& c : out) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(::towlower(c));
    }
    return out;
}

/// Strip an extended-length or UNC-extended prefix, leaving a plain UNC or
/// drive path. `\\?\UNC\server\share` -> `\\server\share`; `\\?\C:` -> `c:`.
///
/// The same spellings `is_unc_or_remote` already closes: a prefix left in place
/// here would make two spellings of one share compare unequal, which fails in
/// the SAFE direction (access refused) but would be a confusing bug.
std::wstring stripExtendedPrefix(std::wstring s) {
    if (s.rfind(L"\\\\?\\unc\\", 0) == 0 || s.rfind(L"\\\\.\\unc\\", 0) == 0) {
        return L"\\\\" + s.substr(8);
    }
    if (s.rfind(L"\\\\?\\", 0) == 0 || s.rfind(L"\\\\.\\", 0) == 0) {
        return s.substr(4);
    }
    return s;
}

/// `\\server\share` from a normalised UNC path, or empty when there is no
/// share component.
///
/// RESOLVES DOT SEGMENTS FIRST, and that is the whole security of this function.
///
/// Without it, `\\server\share\..\attackershare\payload.pdf` yields the root
/// `\\server\share` — the same answer as `\\server\share\sub\doc.pdf` — because
/// the first two components are read positionally and the `..` is never
/// applied. Windows resolves that path to `\\server\attackershare`, a DIFFERENT
/// share. So a document on one share could name a target on any other share on
/// the same host and satisfy a same-share check. That is a bypass of the exact
/// guarantee the GitHub #240 opt-in exists to provide, and it is reachable from
/// PDF content through a link target.
///
/// Found by a gate review that reimplemented this algorithm independently and
/// tried the case, not by a test — which is why one exists now.
std::wstring uncShareRoot(const std::wstring& s) {
    if (s.rfind(L"\\\\", 0) != 0) return {};
    const size_t hostStart = 2;
    const size_t hostEnd = s.find(L'\\', hostStart);
    if (hostEnd == std::wstring::npos || hostEnd == hostStart) return {};
    const std::wstring host = s.substr(hostStart, hostEnd - hostStart);

    // Split everything after the host and apply `.` / `..` lexically. Lexical
    // is the right kind of resolution here: it needs no I/O (this runs on
    // attacker-supplied strings, on the GUI thread) and it matches how the
    // Win32 path parser treats these segments.
    std::vector<std::wstring> parts;
    size_t i = hostEnd + 1;
    while (i <= s.size()) {
        const size_t next = s.find(L'\\', i);
        const size_t end = (next == std::wstring::npos) ? s.size() : next;
        const std::wstring seg = s.substr(i, end - i);
        if (seg == L"..") {
            // Escaping past the share is what the attack does. Popping is
            // correct: what remains names whatever the path really reaches, and
            // the comparison then sees a different share.
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != L".") {
            parts.push_back(seg);
        }
        if (next == std::wstring::npos) break;
        i = next + 1;
    }

    if (parts.empty()) return {};   // "\\server", "\\server\", "\\server\.."
    return L"\\\\" + host + L"\\" + parts.front();
}

}  // namespace

std::wstring network_share_root(std::wstring_view path) noexcept {
    if (path.empty()) return {};
    std::wstring s = stripExtendedPrefix(normaliseSeparators(path));

    // Already a UNC.
    if (std::wstring root = uncShareRoot(s); !root.empty()) return root;

#ifdef _WIN32
    // A mapped or subst-ed drive letter: resolve it to its UNC target so `Z:\x`
    // and `\\server\share\x` are recognised as the same share. Without this the
    // setting would be bypassable in the inconvenient direction — a user whose
    // share is mapped to a letter would find the feature simply not work.
    if (s.size() >= 2 && s[1] == L':') {
        const wchar_t name[3] = {static_cast<wchar_t>(::towupper(s[0])), L':', L'\0'};
        wchar_t remote[MAX_PATH + 1] = {};
        DWORD len = MAX_PATH;
        if (::WNetGetConnectionW(name, remote, &len) == NO_ERROR) {
            return uncShareRoot(normaliseSeparators(remote));
        }
        wchar_t target[1024] = {};
        if (::QueryDosDeviceW(name, target, 1024) != 0) {
            // `\??\UNC\server\share` is how a subst onto a UNC appears.
            std::wstring t = normaliseSeparators(target);
            const size_t uncAt = t.find(L"\\unc\\");
            if (uncAt != std::wstring::npos) {
                return uncShareRoot(L"\\\\" + t.substr(uncAt + 5));
            }
        }
    }
#endif
    return {};
}

bool same_network_share(std::wstring_view documentPath,
                       std::wstring_view resourcePath) noexcept {
    const std::wstring a = network_share_root(documentPath);
    if (a.empty()) return false;   // document is not on a share
    const std::wstring b = network_share_root(resourcePath);
    if (b.empty()) return false;   // resource is not on a share
    return a == b;
}

bool same_network_share(std::string_view documentPath,
                       std::string_view resourcePath) noexcept {
    const auto widen = [](std::string_view in) {
        std::wstring out;
        out.resize(in.size());
        std::transform(in.begin(), in.end(), out.begin(), [](char c) {
            return static_cast<wchar_t>(static_cast<unsigned char>(c));
        });
        return out;
    };
    // Share and host names are compared as ASCII here for the same reason
    // is_unc_or_remote narrows: the tokens that decide the answer are ASCII.
    return same_network_share(std::wstring_view{widen(documentPath)},
                              std::wstring_view{widen(resourcePath)});
}

} // namespace pdfe::core::path_security
