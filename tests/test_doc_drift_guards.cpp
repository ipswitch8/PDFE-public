// -*- coding: utf-8 -*-
// test_doc_drift_guards.cpp
//
// Meta-audit follow-up F9 (gaps PDF-106, SEC-13, SEC-29, SEC-30, SEC-80,
// SEC-82, SEC-87, SEC-91, SEC-97, SEC-107, SEC-108).
//
// F9 retired four documents that were not merely stale but ACTIVELY
// MISDIRECTING: docs/security-testing.md said sanitizers and fuzzing were
// "deferred to Linux" when they run natively on Windows on automatic
// schedules and Linux does not compile at all; scripts/security/run_fuzz.ps1
// said "6 harnesses" while its array held 11 and CI built 12; and the
// flaw-corpus COVERAGE.md / README.md recorded 47 (then 70, then 74) tests
// while the source declared 75.
//
// A corrected document drifts again. Every correction F9 made is therefore
// paired with one of the checks below, each of which re-derives the claim
// from the FILE IT RESTS ON rather than trusting the prose:
//
//   SecurityTestingDocClaims  -- every claim in the security-testing.md claim
//                                table names a workflow/preset file and a
//                                literal that must appear in it, verbatim.
//                                Also writes the cross-check evidence JSON.
//   FuzzHarnessCountDocs      -- run_fuzz.ps1's prose count, its HARNESS-COUNT
//                                marker, its $AllHarnesses array, the CMake
//                                fuzz targets and the windows-fuzz.yml build
//                                list must all agree, by count AND by name.
//   FlawCorpusCountDocs       -- COVERAGE.md and README.md corpus counts are
//                                re-derived from the TIER-MARKER sections of
//                                tests/test_external_flaw_corpus.cpp.
//   ThreatModelSourceDocs     -- docs/security-testing.md §(a) is the single
//                                canonical threat model, and no non-audit doc
//                                cites docs/compliance-architecture.md as a
//                                threat-model source (SEC-107).
//   SecurityDocIndex          -- root SECURITY.md resolves to the real
//                                reporting process (SEC-91); the JPX opt-in
//                                (SEC-97) and dependency-bump runbook (SEC-87)
//                                are documented and cross-referenced; no
//                                hardcoded stale suite count survives (SEC-82).
//
// All oracles are text-over-source. Nothing here builds, runs CI, or reaches
// the network.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path projectRoot() {
    return fs::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Strip surrounding markdown backticks from a table cell, if present.
std::string unbacktick(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '`' && s.back() == '`')
        s = s.substr(1, s.size() - 2);
    return trim(s);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') { cur.push_back(c); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Split a markdown table row "| a | b | c |" into its cells.
std::vector<std::string> tableCells(const std::string& line) {
    std::vector<std::string> cells;
    if (line.empty() || line.front() != '|') return cells;
    std::string cur;
    for (size_t i = 1; i < line.size(); ++i) {
        if (line[i] == '|') { cells.push_back(trim(cur)); cur.clear(); }
        else { cur.push_back(line[i]); }
    }
    return cells;
}

// Return the lines strictly between two marker comments.
std::vector<std::string> linesBetween(const std::string& text,
                                      const std::string& beginMarker,
                                      const std::string& endMarker) {
    std::vector<std::string> out;
    const auto b = text.find(beginMarker);
    const auto e = text.find(endMarker);
    if (b == std::string::npos || e == std::string::npos || e <= b) return out;
    return splitLines(text.substr(b + beginMarker.size(), e - b - beginMarker.size()));
}

