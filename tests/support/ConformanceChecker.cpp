#include "support/ConformanceChecker.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <vector>

#ifndef PDFE_REPO_ROOT
#  define PDFE_REPO_ROOT "."
#endif

namespace fs = std::filesystem;

namespace pdfe::test {
// Replace host-rooted absolute paths with stable placeholders.
//
// WHY THIS EXISTS. `flushConformanceRecord()` writes this record into a file
// that is TRACKED IN GIT (docs/audit/.../phase-14/conformance-checker-record.json).
// Several of the values it serialises are absolute paths on whichever machine
// ran the suite: the veraPDF jar, the "how resolved" prose, every entry in
// `searched`, and veraPDF's own stdout, which names the input file and %TEMP%.
// Without this, running the suite rewrites a tracked file with the operator's
// real home directory — which is precisely the string the 2026-08-22 history
// rewrite existed to remove from this repository, and it reappears on every run.
//
// The F48 sweep fixed five such emission points, but all of them were in the
// PYTHON UI runner (`run_ui_suite.py`, plus `tools/gate/scrub_artifacts.py` for
// CI-produced artifacts). Neither covers this gtest-side producer, which is why
// it survived both that sweep and the scrub.
//
// Longest-prefix first: TEMP often sits under the home directory, so replacing
// home first would leave a half-substituted path that no longer matches TEMP.
std::string portablePath(std::string s) {
    const auto sub = [&s](std::string needle, const char* token) {
        if (needle.empty()) return;
        while (!needle.empty() && (needle.back() == '\\' || needle.back() == '/'))
            needle.pop_back();
        if (needle.empty()) return;
        // THREE renderings of the same path have to be matched, not two:
        //   C:/Users/x/PDFE     std::filesystem's preferred form
        //   C:\Users\x\PDFE     what the tools we shell out to emit
        //   C:\\Users\\x\\PDFE  the JSON-ESCAPED form
        // The third is not hypothetical. `mergedCheckObjects()` reads sidecar
        // files that already contain serialised JSON, so every backslash in
        // them is doubled. A matcher that only knows the first two silently
        // passes those straight through -- which is what happened on the first
        // attempt at this fix: `jar_path` and `how_resolved` came out clean
        // while every `output` field still carried the account name.
        std::vector<std::string> variants;
        for (const char sep : {'\\', '/'}) {
            std::string v = needle;
            for (char& c : v)
                if (c == '\\' || c == '/') c = sep;
            variants.push_back(v);
        }
        std::string escaped;
        for (char c : needle) {
            if (c == '\\' || c == '/') escaped += "\\\\";
            else escaped += c;
        }
        variants.push_back(escaped);

        // CASE-INSENSITIVE, because Windows paths are. The same directory is
        // rendered "C:\Users\..." by one tool and "c:\users\..." by another,
        // and a case-sensitive matcher scrubs the first while passing the
        // second straight into a tracked file. The artifact-level guard would
        // catch that, but a leak that only a test failure prevents is weaker
        // than one that cannot be produced.
        const auto ifind = [](const std::string& hay, const std::string& needle,
                              std::size_t from) -> std::size_t {
            if (needle.empty() || needle.size() > hay.size()) return std::string::npos;
            const auto eq = [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a))
                    == std::tolower(static_cast<unsigned char>(b));
            };
            const auto it = std::search(hay.begin() + static_cast<long>(from),
                                        hay.end(), needle.begin(), needle.end(), eq);
            return it == hay.end() ? std::string::npos
                                   : static_cast<std::size_t>(it - hay.begin());
        };

