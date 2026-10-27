// -*- coding: utf-8 -*-
// Phase 10: Test fixture corpus validation. Cheap smoke tests that
// every committed PDF fixture opens via MuPDF, that the Phase-10
// corpus additions (PDF/A sidecars, tampered, OCR variants) are
// present and structurally intact, and that the fixtures README
// lists every fixture we actually have.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// -------- PDF/A corpus: sidecars exist and parse as JSON-ish --------

TEST(FixtureCorpus, PdfASidecarsExistForEveryConformanceLevel)
{
    const std::vector<std::string> levels = {"1b", "2b", "2u", "2a", "3b"};
    for (const auto& lvl : levels) {
        auto pdf  = fixturesDir() / ("test_pdfa_" + lvl + ".pdf");
        auto json = fixturesDir() / ("test_pdfa_" + lvl + ".expected.json");
        ASSERT_TRUE(std::filesystem::exists(pdf)) << pdf;
        ASSERT_TRUE(std::filesystem::exists(json)) << json;

        const std::string body = readAll(json);
        // Cheap JSON-shape asserts without dragging in a parser dep:
        EXPECT_NE(body.find("\"version\""),    std::string::npos) << json;
        EXPECT_NE(body.find("\"conformance\""), std::string::npos) << json;
        EXPECT_NE(body.find("\"colorSpace\""), std::string::npos) << json;
    }
}

TEST(FixtureCorpus, PdfASidecarContainsMatchingConformanceLetter)
{
    // Conformance letter in the JSON (b/u/a) should match the suffix
    // in the filename. Catches mixed-up sidecars at regen time.
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"1b", "b"}, {"2b", "b"}, {"2u", "u"}, {"2a", "a"}, {"3b", "b"},
    };
    for (const auto& [lvl, letter] : cases) {
        auto json = fixturesDir() / ("test_pdfa_" + lvl + ".expected.json");
        const std::string body = readAll(json);
        const std::string needle = "\"conformance\": \"" + letter + "\"";
        EXPECT_NE(body.find(needle), std::string::npos)
            << "missing " << needle << " in " << json;
    }
}

// -------- CA-chain material for signature tests --------

TEST(FixtureCorpus, CaChainBundleIsPresentAndStructurallyValid)
{
    // Shipped by generate_ca_signed_pdf.py. Bundle enables the
    // signature-chain E2E; tests that actually use it still GTEST_SKIP
    // if any component is missing.
    const std::vector<std::string> files = {
        "test_ca_cert.pem",
        "test_signer_ca.pem",
        "test_signer_ca.pfx",
    };
    int missing = 0;
    for (const auto& f : files) {
        if (!std::filesystem::exists(fixturesDir() / f)) ++missing;
    }
    if (missing > 0) {
        GTEST_SKIP() << missing << " CA-chain files missing; run "
                     << "generate_ca_signed_pdf.py to regenerate";
    }
    for (const auto& f : files) {
        EXPECT_GT(std::filesystem::file_size(fixturesDir() / f), 0u)
            << f << " is empty";
    }
    const auto caBody = readAll(fixturesDir() / "test_ca_cert.pem");
    EXPECT_NE(caBody.find("-----BEGIN CERTIFICATE-----"), std::string::npos)
        << "test_ca_cert.pem must be a PEM-encoded certificate";
    const auto signerBody = readAll(fixturesDir() / "test_signer_ca.pem");
    EXPECT_NE(signerBody.find("-----BEGIN CERTIFICATE-----"), std::string::npos)
        << "test_signer_ca.pem must be a PEM-encoded certificate";
}

// -------- Tampered fixture --------

TEST(FixtureCorpus, TamperedPdfExistsAndIsByteDifferentFromSigned)
{
    auto signed_ = fixturesDir() / "test_signed.pdf";
    auto tampered = fixturesDir() / "test_tampered.pdf";
    if (!std::filesystem::exists(signed_)) {
        GTEST_SKIP() << "test_signed.pdf not available";
    }
    ASSERT_TRUE(std::filesystem::exists(tampered));
    EXPECT_NE(readAll(signed_), readAll(tampered))
        << "tampered fixture must differ from the signed baseline";
}

// -------- OCR corpus --------