// A "count table" is a two-column markdown table of "| label | integer |".
std::map<std::string, long> parseCountTable(const std::string& text,
                                            const std::string& beginMarker,
                                            const std::string& endMarker) {
    std::map<std::string, long> out;
    for (const auto& line : linesBetween(text, beginMarker, endMarker)) {
        const auto cells = tableCells(line);
        if (cells.size() < 2) continue;
        const std::string label = unbacktick(cells[0]);
        const std::string value = trim(cells[1]);
        if (label.empty() || value.empty()) continue;
        if (!std::all_of(value.begin(), value.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; }))
            continue;   // header / separator rows
        out[label] = std::stol(value);
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

const fs::path kSecurityTesting = fs::path("docs") / "security-testing.md";
const fs::path kSecurityPolicy  = fs::path("docs") / "security-policy.md";
const fs::path kRunFuzz         = fs::path("scripts") / "security" / "run_fuzz.ps1";
const fs::path kFuzzCMake       = fs::path("tests") / "security" / "fuzz" / "CMakeLists.txt";
const fs::path kWindowsFuzzYml  = fs::path(".github") / "workflows" / "windows-fuzz.yml";
const fs::path kCorpusSource    = fs::path("tests") / "test_external_flaw_corpus.cpp";
const fs::path kCorpusCoverage  = fs::path("tests") / "fixtures" / "flaw_corpus" / "COVERAGE.md";
const fs::path kCorpusReadme    = fs::path("tests") / "fixtures" / "flaw_corpus" / "README.md";
const fs::path kComplianceArch  = fs::path("docs") / "compliance-architecture.md";
const fs::path kRootSecurityMd  = fs::path("SECURITY.md");
const fs::path kDepsTriage      = fs::path("tests") / "security" / "deps" / "triage.md";
const fs::path kPortfile        = fs::path("vcpkg-overlay-ports") / "libmupdf" / "portfile.cmake";
const fs::path kPhase9Evidence  = fs::path("docs") / "audit" / "meta-audit-2026-07" /
                                  "followup" / "phase-9";
const fs::path kCrosscheckOut   = kPhase9Evidence / "doc-claims-crosscheck.json";
const fs::path kCorpusCountsOut = kPhase9Evidence / "corpus-count-crosscheck.json";
const fs::path kFuzzCountsOut   = kPhase9Evidence / "fuzz-harness-count-crosscheck.json";

void writeEvidence(const fs::path& rel, const std::string& body) {
    std::error_code ec;
    fs::create_directories((projectRoot() / rel).parent_path(), ec);
    std::ofstream out(projectRoot() / rel, std::ios::binary);
    out << body;
}

// ---------------------------------------------------------------------------
// Shared derivations
// ---------------------------------------------------------------------------

struct Claim {
    std::string id;
    std::string text;
    std::string file;
    std::string literal;
};

std::vector<Claim> parseClaimTable(const std::string& doc) {
    std::vector<Claim> claims;
    for (const auto& line : linesBetween(doc, "<!-- CLAIM-TABLE-BEGIN -->",
                                              "<!-- CLAIM-TABLE-END -->")) {
        const auto cells = tableCells(line);
        if (cells.size() < 4) continue;
        Claim c{ trim(cells[0]), trim(cells[1]), unbacktick(cells[2]), unbacktick(cells[3]) };
        // Skip the header row and the |---|---| separator.
        if (c.id.empty() || c.id == "Claim id") continue;
        if (c.id.find_first_not_of("-: ") == std::string::npos) continue;
        claims.push_back(c);
    }
    return claims;
}

// The names in run_fuzz.ps1's $AllHarnesses array (comments excluded).
std::set<std::string> harnessArrayNames(const std::string& ps) {
    std::set<std::string> names;
    const auto b = ps.find("$AllHarnesses = @(");
    if (b == std::string::npos) return names;
    // The array literal ends at the first line that is just ")". Searching for
    // the first ')' character instead would stop inside a comment (the array
    // carries explanatory ones), silently truncating the parse -- which would
    // make this check fail-loud rather than pass-loud, but wrongly either way.
    bool closed = false;
    for (const auto& line : splitLines(ps.substr(b))) {
        const std::string t = trim(line);
        if (t == ")") { closed = true; break; }
        if (t.empty() || t[0] == '#') continue;      // comment line
        std::smatch m;
        if (std::regex_search(t, m, std::regex(R"('(fuzz_[a-z0-9_]+)')")))
            names.insert(m[1].str());
    }
    if (!closed) return {};   // unterminated array -> refuse to guess
    return names;
}

std::set<std::string> cmakeFuzzTargets(const std::string& cmake) {
    std::set<std::string> names;
    const std::regex re(R"(pdfe_add_fuzz_target\(\s*(fuzz_[a-z0-9_]+))");
    for (auto it = std::sregex_iterator(cmake.begin(), cmake.end(), re);
         it != std::sregex_iterator(); ++it)
        names.insert((*it)[1].str());
    return names;
}

std::set<std::string> workflowFuzzTargets(const std::string& yml) {
    std::set<std::string> names;
    const std::regex re(R"RX("(fuzz_[a-z0-9_]+)")RX");
    for (auto it = std::sregex_iterator(yml.begin(), yml.end(), re);
         it != std::sregex_iterator(); ++it)
        names.insert((*it)[1].str());
    return names;
}

std::string joined(const std::set<std::string>& s) {
    std::string out;
    for (const auto& x : s) { if (!out.empty()) out += ", "; out += x; }
    return out;
}

// Corpus test counts, derived from the TIER-MARKER sections of the source.
struct CorpusCounts {
    long total = 0;
    long disabled = 0;
    std::map<int, long> perTier;
};

CorpusCounts deriveCorpusCounts(const std::string& src) {
    CorpusCounts c;
    int tier = 0;
    const std::regex tierRe(R"(^//\s*TIER-MARKER:\s*(\d+))");
    const std::regex testRe(R"(^TEST_F\(ExternalFlawCorpusTest,\s*([A-Za-z0-9_]+))");
    for (const auto& line : splitLines(src)) {
        std::smatch m;
        if (std::regex_search(line, m, tierRe)) { tier = std::stoi(m[1].str()); continue; }
        if (std::regex_search(line, m, testRe)) {
            ++c.total;
            if (m[1].str().rfind("DISABLED_", 0) == 0) ++c.disabled;
            if (tier > 0) ++c.perTier[tier];
        }
    }
    return c;
}

// Markdown files under docs/ that are NOT audit records. Audit documents
// DESCRIBE the SEC-107 finding (and quote the miscitation); correcting an
// audit finding's own wording would be rewriting history, so they are out of
// scope for the citation sweep -- deliberately, and stated in
// docs/audit/meta-audit-2026-07/followup/phase-9-dispositions.md.
std::vector<fs::path> nonAuditDocs() {
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path docs = projectRoot() / "docs";
    if (!fs::exists(docs, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(docs, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        const fs::path& p = it->path();
        if (p.extension() != ".md") continue;
        const std::string s = p.generic_string();
        if (s.find("/docs/audit/") != std::string::npos) continue;
        if (s.find("/docs/archive/") != std::string::npos) continue;
        out.push_back(p);
    }
    return out;
}

} // namespace

// ===========================================================================
// 1. docs/security-testing.md claim table (SEC-29, SEC-30)
// ===========================================================================

// The table must exist and carry real content. A check that silently passes
// because it parsed zero rows is worse than no check at all, so this guards
// the parser's own input before the next test trusts it.
TEST(SecurityTestingDocClaims, ClaimTableExistsAndIsNonTrivial) {
    const std::string doc = readFile(projectRoot() / kSecurityTesting);
    ASSERT_FALSE(doc.empty()) << kSecurityTesting.string() << " must be readable";
    EXPECT_TRUE(contains(doc, "<!-- CLAIM-TABLE-BEGIN -->"))
        << "the machine-checked claim table markers were removed from "
        << kSecurityTesting.string();
    EXPECT_TRUE(contains(doc, "<!-- CLAIM-TABLE-END -->"));

    const auto claims = parseClaimTable(doc);
    EXPECT_GE(claims.size(), 10u)
        << "the claim table parsed " << claims.size() << " rows; the sanitizer/fuzz "
           "posture cannot be described in fewer. Either rows were deleted or the "
           "table format changed under the parser.";
}

// GitHub #178 -- CLAIM IDS MUST BE UNIQUE.
//
// The table shipped with TWO rows both labelled `C13` (the fuzz-duration claim
// and the MSVC ASan preset claim). Pipeline backlog-workable-p82 phase-18 found
// it, judged it harmless because the parser keys on position rather than id,
// and left it in place. That judgement was right about the parser and wrong
// about the cost: an id is how a human cites a row, and two rows answering to
// `C13` means every reference to "C13" is ambiguous -- including the ones in
// docs/audit/.../doc-claims-crosscheck.json, which duly recorded two different
// C13 entries.
//
// The duplicate is now renumbered to C23. It is deliberately OUT OF SEQUENCE
// rather than inserted as C14: renumbering in place would have shifted nine
// following ids, and every existing citation of C14..C22 elsewhere in the repo
// would have silently started pointing at the wrong row. An out-of-order id is
// ugly; a silently repointed citation is a defect.
//
// This test is the part that matters. Without it the collision simply recurs
// the next time a row is appended by copy-paste, which is how it arrived.
TEST(SecurityTestingDocClaims, ClaimIdsAreUnique) {
    const std::string doc = readFile(projectRoot() / kSecurityTesting);
    ASSERT_FALSE(doc.empty()) << kSecurityTesting.string() << " must be readable";

    const auto claims = parseClaimTable(doc);
    // Non-vacuity: an empty parse would make "no duplicates" trivially true.
    // This is the guard against the table format drifting out from under the
    // parser and this test reporting success over zero rows.
    ASSERT_GE(claims.size(), 10u)
        << "parsed only " << claims.size() << " claim rows; a uniqueness check "
           "over an empty or truncated parse proves nothing";

    std::map<std::string, int> seen;
    for (const auto& c : claims) ++seen[c.id];

    std::vector<std::string> dupes;
    for (const auto& kv : seen) {
        if (kv.second > 1) {
            dupes.push_back(kv.first + " x" + std::to_string(kv.second));
        }
    }

    EXPECT_TRUE(dupes.empty())
        << "duplicate claim id(s) in " << kSecurityTesting.string() << ": "
        << [&] {
               std::string s;
               for (const auto& d : dupes) { if (!s.empty()) s += ", "; s += d; }
               return s;
           }()
        << ".\nAn id is how a row is cited. Two rows with one id makes every "
           "citation of it ambiguous. Give the new row the next unused id -- "
           "appending is correct; renumbering in place silently repoints every "
           "existing citation of the ids that follow.";
}

// THE check for gap SEC-29. Every claim the document makes about the sanitizer
// and fuzz lanes must name the file it rests on and a literal that appears in
// that file. This is what makes the section un-invertible: the old text could
// say "Linux-only" precisely because nothing compared it to .github/workflows/.
//
// This test also writes the cross-check evidence JSON, so the artifact is
// machine-generated from the same parse rather than hand-maintained beside it.
TEST(SecurityTestingDocClaims, EveryClaimMatchesItsFile) {
    const std::string doc = readFile(projectRoot() / kSecurityTesting);
    ASSERT_FALSE(doc.empty());
    const auto claims = parseClaimTable(doc);
    ASSERT_FALSE(claims.empty()) << "no claims parsed -- see ClaimTableExistsAndIsNonTrivial";

    struct Result { Claim claim; bool fileExists; bool literalFound; };
    std::vector<Result> results;
    for (const auto& c : claims) {
        const fs::path target = projectRoot() / c.file;
        std::error_code ec;
        const bool exists = fs::exists(target, ec) && fs::is_regular_file(target, ec);
        const std::string body = exists ? readFile(target) : std::string();
        results.push_back({ c, exists, exists && contains(body, c.literal) });
    }

    // Evidence artifact (criterion 1). Written before the assertions so the
    // JSON records a FAILING cross-check too -- an artifact that only ever
    // exists when everything passed proves nothing.
    long mismatches = 0;
    for (const auto& r : results) if (!r.fileExists || !r.literalFound) ++mismatches;

    std::ostringstream js;
    js << "{\n";
    js << "  \"generated_by\": \"SecurityTestingDocClaims.EveryClaimMatchesItsFile "
          "(tests/test_doc_drift_guards.cpp)\",\n";
    js << "  \"phase\": \"phase-9\",\n";
    js << "  \"gaps\": [\"SEC-29\", \"SEC-30\", \"SEC-13\"],\n";
    js << "  \"document\": \"docs/security-testing.md\",\n";
    js << "  \"claims_checked\": " << results.size() << ",\n";
    js << "  \"mismatches\": " << mismatches << ",\n";
    js << "  \"verdict\": \"" << (mismatches == 0 ? "PASS" : "FAIL") << "\",\n";
    js << "  \"claims\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        js << "    {\"id\": \"" << jsonEscape(r.claim.id) << "\", "
           << "\"claim\": \"" << jsonEscape(r.claim.text) << "\", "
           << "\"file\": \"" << jsonEscape(r.claim.file) << "\", "
           << "\"literal\": \"" << jsonEscape(r.claim.literal) << "\", "
           << "\"file_exists\": " << (r.fileExists ? "true" : "false") << ", "
           << "\"literal_found\": " << (r.literalFound ? "true" : "false") << ", "
           << "\"match\": " << ((r.fileExists && r.literalFound) ? "true" : "false")
           << "}" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    js << "  ]\n}\n";

    writeEvidence(kCrosscheckOut, js.str());

    for (const auto& r : results) {
        EXPECT_TRUE(r.fileExists)
            << "claim " << r.claim.id << " rests on " << r.claim.file
            << ", which does not exist";
        if (r.fileExists) {
            EXPECT_TRUE(r.literalFound)
                << "claim " << r.claim.id << " (\"" << r.claim.text << "\") says "
                << r.claim.file << " contains \"" << r.claim.literal
                << "\" -- it does not. Either the workflow changed and the doc "
                   "was not updated in the same commit, or the claim was always "
                   "false. This is gap SEC-29 recurring.";
        }
    }
    EXPECT_EQ(mismatches, 0)
        << "doc-claims cross-check recorded " << mismatches << " mismatch(es); "
        << "evidence written to " << kCrosscheckOut.generic_string();
}

// The specific inversion SEC-29 was raised for must not come back. The §(e)
// heading itself is the oracle: the old one asserted deferral to Linux as the
// current state.
TEST(SecurityTestingDocClaims, InvertedDeferredToLinuxSectionDoesNotReturn) {
    const std::string doc = readFile(projectRoot() / kSecurityTesting);
    ASSERT_FALSE(doc.empty());

    EXPECT_FALSE(contains(doc, "## (e) Sanitizers + fuzzing: deferred to Linux"))
        << "the inverted §(e) heading is back. Sanitizers and fuzzing run "
           "natively on Windows on automatic schedules; Linux does not compile "
           "at all (CI-LIMITATION-PDFE-IS-WINDOWS-ONLY).";
    EXPECT_TRUE(contains(doc, "## (e) Sanitizers + fuzzing: Windows-native"))
        << "§(e) must describe the Windows-native posture that actually exists";

    // MSan and TSan are the only genuinely-absent sanitizers; the doc must keep
    // saying so rather than quietly upgrading them along with the rest.
    EXPECT_TRUE(contains(doc, "MSan")) << "MSan's genuine absence must stay recorded";
    EXPECT_TRUE(contains(doc, "TSan")) << "TSan's genuine absence must stay recorded";
}

// SEC-30. sanitizer.yml (RETIRED) quotes the trigger of its ACTIVE replacement
// in a comment block, precisely so a reader is not left guessing. That quoted
// copy went stale once already: it said the replacement was "push-triggered on
// master" for weeks after the push trigger had been removed. A quoted trigger
// that nothing compares is a second source of truth, so compare it.
TEST(SecurityTestingDocClaims, RetiredLaneQuotesItsReplacementsRealTrigger) {
    const std::string retired = readFile(projectRoot() / ".github" / "workflows" /
                                         "sanitizer.yml");
    const std::string active  = readFile(projectRoot() / ".github" / "workflows" /
                                         "windows-sanitizer.yml");
    ASSERT_FALSE(retired.empty()) << ".github/workflows/sanitizer.yml must be readable";
    ASSERT_FALSE(active.empty())  << ".github/workflows/windows-sanitizer.yml must be readable";

    // REWRITTEN 2026-08-13. This used to extract a cron from BOTH files and
    // require them to be equal -- the retired lane quotes its replacement's
    // trigger in a comment, and that quoted copy had gone stale once before
    // (it claimed "push-triggered on master" for weeks after the push trigger
    // was removed: SEC-30 / SEC-109).
    //
    // The replacement no longer HAS a cron. Every schedule in this repository
    // was removed on the owner's instruction
    // (docs/audit/ci-manual-only-posture.md), so requiring one was asserting the
    // world had not changed. The DRIFT property is what mattered and it is kept,
    // pointed at what is actually true now: the active lane must be
    // manual-only, and the retired lane must not still be telling readers it
    // fires on a timer.
    const std::regex cronRe(R"(cron:\\s*'([^']+)')");
    std::smatch mActive;
    EXPECT_FALSE(std::regex_search(active, mActive, cronRe))
        << "windows-sanitizer.yml has reacquired a cron schedule; if that is "
           "intended, update docs/audit/ci-manual-only-posture.md FIRST -- three "
           "Python gate checkers and two gtest cases read that decision -- and "
           "then re-point this guard back at the equality it used to assert";

    std::smatch mRetired;
    EXPECT_FALSE(std::regex_search(retired, mRetired, cronRe))
        << "sanitizer.yml still quotes a cron (" << mRetired[1].str()
        << ") as its replacement's trigger, but windows-sanitizer.yml no longer "
           "has one. That is exactly the stale second-source-of-truth SEC-30 was "
           "filed for, in the opposite direction";

    EXPECT_TRUE(contains(retired, "windows-sanitizer.yml"))
        << "sanitizer.yml must name the workflow that replaced it";
}

// ===========================================================================
// 2. run_fuzz.ps1 harness count (SEC-13)
// ===========================================================================

TEST(FuzzHarnessCountDocs, ProseCountMatchesHarnessArrayAndBuildSystem) {
    const std::string ps    = readFile(projectRoot() / kRunFuzz);
    const std::string cmake = readFile(projectRoot() / kFuzzCMake);
    const std::string yml   = readFile(projectRoot() / kWindowsFuzzYml);
    ASSERT_FALSE(ps.empty())    << kRunFuzz.string() << " must be readable";
    ASSERT_FALSE(cmake.empty()) << kFuzzCMake.string() << " must be readable";
    ASSERT_FALSE(yml.empty())   << kWindowsFuzzYml.string() << " must be readable";

    const std::set<std::string> arrayNames = harnessArrayNames(ps);
    const std::set<std::string> cmakeNames = cmakeFuzzTargets(cmake);
    const std::set<std::string> ymlNames   = workflowFuzzTargets(yml);
    ASSERT_FALSE(arrayNames.empty()) << "$AllHarnesses did not parse -- the array "
                                        "shape changed under this check";
    ASSERT_FALSE(cmakeNames.empty()) << "no pdfe_add_fuzz_target() calls parsed";

    // (a) The machine-readable marker in the help block.
    std::smatch m;
    ASSERT_TRUE(std::regex_search(ps, m, std::regex(R"(HARNESS-COUNT:\s*(\d+))")))
        << "run_fuzz.ps1 must carry a HARNESS-COUNT marker in its help block";
    const long markerCount = std::stol(m[1].str());

    // (b) The prose sentence a human actually reads.
    ASSERT_TRUE(std::regex_search(ps, m, std::regex(R"(Runs each of the (\d+) fuzz harness)")))
        << "run_fuzz.ps1's .DESCRIPTION must state the harness count";
    const long proseCount = std::stol(m[1].str());

    // (c) The "Default: all N" sentence in .PARAMETER Harness.
    ASSERT_TRUE(std::regex_search(ps, m, std::regex(R"(Default:\s*all\s*(\d+))")))
        << "run_fuzz.ps1's -Harness parameter doc must state the default count "
           "numerically ('all six' was the original SEC-13 drift)";
    const long defaultCount = std::stol(m[1].str());

    const long arrayCount = static_cast<long>(arrayNames.size());

    {   // Evidence artifact for the phase gate (criterion 2).
        std::ostringstream js;
        js << "{\n  \"generated_by\": \"FuzzHarnessCountDocs."
              "ProseCountMatchesHarnessArrayAndBuildSystem "
              "(tests/test_doc_drift_guards.cpp)\",\n";
        js << "  \"phase\": \"phase-9\",\n  \"gap\": \"SEC-13\",\n";
        js << "  \"prose_count\": " << proseCount << ",\n";
        js << "  \"marker_count\": " << markerCount << ",\n";
        js << "  \"default_all_count\": " << defaultCount << ",\n";
        js << "  \"array_count\": " << arrayCount << ",\n";
        js << "  \"cmake_target_count\": " << cmakeNames.size() << ",\n";
        js << "  \"workflow_target_count\": " << ymlNames.size() << ",\n";
        js << "  \"array_names\": \"" << jsonEscape(joined(arrayNames)) << "\",\n";
        js << "  \"agree\": " << ((proseCount == arrayCount && markerCount == arrayCount &&
                                   defaultCount == arrayCount && arrayNames == cmakeNames &&
                                   cmakeNames == ymlNames) ? "true" : "false") << "\n}\n";
        writeEvidence(kFuzzCountsOut, js.str());
    }

    EXPECT_EQ(proseCount, arrayCount)
        << "run_fuzz.ps1 says it runs " << proseCount << " harnesses but "
           "$AllHarnesses holds " << arrayCount << " (" << joined(arrayNames) << "). "
           "This is exactly gap SEC-13.";
    EXPECT_EQ(markerCount, arrayCount)
        << "the HARNESS-COUNT marker (" << markerCount << ") disagrees with "
           "$AllHarnesses (" << arrayCount << ")";
    EXPECT_EQ(defaultCount, arrayCount)
        << "'Default: all " << defaultCount << "' disagrees with $AllHarnesses ("
        << arrayCount << ")";

    // (d) The script must actually run what the build system builds and CI
    //     campaigns. fuzz_image_decode existed in CMake and CI for a whole
    //     phase before the dispatcher learned about it.
    EXPECT_EQ(arrayNames, cmakeNames)
        << "run_fuzz.ps1's $AllHarnesses and tests/security/fuzz/CMakeLists.txt "
           "disagree.\n  script: " << joined(arrayNames)
        << "\n  cmake : " << joined(cmakeNames);
    EXPECT_EQ(cmakeNames, ymlNames)
        << "tests/security/fuzz/CMakeLists.txt and windows-fuzz.yml disagree.\n"
           "  cmake: " << joined(cmakeNames) << "\n  ci   : " << joined(ymlNames);
}

// ===========================================================================
// 3. Flaw-corpus counts (PDF-106)
// ===========================================================================

TEST(FlawCorpusCountDocs, CoverageAndReadmeMatchTheSource) {
    const std::string src      = readFile(projectRoot() / kCorpusSource);
    const std::string coverage = readFile(projectRoot() / kCorpusCoverage);
    const std::string readme   = readFile(projectRoot() / kCorpusReadme);
    ASSERT_FALSE(src.empty())      << kCorpusSource.string() << " must be readable";
    ASSERT_FALSE(coverage.empty()) << kCorpusCoverage.string() << " must be readable";
    ASSERT_FALSE(readme.empty())   << kCorpusReadme.string() << " must be readable";

    const CorpusCounts actual = deriveCorpusCounts(src);
    ASSERT_GT(actual.total, 0) << "no ExternalFlawCorpusTest cases parsed";
    ASSERT_EQ(actual.perTier.size(), 4u)
        << "expected four TIER-MARKER sections in " << kCorpusSource.string()
        << "; found " << actual.perTier.size();

    const char* kTotalLabel = "`ExternalFlawCorpusTest` cases, total";
    const char* kDisabledLabel = "`DISABLED_` cases";

    {   // Evidence artifact for the phase gate (criterion 4).
        const auto covDeclared = parseCountTable(coverage, "<!-- CORPUS-COUNTS-BEGIN -->",
                                                           "<!-- CORPUS-COUNTS-END -->");
        const auto rdmDeclared = parseCountTable(readme, "<!-- CORPUS-COUNTS-BEGIN -->",
                                                         "<!-- CORPUS-COUNTS-END -->");
        const auto get = [](const std::map<std::string, long>& m, const std::string& k) {
            const auto it = m.find(k);
            return it == m.end() ? -1L : it->second;
        };
        const std::string totalKey = unbacktick(kTotalLabel);
        const std::string disabledKey = unbacktick(kDisabledLabel);
        std::ostringstream js;
        js << "{\n  \"generated_by\": \"FlawCorpusCountDocs.CoverageAndReadmeMatchTheSource "
              "(tests/test_doc_drift_guards.cpp)\",\n";
        js << "  \"phase\": \"phase-9\",\n  \"gap\": \"PDF-106\",\n";
        js << "  \"source\": \"tests/test_external_flaw_corpus.cpp\",\n";
        js << "  \"derived\": {\"total\": " << actual.total
           << ", \"disabled\": " << actual.disabled;
        for (const auto& [tier, count] : actual.perTier)
            js << ", \"tier" << tier << "\": " << count;
        js << "},\n";
        js << "  \"COVERAGE.md\": {\"total\": " << get(covDeclared, totalKey)
           << ", \"disabled\": " << get(covDeclared, disabledKey);
        for (const auto& [tier, count] : actual.perTier) {
            (void)count;
            js << ", \"tier" << tier << "\": "
               << get(covDeclared, "Tier " + std::to_string(tier));
        }
        js << "},\n";
        js << "  \"README.md\": {\"total\": " << get(rdmDeclared, totalKey)
           << ", \"disabled\": " << get(rdmDeclared, disabledKey);
        for (const auto& [tier, count] : actual.perTier) {
            (void)count;
            js << ", \"tier" << tier << "\": "
               << get(rdmDeclared, "Tier " + std::to_string(tier));
        }
        js << "}\n}\n";
        writeEvidence(kCorpusCountsOut, js.str());
    }

    for (const auto& [name, doc] : std::vector<std::pair<const char*, const std::string*>>{
             { "COVERAGE.md", &coverage }, { "README.md", &readme } }) {
        const auto declared = parseCountTable(*doc, "<!-- CORPUS-COUNTS-BEGIN -->",
                                                    "<!-- CORPUS-COUNTS-END -->");
        ASSERT_FALSE(declared.empty())
            << name << " must carry a CORPUS-COUNTS block; without it the counts "
                       "drift the way they did through 47 -> 70 -> 74 while the "
                       "source held " << actual.total;

        ASSERT_TRUE(declared.count(unbacktick(kTotalLabel)))
            << name << " CORPUS-COUNTS block has no total row";
        EXPECT_EQ(declared.at(unbacktick(kTotalLabel)), actual.total)
            << name << " declares " << declared.at(unbacktick(kTotalLabel))
            << " corpus tests; the source declares " << actual.total
            << " TEST_F(ExternalFlawCorpusTest, ...) cases. Gap PDF-106.";

        ASSERT_TRUE(declared.count(unbacktick(kDisabledLabel)))
            << name << " CORPUS-COUNTS block has no DISABLED_ row";
        EXPECT_EQ(declared.at(unbacktick(kDisabledLabel)), actual.disabled)
            << name << " declares " << declared.at(unbacktick(kDisabledLabel))
            << " DISABLED_ corpus tests; the source has " << actual.disabled;

        for (const auto& [tier, count] : actual.perTier) {
            const std::string label = "Tier " + std::to_string(tier);
            ASSERT_TRUE(declared.count(label))
                << name << " CORPUS-COUNTS block is missing a '" << label << "' row";
            EXPECT_EQ(declared.at(label), count)
                << name << " declares " << declared.at(label) << " " << label
                << " tests; the source has " << count;
        }

        long tierSum = 0;
        for (const auto& [tier, count] : actual.perTier) { (void)tier; tierSum += count; }
        EXPECT_EQ(tierSum, actual.total)
            << "per-tier counts sum to " << tierSum << " but the source declares "
            << actual.total << " tests -- some TEST_F sits outside every "
               "TIER-MARKER section, so the doc table cannot be complete";
    }
}

// The historical verification records are dated evidence of past runs and are
// deliberately preserved rather than rewritten. What must NOT survive is a
// stale figure presented as the CURRENT count.
TEST(FlawCorpusCountDocs, HistoricalRecordsArePreservedAndLabelled) {
    const std::string coverage = readFile(projectRoot() / kCorpusCoverage);
    ASSERT_FALSE(coverage.empty());

    EXPECT_TRUE(contains(coverage, "Verification record (2026-07-16)"))
        << "the 2026-07-16 record was deleted; history must be preserved, not erased";
    EXPECT_TRUE(contains(coverage, "Verification record (2026-07-30)"))
        << "the 2026-07-30 Tier-4 record was deleted";
    EXPECT_TRUE(contains(coverage, "Verification record (2026-07-31)"))
        << "the 2026-07-31 #50-fixed record was deleted";
    EXPECT_TRUE(contains(coverage, "Current counts (machine-checked)"))
        << "COVERAGE.md must state its CURRENT counts separately from the dated "
           "history, or a reader takes the newest record as today's truth";
}

// ===========================================================================
// 3b. Flaw-corpus FIXTURE inventory (F16 / PDF-107)
// ===========================================================================
//
// FlawCorpusCountDocs above counts TEST CASES. Nothing counted the FIXTURE
// FILES, and the two are different quantities (several Tier-4 cases share one
// fixture). The consequence was live when F16 looked: 72 .pdf files sat under
// tests/fixtures/flaw_corpus/ while README.md's tables listed 71 --
// encryption/identity_stmf.pdf, added by phase-3 for gap PDF-25, was in the
// corpus but in no table, so a reader auditing "what is in this corpus"
// enumerated the wrong set.
//
// This check makes the README's tables the inventory: every .pdf on disk must
// appear in exactly one marker-delimited table, every row must name a file that
// exists, and the FIXTURE-COUNTS block in BOTH documents must equal the derived
// group sizes and the on-disk total.

namespace {

const fs::path kCorpusFixtureDir = fs::path("tests") / "fixtures" / "flaw_corpus";
const fs::path kPhase16Evidence  = fs::path("docs") / "audit" / "meta-audit-2026-07" /
                                   "followup" / "phase-16";
const fs::path kFixtureInvOut    = kPhase16Evidence / "corpus-fixture-inventory.json";

struct FixtureGroup {
    std::string id;                       // marker id, e.g. "tier-4"
    std::string label;                    // FIXTURE-COUNTS row label
    std::vector<std::string> paths;       // as listed, corpus-relative posix
};

// Rows of a marker-delimited fixture table: cell 0 is a corpus-relative path.
std::vector<std::string> fixtureTablePaths(const std::string& doc, const std::string& id) {
    std::vector<std::string> out;
    for (const auto& line : linesBetween(doc, "<!-- FIXTURE-TABLE-BEGIN: " + id + " -->",
                                              "<!-- FIXTURE-TABLE-END: " + id + " -->")) {
        const auto cells = tableCells(line);
        if (cells.empty()) continue;
        const std::string p = unbacktick(cells[0]);
        if (p.size() < 5) continue;
        if (p.substr(p.size() - 4) != ".pdf") continue;   // header/separator rows
        out.push_back(p);
    }
    return out;
}

std::set<std::string> corpusPdfsOnDisk() {
    std::set<std::string> out;
    std::error_code ec;
    const fs::path root = projectRoot() / kCorpusFixtureDir;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        if (it->path().extension() != ".pdf") continue;
        out.insert(fs::relative(it->path(), root).generic_string());
    }
    return out;
}

std::string joinedVec(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& x : v) { if (!out.empty()) out += ", "; out += x; }
    return out;
}

} // namespace

TEST(FlawCorpusFixtureDocs, EveryFixtureOnDiskIsListedExactlyOnceAndCounted) {
    const std::string readme   = readFile(projectRoot() / kCorpusReadme);
    const std::string coverage = readFile(projectRoot() / kCorpusCoverage);
    ASSERT_FALSE(readme.empty())   << kCorpusReadme.string() << " must be readable";
    ASSERT_FALSE(coverage.empty()) << kCorpusCoverage.string() << " must be readable";

    std::vector<FixtureGroup> groups = {
        { "tiers-1-3",     "Tiers 1-3 fixture files",     {} },
        { "tier-4",        "Tier 4 fixture files",        {} },
        { "supplementary", "Supplementary fixture files", {} },
        // p85 phase-3. The PDF/A + PDF/UA clause corpus (GitHub #8, #14, #7)
        // and the structurally-valid subset of tests/security/corpus/ (#37).
        // A fourth group rather than an extension of `supplementary`: these
        // are not upstream downloads, they are locally generated or copied,
        // and folding them into a table whose other rows all cite an upstream
        // suite would misdescribe their provenance to exactly the reader this
        // inventory exists for.
        { "conformance", "Conformance fixture files", {} },
    };
    for (auto& g : groups) {
        g.paths = fixtureTablePaths(readme, g.id);
        ASSERT_FALSE(g.paths.empty())
            << "no rows parsed from the '" << g.id << "' fixture table in "
            << kCorpusReadme.string() << ". A check that parses nothing passes "
               "vacuously -- the markers <!-- FIXTURE-TABLE-BEGIN: " << g.id
            << " --> / <!-- FIXTURE-TABLE-END: " << g.id << " --> must bracket it.";
    }

    // Listed inventory, with duplicate detection across and within groups.
    std::map<std::string, std::vector<std::string>> owners;   // path -> group ids
    for (const auto& g : groups)
        for (const auto& p : g.paths) owners[p].push_back(g.id);

    std::vector<std::string> duplicated;
    for (const auto& [p, gs] : owners)
        if (gs.size() > 1) duplicated.push_back(p + " (" + joinedVec(gs) + ")");
    EXPECT_TRUE(duplicated.empty())
        << "these fixtures are listed in more than one table, so the per-table "
           "counts double-count them: " << joinedVec(duplicated);

    std::set<std::string> listed;
    for (const auto& [p, gs] : owners) { (void)gs; listed.insert(p); }
    const std::set<std::string> disk = corpusPdfsOnDisk();
    ASSERT_FALSE(disk.empty()) << "no .pdf files found under "
                               << kCorpusFixtureDir.string();

    std::vector<std::string> undocumented, phantom;
    std::set_difference(disk.begin(), disk.end(), listed.begin(), listed.end(),
                        std::back_inserter(undocumented));
    std::set_difference(listed.begin(), listed.end(), disk.begin(), disk.end(),
                        std::back_inserter(phantom));

    long declaredTotalReadme = -1;
    {   // Evidence artifact for the phase gate (criterion 4).
        const auto rdm = parseCountTable(readme, "<!-- FIXTURE-COUNTS-BEGIN -->",
                                                 "<!-- FIXTURE-COUNTS-END -->");
        const auto cov = parseCountTable(coverage, "<!-- FIXTURE-COUNTS-BEGIN -->",
                                                   "<!-- FIXTURE-COUNTS-END -->");
        const auto get = [](const std::map<std::string, long>& m, const std::string& k) {
            const auto it = m.find(k);
            return it == m.end() ? -1L : it->second;
        };
        declaredTotalReadme = get(rdm, "Fixture files on disk, total");
        std::ostringstream js;
        js << "{\n  \"generated_by\": \"FlawCorpusFixtureDocs."
              "EveryFixtureOnDiskIsListedExactlyOnceAndCounted "
              "(tests/test_doc_drift_guards.cpp)\",\n";
        js << "  \"phase\": \"phase-16\",\n  \"gap\": \"PDF-107\",\n";
        js << "  \"corpus_dir\": \"tests/fixtures/flaw_corpus\",\n";
        js << "  \"per_group\": [\n";
        for (size_t i = 0; i < groups.size(); ++i) {
            js << "    {\"group\": \"" << groups[i].id << "\", \"listed\": "
               << groups[i].paths.size()
               << ", \"README.md_declared\": " << get(rdm, groups[i].label)
               << ", \"COVERAGE.md_declared\": " << get(cov, groups[i].label) << "}"
               << (i + 1 < groups.size() ? "," : "") << "\n";
        }
        js << "  ],\n";
        js << "  \"listed_total\": " << listed.size() << ",\n";
        js << "  \"on_disk_total\": " << disk.size() << ",\n";
        js << "  \"README.md_declared_total\": " << declaredTotalReadme << ",\n";
        js << "  \"COVERAGE.md_declared_total\": "
           << get(cov, "Fixture files on disk, total") << ",\n";
        js << "  \"on_disk_not_listed\": \"" << jsonEscape(joinedVec(undocumented)) << "\",\n";
        js << "  \"listed_not_on_disk\": \"" << jsonEscape(joinedVec(phantom)) << "\",\n";
        js << "  \"listed_twice\": \"" << jsonEscape(joinedVec(duplicated)) << "\",\n";
        js << "  \"mismatches\": "
           << (undocumented.size() + phantom.size() + duplicated.size()) << "\n}\n";
        writeEvidence(kFixtureInvOut, js.str());
    }

    EXPECT_TRUE(undocumented.empty())
        << "these .pdf files are in tests/fixtures/flaw_corpus/ but in NO table of "
        << kCorpusReadme.string() << ": " << joinedVec(undocumented)
        << ". identity_stmf.pdf was exactly this, unlisted from phase-3 to F16.";
    EXPECT_TRUE(phantom.empty())
        << kCorpusReadme.string() << " lists fixtures that are not on disk: "
        << joinedVec(phantom);

    for (const auto& [name, doc] : std::vector<std::pair<const char*, const std::string*>>{
             { "README.md", &readme }, { "COVERAGE.md", &coverage } }) {
        const auto declared = parseCountTable(*doc, "<!-- FIXTURE-COUNTS-BEGIN -->",
                                                    "<!-- FIXTURE-COUNTS-END -->");
        ASSERT_FALSE(declared.empty())
            << name << " must carry a FIXTURE-COUNTS block; fixture files are a "
                       "different quantity from test cases and nothing counted them "
                       "before F16";
        long sum = 0;
        for (const auto& g : groups) {
            ASSERT_TRUE(declared.count(g.label))
                << name << " FIXTURE-COUNTS block is missing a '" << g.label << "' row";
            EXPECT_EQ(declared.at(g.label), static_cast<long>(g.paths.size()))
                << name << " declares " << declared.at(g.label) << " " << g.label
                << "; the '" << g.id << "' table in README.md lists " << g.paths.size();
            sum += static_cast<long>(g.paths.size());
        }
        const char* kTotal = "Fixture files on disk, total";
        ASSERT_TRUE(declared.count(kTotal))
            << name << " FIXTURE-COUNTS block is missing the on-disk total row";
        EXPECT_EQ(declared.at(kTotal), static_cast<long>(disk.size()))
            << name << " declares " << declared.at(kTotal)
            << " fixture files on disk; the directory holds " << disk.size();
        EXPECT_EQ(sum, static_cast<long>(disk.size()))
            << "the per-group tables list " << sum << " fixtures but "
            << disk.size() << " .pdf files are on disk";
    }
}

// ===========================================================================
// 3c. Flaw-corpus COVERAGE.md section claims (F16 / PDF-108, PDF-109)
// ===========================================================================
//
// The §8 row used to read: Excluded: bomb-giant "(needs a resource-limited
// sandbox harness)". No such harness existed anywhere in the tree, so the
// stated reason for the exclusion was not true -- it described a dependency on
// work nobody was doing. A coverage matrix whose statuses cite absent things is
// worse than one that says nothing, because it reads as a plan.
//
// This check requires every test file the matrix names to (a) exist and (b) be
// registered in tests/CMakeLists.txt, which is a hand-maintained list, so a
// cited file that never compiles is caught too. It also pins the two statuses
// F16 rewrote so they cannot quietly revert to a claim about work that is not
// there.

namespace {

const fs::path kTestsCMake = fs::path("tests") / "CMakeLists.txt";

// Rows of the "Coverage by reference section" table, keyed by section number.
// .first is the STATUS cell, .second the coverage notes; they are kept apart
// because the rules below differ (a section-level "Deferred" status must carry
// an owner and a date; a "Deferred:" note INSIDE a covered section is a named
// residual item, not an open section, and is out of scope for that rule).
std::map<int, std::pair<std::string, std::string>> coverageSectionRows(
        const std::string& doc) {
    std::map<int, std::pair<std::string, std::string>> rows;
    for (const auto& line : splitLines(doc)) {
        const auto cells = tableCells(line);
        if (cells.size() < 4) continue;
        const std::string id = trim(cells[0]);
        if (id.empty() || !std::all_of(id.begin(), id.end(),
                                       [](unsigned char c) { return std::isdigit(c) != 0; }))
            continue;
        rows[std::stoi(id)] = { cells[2], cells[3] };
    }
    return rows;
}

} // namespace

TEST(FlawCorpusCoverageClaims, EveryCitedTestFileExistsAndIsRegistered) {
    const std::string coverage = readFile(projectRoot() / kCorpusCoverage);
    const std::string cmake    = readFile(projectRoot() / kTestsCMake);
    ASSERT_FALSE(coverage.empty());
    ASSERT_FALSE(cmake.empty()) << kTestsCMake.string() << " must be readable";

    const auto rows = coverageSectionRows(coverage);
    ASSERT_GE(rows.size(), 13u)
        << "parsed only " << rows.size() << " section rows from "
        << kCorpusCoverage.string() << "; the coverage matrix shape changed under "
           "this check and it would otherwise pass vacuously";

    // Every tests/*.cpp the matrix cites must exist AND be in the explicit
    // source list. tests/CMakeLists.txt is not a glob: an unregistered file is
    // a test that never runs, cited as if it did.
    const std::regex citeRe(R"((tests/[A-Za-z0-9_./-]+\.cpp))");
    size_t checked = 0;
    for (const auto& [section, cells] : rows) {
        const std::string body = cells.first + " | " + cells.second;
        for (auto it = std::sregex_iterator(body.begin(), body.end(), citeRe);
             it != std::sregex_iterator(); ++it) {
            const std::string rel = (*it)[1].str();
            ++checked;
            EXPECT_TRUE(fs::exists(projectRoot() / rel))
                << "COVERAGE.md §" << section << " cites " << rel
                << ", which is not in the tree";
            const std::string base = fs::path(rel).filename().string();
            EXPECT_TRUE(contains(cmake, base))
                << "COVERAGE.md §" << section << " cites " << rel
                << ", which is absent from " << kTestsCMake.string()
                << " -- the list is explicit, so that test never runs";
        }
    }
    EXPECT_GT(checked, 0u) << "no test-file citations parsed out of the matrix";
}

TEST(FlawCorpusCoverageClaims, BombExclusionRestsOnAControlThatExists) {
    const std::string coverage = readFile(projectRoot() / kCorpusCoverage);
    ASSERT_FALSE(coverage.empty());
    const auto rows = coverageSectionRows(coverage);
    ASSERT_TRUE(rows.count(8)) << "COVERAGE.md has no §8 row";
    const std::string row = rows.at(8).first + " | " + rows.at(8).second;

    EXPECT_FALSE(contains(row, "needs a resource-limited sandbox harness"))
        << "the §8 bomb exclusion again cites a sandbox harness as missing "
           "future work. tests/test_resource_bomb_sandbox.cpp exists; gap "
           "PDF-109 was exactly this false rationale.";
    EXPECT_TRUE(contains(row, "tests/test_resource_bomb_sandbox.cpp"))
        << "§8 must name the harness its bomb position now rests on";
    EXPECT_TRUE(contains(row, "gen_decompression_bomb.py"))
        << "§8 must name the generator of the bomb fixture it relies on, or the "
           "fixture cannot be reproduced or audited";

    EXPECT_TRUE(fs::exists(projectRoot() / "tests" / "test_resource_bomb_sandbox.cpp"));
    EXPECT_TRUE(fs::exists(projectRoot() / "tests" / "fixtures" /
                           "gen_decompression_bomb.py"));
    EXPECT_TRUE(fs::exists(projectRoot() / "tests" / "fixtures" / "flaw_corpus" /
                           "corrupt" / "decompression-bomb.pdf"));
}

// A deferral is legitimate; an OPEN-ENDED one is how §4 and §11 sat untouched
// from the corpus's first day. F16 closed §4 and re-recorded §11 with a date
// and a named owner, and this keeps that shape.
TEST(FlawCorpusCoverageClaims, RemainingDeferralsCarryADateAndAnOwner) {
    const std::string coverage = readFile(projectRoot() / kCorpusCoverage);
    ASSERT_FALSE(coverage.empty());
    const auto rows = coverageSectionRows(coverage);

    for (const auto& [section, cells] : rows) {
        const std::string& body = cells.first;      // STATUS cell only
        if (!contains(body, "Deferred")) continue;
        EXPECT_TRUE(contains(body, "owner "))
            << "COVERAGE.md §" << section << " is Deferred with no named owner";
        EXPECT_TRUE(std::regex_search(body, std::regex(R"(20\d\d-\d\d-\d\d)")))
            << "COVERAGE.md §" << section << " is Deferred with no date; an "
               "undated deferral never comes back";
    }

    ASSERT_TRUE(rows.count(4));
    const std::string row4 = rows.at(4).first + " | " + rows.at(4).second;
    EXPECT_FALSE(contains(row4, "needs its own harness"))
        << "§4's retired blocker ('needs its own harness, not open/render/"
           "no-throw') is back, but tests/test_layout_extraction_corpus.cpp is "
           "that harness (gap PDF-107)";
    EXPECT_TRUE(contains(row4, "tests/test_layout_extraction_corpus.cpp"));
}

// ===========================================================================
// 4. Threat-model source consolidation (SEC-107)
// ===========================================================================

TEST(ThreatModelSourceDocs, SecurityTestingIsTheSingleCanonicalThreatModel) {
    const std::string doc = readFile(projectRoot() / kSecurityTesting);
    ASSERT_FALSE(doc.empty());
    EXPECT_TRUE(contains(doc, "single canonical threat model"))
        << "docs/security-testing.md §(a) must declare itself the one canonical "
           "threat model, or 'which document is the threat model' is open again";
    EXPECT_TRUE(contains(doc, "## (a) Threat model"))
        << "§(a) is the threat model; the heading must survive";
}

// F9 chose OPTION B for SEC-107: stop citing compliance-architecture.md as a
// threat-model source rather than invent threat content for a PDF/A
// implementation plan. This asserts the choice holds.
//
// Scope note: docs/audit/** and docs/archive/** are EXCLUDED. Those files
// record the finding itself (engine-security-gaps.md quotes the miscitation
// verbatim) and rewriting an audit's own wording would be falsifying history.
TEST(ThreatModelSourceDocs, ComplianceArchitectureIsNotCitedAsAThreatModelSource) {
    const std::string arch = readFile(projectRoot() / kComplianceArch);
    ASSERT_FALSE(arch.empty()) << kComplianceArch.string() << " must be readable";

    // The document itself must carry the disclaimer, so a reader arriving
    // directly is not misled either.
    EXPECT_TRUE(contains(arch, "NOT A THREAT-MODEL SOURCE"))
        << "docs/compliance-architecture.md must state that it is not a "
           "threat-model source (SEC-107)";

    const auto docs = nonAuditDocs();
    ASSERT_FALSE(docs.empty()) << "no non-audit docs found -- the sweep would "
                                  "vacuously pass";

    std::vector<std::string> offenders;
    for (const auto& p : docs) {
        if (fs::equivalent(p, projectRoot() / kComplianceArch)) continue;
        const std::string body = readFile(p);
        size_t at = 0;
        while ((at = body.find("compliance-architecture", at)) != std::string::npos) {
            const size_t lo = at > 400 ? at - 400 : 0;
            const size_t len = std::min<size_t>(body.size() - lo, 800);
            const std::string window = body.substr(lo, len);
            const bool nearThreatModel =
                contains(window, "threat model") || contains(window, "threat-model") ||
                contains(window, "Threat model") || contains(window, "Threat-model") ||
                contains(window, "THREAT-MODEL");
            // A NEGATIVE mention -- "this is not a threat-model source" -- is the
            // correction, not the defect. These phrases are the only accepted
            // negations; a bare citation next to threat-model language still
            // fails. Keep this list short and explicit: every entry is a hole.
            const bool negated =
                contains(window, "NOT A THREAT-MODEL SOURCE") ||
                contains(window, "is not a threat model") ||
                contains(window, "neither is") ||
                contains(window, "miscounted as threat-model coverage") ||
                contains(window, "stop citing");
            if (nearThreatModel && !negated) {
                offenders.push_back(fs::relative(p, projectRoot()).generic_string());
                break;
            }
            at += 1;
        }
    }

    std::string list;
    for (const auto& o : offenders) { if (!list.empty()) list += ", "; list += o; }
    EXPECT_TRUE(offenders.empty())
        << "docs/compliance-architecture.md is cited near threat-model language in: "
        << list << ". F9 removed it from the threat-model source list; the single "
           "canonical location is docs/security-testing.md §(a).";
}

// ===========================================================================
// 5. Security-document index and cross-references (SEC-91, SEC-97, SEC-87, SEC-82)
// ===========================================================================

TEST(SecurityDocIndex, RootSecurityMdResolvesToTheRealReportingProcess) {
    const std::string sec = readFile(projectRoot() / kRootSecurityMd);
    ASSERT_FALSE(sec.empty())
        << "SECURITY.md must exist at the repo root so GitHub's Security tab and "
           "the 'Report a vulnerability' affordance resolve to a real process "
           "(gap SEC-91)";

    EXPECT_TRUE(contains(sec, "docs/security-policy.md"))
        << "SECURITY.md must link to the authoritative policy, not restate it";
    EXPECT_TRUE(contains(sec, "security/advisories/new"))
        << "SECURITY.md must give the private-advisory reporting URL";

    const std::string policy = readFile(projectRoot() / kSecurityPolicy);
    ASSERT_FALSE(policy.empty());
    EXPECT_TRUE(contains(policy, "## Reporting a security issue"))
        << "SECURITY.md points at docs/security-policy.md §'Reporting a security "
           "issue'; that section must still exist";
}

TEST(SecurityDocIndex, SecurityPolicyDocumentsTheJpxOptIn) {
    const std::string policy = readFile(projectRoot() / kSecurityPolicy);
    ASSERT_FALSE(policy.empty());

    // Gap SEC-97: the JPX control shipped with ZERO hits in any security doc.
    EXPECT_TRUE(contains(policy, "Security/jpxRenderingEnabled"))
        << "the JPX opt-in setting key must appear in the user-facing policy";
    EXPECT_TRUE(contains(policy, "JPEG 2000") || contains(policy, "JPEG2000"))
        << "the JPX control must be described, not just keyed";
    EXPECT_TRUE(contains(policy, "## What requires user opt-in"))
        << "the opt-in section the JPX control belongs to must exist";
    EXPECT_TRUE(contains(policy, "0011-jpx-openjpeg-heap-overflow"))
        << "the policy must cite the DCA that justifies default-deny, so the "
           "control cannot be relaxed by someone who never learns why it exists";

    // The version-history table must record the change, not just the body.
    EXPECT_TRUE(contains(policy, "## Version history"));
    EXPECT_TRUE(contains(policy, "| 0.1.3 |"))
        << "adding a control to the policy without a version-history row is how "
           "the table's last entry got stuck at 0.1.1 (SEC-97)";
}

TEST(SecurityDocIndex, DependencyBumpRunbookExistsAndPortfilePointsAtIt) {
    const std::string policy   = readFile(projectRoot() / kSecurityPolicy);
    const std::string portfile = readFile(projectRoot() / kPortfile);
    ASSERT_FALSE(policy.empty());
    ASSERT_FALSE(portfile.empty()) << kPortfile.string() << " must be readable";

    EXPECT_TRUE(contains(policy, "Dependency bump runbook"))
        << "gap SEC-87: bumping MuPDF/QPDF must have a written procedure, because "
           "the overlay ports carry security patches a naive bump silently drops";
    EXPECT_TRUE(contains(policy, "port-version"))
        << "the runbook must name the port-version bump; without it vcpkg serves "
           "the previously built binary and the patch never ships";

    // The pointer must exist where someone doing the bump will actually be.
    EXPECT_TRUE(contains(portfile, "0001-pdfe-jpx-decode-gate.patch"))
        << "the JPX decode-gate patch must still be applied by the overlay port -- "
           "removing it re-opens the #50 heap-overflow path";
    EXPECT_TRUE(contains(portfile, "Dependency bump runbook"))
        << "portfile.cmake must reference the runbook (SEC-87); the person editing "
           "VERSION/SHA512 reads this file, not the security policy";
}

// Gap SEC-82: hardcoded suite-size figures in security docs go stale silently.
// The specific number that was wrong is banned; a general "no digits" rule
// would be unenforceable, so this guards the known-bad literals and the
// documents they lived in.
TEST(SecurityDocIndex, NoStaleHardcodedSuiteCountSurvives) {
    struct Target { fs::path path; const char* why; };
    const std::vector<Target> targets = {
        { kSecurityTesting, "docs/security-testing.md §(b) claimed a fixed 2068-test suite" },
        { kDepsTriage,      "tests/security/deps/triage.md step 4 claimed the same figure" },
    };
    for (const auto& t : targets) {
        const std::string body = readFile(projectRoot() / t.path);
        ASSERT_FALSE(body.empty()) << t.path.string() << " must be readable";
        // The corrections explain what the figure USED to be; that mention is
        // in a parenthetical that names the gap. A bare re-assertion is not.
        const std::regex bare(R"(the (regular|existing) 2068-test)");
        EXPECT_FALSE(std::regex_search(body, bare))
            << t.why << "; it is back. State the live figure's source "
               "(`ctest -N`) rather than a number nothing checks.";
    }
}

// tests/security/deps/triage.md carried the SAME heading twice with
// CONTRADICTORY bodies -- one saying the MuPDF/QPDF advisory check was
// verified by a live osv-scanner run, the other saying it was unverified.
// (Gap SEC-80. The uniqueness lint proper lives in test_ci_supplychain.cpp,
// next to the other supply-chain oracles; this asserts the contradiction
// itself is gone.)
TEST(SecurityDocIndex, DepsTriageNoLongerContradictsItself) {
    const std::string body = readFile(projectRoot() / kDepsTriage);
    ASSERT_FALSE(body.empty());
    EXPECT_FALSE(contains(body, "but **this is unverified** until"))
        << "the stale duplicate advisory-check section is back in "
        << kDepsTriage.string() << "; it contradicts the verified one above it "
           "(gap SEC-80)";
    EXPECT_TRUE(contains(body, "verified by the live osv-scanner"))
        << "the VERIFIED advisory-check record must be the one that survives";
}

// ===========================================================================
// Meta-audit follow-up F17 — gaps A1-18, A1-19, A1-71, A1-81.
//
// Four accepted-but-never-landed recommendations from the original core audit.
// Each fix is a one-line change somewhere nothing re-reads, so each is paired
// with an oracle that re-derives the claim from the file it rests on.
// ===========================================================================

namespace {
const fs::path kGateEvidenceScript =
    fs::path("docs") / "audit" / "phase1-evidence" / "verify_gate_hardening.sh";
const fs::path kGateEvidenceReadme =
    fs::path("docs") / "audit" / "phase1-evidence" / "README.md";
const fs::path kValidationWorkflow =
    fs::path(".github") / "workflows" / "windows-validation.yml";
} // namespace

// A1-18 — the AC1/AC6 evidence harness hardcoded "$REPO/.claude/hooks/
// phase-gate.sh". The project-scoped hooks were later removed, so every run
// died with "FATAL: hook not found" while the README still presented the
// evidence as standing. The harness must resolve the hook from either the
// repo-local or the user-global directory.
TEST(GateEvidenceHarness, ResolvesTheHookFromEitherHookDirectory) {
    const std::string body = readFile(projectRoot() / kGateEvidenceScript);
    ASSERT_FALSE(body.empty()) << kGateEvidenceScript.string() << " is missing";

    EXPECT_TRUE(contains(body, ".claude/hooks/phase-gate.sh"))
        << "the harness no longer names the repo-local hook path at all";
    EXPECT_TRUE(contains(body, "CLAUDE_HOOKS_DIR") &&
                contains(body, "$HOME/.claude/hooks"))
        << "the harness must fall back to the user-global hook directory "
           "(overridable via CLAUDE_HOOKS_DIR); with only the repo-local path "
           "it aborts on every run and the AC1/AC6 evidence is unreproducible "
           "(gap A1-18)";

    // The old failure mode was an unconditional abort keyed to the single
    // hardcoded path. Assert that shape is gone.
    const std::regex hardcodedAbort(
        R"(HOOK="\$REPO/\.claude/hooks/phase-gate\.sh"[\s\S]{0,120}exit 3)");
    EXPECT_FALSE(std::regex_search(body, hardcodedAbort))
        << "the harness is back to aborting on the hardcoded repo-local path";
}

// A1-19 — the "Known residual" paragraph was inverted: it called the
// user-global hook "legacy" and the repo-local one authoritative, when the
// repo-local one no longer exists. The document must agree with the tree.
TEST(GateEvidenceHarness, KnownResidualMatchesWhichHookActuallyExists) {
    const std::string body = readFile(projectRoot() / kGateEvidenceReadme);
    ASSERT_FALSE(body.empty()) << kGateEvidenceReadme.string() << " is missing";

    const bool repoLocalHookExists =
        fs::exists(projectRoot() / ".claude" / "hooks" / "phase-gate.sh");

    // The exact inverted sentence that shipped, verbatim.
    const bool claimsRepoLocalAuthoritative =
        contains(body, "For PDFE the repo-local hook\nis **authoritative**") ||
        contains(body, "For PDFE the repo-local hook is **authoritative**");

    if (!repoLocalHookExists) {
        EXPECT_FALSE(claimsRepoLocalAuthoritative)
            << kGateEvidenceReadme.string()
            << " says the repo-local hook is authoritative, but "
               ".claude/hooks/phase-gate.sh does not exist in this tree "
               "(gap A1-19)";
        EXPECT_TRUE(contains(body, "user-global hook is now the **only**") ||
                    contains(body, "the only\nhook"))
            << "with no repo-local hook the residual section must say the "
               "user-global hook is the only one in force";
    } else {
        EXPECT_TRUE(claimsRepoLocalAuthoritative)
            << "a repo-local hook exists again; the residual section must be "
               "updated to describe it (gap A1-19)";
    }
}

// A1-71 (UI-HYGIENE-01) — cancel-in-progress on the ~9h single-runner
// validation lane destroys a run that is hours in (and wedges the runner).
TEST(ValidationLaneHygiene, LongSweepDoesNotCancelItselfInProgress) {
    const std::string body = readFile(projectRoot() / kValidationWorkflow);
    ASSERT_FALSE(body.empty()) << kValidationWorkflow.string() << " is missing";

    const auto pos = body.find("concurrency:");
    ASSERT_NE(pos, std::string::npos)
        << "the concurrency block is gone from the validation lane";
    const std::string block = body.substr(pos, 1500);
    EXPECT_FALSE(contains(block, "cancel-in-progress: true"))
        << "windows-validation.yml sets cancel-in-progress true again "
           "(gap A1-71)";
    EXPECT_TRUE(contains(block, "cancel-in-progress: false"))
        << "windows-validation.yml sets cancel-in-progress true again. This "
           "lane is the ~9-hour full sweep on a single self-hosted runner; a "
           "new push then destroys hours of the only evidence it produces "
           "(gap A1-71).";
}

// A1-81 (C-3) — the matrix-health gate on the full-sweep lane tolerated a
// five-row green drop and did not require the rows to have actually run.
TEST(ValidationLaneHygiene, MatrixHealthGateIsFullSweepStrict) {
    const std::string body = readFile(projectRoot() / kValidationWorkflow);
    ASSERT_FALSE(body.empty());

    // The last mention is the invocation itself; earlier ones are the
    // explanatory comment above it (which legitimately quotes the old flags).
    const auto pos = body.rfind("check_matrix_health.py");
    ASSERT_NE(pos, std::string::npos)
        << "the matrix-health gate step has been removed from the validation "
           "lane";
    const std::string step = body.substr(pos, 400);

    EXPECT_FALSE(std::regex_search(step, std::regex(R"(--floor-delta\s+[1-9])")))
        << "the matrix-health gate tolerates a green-row drop again. This step "
           "runs only on the full sweep, so any drop is a real coverage loss "
           "to triage, not a tolerance band (gap A1-81).";
    EXPECT_TRUE(contains(step, "--require-ran"))
        << "the full-sweep matrix-health gate must pass --require-ran, or a "
           "committed-green row that never executed counts as 'not red' "
           "(gap A1-81 / A1-83).";
}

// GitHub #142 — the matrix-health gate read ONLY committed-`green` rows, so a
// row committed as `planned` was invisible to it rather than merely
// unsatisfied. The gate could ratify finished work and could say nothing at
// all about work not yet started.
//
// The ceiling is a LITERAL in the workflow, deliberately: a ceiling derived
// from the matrix it checks would compare the file against itself and could
// never fail. This guard is what keeps that literal honest — it re-derives
// the real count from the committed CSV on every suite run, so the workflow
// number and the matrix cannot drift apart, and it runs in the C++ suite
// rather than only on the ~9-hour sweep the flag itself sits on.
TEST(ValidationLaneHygiene, MatrixHealthGateRatchetsPlannedRows) {
    const std::string body = readFile(projectRoot() / kValidationWorkflow);
    ASSERT_FALSE(body.empty());

    const auto pos = body.rfind("check_matrix_health.py");
    ASSERT_NE(pos, std::string::npos)
        << "the matrix-health gate step has been removed from the validation "
           "lane";
    const std::string step = body.substr(pos, 600);

    std::smatch m;
    ASSERT_TRUE(std::regex_search(step, m,
                                  std::regex(R"(--max-planned\s+(\d+))")))
        << "the full-sweep matrix-health gate no longer pins a ceiling on "
           "committed 'planned' rows, so un-started scaffolding can "
           "accumulate invisibly again (GitHub #142).";
    const int ceiling = std::stoi(m[1].str());

    EXPECT_TRUE(contains(step, "--fail-on-promotable"))
        << "without --fail-on-promotable a row committed as planned/blocked "
           "that records green in a live sweep is an unexpected pass that "
           "nothing announces — the strict-xfail lesson, at the matrix layer "
           "(GitHub #142).";

    // Re-derive the real count from the committed matrix.
    const fs::path matrix = projectRoot() / "tests" / "ui_automation" /
                            "matrix" / "ui_test_matrix.csv";
    const std::string csv = readFile(matrix);
    ASSERT_FALSE(csv.empty()) << matrix.string() << " is missing";

    std::istringstream in(csv);
    std::string header;
    ASSERT_TRUE(std::getline(in, header));
    // Locate the status column by NAME. A hardcoded index silently reads the
    // wrong column the day someone inserts one, and would then report 0
    // planned rows for ever.
    int statusCol = -1;
    {
        std::istringstream hs(header);
        std::string field;
        int idx = 0;
        while (std::getline(hs, field, ',')) {
            if (!field.empty() && field.back() == '\r') field.pop_back();
            if (field == "status") { statusCol = idx; break; }
            ++idx;
        }
    }
    ASSERT_GE(statusCol, 0) << "no 'status' column in " << matrix.string();

    int planned = 0, green = 0, rows = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        // The matrix has quoted fields containing commas; split respecting
        // double quotes rather than on every comma.
        std::vector<std::string> fields;
        std::string cur;
        bool inQuotes = false;
        for (char c : line) {
            if (c == '"') { inQuotes = !inQuotes; continue; }
            if (c == ',' && !inQuotes) { fields.push_back(cur); cur.clear(); continue; }
            cur.push_back(c);
        }
        fields.push_back(cur);
        if (static_cast<int>(fields.size()) <= statusCol) continue;
        ++rows;
        const std::string& s = fields[statusCol];
        if (s == "planned") ++planned;
        if (s == "green") ++green;
    }

    // NON-VACUITY. A parser that returned nothing would report 0 planned rows
    // and pass any ceiling. Both floors are far below the real figures and
    // exist only to prove the parse happened at all.
    //
    // The figures here are a DATED RECORD, deliberately not re-derived: as of
    // 2026-09-08 the matrix held 787 rows / 720 green. GitHub #175 counted
    // nine transcribed copies of those numbers of which one was checked; this
    // one is a comment illustrating why the floors are 100, so it is pinned to
    // a date rather than either drifting silently or being machine-checked
    // (the floors themselves are what this test asserts, and they are
    // deliberately insensitive to the exact counts).
    ASSERT_GT(rows, 100) << "the matrix parse produced almost nothing — this "
                            "guard's verdict would be meaningless";
    ASSERT_GT(green, 100) << "no green rows parsed; the status column is not "
                             "being read correctly";

    EXPECT_LE(planned, ceiling)
        << "the committed matrix holds " << planned << " 'planned' rows but "
        << kValidationWorkflow.string() << " pins the ceiling at " << ceiling
        << ". Either implement the rows or raise the ceiling deliberately — "
           "the point of GitHub #142 is that this number is never allowed to "
           "grow without someone seeing it.";
}