        for (const auto& variant : variants) {
            for (std::size_t at = ifind(s, variant, 0); at != std::string::npos;
                 at = ifind(s, variant, at + 1)) {
                s.replace(at, variant.size(), token);
            }
        }
    };
    const char* tmp  = std::getenv("TEMP");
    const char* tmp2 = std::getenv("TMP");
    const char* home = std::getenv("USERPROFILE");
    if (tmp)  sub(tmp,  "<temp>");
    if (tmp2) sub(tmp2, "<temp>");
    sub(std::string(PDFE_REPO_ROOT), "<repo>");
    if (home) sub(home, "<home>");

    // BACKSTOP: anything still drive-qualified becomes <path>.
    //
    // The three roots above are the ones a tool is EXPECTED to live under. They
    // are not the ones it necessarily does. Measured 2026-10-06: veraPDF is
    // installed at `C:\verapdf` on this host, which is neither repo, home nor
    // temp, so `jar_path` and `how_resolved` were written verbatim and
    // `UiSuitePathHygiene.TheTrackedConformanceRecordIsPortable` failed with
    // "carries 3 absolute path(s)" -- against a file that is TRACKED IN GIT.
    //
    // Enumerating roots cannot be made complete: the next tool installs
    // somewhere else. The portability test asserts there is NO absolute path in
    // this file, so the scrubber has to assert the same thing rather than a
    // list of places it knows about. Same shape as the climbing-relative-path
    // backstop in tests/scripts/run_ui_suite.py, and for the same reason: a
    // path SHAPE can be missed again, so the final pass does not depend on
    // recognising which root it came from.
    //
    // Deliberately AFTER the named roots, so a path under the repo still reads
    // `<repo>/...` -- which is useful -- and only the unrecognised remainder
    // collapses to `<path>`.
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const bool drive = (i + 1 < s.size())
                           && std::isalpha(static_cast<unsigned char>(s[i]))
                           && s[i + 1] == ':'
                           && (i + 2 < s.size() && (s[i + 2] == '\\' || s[i + 2] == '/'));
        // A UNC ROOT IS A PATH TOO, and it was invisible here until claim-check
        // round 6 (C11). `isalpha(s[i]) && s[i+1] == ':'` can never match
        // `\\server\share\...`, so a UNC-rooted JAVA_HOME or veraPDF install root
        // -- both free-form strings this file reads straight out of the
        // environment or the filesystem, not fixtures -- was not recognised as a
        // path at all and passed through COMPLETELY unscrubbed, account name
        // included, into the git-tracked record. Measured:
        //
        //   portablePath(R"(\\fileserver\profiles$\<account>\verapdf\cli.jar)")
        //       -> returned verbatim
        //
        // The header said "every host-specific path"; it meant "every
        // drive-lettered one".
        const bool unc = (i + 2 < s.size())
                         && (s[i] == '\\' || s[i] == '/')
                         && (s[i + 1] == '\\' || s[i + 1] == '/')
                         && s[i + 2] != '\\' && s[i + 2] != '/';
        if (!drive && !unc) {
            out += s[i++];
            continue;
        }
        // Consume the whole path token, stopping at the first character that
        // cannot be part of one.
        //
        // THE STOP SET IS DERIVED, NOT ENUMERATED BY INCIDENT. Two rounds of
        // this fix each added the single character that had just been observed
        // going wrong, which is how the second one was found:
        //
        //   round 1  "...install root <repo>\...\verapdf; java resolved to java"
        //         -> "...install root <path> java resolved to java"          (`;` eaten)
        //   round 2  "Error: C:\verapdf\bin\cli.jar: No such file"
        //         -> "Error: <path> No such file"                            (`:` eaten)
        //
        // Both lose prose from a GIT-TRACKED diagnostic, and `:` is worse than
        // `;` because `checkObjectJson()` scrubs raw captured subprocess output,
        // where "path: message" is the ordinary tool-diagnostic shape. Adding
        // `:` alone would just invite a third round, so the set is now taken
        // from the authority instead: Win32 forbids  < > : " | ? *  in a path
        // COMPONENT, so none of them can be interior to a path token.
        //
        // That derivation was still not the whole answer -- see immediately
        // below, which supersedes it. Do not stop reading here.
        //
        // TWO KINDS OF STOP, AND CONFLATING THEM LEAKS (claim-check round 5, C10).
        //
        // Round 4 put `'` in the stop set while its own comment claimed `'` was
        // deliberately excluded as "legal in a filename". The code won; nothing
        // tested it, because the table-driven test omitted `'` on the strength of
        // that comment. Measured on the unmodified tree:
        //
        //   portablePath(R"(C:\v\O'Neill\cli.jar)") -> "<path>'Brien\cli.jar"
        //
        // `O'Neill` is an ordinary legal Windows directory and account name. The
        // token halted mid-component and STRANDED THE REST OF THE PATH verbatim.
        //
        // That also refuted round 4's stated reasoning -- "stopping early cannot
        // leak, the account name lives in the part already consumed". True only
        // when the stop character comes AFTER the path. A stop character INSIDE a
        // component strands the remainder, and `;` and `,` are legal in Windows
        // filenames too, so they had the same flaw.
        //
        // So the set is split by what the character actually is:
        //
        //  FORBIDDEN  < > : " | ? *   Win32 rejects these in a path COMPONENT, so
        //                             they can never be interior to one. They end
        //                             the token unconditionally.
        //  WHITESPACE                 ends a token in any prose.
        //  DELIMITER  ; ,             LEGAL in a filename, so these end the token
        //                             only when ACTING as punctuation: at end of
        //                             text, before whitespace, or before another
        //                             drive-qualified path (a PATH-style list).
        //                             Inside a name -- "a,b.txt" -- they are just
        //                             characters and must not split it.
        //
        // NOT stop characters at all, because all are legal in a path:
        //   ' ( ) &   -- e.g. "C:\v\O'Neill\...", "C:\v\(x86)\...", "C:\a&b\..."
        //
        // A SPACE STILL STRANDS THE TAIL, and that is a documented limit, not an
        // oversight: "C:\Program Files\x" and "C:\tmp\a and then some prose" are
        // indistinguishable without quoting, so consuming past a space would eat
        // sentences. The limit is bounded, and a test pins the bound: a stranded
        // tail is never drive-qualified, and `<home>`/`<repo>`/`<temp>` are
        // substituted BEFORE this loop runs, so a path under the operator's
        // profile loses the ACCOUNT NAME wholesale before any space is reached.
        //
        // SCANNING STARTS AT i + 2, PAST THE "X:" THE DRIVE CHECK JUST MATCHED,
        // which is load-bearing now that `:` is forbidden: starting at i would
        // halt on the drive's own colon, emit `<path>`, and leave `:\verapdf\...`
        // as literal text. There is a test for exactly that.
        const auto isSpace = [](char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        };
        const auto isForbidden = [](char c) {
            switch (c) {
                case '<': case '>': case ':': case '"':
                case '|': case '?': case '*':
                    return true;
                default:
                    return false;
            }
        };
        const auto startsDrive = [&s](std::size_t k) {
            return k + 2 < s.size()
                   && std::isalpha(static_cast<unsigned char>(s[k]))
                   && s[k + 1] == ':'
                   && (s[k + 2] == '\\' || s[k + 2] == '/');
        };
        const auto delimits = [&](std::size_t k) {
            if (s[k] != ';' && s[k] != ',') return false;
            if (k + 1 >= s.size()) return true;
            return isSpace(s[k + 1]) || startsDrive(k + 1);
        };
        // Past the two characters the root check just matched -- "X:" for a drive,
        // "\\" for a UNC root. Skipping them is load-bearing for the drive case,
        // where `:` is forbidden and scanning from `i` would halt on the drive's
        // own colon; for UNC it simply avoids re-reading the separators.
        // GitHub #283. A SPACE NO LONGER UNCONDITIONALLY STRANDS THE TAIL.
        //
        // The accepted limit was that `C:\Program Files (x86)\v\cli.jar` came
        // out as `<path> Files (x86)\v\cli.jar` -- the token halted at the
        // first space and the remainder, including further path components,
        // was emitted verbatim. The reasoning for accepting it was sound as
        // far as it went: `C:\tmp\a and then some prose` and
        // `C:\Program Files\x` are indistinguishable without quoting, so
        // consuming past every space would eat sentences.
        //
        // But "consume past every space" is not the only alternative to
        // "stop at the first one". A space is INTERIOR to the path when a
        // later part of the same run still carries a path SEPARATOR -- which
        // is what distinguishes `Program Files (x86)\v\cli.jar` from
        // `and then some prose`. So the bound moves from "first space" to
        // "first space not followed by more of this path", which is strictly
        // narrower and keeps prose intact.
        //
        // THREE GUARDS KEEP IT NARROW, each there for a case that would
        // otherwise eat text:
        //
        //   1. the lookahead stops at a FORBIDDEN character, so
        //      `C:\a b: message` does not swallow the diagnostic;
        //   2. it stops at a token that starts a NEW drive-qualified path, so
        //      a PATH-style list `C:\a b and C:\c\d` does not merge two
        //      entries into one;
        //   3. it spans at most kMaxInteriorSpaces spaces. `Program Files
        //      (x86)` needs two; a sentence that happens to mention a
        //      backslash eight words later is prose, and a bound that admits
        //      it is not a bound. The limit is deliberately small and stated,
        //      not tuned to a corpus.
        //
        // What is still NOT fixed, and is now the residual: a sentence with a
        // separator-bearing word within the span, such as
        // `C:\tmp\a and the file b\c`, consumes the prose. That is a narrower
        // hole than the one it replaces and it cannot leak the account name --
        // `<home>`/`<repo>`/`<temp>` are substituted BEFORE this loop, so a
        // path under the operator's profile has already lost the name
        // wholesale. `ConformancePathScrub` pins both directions.
        constexpr int kMaxInteriorSpaces = 2;
        const auto spaceIsInterior = [&](std::size_t at) {
            int spaces = 0;
            for (std::size_t k = at; k < s.size(); ++k) {
                if (isForbidden(s[k])) return false;      // guard 1
                // GUARD 0 -- A NEWLINE ENDS THE TOKEN UNCONDITIONALLY, and
                // this was a real regression, not a precaution.
                //
                // `isSpace` covers '\n' and '\r' as well as ' ' and '\t', so
                // the first version of this lookahead happily crossed a LINE
                // BREAK looking for a separator. Measured on the git-tracked
                // conformance record, which this scrubber writes:
                //
                //   before  "FAIL <temp><path> 1b\n  FAIL 6.7.3-1"
                //   after   "FAIL <temp><path>  FAIL 6.7.3-1"
                //
                // The flavour token AND the line break were swallowed, because
                // a separator appeared further down the multi-line diagnostic.
                // The PASS entries -- single-line, nothing after them -- kept
                // their " 1b", which is why the regression was visible only in
                // the FAIL rows and only in a tracked file.
                //
                // A path never spans lines, so crossing one cannot be
                // continuing a path; it can only be eating prose. Neither the
                // phase's own tests nor a gate's newline probe caught this.
                // The tracked artifact did.
                if (s[k] == '\n' || s[k] == '\r') return false;
                // ...AND AN ESCAPED ONE. This is the case that actually did
                // the damage, and GUARD 0 above does not cover it.
                //
                // `mergedCheckObjects()` reads sidecars that already contain
                // SERIALISED JSON, so a line break in them is the two
                // characters backslash-n, not 0x0A. A real path separator in
                // that text appears DOUBLED (`\\`). So the lookahead, hunting
                // for a separator, found the backslash of `\n` and concluded
                // the space before " 1b" was interior to a path -- eating the
                // flavour token and the line break:
                //
                //   sidecar  ...claim_check_1b.pdf 1b\n  FAIL 6.7.3-1
                //   record   ...<path>  FAIL 6.7.3-1
                //
                // A single backslash followed by n, r or t is an ESCAPE, not a
                // separator, and the same reasoning as GUARD 0 applies: a path
                // does not span a line break, literal or escaped.
                if (s[k] == '\\' && k + 1 < s.size()
                    && (s[k + 1] == 'n' || s[k + 1] == 'r' || s[k + 1] == 't')) {
                    return false;
                }
                if (isSpace(s[k])) {
                    if (++spaces > kMaxInteriorSpaces) return false;  // guard 3
                    // A new drive-qualified path begins the next token.
                    if (startsDrive(k + 1)) return false;            // guard 2
                    continue;
                }
                if (s[k] == '\\' || s[k] == '/') return true;
            }
            return false;
        };

        std::size_t j = i + 2;
        while (j < s.size() && !isForbidden(s[j]) && !delimits(j)) {
            if (isSpace(s[j]) && !spaceIsInterior(j)) break;
            ++j;
        }
        // Never leave a trailing space inside the consumed token: a path that
        // ends at the span limit would otherwise absorb the separator between
        // it and the following prose.
        while (j > i + 2 && isSpace(s[j - 1])) --j;
        out += "<path>";
        i = j;
    }

    // FINAL PASS: THE ACCOUNT NAME ITSELF, WHATEVER SHAPE IT ARRIVED IN.
    //
    // Everything above reasons about PATHS. claim-check round 6 (C11) showed that
    // the thing which must never reach a tracked file -- the operator's account
    // name -- can survive every one of those passes, because the passes depend on
    // recognising the path AROUND it:
    //
    //   D:\My Documents\<account>\jdk\bin\java.exe
    //       -> "<path> Documents\<account>\jdk\bin\java.exe"
    //
    // Not under <home>, <repo> or <temp>, so no named root matched; drive-rooted,
    // so the backstop fired; and the space inside "My Documents" ended the token
    // early, stranding the account name verbatim in the tail. The comment above
    // claimed the account name "is gone before any space is reached", which was
    // true only for paths literally prefixed by %USERPROFILE%.
    //
    // So this pass does not reason about paths at all. It removes the account
    // NAME, wherever it appears and whatever surrounds it -- the same blunt
    // backstop, for the same reason, as the one in
    // tests/scripts/run_ui_suite.py: a SHAPE can always be missed again, so the
    // last line of defence must not depend on recognising one.
    //
    // Bounded deliberately: only the basename of USERPROFILE, only when it is at
    // least 3 characters, so a pathologically short or empty account name cannot
    // turn this into a mass rewrite of ordinary words.
    if (home != nullptr && *home != '\0') {
        std::string account(home);
        while (!account.empty()
               && (account.back() == '\\' || account.back() == '/')) {
            account.pop_back();
        }
        const auto cut = account.find_last_of("\\/");
        if (cut != std::string::npos) account = account.substr(cut + 1);

        if (account.size() >= 3) {
            const auto ieq = [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a))
                    == std::tolower(static_cast<unsigned char>(b));
            };
            // WHOLE TOKEN ONLY. A bare substring replace corrupts ordinary text,
            // and the `>= 3` bound does not save it -- karen demonstrated both:
            //
            //   account "ask"    : "the task basket" -> "the t<user> b<user>et"
            //   account "acct" : "schacctl"      -> "sch<user>l"
            //
            // The negative control written for this pass only exercised the name
            // flanked by SPACES, so it passed throughout. Requiring a
            // non-alphanumeric character (or a string edge) on both sides keeps
            // every case this pass exists for -- in a path the name is flanked by
            // `\` or `/`, in prose by spaces -- while leaving a longer word that
            // merely contains it alone.
            const auto isWordChar = [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0;
            };
            std::string scrubbed;
            scrubbed.reserve(out.size());
            std::size_t k = 0;
            while (k < out.size()) {
                const bool fits = k + account.size() <= out.size();
                const bool matches =
                    fits && std::equal(account.begin(), account.end(),
                                       out.begin() + static_cast<long>(k), ieq);
                const bool leftEdge  = (k == 0) || !isWordChar(out[k - 1]);
                const bool rightEdge = !fits
                    || (k + account.size() == out.size())
                    || !isWordChar(out[k + account.size()]);
                if (matches && leftEdge && rightEdge) {
                    scrubbed += "<user>";
                    k += account.size();
                } else {
                    scrubbed += out[k++];
                }
            }
            out.swap(scrubbed);
        }
    }

    return out;
}

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::string trimmed(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}