TEST(FixtureCorpus, OcrVariantsEachHaveExpectedTextSidecar)
{
    const std::vector<std::string> names = {
        "test_ocr_clean", "test_ocr_skewed", "test_ocr_multicol",
    };
    for (const auto& name : names) {
        auto pdf = fixturesDir() / (name + ".pdf");
        auto txt = fixturesDir() / (name + ".expected.txt");
        if (!std::filesystem::exists(pdf)) {
            // Regeneration is optional in a minimal checkout. Skip
            // rather than fail if the generator wasn't run.
            GTEST_SKIP() << pdf << " missing — run "
                         << "generate_ocr_variants.py to regenerate";
        }
        ASSERT_TRUE(std::filesystem::exists(txt))
            << txt << " must accompany " << pdf;
        EXPECT_GT(std::filesystem::file_size(txt), 0u)
            << txt << " must not be empty";
    }
}

// -------- Structural: every *.pdf fixture opens via MuPDF --------

TEST(FixtureCorpus, EveryPdfFixtureOpensCleanly)
{
    auto ctx = std::make_shared<MuPdfContext>();
    int opened = 0;
    int skipped = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fixturesDir())) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".pdf") continue;
        const auto name = entry.path().filename().string();
        // Encrypted-with-user-password fixtures cannot open without
        // the password; skip those here — they're exercised in
        // the encryption-specific suites.
        if (name.find("encrypted") != std::string::npos) { skipped++; continue; }
        // Tampered fixture's bytes are deliberately mangled — opening
        // may succeed or warn depending on where the flip landed. Skip
        // strict open-check for it.
        if (name == "test_tampered.pdf") { skipped++; continue; }

        try {
            auto doc = Document::open(ctx, entry.path());
            if (doc) {
                EXPECT_GE(doc->pageCount(), 0) << entry.path();
                opened++;
            } else {
                ADD_FAILURE() << "Document::open returned null for " << entry.path();
            }
        } catch (const std::exception& e) {
            ADD_FAILURE() << "Fixture " << entry.path()
                          << " failed to open: " << e.what();
        }
    }
    EXPECT_GT(opened, 10) << "expected at least 10 PDF fixtures to open";
    EXPECT_GE(skipped, 0);
}

// -------- README mentions every fixture we actually ship --------

TEST(FixtureCorpus, ReadmeListsEveryPdfFixture)
{
    auto readme = fixturesDir() / "README.md";
    if (!std::filesystem::exists(readme)) {
        GTEST_SKIP() << "README.md missing";
    }
    const std::string body = readAll(readme);
    std::vector<std::string> missing;
    for (const auto& entry : std::filesystem::directory_iterator(fixturesDir())) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".pdf") continue;
        const std::string name = entry.path().filename().string();
        if (body.find(name) == std::string::npos) {
            missing.push_back(name);
        }
    }
    for (const auto& m : missing) {
        ADD_FAILURE() << "README.md does not mention " << m;
    }
}

// ---------------------------------------------------------------------------
// Meta-audit follow-up F17 (gaps A1-09, A1-28).
//
// The register's claim was "~1,150 tests can skip to green": every
// `GTEST_SKIP() << "<fixture> not available"` site is a branch that turns a
// missing file into a green result. The configure-time fixture floor in
// tests/CMakeLists.txt pins the worst offenders, but nothing re-derived the
// relationship between the skip sites in the source and the fixtures on disk,
// so a fixture could be deleted (or a new skip added naming a file nobody ever
// committed) and the suite would stay green while silently losing coverage.
//
// The two guards below close that by re-deriving both directions from source:
//
//   * every repo-fixture name (.pdf/.pfx) that a skip message names must
//     actually exist under tests/fixtures/ -- unless the referencing file
//     resolves it from the gitignored external corpus (test_PDFs/), which is
//     by construction not committed. That exemption is derived from the file
//     under inspection, not from a hand-maintained allow-list.
//   * every fixture named by five or more skip sites must appear in the
//     _pdfe_required_fixtures floor in tests/CMakeLists.txt, so its absence
//     fails CMake configure instead of turning a large block of tests green.
//
// Both are text-over-source oracles: nothing here opens a PDF.
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path repoRootDir()
{
    return fixturesDir().parent_path().parent_path();
}

// name -> (site count, referenced-from-external-corpus)
struct SkipRef {
    int sites = 0;
    bool externalCorpus = false;
};

