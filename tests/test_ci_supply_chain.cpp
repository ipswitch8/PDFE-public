// CI supply-chain guards (GitHub #70).
//
// The premise of every other guard in this suite is that CI runs what this
// repository says it runs. That premise was false. Before this phase:
//
//   * 61 of 61 external `uses:` references in .github/workflows resolved a
//     MUTABLE tag at run time. `actions/checkout@v4` is not a version, it is a
//     pointer that whoever controls the tag may re-aim -- and one of them,
//     `lukka/run-vcpkg@v11`, was not even a tag but a BRANCH, which carries no
//     tag-protection semantics at all.
//   * Three raw downloads ran with nothing verifying the bytes. One of them was
//     an installer .exe that was then EXECUTED on the self-hosted runner that
//     builds and signs the product.
//
// A green suite proves nothing about a build whose inputs are chosen by a third
// party after the fact. These tests are the standing oracle for that: they fail
// the build if a future edit reintroduces a floating reference or an unverified
// fetch, which is the shape every one of these regressions has taken.
//
// Deliberately implemented as a text scan rather than a YAML parse: the property
// under test is about the literal reference text a maintainer writes and reads
// in review, and a parser would happily normalise away the distinction between
// a tag and a SHA that this file exists to police.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> splitLines(const std::string& body)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : body) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string trimLeft(const std::string& s)
{
    const auto i = s.find_first_not_of(" \t");
    return i == std::string::npos ? std::string() : s.substr(i);
}