std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    return out;
}

/// Run @p command, capture stdout+stderr into @p captured, return the exit code.
///
/// On Windows std::system hands the string to `cmd /c`, which strips the outer
/// quote pair when the string begins with a quote — so a command whose FIRST
/// token is a quoted absolute path (which every path under "C:/Program Files"
/// must be) needs an extra enclosing pair. That is what the wrapper below adds.
int runCaptured(const std::string& command, std::string& captured) {
    fs::path outFile = fs::temp_directory_path()
        / ("pdfe_conformance_" + std::to_string(
               std::hash<std::string>{}(command)) + ".out");
    std::error_code ec;
    fs::remove(outFile, ec);

    std::string full = command + " > \"" + outFile.string() + "\" 2>&1";
#ifdef _WIN32
    full = "\"" + full + "\"";
#endif
    const int rc = std::system(full.c_str());

    captured.clear();
    {
        std::ifstream in(outFile, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            captured = trimmed(ss.str());
        }
    }
    fs::remove(outFile, ec);
    return rc;
}

// NOTE: deliberately NOT named `quoted` — ADL on a std::string argument
// would pull in std::quoted (from <iomanip>, included transitively), whose
// stream-manipulator proxy has no operator+ and produces a baffling error.
std::string quotedArg(const std::string& s) { return "\"" + s + "\""; }