// ===========================================================================
// Meta-audit follow-up F17 — gaps A1-64, A1-66, A1-67, A1-68, A1-70, A1-83,
// A1-100.
//
// These seven live in the Python UI/visual harness, which the GoogleTest suite
// does not execute. Their own pytest unit tests are the primary evidence; the
// guards below are the RATCHET that keeps the repaired behaviour from being
// quietly reverted in a lane the C++ suite does run. Each re-derives its claim
// from the harness source, in the text-over-source style of this file.
// ===========================================================================

namespace {
const fs::path kRenderOracle =
    fs::path("tests") / "ui_automation" / "harness" / "render_oracle.py";
const fs::path kVerifyMatrix =
    fs::path("tests") / "ui_automation" / "matrix" / "verify_matrix.py";
const fs::path kCoverageResolver =
    fs::path("tests") / "ui_automation" / "matrix" / "coverage_resolver.py";
} // namespace

// A1-68 (UI-ORACLE-02) — `matches_golden` used to seed a missing golden and
// return True, so a broken first capture became the defended baseline and the
// comparison "passed" having compared nothing.
TEST(HarnessOracleIntegrity, MatchesGoldenDoesNotSelfSeed) {
    const std::string body = readFile(projectRoot() / kRenderOracle);
    ASSERT_FALSE(body.empty()) << kRenderOracle.string() << " is missing";

    // Strip comment lines first: the file documents the defect it replaced,
    // quoting the old code verbatim, and that quotation must not be mistaken
    // for the code itself.
    std::string code;
    for (const auto& line : splitLines(body)) {
        const std::string t = trim(line);
        if (!t.empty() && t.front() == '#') continue;
        code += line;
        code += '\n';
    }

    // The exact self-poisoning shape: a not-exists branch that saves and
    // returns a truthy verdict with no gate in between.
    const std::regex selfSeed(
        R"(not\s+p\.exists\(\)\s*:\s*\n\s*save_golden[\s\S]{0,80}return True)");
    EXPECT_FALSE(std::regex_search(code, selfSeed))
        << "matches_golden self-seeds a missing golden and returns True again "
           "(gap A1-68). A first capture that is already wrong then becomes "
           "the baseline every later run is compared against.";

    EXPECT_TRUE(contains(code, "raise GoldenMissingError"))
        << "a missing golden must RAISE by default, not pass";
    EXPECT_TRUE(contains(body, "seed_golden"))
        << "seeding must exist as an EXPLICIT, separately named route";
    EXPECT_TRUE(contains(body, "SEEDED_GOLDENS"))
        << "a seeding run must be distinguishable from a verification run; "
           "the seed ledger is what makes that visible";
}

