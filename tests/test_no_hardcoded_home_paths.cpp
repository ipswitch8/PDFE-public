// A test whose input lives in one developer's home directory runs nowhere but
// that developer's machine, and it does not say so: it GTEST_SKIP()s, and a
// skip reads as "this environment lacks an optional fixture", not "this oracle
// has never been evaluated by anyone".
//
// GitHub #261 was six of them at once -- three in tests/test_c2_image_cache.cpp
// pointed at `C:/Users/dev/...Desktop/...`, three in
// tests/test_tessdata_font_detection.cpp at `C:/Users/dev/<a bank letter>.pdf`.
// Six of the suite's 22 live skips, for as long as the paths had been wrong.
// Two of the six documented in-source that they could never be re-pointed,
// which was true of the DOCUMENTS (third-party personal data, and a 279 MB set
// of construction specifications for a named school) and false of the tests:
// what each case asserts needs the document's SHAPE, and committed fixtures
// have it.
//
// This file is the class-level guard. It does not care about those six; it
// fails on the NEXT one, before it can be merged and start skipping.
//
// WHAT IT MATCHES, and why the pattern is this narrow. A user-profile root
// followed by a name that is not this project's own. That is the shape that
// cannot survive leaving its author's machine. It deliberately does NOT flag:
//
//   - `C:\\Windows\\Fonts\\...` and other system paths, which exist on every
//     Windows host and are legitimate test inputs;
//   - a path assembled from an environment variable at run time, which is an
//     opt-in a developer chooses rather than a default nobody else can meet;
//   - paths inside a string that the test SCANS FOR rather than opens -- this
//     file itself holds the offending literals, and so do the two files that
//     record the defect, so a guard that matched any occurrence anywhere would
//     redden on its own evidence.
//
// The last exclusion is the one worth stating: a source-scanning guard that
// cannot express its own subject without tripping is not usable, and the usual
// wrong fix is to delete the evidence rather than scope the scan.

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
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The user-profile roots a hardcoded absolute path can begin with. Both
// separator conventions, because a C++ literal writes the Windows one doubled
// and a path built from a raw string does not.
const std::vector<std::string>& profileRoots()
{
    static const std::vector<std::string> kRoots = {
        "c:/users/",
        "c:\\\\users\\\\",   // as it appears in a C++ string literal
        "c:\\users\\",       // as it appears in a raw string
        "/home/",
        "/users/",
    };
    return kRoots;
}

// Files that legitimately CONTAIN the pattern because they document or scan
// for it. Relative to the repo root, forward slashes.
bool isEvidenceFile(const std::string& rel)
{
    static const std::vector<std::string> kAllowed = {
        "tests/test_no_hardcoded_home_paths.cpp",
    };
    return std::find(kAllowed.begin(), kAllowed.end(), rel) != kAllowed.end();
}

struct Hit {
    std::string file;
    int line = 0;
    std::string text;
};

std::string stripLeading(const std::string& s)
{
    const auto at = s.find_first_not_of(" \t");
    return at == std::string::npos ? std::string() : s.substr(at);
}