/// First existing `cli-*.jar` (current veraPDF layout) or `verapdf.jar`
/// (legacy greenfield layout) under @p installDir. Empty when none.
std::string findVeraPdfJarIn(const fs::path& installDir) {
    std::error_code ec;
    const fs::path candidates[] = {
        installDir / "bin",
        installDir / "greenfield",
        installDir,
    };
    for (const auto& dir : candidates) {
        if (!fs::is_directory(dir, ec)) continue;
        // Deterministic order: sort the directory listing before picking, so
        // two jars never resolve differently between runs or machines.
        std::vector<std::string> hits;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            const std::string name = e.path().filename().string();
            const bool isCli = name.rfind("cli-", 0) == 0
                && name.size() > 4
                && name.substr(name.size() - 4) == ".jar";
            const bool isLegacy = (name == "verapdf.jar");
            if (isCli || isLegacy) hits.push_back(e.path().string());
        }
        if (!hits.empty()) {
            std::sort(hits.begin(), hits.end());
            return hits.back();   // highest version wins, deterministically
        }
    }
    return {};
}

std::string envOr(const char* name, const std::string& fallback) {
    if (const char* v = std::getenv(name)) {
        if (*v) return v;
    }
    return fallback;
}

/// Ordered list of conventional veraPDF install roots. NO checker-specific
/// variable is consulted — only the platform's own home/program directories.
std::vector<fs::path> veraPdfInstallRoots() {
    std::vector<fs::path> roots;
    // 1. The provisioned location this repository documents and
    //    tools/conformance/fetch-verapdf.py populates.
    roots.emplace_back(fs::path(PDFE_REPO_ROOT) / "tools" / "conformance" / "verapdf");
#ifdef _WIN32
    roots.emplace_back("C:/verapdf");
    roots.emplace_back("C:/Program Files/verapdf");
    roots.emplace_back("C:/Program Files (x86)/verapdf");
    const std::string profile = envOr("USERPROFILE", "");
    if (!profile.empty()) roots.emplace_back(fs::path(profile) / "verapdf");
#else
    roots.emplace_back("/opt/verapdf");
    roots.emplace_back("/usr/local/verapdf");
    roots.emplace_back("/usr/share/verapdf");
    const std::string home = envOr("HOME", "");
    if (!home.empty()) roots.emplace_back(fs::path(home) / "verapdf");
#endif
    return roots;
}