// A1-70 (UI-MATRIX-02) — verify_matrix.py validated only id uniqueness and
// enum validity, so a row could claim green while naming no test at all.
TEST(HarnessOracleIntegrity, MatrixVerifierResolvesRowsToRealTests) {
    const std::string verifier = readFile(projectRoot() / kVerifyMatrix);
    ASSERT_FALSE(verifier.empty()) << kVerifyMatrix.string() << " is missing";
    const std::string resolver = readFile(projectRoot() / kCoverageResolver);
    ASSERT_FALSE(resolver.empty())
        << kCoverageResolver.string()
        << " is missing — the row->test resolution that gap A1-70 asked for "
           "has been removed";

    EXPECT_TRUE(contains(verifier, "coverage_resolver") ||
                contains(verifier, "resolve_coverage"))
        << "verify_matrix.py no longer cross-checks rows against real tests "
           "(gap A1-70); it is back to checking only ids and enums";

    // A claim must not resolve to a test that cannot run.
    EXPECT_TRUE(contains(resolver, "DISABLED_"))
        << "the resolver must refuse DISABLED_ gtests as coverage";
    EXPECT_TRUE(contains(resolver, "skip"))
        << "the resolver must refuse blanket-skipped pytest cases as coverage";
}

// A1-64 / A1-83 — the two WinAppDriver suites skipped every run behind a
// version guard that could not work (it read the version from the wrong place
// and returned None, so the floor never applied), and a skip was reported the
// same way as a pass.
TEST(HarnessOracleIntegrity, NotRunIsReportedSeparatelyFromPassing) {
    const fs::path notrun = projectRoot() / "tests" / "notrun.py";
    ASSERT_TRUE(fs::exists(notrun))
        << "tests/notrun.py is gone — a skipped UI suite is once again "
           "indistinguishable from one that ran and asserted (gaps A1-64, "
           "A1-83)";
    const std::string body = readFile(notrun);
    EXPECT_TRUE(contains(body, "not_run"))
        << "the NOT-RUN ledger must name its own status";

    const fs::path driver = projectRoot() / "tests" / "ui_automation" /
                            "helpers" / "winappdriver_dialog_driver.py";
    const std::string drv = readFile(driver);
    ASSERT_FALSE(drv.empty()) << driver.string() << " is missing";
    EXPECT_TRUE(contains(drv, "parse_status_version"))
        << "the WinAppDriver version probe must parse /status through the "
           "tested helper; the inline `value.build` read returned None on "
           "every real 1.2.x service, so the version floor was unenforceable "
           "(gap A1-64)";
}