// True when `line` USES a path under a user profile as a filesystem location.
// `line` is already lower-cased.
//
// The distinction between USING such a path and merely MENTIONING one is the
// whole difficulty, and getting it wrong in the lenient direction is what makes
// a source-scanning guard unusable. The first cut of this predicate flagged 17
// lines, of which 14 were tests of path-CLASSIFICATION functions --
// `EXPECT_FALSE(is_unc_or_remote("C:\\Users\\me\\doc.pdf"))` and a table of
// crash-handler path vectors. Those strings are arguments to a pure function;
// nothing ever opens them, and they must keep naming a profile path because
// that is the input whose classification is under test.
//
// So a hit needs a profile root inside a string literal, AND evidence that the
// literal is a filesystem location rather than a value: either a construction
// or open on the same line, or a bare continuation-line literal (the shape of a
// multi-line path constant). An assertion on the line rules it out -- an
// assertion is asking a question about the string, not opening it.
bool looksLikeHardcodedProfilePath(const std::string& line)
{
    for (const auto& root : profileRoots()) {
        const auto at = line.find(root);
        if (at == std::string::npos) continue;

        // It must be inside a string literal -- a comment mentioning the shape
        // is not a defect. Require a quote before the match on the same line.
        const auto quote = line.rfind('"', at);
        if (quote == std::string::npos) continue;

        // A line that is wholly a comment is discussion, not code.
        const auto slashes = line.find("//");
        if (slashes != std::string::npos && slashes < quote) continue;

        // getenv-derived paths are an opt-in, not a default.
        if (line.find("getenv") != std::string::npos) continue;

        // An assertion about the string is not a use of it as a location.
        if (line.find("expect_") != std::string::npos) continue;
        if (line.find("assert_") != std::string::npos) continue;

        static const char* kFilesystemUse[] = {
            "path(", "fs::path", "filesystem::path", "ifstream", "ofstream",
            "open(", "exists(", "create_directories", "fopen",
        };
        for (const char* tok : kFilesystemUse) {
            if (line.find(tok) != std::string::npos) return true;
        }

        // A continuation line that is nothing but a string literal -- how a
        // long path constant is written across two lines.
        const std::string head = stripLeading(line);
        if (!head.empty() && head[0] == '"') return true;
    }
    return false;
}

std::vector<Hit> scanTestSources()
{
    std::vector<Hit> hits;
    const fs::path testsDir = repoRoot() / "tests";
    if (!fs::exists(testsDir)) return hits;

    for (const auto& entry : fs::recursive_directory_iterator(testsDir)) {
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();
        const std::string ext = lower(p.extension().string());
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;

        // Build/run artefacts live under tests/ too; they are not sources.
        std::string rel = fs::relative(p, repoRoot()).generic_string();
        if (rel.find("/_") != std::string::npos) continue;
        if (isEvidenceFile(rel)) continue;

        std::istringstream in(readFile(p));
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (looksLikeHardcodedProfilePath(lower(line))) {
                hits.push_back(Hit{rel, n, line});
            }
        }
    }
    return hits;
}

}  // namespace

// The whole family: any test source that resolves an input from a path under a
// user profile. GitHub #261's six are fixed; this is what stops the seventh.
TEST(HardcodedInputPaths, NoTestSourceResolvesAnInputFromAUserProfile)
{
    const std::vector<Hit> hits = scanTestSources();

    std::ostringstream detail;
    for (const auto& h : hits) {
        detail << "\n  " << h.file << ":" << h.line << "  " << h.text;
    }

    EXPECT_TRUE(hits.empty())
        << hits.size() << " test source line(s) hardcode an absolute path "
           "under a user profile. Such a test runs on exactly one machine and "
           "GTEST_SKIP()s everywhere else, where the skip reads as a missing "
           "optional fixture rather than as an oracle nobody has ever "
           "evaluated -- GitHub #261, six tests, for months. Resolve the input "
           "from TEST_FIXTURES_DIR, or from an environment variable with a "
           "committed-fixture default and NO fall-through to a skip."
        << detail.str();
}