/// Ordered list of java candidates: PATH first, then JAVA_HOME (a standard
/// platform variable, not a checker-specific opt-in), then conventional JDK
/// install directories. Returns the first that answers `-version`.
std::string resolveJava(std::vector<std::string>& searched) {
    std::vector<std::string> candidates;
    candidates.emplace_back("java");                       // PATH
    const std::string javaHome = envOr("JAVA_HOME", "");
    if (!javaHome.empty()) {
#ifdef _WIN32
        candidates.emplace_back((fs::path(javaHome) / "bin" / "java.exe").string());
#else
        candidates.emplace_back((fs::path(javaHome) / "bin" / "java").string());
#endif
    }
#ifdef _WIN32
    // Conventional JDK roots, newest first (directory listing sorted, reversed).
    std::error_code ec;
    for (const char* root : {"C:/Program Files/Java",
                             "C:/Program Files/Eclipse Adoptium",
                             "C:/Program Files/Microsoft/jdk"}) {
        if (!fs::is_directory(root, ec)) continue;
        std::vector<std::string> jdks;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (ec) break;
            const fs::path exe = e.path() / "bin" / "java.exe";
            if (fs::exists(exe, ec)) jdks.push_back(exe.string());
        }
        std::sort(jdks.begin(), jdks.end());
        for (auto it = jdks.rbegin(); it != jdks.rend(); ++it) {
            candidates.push_back(*it);
        }
    }
#endif
    for (const auto& c : candidates) {
        searched.push_back("java: " + c);
        std::string out;
        const int rc = runCaptured(quotedArg(c) + " -version", out);
        if (rc == 0 && !out.empty()) return c;
    }
    return {};
}

CheckerResolution computeResolution() {
    CheckerResolution r;

    std::string jar;
    std::string matchedRoot;
    for (const auto& root : veraPdfInstallRoots()) {
        r.searched.push_back("verapdf root: " + root.string());
        jar = findVeraPdfJarIn(root);
        if (!jar.empty()) { matchedRoot = root.string(); break; }
    }

    const std::string java = resolveJava(r.searched);

    if (jar.empty()) {
        r.available = false;
        r.reason =
            "No veraPDF CLI jar found in the provisioned location "
            "(<repo>/tools/conformance/verapdf) or in any conventional system "
            "install root; every location searched is listed in `searched`.";
        r.prerequisite =
            "Run `python tools/conformance/fetch-verapdf.py` (downloads the "
            "veraPDF greenfield installer from software.verapdf.org and "
            "headless-installs it into <repo>/tools/conformance/verapdf), or "
            "install veraPDF to one of the conventional roots listed in "
            "`searched`. Requires a JRE/JDK 11+ and outbound HTTPS.";
        if (java.empty()) {
            r.prerequisite += " A Java runtime is ALSO absent and would be "
                              "needed to execute the checker.";
        }
        return r;
    }
    if (java.empty()) {
        r.available = false;
        r.jarPath = jar;
        r.reason = "veraPDF jar found at " + jar
                 + " but no working Java runtime could be located "
                   "(PATH, JAVA_HOME, and the conventional JDK roots were all "
                   "tried; see `searched`).";
        r.prerequisite = "Install a JRE/JDK 11+ and put `java` on PATH or set "
                         "JAVA_HOME.";
        return r;
    }

    r.available = true;
    r.jarPath = jar;
    r.javaCommand = java;
    r.howResolved = "veraPDF CLI jar under install root " + matchedRoot
                  + "; java resolved to " + java;

    std::string banner;
    if (runCaptured(quotedArg(java) + " -jar " + quotedArg(jar) + " --version",
                    banner) == 0) {
        // First line is "veraPDF <version>".
        r.version = trimmed(banner.substr(0, banner.find('\n')));
    }
    return r;
}

// ---------------------------------------------------------------------------
// Record accumulation
// ---------------------------------------------------------------------------

std::mutex& recordMutex() {
    static std::mutex m;
    return m;
}

std::vector<CheckerVerdict>& recordedVerdicts() {
    static std::vector<CheckerVerdict> v;
    return v;
}