// A1-66 / A1-100 / A1-67 — the visual-baseline set was partly unseeded and
// inert, and the matrix's oracle mix was never measured.
TEST(HarnessOracleIntegrity, VisualBaselineAndOracleMixAreMeasured) {
    const fs::path inv =
        projectRoot() / "tests" / "visual" / "baseline_inventory.py";
    EXPECT_TRUE(fs::exists(inv))
        << "tests/visual/baseline_inventory.py is gone; the seeded-vs-unseeded "
           "canvas count (8 of 14 at F17) is unmeasured again (gaps A1-66, "
           "A1-100)";

    const fs::path kinds = projectRoot() / "tests" / "ui_automation" /
                           "matrix" / "oracle_kinds.py";
    EXPECT_TRUE(fs::exists(kinds))
        << "tests/ui_automation/matrix/oracle_kinds.py is gone; the "
           "structural-vs-pixel oracle mix (gap A1-67) is unmeasured again";
}

// ===========================================================================
// Meta-audit follow-up F17 — gap A1-108 ("RTM kept live", P2 recommendation 9).
//
// docs/audit/rtm-coverage.md's newest section was "Phase 5" while round-2,
// round-3, windows-fixes and the JPX opt-in had all shipped. The
// recommendation was accepted and then relied on somebody remembering it.
// This guard turns it into a mechanism: the RTM must name the follow-up phase
// it reflects, and that phase must be the newest one whose dispositions file
// exists in the tree. Landing phase-N+1's dispositions without re-reading the
// RTM fails here.
// ===========================================================================