// The fixture names that genuinely live in the gitignored external corpus
// (test_PDFs/ at the repository root), derived from the tree rather than from
// a hand-maintained allow-list:
//
//   * `.gitignore`'s `!test_PDFs/<name>` re-includes -- the corpus files the
//     project has deliberately committed; and
//   * any `"test_PDFs" / "<name>"` path construction in tests/*.cpp, which is
//     how a test names a corpus file it resolves directly.
//
// This replaces an exemption that asked only whether the *file* contained the
// string "test_PDFs" anywhere, which was wrong in two compounding ways:
//
//   1. It excused EVERY skip in such a file, whichever fixture the skip named
//      -- including committed fixtures under tests/fixtures/ that have nothing
//      to do with the corpus. test_fixture_corpus.cpp matched its own
//      predicate, so this guard also exempted its own skip sites.
//   2. Because `refs` is keyed by fixture NAME and the flag was OR-ed across
//      files, one external file naming a fixture exempted that name's skip
//      sites in every OTHER file too. test_single.pdf -- a committed fixture
//      named by skips across many files -- was exempt for that reason alone,
//      so deleting it would have left this guard green.
//
// Deriving the exemption per NAME from the corpus itself closes both.
//
// ASYMMETRY, deliberate. The skip SCAN below stays a raw-text scan: a stray
// GTEST_SKIP literal in a comment only ever ADDS a name to be checked, so the
// worst case is extra redness -- it fails closed. The exemption derivation is
// the opposite: anything it admits REMOVES a check, so a mention in prose must
// not widen it. The phase-2 claim-check gate found exactly that hole, using
// this file's own explanatory comment above (which contains the literal
// pattern) as the proof -- a comment describing the rule was silently obeying
// it. Hence code-only text here, raw text there.
// KNOWN LIMIT, stated rather than quietly hoped over. Line splicing is handled
// where it realistically occurs -- a `//` comment whose line ends in a
// backslash -- but NOT when the splice falls inside the `//` token itself
// (`/\` + newline + `/`), which is a comment under the standard and is read
// here as code. Closing that needs a full translation-phase-2 pre-pass, and a
// blanket pre-pass would corrupt raw string literals, where splicing must not
// apply. The realistic vector is an accidental trailing backslash; the split
// token is only reachable deliberately. If that ever stops being true, the fix
// is a splice-aware lookahead, not another special case: this hole has now been
// entered three times by three different doors, which says the shape recurs.
std::string stripCppComments(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    enum class St { Code, Line, Block, Str, Chr, Raw };
    St st = St::Code;
    std::string rawDelim;

    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        switch (st) {
        case St::Code:
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                st = St::Line;
                i += 2;
            } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
                st = St::Block;
                i += 2;
            } else if (c == '"') {
                // R"delim( ... )delim" -- `//` inside one is not a comment.
                if (i > 0 && s[i - 1] == 'R') {
                    const std::size_t open = s.find('(', i + 1);
                    if (open != std::string::npos) {
                        rawDelim = s.substr(i + 1, open - i - 1);
                        out += s.substr(i, open - i + 1);
                        i = open + 1;
                        st = St::Raw;
                        break;
                    }
                }
                out += c;
                ++i;
                st = St::Str;
            } else if (c == '\'') {
                out += c;
                ++i;
                st = St::Chr;
            } else {
                out += c;
                ++i;
            }
            break;
        case St::Line:
            // C++ line splicing (translation phase 2): a `//` comment whose
            // line ends in a backslash CONTINUES onto the next physical line --
            // the two lines are one comment. Ending the comment at the first
            // raw '\n' would hand the continuation back as live code, which is
            // how the phase-2 claim-check gate reopened this exact hole on its
            // second pass. Handled here rather than as a global pre-splice pass
            // because splicing must NOT be applied inside a raw string literal.
            if (c == '\\') {
                std::size_t j = i + 1;
                if (j < s.size() && s[j] == '\r') ++j;
                if (j < s.size() && s[j] == '\n') {
                    out += '\n';  // keep line numbering sane; stay in comment
                    i = j + 1;
                    break;
                }
            }
            if (c == '\n') {
                out += c;
                st = St::Code;
            }
            ++i;
            break;
        case St::Block:
            if (c == '*' && i + 1 < s.size() && s[i + 1] == '/') {
                i += 2;
                st = St::Code;
            } else {
                if (c == '\n') out += c;  // keep line numbering sane
                ++i;
            }
            break;
        case St::Str:
        case St::Chr: {
            const char quote = (st == St::Str) ? '"' : '\'';
            out += c;
            if (c == '\\' && i + 1 < s.size()) {
                out += s[i + 1];
                i += 2;
                break;
            }
            if (c == quote) st = St::Code;
            ++i;
            break;
        }
        case St::Raw: {
            const std::string close = ")" + rawDelim + "\"";
            if (s.compare(i, close.size(), close) == 0) {
                out += close;
                i += close.size();
                st = St::Code;
            } else {
                out += c;
                ++i;
            }
            break;
        }
        }
    }
    return out;
}