std::string nowIso8601() {
    const std::time_t t = std::time(nullptr);
    char buf[32] = {0};
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

// Defined below (next to the sidecar writer it pairs with).
std::vector<std::string> mergedCheckObjects();

std::string buildRecordJson() {
    const CheckerResolution& res = resolveConformanceChecker();
    std::ostringstream o;
    o << "{\n";
    // SCHEMA 2, not 1. Version bumped because the MEANING of `output` changed:
    // in schema 1 it was veraPDF's stdout quoted verbatim; in schema 2 it is
    // that stdout with host paths normalised. A consumer that diffed `output`
    // against a live veraPDF run would silently stop matching, so this is a
    // semantic change and not a cosmetic one, even though no machine consumer
    // reads the field today (checked: only documents reference this file).
    o << "  \"schema\": \"pdfe.conformance-checker-record/2\",\n";
    o << "  \"phase\": \"phase-14\",\n";
    o << "  \"gap_ids\": [\"PDF-10\", \"PDF-17\"],\n";
    o << "  \"generated\": \"" << nowIso8601() << "\",\n";
    o << "  \"generated_by\": \"tests/support/ConformanceChecker.cpp "
         "(pdfe_tests)\",\n";
    // F73's closure criterion: the choice between (a) scrub and say so, (b)
    // keep `output` verbatim and relativise only our own structured fields, and
    // (c) stop committing the record, must be made EXPLICITLY and written into
    // the schema — not decided by accident inside an unrelated phase. It is
    // recorded here, in the artifact itself, so a reader of the JSON learns it
    // without having to find a register row.
    o << "  \"path_normalisation\": {\n";
    o << "    \"choice\": \"(a) scrub every field, including `output`\",\n";
    o << "    \"placeholders\": [\"<repo>\", \"<home>\", \"<temp>\"],\n";
    o << "    \"applies_to\": [\"jar_path\", \"how_resolved\", "
         "\"reason_unavailable\", \"prerequisite\", \"searched[]\", "
         "\"checks[].output\"],\n";
    o << "    \"why\": \"This file is TRACKED. Leaving `output` verbatim (option "
         "b) would have kept it a faithful quote at the cost of writing the "
         "build machine's home directory into git on every run -- the exact "
         "string a 1,290-commit history rewrite existed to remove. Faithfulness "
         "of a quote loses to not re-leaking an operator's identity, and the "
         "verdict/exit_code carry the evidential weight anyway. Option (c) was "
         "rejected because the record is committed deliberately: phase-14 cites "
         "it as the evidence that veraPDF resolved without a human-supplied "
         "variable.\",\n";
    o << "    \"consequence\": \"`output` is NOT byte-identical to veraPDF's "
         "stdout. Do not diff it against a live run; compare verdict and "
         "exit_code instead.\"\n";
    o << "  },\n";
    o << "  \"checker\": {\n";
    o << "    \"tool\": \"" << jsonEscape(res.tool) << "\",\n";
    o << "    \"resolved_without_human_supplied_variable\": true,\n";
    o << "    \"available\": " << (res.available ? "true" : "false") << ",\n";
    o << "    \"jar_path\": \"" << jsonEscape(portablePath(res.jarPath)) << "\",\n";
    o << "    \"java_command\": \"" << jsonEscape(res.javaCommand) << "\",\n";
    o << "    \"version\": \"" << jsonEscape(res.version) << "\",\n";
    o << "    \"how_resolved\": \"" << jsonEscape(portablePath(res.howResolved)) << "\",\n";
    o << "    \"reason_unavailable\": \"" << jsonEscape(portablePath(res.reason)) << "\",\n";
    o << "    \"prerequisite\": \"" << jsonEscape(portablePath(res.prerequisite)) << "\",\n";
    o << "    \"searched\": [";
    for (size_t i = 0; i < res.searched.size(); ++i) {
        o << (i ? ",\n      " : "\n      ")
          << "\"" << jsonEscape(portablePath(res.searched[i])) << "\"";
    }
    o << (res.searched.empty() ? "" : "\n    ") << "]\n";
    o << "  },\n";
    o << "  \"ran\": " << (res.available ? "true" : "false") << ",\n";
    // "checks" is merged across ALL shard processes via the sidecar files, so
    // a 16-shard run produces a complete record instead of the last shard's
    // subset. "checks_this_process" records how many THIS process contributed,
    // so a reader can always tell the two apart.
    std::size_t thisProcess = 0;
    {
        std::lock_guard<std::mutex> lock(recordMutex());
        thisProcess = recordedVerdicts().size();
    }
    o << "  \"checks_this_process\": " << thisProcess << ",\n";
    o << "  \"checks_merged_from\": \"gate-artifacts/phase-14/checks/"
         "*.json (one sidecar per file+flavour, merged across shards)\",\n";
    o << "  \"checks\": [";
    const std::vector<std::string> objs = mergedCheckObjects();
    for (size_t i = 0; i < objs.size(); ++i) {
        o << (i ? ",\n    " : "\n    ") << objs[i];
    }
    o << (objs.empty() ? "" : "\n  ") << "]\n";
    o << "}\n";
    return o.str();
}

void writeIfPossible(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) out << text;
}

// ---------------------------------------------------------------------------
// Idempotent record write (GitHub #155)
// ---------------------------------------------------------------------------
//
// THE DEFECT. `flushConformanceRecord()` writes into a file that is TRACKED IN
// GIT, and it wrote it unconditionally on every call. Nothing substantive
// changed between runs -- the verdicts, the ISO clause ids and the per-file
// results were byte-identical -- but three classes of field are regenerated
// each time:
//
//   1. the top-level `generated` timestamp,
//   2. each check's `recorded` timestamp (fresh per sidecar, per run),
//   3. the RANDOM per-run temporary directory name inside `output`. Note
//      `portablePath()` already maps the %TEMP% ROOT to `<temp>`, so the
//      leak-relevant part is gone; what survives is the random leaf that
//      QTemporaryDir-style naming appends, e.g.
//      `<temp>\\PDFE-EylaBd\\claim_check_1b.pdf`.
//
// So every full-suite run left the working tree dirty. That is not cosmetic:
// this project forbids `git add -A` precisely because the documented workflow
// is "read `git status`, then stage named files", and a permanently-dirty entry
// trains the reader to skim past that output. It was restored with
// `git checkout --` in every phase of eight consecutive pipelines.
//
// THE FIX, and why it is this one. Issue #155 offered three options: move the
// record out of `docs/`, gitignore it, or make the writer idempotent. The third
// was chosen because the record IS wanted as committed evidence -- phase-14
// cites it as proof veraPDF resolved without a human-supplied variable -- so
// hiding it or relocating it would cost the evidence to buy the clean tree.
//
// Rather than DELETE the volatile fields (which would destroy genuinely useful
// provenance: when a record was produced, and which entries are stale sidecars
// from a deleted check), the writer compares the SUBSTANTIVE content and skips
// the write when it has not changed. On a genuine change the file is rewritten
// in full, fresh timestamps and all.
//
// The verdicts themselves are deliberately untouched. #155 says so explicitly:
// the `FAIL 6.7.3-1` / `FAIL 6.7.9-3` / `FAIL 6.1.7-2` results are a separate,
// real PDF/A finding and must not be changed by this fix.