TEST(RtmLiveness, RtmNamesTheNewestFollowupPhaseItReflects) {
    const fs::path rtmPath = fs::path("docs") / "audit" / "rtm-coverage.md";
    const std::string rtm = readFile(projectRoot() / rtmPath);
    ASSERT_FALSE(rtm.empty()) << rtmPath.string() << " is missing";

    // Newest phase-N-dispositions.md under the follow-up directory.
    const fs::path followup = projectRoot() / "docs" / "audit" /
                              "meta-audit-2026-07" / "followup";
    ASSERT_TRUE(fs::exists(followup)) << followup.string();
    int newest = -1;
    const std::regex dispRe(R"(^phase-(\d+)-dispositions\.md$)");
    for (const auto& e : fs::directory_iterator(followup)) {
        if (!e.is_regular_file()) continue;
        std::smatch m;
        const std::string name = e.path().filename().string();
        if (std::regex_match(name, m, dispRe))
            newest = std::max(newest, std::stoi(m[1].str()));
    }
    ASSERT_GT(newest, 0) << "no phase-N-dispositions.md found under "
                         << followup.string();

    const std::string marker =
        "**Reflects follow-up phase: " + std::to_string(newest) + ".**";
    EXPECT_TRUE(contains(rtm, marker))
        << rtmPath.string() << " does not carry the marker\n  " << marker
        << "\nPhase " << newest
        << " has landed its dispositions, so the RTM is behind again "
           "(gap A1-108). Re-read the RTM against what that phase changed, "
           "record the row changes, and advance the marker. Do not advance "
           "the marker alone.";
}

