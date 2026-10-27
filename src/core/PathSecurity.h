#pragma once

#include <string>
#include <string_view>

namespace pdfe::core::path_security {

/// Returns true if `path` resolves to a remote / network location that
/// PDFE refuses to touch at its input boundary. Concretely:
///
///   * Windows UNC: `\\server\share\…`, `\\?\UNC\server\share\…`,
///     `//server/share/…`, mixed-slash `\/server\share\…`
///   * file:// URIs that resolve to UNC: `file://server/share/…`,
///     `file:////server/share/…`, `file:\\\\server\share\…`
///   * Non-file URI schemes that imply network fetch:
///     `http(s)://`, `ftp://`, `smb://`, `nfs://`, `webdav(s)://`, `dav://`,
///     `cifs://`, `\\?\Volume{…}` device-namespace UNCs are still UNC.
///
/// Plain local paths (`C:\foo`, `/tmp/foo`, relative `./foo`) and
/// `file:///` URIs that resolve to a local drive (`file:///C:/foo`,
/// `file:///tmp/foo`) return false.
///
/// …AND (SEC-50, added phase-8) a Windows drive letter that currently
/// RESOLVES to a remote target, which no amount of looking at the string
/// can reveal:
///
///   * `net use Z: \\server\share` — `Z:\x.pdf` is byte-for-byte a local
///     path and reaches the same kernel redirector as `\\server\share\x.pdf`.
///   * `subst Z: \\server\share` — same, via the DOS device table.
///
/// This is why the check is NOT purely lexical any more. It was, and the
/// gate was bypassable by one `net use`: `Z:\evil.exe` failed the
/// `isSep(s[0]) && isSep(s[1])` test and was classified LOCAL on both the
/// /Launch refusal and the Document::open boundary — the exact
/// NetBIOS/LLMNR leak the gate exists to stop.
///
/// **The resolving behaviour is deliberately attached to THIS name**, the
/// one every call site already uses, rather than being offered as a
/// separate stronger function. A stronger sibling would have to be adopted
/// site by site, and this project's recurring failure is the site that
/// never gets migrated. Opting DOWN to the weak check requires typing
/// `is_lexically_unc_or_remote`, which says what it is.
///
/// Cost: one `GetDriveTypeW` — and only for a path that actually begins
/// `X:` — plus `WNetGetConnectionW`/`QueryDosDeviceW` only when that first
/// call is inconclusive. Deliberately NOT cached: a drive mapped mid-session
/// would leave a cached "local" verdict standing, and a stale verdict here
/// fails open. On non-Windows the resolution step compiles out and the
/// result is the lexical answer.
///
/// Pass either a UTF-8 std::string_view or a wide std::wstring_view;
/// both overloads canonicalize to the same check. To check a
/// std::filesystem::path, call `.string()` (or `.wstring()`) on it
/// first — there is intentionally no path overload because string
/// literals would be ambiguous between string_view and path.
bool is_unc_or_remote(std::string_view path) noexcept;
bool is_unc_or_remote(std::wstring_view path) noexcept;

/// The pre-SEC-50 behaviour: the lexical check ALONE, touching neither the
/// filesystem nor the DOS device table.
///
/// This exists so the lexical layer stays directly testable, and so a
/// caller that genuinely must not perform I/O (a hot loop over untrusted
/// strings, a check running before the drive table is meaningful) can say
/// so explicitly. It is NOT the general-purpose input-boundary gate —
/// `is_unc_or_remote` is. A mapped or `subst`-ed drive letter passes this
/// one.
bool is_lexically_unc_or_remote(std::string_view path) noexcept;
bool is_lexically_unc_or_remote(std::wstring_view path) noexcept;

/// True if the DOS drive `driveLetter` ('A'..'Z', case-insensitive)
/// currently resolves to a remote/network target.
///
/// Three probes, in increasing cost, first hit wins:
///   1. `GetDriveTypeW` == `DRIVE_REMOTE` — catches `net use`.
///   2. `WNetGetConnectionW` succeeds — catches a redirected drive whose
///      type query is inconclusive.
///   3. `QueryDosDeviceW` target begins `\??\UNC\`, `\Device\Mup\` or
///      `\Device\LanmanRedirector\` — catches `subst Z: \\server\share`,
///      which is not a network connection at all and so is invisible to
///      both calls above. One `\??\X:` indirection hop is followed, with a
///      small bound, so a `subst` chain cannot loop.
///
/// Returns false for a letter outside 'A'..'Z', for a drive that does not
/// exist, and always on non-Windows. Note the asymmetry: an unknown or
/// unreadable drive answers "not remote", so this function widens the
/// refusal set and never narrows it.
bool drive_letter_resolves_to_remote(char driveLetter) noexcept;

/// Narrower variant for /URI link follow-ups in PDFs.
///
/// Three-tier policy (NOT a closed allowlist — important):
///   1. Browser-safe schemes (`http://`, `https://`, `mailto:`, `tel:`,
///      `sms:`, `news:`, `nntp:`, `gopher:`) → PASSED THROUGH as safe.
///      These go to the user's registered handler, which has its own
///      DNS path and sandbox. Blocking them would be a user-visible
///      regression (every clickable web link in every PDF would fail).
///   2. Known-bad schemes (UNC, file://-to-UNC, smb/cifs/nfs/
///      webdav(s)/dav(s)/ssh/sftp/ftp(s)/tftp/ldap(s)) → REFUSED.
///      These reach the Win32 kernel resolver via ShellExecute and
///      trigger NetBIOS/LLMNR/mDNS broadcasts before any registered
///      handler can sandbox them. This is the phase-3 leak vector.
///   3. Unknown schemes (`data:`, `javascript:`, custom app schemes
///      like `slack:`, `zoommtg:`) → PASSED THROUGH to the OS shell.
///      The OS handles these via the registered-protocol-handler
///      mechanism; if no handler is registered, the OS shell rejects
///      them. None of these trigger the kernel resolver in PDFE's own
///      process, so they are not the threat this function targets.
///
/// Use this from /URI action handlers; use is_unc_or_remote() at the
/// Document::open boundary (where http/https doesn't make sense as a
/// PDF input anyway).
bool is_unsafe_link_target(std::string_view uri) noexcept;

/// GitHub #240 — do these two paths live on the SAME network share?
///
/// The scoping for the opt-in setting that lets a document opened FROM a share
/// reach other resources ON THAT SHARE. Same share, not "any network location":
/// a document on `\\finance\reports` getting at `\\finance\reports\logo.png` is
/// the case users need; the same document reaching `\\dc01\sysvol` or
/// `\\attacker\payload` is not, and is the reason this is a comparison rather
/// than a boolean.
///
/// Compares the `\\server\share` prefix only — case-insensitively, since
/// Windows share and host names are, and after resolving a mapped or `subst`-ed
/// drive letter to its UNC target so `Z:\x` and `\\server\share\x` are seen as
/// the same share when `Z:` maps there. A drive that cannot be resolved answers
/// false, so an unresolvable input narrows access rather than widening it.
///
/// Returns false when EITHER path is not on a network share, including when
/// both are local — the caller's question is "may this network document reach
/// this network resource", and a local pair is not that question. Also false
/// for a UNC naming only a host (`\\server`) with no share component: there is
/// no share to be the same as.
bool same_network_share(std::wstring_view documentPath,
                        std::wstring_view resourcePath) noexcept;
bool same_network_share(std::string_view documentPath,
                        std::string_view resourcePath) noexcept;

/// The `\\server\share` prefix of @p path, lowercased, or empty when @p path is
/// not on a network share (or names a host with no share).
///
/// Exposed because `same_network_share` is an equality test over this, and a
/// test that can only observe the comparison cannot tell a correct answer from
/// two paths that both resolved to nothing.
std::wstring network_share_root(std::wstring_view path) noexcept;

} // namespace pdfe::core::path_security