/// Reduce @p json to just its substantive content, by neutralising exactly the
/// three volatile classes documented above. Used ONLY for the changed/unchanged
/// comparison -- never written anywhere.
///
/// Deliberately a textual normaliser rather than a JSON parse: the record is
/// assembled by concatenating pre-serialised sidecar text, there is no JSON
/// parser linked into this translation unit, and a parse would fail closed
/// (i.e. rewrite every time) on any malformed legacy file for no benefit.
std::string substantiveRecordDigest(const std::string& json) {
    std::string s = json;

    // (1) + (2): blank the VALUE of every `"<key>": "<iso timestamp>"`.
    const auto blankValues = [&s](const std::string& key) {
        const std::string needle = "\"" + key + "\": \"";
        for (std::size_t at = s.find(needle); at != std::string::npos;
             at = s.find(needle, at + 1)) {
            const std::size_t from = at + needle.size();
            const std::size_t end = s.find('"', from);
            if (end == std::string::npos) break;
            s.replace(from, end - from, "<normalised>");
        }
    };
    blankValues("generated");
    blankValues("recorded");

    // (4) `checks_this_process` is a FOURTH volatile class, found by running
    // the suite rather than by reading the issue -- #155 lists only the two
    // timestamp classes and the temp path.
    //
    // It counts how many checks THIS process contributed, as opposed to the
    // merged `checks` array assembled from every shard's sidecars. That number
    // is a property of HOW the suite was invoked -- the shard count, and any
    // --gtest_filter in effect -- and not of anything the checker found. A
    // 16-shard run records whichever count the last shard to flush happened to
    // have; a filtered run that touches no conformance test records 0. Both
    // were observed dirtying the tracked file while every verdict stayed
    // identical, so leaving it substantive would have left #155 half-fixed.
    //
    // It is still WRITTEN, and still correct whenever the record is genuinely
    // rewritten -- it simply does not by itself justify rewriting a tracked
    // evidence file.
    {
        const std::string needle = "\"checks_this_process\": ";
        for (std::size_t at = s.find(needle); at != std::string::npos;
             at = s.find(needle, at + 1)) {
            const std::size_t from = at + needle.size();
            std::size_t end = from;
            while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end])))
                ++end;
            if (end > from) s.replace(from, end - from, "<normalised>");
        }
    }

    // (5) LINE ENDINGS. This repo sets core.autocrlf=true, so git writes CRLF
    // into the working tree on checkout while this writer emits LF. Comparing
    // raw bytes would therefore report "changed" for every record on any
    // freshly checked-out tree -- passing in the session that wrote the file
    // and failing after any checkout, which is the exact failure mode this
    // project has recorded before for guards that compare tracked text.
    {
        std::string norm;
        norm.reserve(s.size());
        for (char c : s)
            if (c != '\r') norm += c;
        s.swap(norm);
    }

    // (3) Collapse the single path segment immediately following each `<temp>`
    // placeholder, whatever it is called. Matching the SEGMENT rather than a
    // `PDFE-` prefix means a change to the fixture's temp-dir naming cannot
    // quietly reintroduce the churn this fix exists to remove.
    const std::string tok = "<temp>";
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, tok.size(), tok) == 0) {
            out += tok;
            i += tok.size();
            // Consume the separator run (`\`, `\\` escaped, or `/`) verbatim.
            while (i < s.size() && (s[i] == '\\' || s[i] == '/')) {
                out += s[i];
                ++i;
            }
            // Replace the segment itself, if there is one.
            const std::size_t segStart = i;
            while (i < s.size() && s[i] != '\\' && s[i] != '/' && s[i] != '"')
                ++i;
            if (i > segStart) out += "<tempdir>";
            continue;
        }
        out += s[i];
        ++i;
    }
    return out;
}

/// Write @p text to @p path only if its substantive content differs from what
/// is already there. Returns true if a write actually happened.
///
/// An unreadable or absent destination counts as "differs", so a missing record
/// is always created.
bool writeRecordIfChanged(const fs::path& path, const std::string& text) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            if (substantiveRecordDigest(ss.str())
                == substantiveRecordDigest(text)) {
                return false;  // nothing substantive changed -- leave it alone
            }
        }
    }
    writeIfPossible(path, text);
    return true;
}

// ---------------------------------------------------------------------------
// Cross-process check merge (phase-14 karen finding)
// ---------------------------------------------------------------------------
//
// The record used to be a whole-file truncating write of only THIS process's
// verdicts. The canonical suite run is sharded into 16 sequential processes
// (tests/scripts/run_gtest.py --shards 16), so whichever shard happened to
// finish last overwrote the record with its own subset — the artifact then
// showed 1 of 5 checks while honestly appearing complete. Silent truncation of
// an evidence artifact is exactly the failure mode this project treats as
// serious, so each check is now ALSO written as its own sidecar file and the
// record is assembled by merging every sidecar found.
//
// Sidecars are keyed by file+flavour, so a re-run of the same check overwrites
// its own entry rather than accumulating duplicates.
//
// KNOWN LIMITATION, stated rather than hidden: sidecars persist between suite
// runs, so a check that is DELETED from the suite leaves its last sidecar
// behind. Each sidecar therefore carries its own "recorded" timestamp so a
// reader can identify an entry that did not come from the current run. Clear
// the checks/ directory to reset.

std::string sidecarKey(const CheckerVerdict& v) {
    std::string key = fs::path(v.file).filename().string() + "__" + v.flavour;
    for (char& ch : key) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (!std::isalnum(u) && ch != '_' && ch != '-' && ch != '.') ch = '_';
    }
    return key;
}

std::string checkObjectJson(const CheckerVerdict& v) {
    std::ostringstream o;
    o << "{";
    o << "\"file\": \"" << jsonEscape(v.file) << "\", ";
    o << "\"flavour\": \"" << jsonEscape(v.flavour) << "\", ";
    o << "\"ran\": " << (v.ran ? "true" : "false") << ", ";
    o << "\"verdict\": \""
      << (v.ran ? (v.valid ? "PASS" : "FAIL") : "NOT_RUN") << "\", ";
    o << "\"exit_code\": " << v.exitCode << ", ";
    o << "\"recorded\": \"" << nowIso8601() << "\", ";
    o << "\"reason\": \"" << jsonEscape(v.reason) << "\", ";
    o << "\"output\": \"" << jsonEscape(portablePath(v.output)) << "\"}";
    return o.str();
}

fs::path checksDir() {
    return fs::path(PDFE_REPO_ROOT) / ".claude" / "pipelines"
           / "meta-audit-followup" / "gate-artifacts" / "phase-14" / "checks";
}

// Read every sidecar back verbatim. Each file holds exactly one JSON object
// written by checkObjectJson(), so they can be concatenated without parsing.
std::vector<std::string> mergedCheckObjects() {
    std::vector<std::string> objs;
    std::error_code ec;
    const fs::path dir = checksDir();
    if (!fs::exists(dir, ec)) return objs;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".json") {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());  // deterministic ordering
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        // Scrub on the way IN as well as on the way out. `checkObjectJson()`
        // already runs `portablePath` over the captured output it writes, so a
        // sidecar this binary produced is clean -- but these files persist
        // across runs and are not cleared between them, so the directory can
        // still hold objects written by an OLDER binary (or, on a shared
        // checkout, another machine). Those merge in verbatim and land in a
        // TRACKED file. Scrubbing only at write time fixes the future and
        // leaves every already-written sidecar leaking, which is exactly what
        // happened when this was first repaired.
        std::string text = portablePath(trimmed(ss.str()));
        if (!text.empty()) objs.push_back(text);
    }
    return objs;
}

} // namespace