// Corpus fixture names named by a real `"test_PDFs" / "<name>"` construction in
// CODE. Split out from the file walk so it can be tested on synthetic input --
// in particular, that a commented-out mention contributes nothing.
std::set<std::string> corpusNamesInCode(const std::string& body)
{
    std::set<std::string> names;
    const std::string code = stripCppComments(body);
    const std::regex corpusPathRe(R"RX("test_PDFs"\s*/\s*"([^"]+)")RX");
    for (std::sregex_iterator it(code.begin(), code.end(), corpusPathRe), end;
         it != end; ++it) {
        names.insert(std::filesystem::path((*it)[1].str()).filename().string());
    }
    return names;
}

std::set<std::string> externalCorpusFixtureNames()
{
    std::set<std::string> names;

    // (a) .gitignore re-includes: `!test_PDFs/<name>`
    std::istringstream ig(readAll(repoRootDir() / ".gitignore"));
    const std::regex reincludeRe(R"(^!test_PDFs/(.+?)\s*$)");
    for (std::string line; std::getline(ig, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, reincludeRe))
            names.insert(std::filesystem::path(m[1].str()).filename().string());
    }

    // (b) real `"test_PDFs" / "<name>"` constructions in the test sources,
    //     comments excluded (see the asymmetry note above).
    for (const auto& entry :
         std::filesystem::directory_iterator(repoRootDir() / "tests")) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".cpp") continue;
        const auto found = corpusNamesInCode(readAll(entry.path()));
        names.insert(found.begin(), found.end());
    }
    return names;
}

std::map<std::string, SkipRef> scanFixtureAbsenceSkips()
{
    std::map<std::string, SkipRef> refs;
    const std::regex skipRe(R"RX(GTEST_SKIP\(\)\s*<<\s*"([^"]*)")RX");
    const std::regex fileRe(R"([A-Za-z0-9_./-]+\.(?:pdf|pfx))");

    const auto external = externalCorpusFixtureNames();

    const auto testsDir = repoRootDir() / "tests";
    for (const auto& entry : std::filesystem::directory_iterator(testsDir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".cpp") continue;
        const std::string body = readAll(entry.path());

        for (std::sregex_iterator it(body.begin(), body.end(), skipRe), end;
             it != end; ++it) {
            const std::string msg = (*it)[1].str();
            for (std::sregex_iterator f(msg.begin(), msg.end(), fileRe), fend;
                 f != fend; ++f) {
                const std::string name = (*f)[0].str();
                auto& r = refs[name];
                r.sites += 1;
                // Exemption is a property of the NAMED fixture, not of the
                // file the skip happens to sit in.
                r.externalCorpus =
                    external.count(
                        std::filesystem::path(name).filename().string()) > 0;
            }
        }
    }
    return refs;
}

bool fixtureExists(const std::string& name)
{
    const auto base = std::filesystem::path(name).filename();
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(fixturesDir())) {
        if (entry.is_regular_file() && entry.path().filename() == base)
            return true;
    }
    return false;
}

} // namespace