// NON-VACUITY. The scan above is only worth its green if it can go red, and a
// source-scanning guard is the classic shape that cannot: a pattern that
// matches nothing reports "no hits" exactly as a clean tree does. This drives
// the predicate directly with lines that must and must not match, so the
// distinction is asserted rather than assumed.
TEST(HardcodedInputPaths, ThePredicateSeparatesADefaultFromAMention)
{
    // Must match: a literal used as a filesystem location.
    EXPECT_TRUE(looksLikeHardcodedProfilePath(
        lower(R"(    fs::path p = "C:/Users/dev/scan.pdf";)")))
        << "a forward-slash profile path in a string literal must be caught";
    EXPECT_TRUE(looksLikeHardcodedProfilePath(
        lower(R"XX(    auto p = std::filesystem::path("C:\\Users\\dev\\x.pdf");)XX")))
        << "a backslash profile path in a path() construction must be caught";
    EXPECT_TRUE(looksLikeHardcodedProfilePath(
        lower(R"(    std::ifstream in("/home/dev/scan.pdf");)")))
        << "a POSIX home path being opened must be caught";
    EXPECT_TRUE(looksLikeHardcodedProfilePath(
        lower(R"(            "C:/Users/dev/source/PDFE/.claude/pipelines/x/")")))
        << "a continuation line that is nothing but a profile path literal is "
           "how a long path constant is written, and must be caught";

    // Must NOT match: discussion, system paths, and env-var opt-ins.
    EXPECT_FALSE(looksLikeHardcodedProfilePath(
        lower(R"(    // was "C:/Users/dev/scan.pdf" before GitHub #261)")))
        << "a comment describing the defect is not the defect";
    EXPECT_FALSE(looksLikeHardcodedProfilePath(
        lower(R"(    analyzer->analyzeFromFile("C:\\Windows\\Fonts\\arial.ttf");)")))
        << "a system path exists on every host and is a legitimate input";
    EXPECT_FALSE(looksLikeHardcodedProfilePath(
        lower(R"(    if (const char* e = std::getenv("PDFE_SCANNED_PDF")) {)")))
        << "an environment-variable opt-in is not a hardcoded default";
    EXPECT_FALSE(looksLikeHardcodedProfilePath(
        lower(R"(    return fixturesDir() / "test_scanned.pdf";)")))
        << "a fixture-relative path must not be flagged";

    // The 14 false positives the first cut produced, in their real shapes.
    EXPECT_FALSE(looksLikeHardcodedProfilePath(
        lower(R"(    EXPECT_FALSE(ps::is_unc_or_remote("C:\\Users\\me\\doc.pdf"));)")))
        << "a path-classification assertion must keep naming a profile path: "
           "that string is the input under test, not a location anyone opens";
    EXPECT_FALSE(looksLikeHardcodedProfilePath(
        lower(R"(    EXPECT_FALSE(ps::is_unc_or_remote(std::filesystem::path("C:\\Users\\me\\x.pdf").string()));)")))
        << "a construction INSIDE an assertion is still an assertion";
    EXPECT_FALSE(looksLikeHardcodedProfilePath(
        lower(R"(        {"C:\\Users\\x\\dumps",        true,  "plain drive-absolute local path"},)")))
        << "a row in a table of path test-vectors is data, not a location";
    EXPECT_FALSE(looksLikeHardcodedProfilePath(
        lower(R"(constexpr const char* kLocalOut = "C:\\Users\\Public\\nonexistent_out.pdf";)")))
        << "a deliberately-nonexistent constant that nothing opens on this "
           "line must not be flagged";
}

// The scan must actually READ something. A recursive_directory_iterator that
// walked zero files would satisfy the emptiness assertion above forever, which
// is the "empty collection" shape in the project's own vacuous-oracle list.
TEST(HardcodedInputPaths, TheScanVisitsARealBodyOfTestSources)
{
    const fs::path testsDir = repoRoot() / "tests";
    ASSERT_TRUE(fs::exists(testsDir))
        << "PDFE_REPO_ROOT does not point at a tree with a tests/ directory: "
        << repoRoot().string();

    int sources = 0;
    for (const auto& entry : fs::recursive_directory_iterator(testsDir)) {
        if (!entry.is_regular_file()) continue;
        if (lower(entry.path().extension().string()) == ".cpp") ++sources;
    }
    EXPECT_GT(sources, 300)
        << "only " << sources << " .cpp files found under " << testsDir.string()
        << "; the suite has many hundreds, so a number this small means the "
           "scan is looking at the wrong tree and its green says nothing";
}