// Checking only the NEWEST marker leaves a hole this guard cannot see: F23
// shipped with no RTM re-read, and advancing the marker to 24 made the check
// above green while 23 stayed unrecorded. A guard that can be satisfied by
// moving past the omission does not detect the omission. Every phase from the
// convention's adoption onward must carry its own marker.
TEST(RtmLiveness, EveryFollowupPhaseSinceTheConventionCarriesItsOwnMarker) {
    // F17 installed this guard and wrote the first marker; phases before it
    // predate the convention and are deliberately out of scope.
    constexpr int kFirstMarkedPhase = 17;

    const fs::path rtmPath = fs::path("docs") / "audit" / "rtm-coverage.md";
    const std::string rtm = readFile(projectRoot() / rtmPath);
    ASSERT_FALSE(rtm.empty()) << rtmPath.string() << " is missing";

    const fs::path followup = projectRoot() / "docs" / "audit" /
                              "meta-audit-2026-07" / "followup";
    ASSERT_TRUE(fs::exists(followup)) << followup.string();

    std::vector<int> phases;
    const std::regex dispRe(R"(^phase-(\d+)-dispositions\.md$)");
    for (const auto& e : fs::directory_iterator(followup)) {
        if (!e.is_regular_file()) continue;
        std::smatch m;
        const std::string name = e.path().filename().string();
        if (std::regex_match(name, m, dispRe)) {
            const int n = std::stoi(m[1].str());
            if (n >= kFirstMarkedPhase) phases.push_back(n);
        }
    }
    ASSERT_FALSE(phases.empty())
        << "no phase-" << kFirstMarkedPhase
        << "+-dispositions.md found under " << followup.string();
    std::sort(phases.begin(), phases.end());

    // The marker must be a real section heading, not a mention of the string
    // inside prose. F24 discussed the missing phase-23 marker in a paragraph;
    // a substring search over the whole document would have matched that
    // discussion and reported the hole as filled by the text describing it.
    // readFile() is binary and core.autocrlf=true, so a tracked file is CRLF on
    // disk. Matching a "\n"-prefixed literal against those bytes fails after any
    // checkout while passing in the session that wrote the file — hazard H3, and
    // the exact defect F20 found in F19DocClaims. Strip CR before matching.
    std::string crlfNormalised = rtm;
    crlfNormalised.erase(
        std::remove(crlfNormalised.begin(), crlfNormalised.end(), '\r'),
        crlfNormalised.end());
    std::vector<int> missing;
    for (int n : phases) {
        const std::string lineStart =
            "\n**Reflects follow-up phase: " + std::to_string(n) + ".**";
        if (crlfNormalised.find(lineStart) == std::string::npos)
            missing.push_back(n);
    }

    std::string detail;
    for (int n : missing) detail += " " + std::to_string(n);
    EXPECT_TRUE(missing.empty())
        << rtmPath.string() << " is missing a re-read marker at the START OF A"
        << " LINE for follow-up phase(s):" << detail
        << "\nEach of those phases landed a phase-N-dispositions.md without a"
           " corresponding RTM re-read (gap A1-108). Re-read the RTM against"
           " what that phase changed and record the row changes; do not add"
           " the marker alone, and do not rely on the newest-marker check"
           " above, which by construction cannot see a hole in the middle.";
}

// ---------------------------------------------------------------------------
// F25 / gap WIN-45 — reachability failures may never be reported as skips
// ---------------------------------------------------------------------------
// A UIA/pipe lookup that fails is exactly the defect the live-UI test exists to
// catch. Reporting it with pytest.skip renders it as a green session: the
// false-green shape WIN-45 is about. F11 converted the sites spelled
// "not reachable via UIA"; it keyed on that literal phrase, so the far more
// common "not reachable." spelling dodged the grep entirely and 218 sites
// survived across 27 files. F25 converted those.
//
// F25's own first sweep then repeated the mistake in miniature: keyed on
// "not reachable", it missed 7 further sites spelled "unreachable" (one word).
//
// The karen gate on F25 then caught the SAME failure mode a THIRD time, in a
// disguise neither sweep could see: at 3 sites the reachability wording lived
// in the record_matrix_result message while the pytest.skip message said
// something innocuous ("atlas chain prerequisite failed"). Both the sweep and
// the first version of THIS guard inspected only the skip's own argument, so
// both were blind to it. Correcting the detection rule then surfaced a FOURTH
// spelling of the same event — "absent from / not in / missing from [the] UIA
// tree" — at 20 further sites. 23 sites converted in that pass.
//
// This guard therefore matches the CONCEPT rather than any phrase, and looks at
// the record_matrix_result beside the skip as well as the skip itself. There is
// no allowlist because there are no
// survivors: every site classified as a UIA-lookup failure was converted and
// the after-count is zero, which is asserted directly below. If a future phase
// ever needs a genuine environment-gated skip here, it must justify the site in
// phase-25-dispositions.md AND amend this guard deliberately — the empty
// expectation is the point.

namespace {

// Extract the argument text of a call whose '(' sits at `openParen`.
// String-literal aware so a ')' or a '#' inside a message cannot end the scan
// early, and so text AFTER the call (a trailing comment, the next statement)
// can never be mistaken for part of the skip message.
std::string extractCallArgs(const std::string& body, std::size_t openParen,
                            std::size_t* endOut = nullptr) {
    std::size_t i = openParen + 1;
    int depth = 1;
    const std::size_t begin = i;
    while (i < body.size() && depth > 0) {
        const char c = body[i];
        if (c == '\'' || c == '"') {
            // Determine single vs triple quote, then skip the whole literal.
            const bool triple = (i + 2 < body.size() && body[i + 1] == c &&
                                 body[i + 2] == c);
            const std::size_t qlen = triple ? 3 : 1;
            std::size_t j = i + qlen;
            while (j < body.size()) {
                if (body[j] == '\\') { j += 2; continue; }
                if (body[j] == c) {
                    if (!triple) break;
                    if (j + 2 < body.size() && body[j + 1] == c &&
                        body[j + 2] == c) break;
                }
                ++j;
            }
            i = j + qlen;
            continue;
        }
        if (c == '#') {  // comment inside a multi-line call: skip to EOL
            while (i < body.size() && body[i] != '\n') ++i;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') --depth;
        if (depth == 0) break;
        ++i;
    }
    if (i > body.size()) i = body.size();
    // One past the closing ')', so a caller can tell what immediately follows.
    if (endOut) *endOut = (i < body.size()) ? i + 1 : body.size();
    return body.substr(begin, i - begin);
}

// `core.autocrlf=true` (hazard H3): readFile is binary, so a checked-out file
// carries CRLF while a freshly written one may not. Every scan below runs on
// the normalised form so the guard cannot pass in the session that wrote a file
// and fail after the next checkout.
std::string normaliseCrlf(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\r') out.push_back(c);
    return out;
}

// Every pytest.skip(...) call site in `body`, as (byte offset, argument text).
std::vector<std::pair<std::size_t, std::string>> pytestSkipCalls(
    const std::string& body) {
    std::vector<std::pair<std::size_t, std::string>> out;
    const std::string needle = "pytest.skip";
    std::size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::string::npos) {
        std::size_t p = pos + needle.size();
        while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
        if (p < body.size() && body[p] == '(')
            out.emplace_back(pos, extractCallArgs(body, p));
        pos += needle.size();
    }
    return out;
}

bool mentionsReachability(const std::string& s) {
    // FOUR spellings, not two, and not three. Each widening was forced by the
    // same mistake being caught one step later:
    //   F11  keyed on "not reachable via UIA"  -> 218 "not reachable." survived
    //   F25  keyed on "not reachable"          -> 7 "unreachable" survived
    //   F25's karen gate then forced this pass, whose corrected sweep also
    //   surfaced 20 sites spelling the SAME UIA lookup failure as
    //   "absent from / not in / missing from [the] UIA tree".
    // "widget X is not in the UIA tree" and "widget X is not reachable" are the
    // same event described twice. Match the CONCEPT, in any case, or this list
    // grows a fifth entry next phase.
    static const std::regex re(
        R"(not\s+reachable|unreachable|(?:not\s+in|absent\s+from|missing\s+from)\s+(?:the\s+)?UIA)",
        std::regex::icase);
    return std::regex_search(s, re);
}

// The message a `record_matrix_result(...)` call carries, when such a call is
// the statement IMMEDIATELY preceding the byte offset `at` — only whitespace
// and whole-line comments may separate them. Empty when there is none.
//
// This exists because the karen gate on F25 caught the defect recurring a THIRD
// time in a new disguise: the reachability wording had migrated OUT of the
// pytest.skip message and INTO the record_matrix_result message beside it, e.g.
//
//     record_matrix_result(tid, "blocked", "AtlasComparisonDialog unreachable")
//     pytest.skip("atlas chain prerequisite failed")
//
// A guard that inspects only the skip's own argument is blind to that, and the
// shape is WORSE than a plain skip: a 'blocked' row exists, so the bookkeeping
// looks handled, while the session still renders green.
std::string precedingRecordMessage(const std::string& body, std::size_t at) {
    static const std::string rec = "record_matrix_result";
    std::size_t search = 0;
    std::size_t bestEnd = std::string::npos;
    std::string bestArgs;
    while (true) {
        const std::size_t pos = body.find(rec, search);
        if (pos == std::string::npos || pos >= at) break;
        search = pos + rec.size();
        std::size_t p = search;
        while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
        if (p >= body.size() || body[p] != '(') continue;
        std::size_t end = 0;
        std::string args = extractCallArgs(body, p, &end);
        if (end <= at) { bestEnd = end; bestArgs = args; }
    }
    if (bestEnd == std::string::npos) return {};
    for (std::size_t i = bestEnd; i < at; ++i) {
        const char c = body[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '#') {  // a comment line between the two statements is fine
            while (i < at && body[i] != '\n') ++i;
            continue;
        }
        return {};  // real code sits between them: not immediately preceding
    }
    return bestArgs;
}

// The full detection rule: a pytest.skip reports a reachability failure when
// EITHER its own message says so, OR the record_matrix_result immediately
// before it does.
bool skipReportsReachability(const std::string& body, std::size_t skipOff,
                             const std::string& skipArgs) {
    if (mentionsReachability(skipArgs)) return true;
    return mentionsReachability(precedingRecordMessage(body, skipOff));
}

// Line number of a byte offset, for an actionable failure message.
int lineOf(const std::string& body, std::size_t off) {
    return 1 + static_cast<int>(std::count(body.begin(),
                                           body.begin() + static_cast<long>(off),
                                           '\n'));
}

}  // namespace

// The detector must be provably able to SEE a violation. A guard whose scanner
// silently matches nothing passes vacuously forever and is decoration — this
// programme has shipped exactly that before. These synthetic cases are the
// in-suite equivalent of the external mutation check.
TEST(ReachabilitySkipGuard, DetectorRecognisesAllSpellingsAndIgnoresConversions) {
    struct Case { const char* src; bool flagged; const char* why; };
    const Case cases[] = {
        {"    pytest.skip(\"pdfCanvas not reachable.\")\n", true,
         "the 'not reachable.' spelling F11 missed"},
        {"    pytest.skip(\"toolAction not reachable via UIA.\")\n", true,
         "the 'not reachable via UIA' spelling F11 converted"},
        {"    pytest.skip(\n        \"a not\\n\"\n        \"   reachable thing\")\n",
         false, "line-split words are not the phrase (documents the limit)"},
        {"    pytest.skip(f\"View > Rotate not reachable: {e}\")\n", true,
         "f-string messages must still be caught"},
        {"    pytest.skip(\"NOT REACHABLE\")\n", true, "case-insensitive"},
        {"    pytest.skip(\"AtlasComparisonDialog unreachable\")\n", true,
         "the one-word 'unreachable' spelling F25 found after F11 and after"
         " its own first sweep"},
        {"    pytest.skip(\"QColorDialog did not appear or was unreachable.\")\n",
         true, "'was unreachable' reads as prose but is the same defect"},
        {"    pytest.skip(\"needs a live desktop session\")\n", false,
         "an unrelated skip must not be flagged"},
        // The F11/F25 conversion itself must NOT trip the guard, or the guard
        // would red the very shape it exists to require.
        {"    _reach = (\"pdfCanvas not reachable.\")\n"
         "    record_matrix_result(current_test_id(), \"blocked\", _reach)\n"
         "    pytest.fail(f\"WIN-45 reachability blocked: {_reach}\")\n",
         false, "the converted pattern uses pytest.fail, not pytest.skip"},
        // A ')' inside the message must not truncate the scan.
        {"    pytest.skip(\"widget (primary) not reachable.\")\n", true,
         "a nested paren in the message must not end the arg scan"},
        // A trailing comment must not be pulled into the message.
        {"    pytest.skip(\"unrelated\")  # not reachable, historically\n", false,
         "text after the call is not part of the skip message"},
        // --- fourth spelling: the UIA tree named directly ---
        {"    pytest.skip(\"PdfCanvas absent from UIA tree\")\n", true,
         "'absent from UIA tree' is the same lookup failure, spelled a 4th way"},
        {"    pytest.skip(\"undoAction not in UIA tree.\")\n", true,
         "'not in UIA tree'"},
        {"    pytest.skip(\"controls missing from UIA tree.\")\n", true,
         "'missing from UIA tree'"},
        {"    pytest.skip(\"boldButton not in the UIA tree after activating\")\n",
         true, "'not in THE UIA tree' — the article must not defeat the match"},
        // --- the split-message shape the karen gate caught (rule B) ---
        {"    record_matrix_result(tid, \"blocked\","
         " \"AtlasComparisonDialog unreachable\")\n"
         "    pytest.skip(\"atlas chain prerequisite failed\")\n", true,
         "reachability in the RECORD message and an innocuous skip message is"
         " the third recurrence of WIN-45 and must be flagged"},
        {"    record_matrix_result(tid, \"blocked\","
         " \"atlasModeCombo not reachable\")\n"
         "    # a comment between the two statements is still 'immediately'\n"
         "    pytest.skip(\"combo absent\")\n", true,
         "a comment may separate the record from the skip"},
        {"    record_matrix_result(tid, \"blocked\", \"could not select item\")\n"
         "    pytest.skip(\"combo selection failed\")\n", false,
         "a NON-reachability blocked record followed by a skip is a different"
         " class and must not be swept in here"},
        {"    record_matrix_result(tid, \"blocked\", \"widget not reachable\")\n"
         "    do_something_else(win)\n"
         "    pytest.skip(\"unrelated environment gate\")\n", false,
         "real code between the record and the skip means it is not the"
         " immediately-preceding statement"},
        // The converted double-record shape must STILL not trip the guard.
        {"    record_matrix_result(\"t\", \"blocked\", \"dialog not reachable\")\n"
         "    _reach = (\"dialog not reachable\")\n"
         "    record_matrix_result(current_test_id(), \"blocked\", _reach)\n"
         "    pytest.fail(f\"WIN-45 reachability blocked: {_reach}\")\n",
         false, "the converted site uses pytest.fail — no skip to flag"},
    };
    for (const auto& c : cases) {
        const std::string src(c.src);
        bool flagged = false;
        for (const auto& call : pytestSkipCalls(src))
            if (skipReportsReachability(src, call.first, call.second))
                flagged = true;
        EXPECT_EQ(flagged, c.flagged)
            << "detector mis-classified: " << c.why << "\nsource:\n" << src;
    }
}