// A line that is entirely a comment -- YAML `#` and PowerShell `#` are the same
// character, so one predicate covers both.
//
// This matters more than it looks. The prose in these workflows DISCUSSES the
// very constructs being policed ("an unverified Invoke-WebRequest to fetch"),
// so a scan that does not skip comments reports the documentation of a fix as
// though it were the defect. That exact self-match has already bitten a guard
// in this repo once.
bool isComment(const std::string& line)
{
    const std::string t = trimLeft(line);
    return !t.empty() && t[0] == '#';
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

bool isHex40(const std::string& s)
{
    if (s.size() != 40) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// Every .yml/.yaml under .github/workflows and .github/actions.
std::vector<fs::path> workflowFiles()
{
    std::vector<fs::path> out;
    for (const char* sub : {"workflows", "actions"}) {
        const fs::path dir = repoRoot() / ".github" / sub;
        if (!fs::is_directory(dir)) continue;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            const auto ext = e.path().extension().string();
            if (ext == ".yml" || ext == ".yaml") out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The `uses:` reference on a line, or "" if the line does not declare one.
std::string usesRef(const std::string& line)
{
    const std::string t = trimLeft(line);
    std::string rest;
    if (t.rfind("uses:", 0) == 0) {
        rest = t.substr(5);
    } else if (t.rfind("- uses:", 0) == 0) {
        rest = t.substr(7);
    } else {
        return {};
    }
    const auto b = rest.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    rest = rest.substr(b);
    const auto e = rest.find_first_of(" \t");
    return e == std::string::npos ? rest : rest.substr(0, e);
}

std::string joined(const std::vector<std::string>& v)
{
    std::string s;
    for (const auto& x : v) s += "\n    " + x;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------

// Criterion 1: no external action may be referenced by a mutable ref.
TEST(CiSupplyChain, EveryExternalUsesReferenceIsShaPinned)
{
    const auto files = workflowFiles();
    ASSERT_FALSE(files.empty()) << "no workflow files found under .github/";

    std::vector<std::string> offenders;
    int scanned = 0;
    for (const auto& f : files) {
        for (const auto& line : splitLines(readFile(f))) {
            if (isComment(line)) continue;
            const std::string ref = usesRef(line);
            if (ref.empty()) continue;
            ++scanned;

            // A local composite action resolves inside THIS repository at the
            // commit already checked out. It carries no external trust, and a
            // SHA would be meaningless -- so it is exempt by nature, not by
            // convenience.
            if (!ref.empty() && ref[0] == '.') continue;

            const auto at = ref.rfind('@');
            if (at == std::string::npos || !isHex40(ref.substr(at + 1))) {
                offenders.push_back(f.filename().string() + ": " + ref);
            }
        }
    }

    EXPECT_GT(scanned, 0)
        << "no `uses:` references were scanned at all, so this guard would "
           "pass vacuously. That is a defect in the guard, not a clean repo.";
    EXPECT_TRUE(offenders.empty())
        << "external action(s) referenced by a MUTABLE ref rather than a 40-hex "
           "commit SHA. A tag -- and worse, a branch -- is repointable by "
           "whoever owns the action, so CI would execute code this repository "
           "never reviewed. Pin with `tools/gate/pin_workflow_actions.py`."
        << joined(offenders);
}

// Criterion 2: a pin nobody can read is a pin nobody will maintain.
TEST(CiSupplyChain, EveryPinnedUsesReferenceNamesItsVersionInAComment)
{
    const auto files = workflowFiles();
    ASSERT_FALSE(files.empty());

    std::vector<std::string> offenders;
    int pinned = 0;
    for (const auto& f : files) {
        for (const auto& line : splitLines(readFile(f))) {
            if (isComment(line)) continue;
            const std::string ref = usesRef(line);
            if (ref.empty() || ref[0] == '.') continue;
            const auto at = ref.rfind('@');
            if (at == std::string::npos || !isHex40(ref.substr(at + 1))) continue;
            ++pinned;
            // The trailing `# vX.Y.Z` is the only thing that makes a 40-hex
            // string legible to a reviewer deciding whether it is current.
            if (!contains(line, "#")) {
                offenders.push_back(f.filename().string() + ": " + ref);
            }
        }
    }

    EXPECT_GT(pinned, 0) << "no pinned references found; guard would be vacuous.";
    EXPECT_TRUE(offenders.empty())
        << "SHA-pinned action(s) with no trailing `# vX.Y.Z` version comment. "
           "Without it nobody can tell what version the SHA is, so the pin can "
           "never be reviewed or updated -- it silently freezes at whatever it "
           "was, which is a different failure from the one pinning solved."
        << joined(offenders);
}

// Criterion 3: bytes fetched over the network must be checked before use.
TEST(CiSupplyChain, EveryRemoteFetchInAWorkflowIsHashVerified)
{
    const auto files = workflowFiles();
    ASSERT_FALSE(files.empty());

    // Downloads are attributed to the step that contains them. A step begins at
    // a `- name:` entry; the hash check must live in that same step, because a
    // check in a neighbouring step does not gate this step's bytes.
    const std::vector<std::string> fetchTokens = {
        "Invoke-WebRequest", "Start-BitsTransfer", "DownloadFile",
        "curl ", "wget ", "iwr "};

    std::vector<std::string> offenders;
    int fetches = 0;
    for (const auto& f : files) {
        const auto lines = splitLines(readFile(f));

        std::vector<size_t> stepStarts;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (isComment(lines[i])) continue;
            if (contains(trimLeft(lines[i]), "- name:")) stepStarts.push_back(i);
        }
        stepStarts.push_back(lines.size());

        for (size_t s = 0; s + 1 < stepStarts.size(); ++s) {
            const size_t begin = stepStarts[s];
            const size_t end = stepStarts[s + 1];

            bool hasFetch = false;
            std::string what;
            for (size_t i = begin; i < end; ++i) {
                if (isComment(lines[i])) continue;
                for (const auto& tok : fetchTokens) {
                    if (contains(lines[i], tok)) {
                        hasFetch = true;
                        if (what.empty()) what = tok;
                    }
                }
            }
            if (!hasFetch) continue;
            ++fetches;

            bool verified = false;
            for (size_t i = begin; i < end; ++i) {
                if (isComment(lines[i])) continue;
                if (contains(lines[i], "Get-FileHash") ||
                    contains(lines[i], "sha256sum") ||
                    contains(lines[i], "--require-hashes")) {
                    verified = true;
                }
            }
            if (!verified) {
                std::string name = trimLeft(lines[begin]);
                offenders.push_back(f.filename().string() + ": step \"" + name +
                                    "\" downloads via " + what +
                                    " with no hash check");
            }
        }
    }

    EXPECT_GT(fetches, 0)
        << "no download steps were found at all. Either the token list has "
           "gone stale or the scan is broken -- both make this guard vacuous.";
    EXPECT_TRUE(offenders.empty())
        << "workflow step(s) download bytes over the network and use them "
           "without verifying a hash. The severe case is an installer that is "
           "then EXECUTED on the self-hosted runner that builds and signs this "
           "product: unverified, that is arbitrary code execution gated on "
           "nothing but DNS and TLS to a third-party host."
        << joined(offenders);
}

// Criterion 4: the FlaUI bootstrap must not fetch an executable it cannot name.
TEST(CiSupplyChain, TheFlaUiInstallerPinsAndVerifiesNugetBeforeRunningIt)
{
    const fs::path script = repoRoot() / "tests" / "ui_automation" / "harness" /
                            "flaui_dlls" / "install_flaui.ps1";
    ASSERT_TRUE(fs::exists(script)) << script.string() << " is missing";

    const std::string body = readFile(script);
    std::string code;
    for (const auto& line : splitLines(body)) {
        if (!isComment(line)) code += line + "\n";
    }

    EXPECT_FALSE(contains(code, "win-x86-commandline/latest"))
        << "install_flaui.ps1 fetches nuget.exe from the `/latest/` URL and "
           "then executes it. \"latest\" is not a version -- it is whatever is "
           "served at the moment of the request -- so no hash can be pinned and "
           "nothing can be verified. Pin a specific version URL instead.";

    EXPECT_TRUE(contains(code, "NUGET_SHA256"))
        << "install_flaui.ps1 must declare an expected SHA256 for nuget.exe; "
           "it downloads and then EXECUTES that binary.";

    // The check must gate execution, not merely follow it. The script downloads
    // to a temporary path and only moves it into place after verifying, so a
    // failed check can never leave an unverified executable where the script
    // later invokes it.
    const auto verifyPos = code.find("Test-Sha256 -Path $tmp");
    const auto movePos = code.find("Move-Item $tmp");
    EXPECT_NE(verifyPos, std::string::npos)
        << "the downloaded nuget.exe is never hash-checked.";
    EXPECT_NE(movePos, std::string::npos)
        << "expected the download-to-temp-then-move pattern.";
    if (verifyPos != std::string::npos && movePos != std::string::npos) {
        EXPECT_LT(verifyPos, movePos)
            << "nuget.exe is moved into place BEFORE its hash is verified. "
               "Verifying an executable after installing it proves nothing.";
    }

    // Every package must be version-pinned; an unpinned `nuget install` floats
    // with whatever the feed resolves that day.
    EXPECT_TRUE(contains(code, "-Version $pkg.Version"))
        << "packages must be installed with an explicit -Version.";
}