const CheckerResolution& resolveConformanceChecker() {
    static const CheckerResolution resolution = computeResolution();
    return resolution;
}

CheckerVerdict runConformanceCheck(const fs::path& pdf,
                                   const std::string& flavour) {
    CheckerVerdict v;
    v.flavour = flavour;
    v.file = pdf.filename().string();

    const CheckerResolution& res = resolveConformanceChecker();
    if (!res.available) {
        v.ran = false;
        v.reason = res.reason;
        return v;
    }
    std::error_code ec;
    if (!fs::exists(pdf, ec)) {
        v.ran = false;
        v.reason = "input file does not exist: " + pdf.string();
        return v;
    }

    // veraPDF CLI: exit 0 == conforms, exit 1 == does not conform, >1 == error.
    // `-v` makes the CLI list the ISO clause ids it failed on. Without it a
    // rejection is a bare "FAIL <path> 1b", which tells a reader nothing about
    // what to fix — and the captured output is what lands in both the test
    // failure message and the phase-14 record.
    const std::string cmd =
        quotedArg(res.javaCommand) + " -jar " + quotedArg(res.jarPath)
        + " --flavour " + flavour + " --format text -v "
        + quotedArg(pdf.string());

    v.exitCode = runCaptured(cmd, v.output);
    if (v.exitCode != 0 && v.exitCode != 1) {
        v.ran = false;
        v.reason = "veraPDF exited " + std::to_string(v.exitCode)
                 + " (neither conforming nor non-conforming): " + v.output;
        return v;
    }
    v.ran = true;
    v.valid = (v.exitCode == 0);
    return v;
}

std::vector<std::string> failedClauses(const CheckerVerdict& verdict) {
    std::vector<std::string> clauses;
    if (!verdict.ran) return clauses;
    std::istringstream in(verdict.output);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trimmed(line);
        // Verbose CLI failure lines are exactly "FAIL <clause>-<test>"; the
        // per-file summary line is "FAIL <path> <flavour>" and carries a space
        // in the remainder, so requiring a single token separates them.
        if (t.rfind("FAIL ", 0) != 0) continue;
        const std::string rest = trimmed(t.substr(5));
        if (rest.empty() || rest.find(' ') != std::string::npos) continue;
        clauses.push_back(rest);
    }
    std::sort(clauses.begin(), clauses.end());
    clauses.erase(std::unique(clauses.begin(), clauses.end()), clauses.end());
    return clauses;
}

std::string conformanceRecordDigest(const std::string& json) {
    return substantiveRecordDigest(json);
}

// GitHub #190. The writer itself, exported. Deliberately a thin forwarder to
// the one `flushConformanceRecord()` uses -- a test-only reimplementation would
// be a second copy of the logic that could stay correct while the shipped one
// rotted, which is the failure this seam exists to prevent.
bool writeConformanceRecordIfChanged(const fs::path& path,
                                     const std::string& text) {
    return writeRecordIfChanged(path, text);
}

bool conformanceFlushHasInformation(std::size_t ownVerdicts,
                                    std::size_t mergedChecks) {
    return ownVerdicts > 0 || mergedChecks > 0;
}

int flushConformanceRecord() {
    // A process that ran no check of its own AND finds no sidecar from any
    // other shard knows NOTHING about `checks`. Writing the record anyway
    // emits "checks": [] over a TRACKED evidence file holding the merged
    // verdicts of a real run — that is not a measurement, it is an erasure.
    //
    // The digest already treats `checks_this_process: 0` as non-substantive
    // for precisely this reason (see blankValues above: "a filtered run that
    // touches no conformance test records 0 ... it does not by itself justify
    // rewriting a tracked evidence file"). An empty MERGE is the same absence
    // one notch more destructive, because it destroys the content rather than
    // a counter, and it was the half left unhandled.
    //
    // Measured 2026-10-18: the sidecar directory lives under the checkout
    // (.claude/pipelines/...), so moving the checkout leaves every sidecar
    // behind. The first shard to flush then blanked the committed record from
    // six checks to none, after which GitHub #190's repair case lost the
    // "verdict" field its premise guard asserts on and failed — while the
    // rerun "healed", because by then a later shard had written sidecars.
    std::size_t ownVerdicts = 0;
    {
        std::lock_guard<std::mutex> lock(recordMutex());
        ownVerdicts = recordedVerdicts().size();
    }
    if (!conformanceFlushHasInformation(ownVerdicts,
                                        mergedCheckObjects().size())) {
        return 0;
    }

    const std::string json = buildRecordJson();
    const fs::path repo(PDFE_REPO_ROOT);
    // Both copies go through the changed/unchanged comparison. Making only the
    // tracked one conditional would leave the two diverging in mtime and
    // content for no reason, and a future reader could not tell which of them
    // reflects the last SUBSTANTIVE change.
    int written = 0;
    if (writeRecordIfChanged(repo / ".claude" / "pipelines"
                                 / "meta-audit-followup" / "gate-artifacts"
                                 / "phase-14"
                                 / "conformance-checker-record.json", json))
        ++written;
    if (writeRecordIfChanged(repo / "docs" / "audit" / "meta-audit-2026-07"
                                 / "followup" / "phase-14"
                                 / "conformance-checker-record.json", json))
        ++written;
    return written;
}

void recordConformanceResult(const CheckerVerdict& verdict) {
    {
        std::lock_guard<std::mutex> lock(recordMutex());
        recordedVerdicts().push_back(verdict);
    }
    // Persist this check as its own sidecar BEFORE rebuilding the record, so
    // the rebuild below picks it up along with every other shard's checks.
    writeIfPossible(checksDir() / (sidecarKey(verdict) + ".json"),
                    checkObjectJson(verdict));
    flushConformanceRecord();
}

} // namespace pdfe::test