TEST(ReachabilitySkipGuard, NoPytestSkipReportsAReachabilityFailure) {
    const fs::path testsDir = projectRoot() / "tests";
    ASSERT_TRUE(fs::exists(testsDir)) << testsDir.string();

    int scanned = 0;
    int totalSkips = 0;
    std::vector<std::string> offenders;

    for (const auto& e : fs::recursive_directory_iterator(testsDir)) {
        if (!e.is_regular_file()) continue;
        const fs::path& p = e.path();
        if (p.extension() != ".py") continue;
        // __pycache__ holds no source; _runtime is generated scratch.
        const std::string pathStr = p.generic_string();
        // /_tmp/ is pytest.ini's --basetemp, so EVERY pytest run leaves fixture
        // files there and this walk would count them as production sources.
        // F29 found it the hard way: a gate self-test that writes a sample
        // converted site into its tmp_path made the sibling guard below report
        // 10 "broken" conversions that were nothing but its own scratch output.
        // pytest.ini already lists all three of these in norecursedirs.
        if (contains(pathStr, "__pycache__") || contains(pathStr, "/_runtime/") ||
            contains(pathStr, "/_tmp/"))
            continue;

        const std::string body = normaliseCrlf(readFile(p));
        if (body.empty()) continue;
        ++scanned;

        for (const auto& call : pytestSkipCalls(body)) {
            ++totalSkips;
            if (!skipReportsReachability(body, call.first, call.second))
                continue;
            const std::string rel =
                fs::relative(p, projectRoot()).generic_string();
            offenders.push_back(rel + ":" +
                                std::to_string(lineOf(body, call.first)));
        }
    }

    // A scan that reached nothing would pass vacuously. Pin both the file count
    // and the fact that pytest.skip is used at all, so a path/extension mistake
    // reds this test instead of silently disarming it.
    EXPECT_GT(scanned, 100)
        << "only " << scanned << " .py files scanned under " << testsDir.string()
        << " — the walk is not reaching the test tree, so this guard is inert";
    EXPECT_GT(totalSkips, 100)
        << "only " << totalSkips << " pytest.skip call sites found across "
        << scanned << " files — the call scanner is not matching, so this guard"
           " is inert";

    std::string detail;
    for (const auto& o : offenders) detail += "\n    " + o;
    EXPECT_TRUE(offenders.empty())
        << offenders.size() << " pytest.skip site(s) report a reachability"
           " failure as a SKIP:" << detail
        << "\n\nA widget/pipe that cannot be reached is the failure the test"
           " exists to catch; skipping it renders as a green session (gap"
           " WIN-45). This includes a skip whose OWN message is innocuous while"
           " the record_matrix_result immediately before it names the"
           " reachability failure — that shape is worse, not better, because"
           " the 'blocked' row makes the bookkeeping look done."
           " Convert each site to the F11/F25 pattern:"
           "\n    _reach = (<original message verbatim>)"
           "\n    record_matrix_result(current_test_id(), \"blocked\", _reach)"
           "\n    pytest.fail("
           "f\"WIN-45 reachability blocked (recorded non-pass, not skipped):"
           " {_reach}\")"
        << "\nIf a site is genuinely environment-gated, justify it in"
           " docs/audit/meta-audit-2026-07/followup/phase-25-dispositions.md"
           " and amend this guard deliberately — cite the env var AND the code"
           " that reads it, because the messages lie: every site F25 examined"
           " that CLAIMED an env dependency was in fact a plain UIA lookup.";
}

// The conversion must stay converted. Guard above catches a skip coming BACK;
// this catches the recorded non-pass being quietly hollowed out into a bare
// pytest.fail that leaves the matrix row unrecorded — the same false-green by
// a different route.
TEST(ReachabilitySkipGuard, ConvertedSitesStillRecordBlockedBeforeFailing) {
    const fs::path testsDir = projectRoot() / "tests";
    ASSERT_TRUE(fs::exists(testsDir)) << testsDir.string();

    int converted = 0;
    std::vector<std::string> broken;

    for (const auto& e : fs::recursive_directory_iterator(testsDir)) {
        if (!e.is_regular_file()) continue;
        const fs::path& p = e.path();
        if (p.extension() != ".py") continue;
        const std::string pathStr = p.generic_string();
        // /_tmp/ is pytest.ini's --basetemp, so EVERY pytest run leaves fixture
        // files there and this walk would count them as production sources.
        // F29 found it the hard way: a gate self-test that writes a sample
        // converted site into its tmp_path made the sibling guard below report
        // 10 "broken" conversions that were nothing but its own scratch output.
        // pytest.ini already lists all three of these in norecursedirs.
        if (contains(pathStr, "__pycache__") || contains(pathStr, "/_runtime/") ||
            contains(pathStr, "/_tmp/"))
            continue;

        std::string body = readFile(p);
        if (body.empty()) continue;
        // readFile() is binary and core.autocrlf=true, so tracked files are
        // CRLF on disk (hazard H3). Normalise before any line-shaped matching.
        body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());

        const std::string marker = "_reach = (";
        std::size_t pos = 0;
        while ((pos = body.find(marker, pos)) != std::string::npos) {
            ++converted;
            // The bookkeeping must follow within the same block. Bound the
            // window so a match from an unrelated later site cannot cover for a
            // hollowed-out one.
            const std::string window = body.substr(pos, 700);
            const bool ok = contains(window, "record_matrix_result") &&
                            contains(window, "current_test_id") &&
                            contains(window, "\"blocked\"") &&
                            contains(window, "pytest.fail");
            if (!ok) {
                const std::string rel =
                    fs::relative(p, projectRoot()).generic_string();
                broken.push_back(rel + ":" + std::to_string(lineOf(body, pos)));
            }
            pos += marker.size();
        }
    }

    // Re-derived from the tree, not inherited: the `# WIN-45` markers count
    // 107 F11-pattern sites (6 files) + 248 F25-pattern sites (37 files) = 355
    // across 38 files. The figure previously asserted here (225, prose "268")
    // was itself wrong — it under-counted F11 as 43 — which is exactly the
    // count-drift hazard this programme keeps hitting. Anything materially
    // below the real total means conversions were deleted rather than kept.
    EXPECT_GE(converted, 350)
        << "only " << converted << " converted reachability sites found;"
           " the tree holds 355 (F11's 107 + F25's 225 + F25-B's 23). A large"
           " drop means the recorded non-passes were removed rather than kept.";

    std::string detail;
    for (const auto& b : broken) detail += "\n    " + b;
    EXPECT_TRUE(broken.empty())
        << broken.size() << " converted site(s) no longer record the blocked"
           " result before failing:" << detail
        << "\nEach _reach assignment must be followed by"
           " record_matrix_result(current_test_id(), \"blocked\", _reach) and"
           " then pytest.fail — a bare fail loses the matrix row.";
}

// GitHub #162 / DCA 0057. The validation lane runs `ctest -L lint`, and that
// step already asserts the selected COUNT and each linter's NAME -- because a
// ctest label filter matching nothing exits 0, so "the step passed" is
// otherwise satisfiable by a label that selects no tests at all.
//
// What nothing guarded is the step itself. #162 existed because four linters
// were REGISTERED as CTest tests and never SELECTED: every ctest call in CI was
// `-L security`. Wiring the lane fixed that, and left the fix in exactly the
// state #161 was in -- present, and protected by nothing. Delete the step, or
// let its hardcoded name list drift from the registrations in
// tests/CMakeLists.txt, and the gap reopens silently.
//
// So this re-derives the linter names from the CMake registrations rather than
// hardcoding a second copy, and fails if the workflow step does not name every
// one of them. A guard that carried its own list would drift with the thing it
// guards.
TEST(ValidationLaneHygiene, LintLaneSelectsEveryRegisteredMethodologyLinter) {
    const std::string cmake = readFile(projectRoot() / "tests" / "CMakeLists.txt");
    ASSERT_FALSE(cmake.empty()) << "tests/CMakeLists.txt is missing";

    // Re-derive the registered lint entries: `add_test(NAME lint_<x>` whose
    // properties carry the `lint` label.
    std::vector<std::string> registered;
    const std::regex re(R"(add_test\s*\(\s*NAME\s+(lint_[A-Za-z0-9_]+))");
    for (std::sregex_iterator it(cmake.begin(), cmake.end(), re), end; it != end; ++it) {
        const std::string name = (*it)[1].str();
        const auto propPos = cmake.find("set_tests_properties(" + name);
        if (propPos == std::string::npos) continue;
        const std::string props = cmake.substr(propPos, 400);
        if (props.find("lint") != std::string::npos) registered.push_back(name);
    }

    // NON-VACUITY. If the parse finds nothing, every assertion below passes
    // trivially -- the empty-collection shape that has produced green tests
    // measuring nothing in this repo before.
    ASSERT_GE(registered.size(), 4u)
        << "parsed " << registered.size() << " lint-labelled CTest registrations "
           "from tests/CMakeLists.txt; expected at least the four methodology "
           "linters. The parse is wrong, or the registrations were removed -- "
           "either way this guard would otherwise assert nothing.";

    const std::string wf = readFile(projectRoot() / kValidationWorkflow);
    ASSERT_FALSE(wf.empty());

    // Find the REAL invocation, not a mention of it. A mutation run proved this
    // matters: replacing the first "-L lint" in the file hit a COMMENT sixty
    // lines above the step and left the guard green. A guard satisfied by prose
    // describing the thing it guards is a decoration -- so require `ctest` and
    // `-L lint` on the same line, and require that line not to be a comment.
    std::string stepRegion;
    for (const auto& line : splitLines(wf)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        // `-N` only LISTS the selected tests; it runs nothing. #162 was that
        // the linters were never RUN, so a listing line is not evidence the
        // lane executes them -- a second mutation proved the guard accepted
        // one. Require an invocation that is not a dry-run listing.
        // The line must BEGIN with the command. Three successive mutations
        // were needed to find this: the guard first matched a comment sixty
        // lines above the step, then a `-N` dry-run listing, then the text
        // inside a Write-Error message *describing* the invocation. Each time
        // it was matching prose about the thing instead of the thing.
        if (t.rfind("ctest", 0) == 0 &&
            t.find("-L lint") != std::string::npos &&
            t.find("-N") == std::string::npos) {
            // The step names each linter in a PowerShell array ABOVE the
            // invocation, so a window that starts at the invocation and runs
            // forward misses them -- it did, and the guard reddened on a clean
            // tree. Take a window that spans both, anchored on the invocation.
            const auto at = wf.find(line);
            const auto from = (at > 2000) ? at - 2000 : static_cast<size_t>(0);
            stepRegion = wf.substr(from, (at - from) + 600);
            break;
        }
    }
    ASSERT_FALSE(stepRegion.empty())
        << kValidationWorkflow << " has no uncommented `ctest ... -L lint` "
           "invocation. GitHub #162 was that the four methodology linters were "
           "registered as CTest tests and never selected, because every ctest "
           "call was `-L security`. Removing this step restores that gap "
           "exactly -- and a comment still describing it does not run anything.";

    for (const auto& name : registered) {
        EXPECT_NE(stepRegion.find(name), std::string::npos)
            << "the `ctest -L lint` step does not name '" << name
            << "', which IS registered with the lint label in "
               "tests/CMakeLists.txt. The step asserts its own hardcoded list, "
               "so a linter added to CMake but not to that list is selected by "
               "the label yet unasserted -- and one removed from CMake leaves a "
               "name the step will never match. Keep the two in step.";
    }
}