TEST(FixtureSkipIntegrity, EverySkipNamedRepoFixtureIsCommitted)
{
    const auto refs = scanFixtureAbsenceSkips();
    ASSERT_GE(refs.size(), 20u)
        << "the scan found almost no fixture-absence skip sites -- the scanner "
           "itself is probably broken, not the tree";

    // Guard that the exemption was actually DERIVED, not merely empty. An
    // empty set fails closed here (every corpus fixture would be reported
    // missing), but it would do so with a confusing message blaming the tree
    // for a broken derivation. crazyones-pdfa.pdf is named by a skip in
    // test_fidelity_r3_19_pdfa_independent.cpp and lives in the corpus, so it
    // must always be present in the set.
    const auto external = externalCorpusFixtureNames();
    ASSERT_FALSE(external.empty())
        << "the external-corpus exemption derived no fixture names at all -- "
           ".gitignore or the `\"test_PDFs\" / \"<name>\"` scan is broken, not "
           "the tree";
    EXPECT_TRUE(external.count("crazyones-pdfa.pdf") > 0)
        << "the exemption no longer recognises crazyones-pdfa.pdf as a corpus "
           "fixture; check the .gitignore re-include parse";

    int checked = 0;
    for (const auto& [name, ref] : refs) {
        if (ref.externalCorpus) continue;   // gitignored corpus, see header
        ++checked;
        EXPECT_TRUE(fixtureExists(name))
            << "tests/*.cpp has " << ref.sites << " GTEST_SKIP site(s) naming '"
            << name << "', but no such file exists under tests/fixtures/. "
               "Those tests skip to green and cover nothing (gap A1-09/A1-28). "
               "Commit the fixture (with a deterministic generator) or delete "
               "the dead skip branch.";
    }
    EXPECT_GE(checked, 20)
        << "expected the majority of skip-named fixtures to be repo fixtures";
}

// --------------------------------------------------------------------------
// Phase-2 claim-check findings A and B. Both were UNMECHANISED when first
// submitted: the exemption widened on comment text, and the recorded corpus
// facts had nothing re-deriving them (their predecessor -- a docstring saying
// "exactly ONE" corpus file was re-included -- had already gone stale).
// --------------------------------------------------------------------------

TEST(FixtureSkipIntegrity, AMentionInACommentCannotWidenTheExemption)
{
    // The exact shape that was live in this file's own comment block.
    EXPECT_TRUE(corpusNamesInCode(
                    "// `\"test_PDFs\" / \"test_single.pdf\"` is how a test "
                    "names a corpus file\n")
                    .empty())
        << "a line comment mentioning the pattern widened the exemption -- "
           "that is the phase-2 finding, reintroduced";

    EXPECT_TRUE(corpusNamesInCode(
                    "/* \"test_PDFs\" / \"test_single.pdf\" */\n").empty())
        << "a block comment mentioning the pattern widened the exemption";

    // ...while a REAL construction still counts.
    EXPECT_EQ(corpusNamesInCode(
                  "auto p = root / \"test_PDFs\" / \"crazyones-pdfa.pdf\";\n")
                  .count("crazyones-pdfa.pdf"),
              1u)
        << "stripping comments also destroyed the real detection -- the "
           "exemption now admits nothing and the guard fails closed on every "
           "genuine corpus fixture";

    // C++ line splicing: a `//` line ending in a backslash continues the
    // comment onto the next physical line, so the construction below is
    // entirely commented out under real C++ semantics. Found by the phase-2
    // claim-check gate on its SECOND pass, after the first two vectors were
    // closed -- same defect shape, third door.
    EXPECT_TRUE(corpusNamesInCode(
                    "// leading comment \\\n"
                    "auto p = root / \"test_PDFs\" / \"test_single.pdf\";\n")
                    .empty())
        << "a backslash-newline line-spliced comment was treated as live code, "
           "re-admitting a committed fixture to the exemption";

    EXPECT_TRUE(corpusNamesInCode(
                    "// leading comment \\\r\n"
                    "auto p = root / \"test_PDFs\" / \"test_single.pdf\";\r\n")
                    .empty())
        << "line splicing not handled for CRLF line endings (core.autocrlf is "
           "true in this repo, so this is the checked-out form)";

    // Splicing must not swallow the line AFTER an ordinary comment.
    EXPECT_EQ(corpusNamesInCode(
                  "// an ordinary comment\n"
                  "auto p = root / \"test_PDFs\" / \"multicolumn.pdf\";\n")
                  .count("multicolumn.pdf"),
              1u)
        << "an unspliced comment swallowed the following line of real code";

    // A `//` inside a string literal is not a comment.
    EXPECT_EQ(corpusNamesInCode(
                  "const char* u = \"http://x\"; auto p = root / \"test_PDFs\" "
                  "/ \"multicolumn.pdf\";\n")
                  .count("multicolumn.pdf"),
              1u)
        << "a `//` inside a string literal was treated as starting a comment";
}

TEST(FixtureSkipIntegrity, TheExemptionAdmitsNoCommittedRepoFixture)
{
    const auto external = externalCorpusFixtureNames();

    // test_single.pdf is the concrete leak instance: a committed fixture under
    // tests/fixtures/ that is named by GTEST_SKIP sites in ~111 files. The old
    // whole-file exemption admitted it, so deleting the fixture would have left
    // this guard green.
    EXPECT_EQ(external.count("test_single.pdf"), 0u)
        << "a committed tests/fixtures/ file is in the external-corpus "
           "exemption set; its skip sites are no longer checked";

    // Prose leaking in shows up as names that are not real files.
    for (const auto& name : external) {
        EXPECT_EQ(name.find('<'), std::string::npos)
            << "'" << name << "' looks like prose, not a fixture name -- the "
               "exemption is being widened by documentation text";
    }
}

TEST(FixtureSkipIntegrity, TheRecordedCorpusFactsStillMatchTheTree)
{
    // Only the INVARIANT is asserted here. The file counts in that document
    // (64 top-level PDFs, 71 recursive, 69 untracked) describe the corpus on
    // the machine that produced it and are false on a clean checkout, where
    // only the re-included files exist -- asserting those would redden CI for
    // an input CI is never going to have. What IS invariant, and what actually
    // went stale before, is WHICH files .gitignore re-includes.
    const std::string ignore = readAll(repoRootDir() / ".gitignore");
    std::set<std::string> reincluded;
    {
        std::istringstream ig(ignore);
        const std::regex re(R"(^!test_PDFs/(.+?)\s*$)");
        for (std::string line; std::getline(ig, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::smatch m;
            if (std::regex_match(line, m, re)) reincluded.insert(m[1].str());
        }
    }
    ASSERT_FALSE(reincluded.empty())
        << "no `!test_PDFs/<name>` re-includes found -- the .gitignore parse "
           "is broken, not the tree";

    const auto factsPath =
        repoRootDir() / "docs" / "audit" / "test-pdfs-corpus-facts.md";
    ASSERT_TRUE(std::filesystem::exists(factsPath))
        << "the corrected-facts reference is gone: " << factsPath;
    const std::string facts = readAll(factsPath);

    for (const auto& name : reincluded) {
        EXPECT_NE(facts.find(name), std::string::npos)
            << name << " is re-included by .gitignore but is not named in "
                       "docs/audit/test-pdfs-corpus-facts.md -- the recorded "
                       "facts have drifted, exactly as the 'exactly ONE' "
                       "docstring did before them";
    }

    std::smatch m;
    const std::regex trackedRe(R"(\|\s*Tracked\s*\|\s*\*\*(\d+)\*\*)");
    ASSERT_TRUE(std::regex_search(facts, m, trackedRe))
        << "the facts document no longer states a tracked count in the "
           "expected `| Tracked | **N**` form; this guard cannot check it";
    EXPECT_EQ(std::stoul(m[1].str()), reincluded.size())
        << "the facts document says " << m[1].str()
        << " tracked corpus file(s) but .gitignore re-includes "
        << reincluded.size();
}

TEST(FixtureSkipIntegrity, HeavilySkippedFixturesArePinnedByTheConfigureFloor)
{
    const auto refs = scanFixtureAbsenceSkips();
    const std::string cml = readAll(repoRootDir() / "tests" / "CMakeLists.txt");
    const auto start = cml.find("set(_pdfe_required_fixtures");
    ASSERT_NE(start, std::string::npos)
        << "the configure-time fixture floor has been removed from "
           "tests/CMakeLists.txt";
    const auto end = cml.find("\n)", start);
    ASSERT_NE(end, std::string::npos);
    const std::string floorBlock = cml.substr(start, end - start);

    constexpr int kPinThreshold = 5;
    for (const auto& [name, ref] : refs) {
        if (ref.externalCorpus) continue;
        if (ref.sites < kPinThreshold) continue;
        const std::string base =
            std::filesystem::path(name).filename().string();
        EXPECT_NE(floorBlock.find("\"" + base + "\""), std::string::npos)
            << base << " backs " << ref.sites
            << " GTEST_SKIP sites but is not pinned in _pdfe_required_fixtures. "
               "Deleting it would silently turn all of them green instead of "
               "failing CMake configure (gap A1-09/A1-28).";
    }
}
