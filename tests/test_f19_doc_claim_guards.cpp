// tests/test_f19_doc_claim_guards.cpp
//
// F19 (meta-audit follow-up phase-19) — standing guards over the documentation
// claims this phase re-verified and corrected.
//
// The A5 cluster is a sweep of the repo's top-level ledgers, and what it found
// was a class of defect this project has hit before: a document asserting a
// capability, a count, or a closure that the tree does not have. Two of them
// were spectacular — `docs/pdfe-vs-acrobat-pro-comparison.md` claimed the UI
// E2E suite had "0 failures" in the same sentence that named the artifact
// recording 11, and `docs/acrobat-pro-parity-status.md` stated two mutually
// contradictory matrix totals 160 lines apart, both of them stale by ~283 rows.
//
// Correcting prose is worth nothing on its own: the prose was correct once and
// drifted because nothing re-derived it. Every guard below therefore reads the
// CLAIM out of the document and the FACT out of the tree, and fails when they
// disagree — in either direction. A guard that only catches understatement
// would not have caught `test_stamp_tokens.cpp (16)` when the file has 12.
//
// Each guard is mutation-tested by flipping the artifact it guards (see the
// F19 dispositions file for the mutation log). None of them can pass
// vacuously: every one asserts on a quantity parsed out of a file, and fails
// loudly if the parse finds nothing.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();

    // Normalise CRLF -> LF. This repository is cloned with core.autocrlf=true,
    // so every tracked text file is LF in the index and CRLF in the working
    // tree — but a file freshly WRITTEN by a tool (as these documents are, by
    // each follow-up phase) stays LF until the next git operation checks it
    // out again. Several guards below match multi-line literals containing
    // '\n', so without this the same unchanged document passes right after it
    // is written and fails after any `git checkout`/`git stash pop` touches
    // it. Found red in the tree during F20 (meta-audit follow-up phase-20):
    // `F19DocClaims.ParityDocMatrixCountsMatchTheCsv` was failing purely on
    // line endings, with no document drift at all.
    //
    // This does not weaken any assertion: the compared text is identical, and
    // every guard here is about content, never about line-ending style.
    std::string out;
    out.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\r' && i + 1 < body.size() && body[i + 1] == '\n')
            continue;
        out += body[i];
    }
    return out;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// A minimal RFC-4180-ish splitter. The UI matrix CSV has quoted note fields
// containing commas, so a naive split on ',' mis-columns roughly a tenth of the
// rows and would make every count below quietly wrong.
// ---------------------------------------------------------------------------
std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else inQuotes = false;
            } else cur += c;
        } else if (c == '"') {
            inQuotes = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

struct MatrixCounts {
    int total = 0;
    int green = 0, blocked = 0, wontFix = 0, planned = 0, xfail = 0, skip = 0;
    int other = 0;
};

// Re-derive the UI matrix status distribution from the CSV itself.
MatrixCounts matrixCounts() {
    MatrixCounts m;
    const fs::path csv = repoRoot() / "tests" / "ui_automation" / "matrix" /
                         "ui_test_matrix.csv";
    std::ifstream in(csv, std::ios::binary);
    std::string line;
    int statusCol = -1;
    bool header = true;
    // Rows may embed newlines only inside quotes; this corpus does not, so a
    // line-at-a-time read is exact. Asserted below by the non-zero total.
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto cells = splitCsvLine(line);
        if (header) {
            for (size_t i = 0; i < cells.size(); ++i)
                if (cells[i] == "status") statusCol = static_cast<int>(i);
            header = false;
            continue;
        }
        if (statusCol < 0 || statusCol >= static_cast<int>(cells.size())) continue;
        ++m.total;
        const std::string& s = cells[statusCol];
        if (s == "green") ++m.green;
        else if (s == "blocked") ++m.blocked;
        else if (s == "wont-fix") ++m.wontFix;
        else if (s == "planned") ++m.planned;
        else if (s == "xfail") ++m.xfail;
        else if (s == "skip") ++m.skip;
        else ++m.other;
    }
    return m;
}

// Drop Markdown blockquote lines.
//
// Every correction this phase made is written as a `>` blockquote that QUOTES
// the sentence it replaced, so the reader can see what changed. Those quotes
// are not assertions -- a guard that scanned them would fire on the correction
// itself and could only be silenced by deleting the audit trail. So the
// "this stale string must not come back" checks, and the test-pointer scan,
// read the document with its blockquotes removed. Body text is still checked
// in full: reinstating a false claim OUTSIDE a correction box still fails.
std::string stripBlockquotes(const std::string& body) {
    std::istringstream in(body);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = line;
        t.erase(0, t.find_first_not_of(" \t"));
        if (!t.empty() && t[0] == '>') continue;
        out << line << '\n';
    }
    return out.str();
}

// Strip blockquote MARKERS while keeping the quoted text (cf. stripBlockquotes
// above, which drops the lines entirely).
//
// Needed because a live claim can legitimately live inside a blockquote:
// menu-wiring-gaps.md states the current distribution in its DISCHARGED banner,
// which is a `>` block, and it wraps mid-claim -- "6 xfail\n> / 2 skip". Any
// scan that does not normalise the marker away simply fails to see that claim
// and reports zero, which is the silent under-coverage this whole issue is
// about.
std::string stripBlockquoteMarkers(const std::string& body) {
    std::istringstream in(body);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        size_t i = line.find_first_not_of(" \t");
        if (i != std::string::npos && line[i] == '>') {
            line.erase(0, i + 1);
            if (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
                line.erase(0, 1);
        }
        out << line << '\n';
    }
    return out.str();
}

// Every `test_<name>.cpp` / `.py` token mentioned in a document.
std::set<std::string> testPointersIn(const std::string& body) {
    std::set<std::string> out;
    const std::regex re(R"(\btest_[A-Za-z0-9_]+\.(?:cpp|py)\b)");
    for (std::sregex_iterator it(body.begin(), body.end(), re), end; it != end; ++it)
        out.insert(it->str());
    return out;
}

bool testFileExistsAnywhere(const std::string& name) {
    const fs::path tests = repoRoot() / "tests";
    if (!fs::exists(tests)) return false;
    for (const auto& e : fs::recursive_directory_iterator(tests)) {
        std::error_code ec;
        if (!e.is_regular_file(ec)) continue;
        if (e.path().filename().string() == name) return true;
    }
    return false;
}

// First integer captured by `re` in `body`, or -1.
long firstNumber(const std::string& body, const std::regex& re) {
    std::smatch m;
    if (!std::regex_search(body, m, re)) return -1;
    if (m.size() < 2) return -1;
    try { return std::stol(m[1].str()); } catch (...) { return -1; }
}

const fs::path kManualVerification = fs::path("docs") / "manual-verification.md";
const fs::path kParityStatus = fs::path("docs") / "acrobat-pro-parity-status.md";
const fs::path kComparison = fs::path("docs") / "pdfe-vs-acrobat-pro-comparison.md";
const fs::path kMenuWiring = fs::path("docs") / "menu-wiring-gaps.md";
const fs::path kGateVerification = fs::path("docs") / "gate-verification.md";
const fs::path kClosure = fs::path("docs") / "coverage-expansion" / "closure.md";
const fs::path kCsCoverage =
    fs::path("docs") / "content-stream-continuity" / "COVERAGE.md";
const fs::path kAgpl = fs::path("docs") / "agpl-library-architecture-reevaluation.md";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The matrix ledgers. Three separate documents quote counts derived from ONE
// CSV, and before F19 all three disagreed with it and with each other.
// ═══════════════════════════════════════════════════════════════════════════

// Gap A5-32. docs/manual-verification.md §0 told the reader a correct tree
// would print 450 rows. It prints 787. A precondition check that fails on a
// good tree trains people to skip preconditions.
TEST(F19DocClaims, ManualVerificationMatrixRowCountMatchesTheCsv) {
    const MatrixCounts m = matrixCounts();
    ASSERT_GT(m.total, 0) << "ui_test_matrix.csv parsed to zero rows; the "
                             "guard would pass vacuously";

    const std::string doc = readFile(repoRoot() / kManualVerification);
    ASSERT_FALSE(doc.empty()) << kManualVerification.string() << " is missing";

    const long claimed =
        firstNumber(doc, std::regex(R"(Should print \*\*(\d+)\*\* rows)"));
    ASSERT_GE(claimed, 0)
        << kManualVerification.string()
        << " no longer states a matrix row count in the form "
           "'Should print **N** rows'. The §0 precondition is the one check in "
           "that document that is trivially machine-derivable; it must stay "
           "stated and stay guarded (gap A5-32).";

    EXPECT_EQ(claimed, m.total)
        << kManualVerification.string() << " §0 says the matrix has " << claimed
        << " rows; ui_test_matrix.csv has " << m.total
        << ". Update the document (gap A5-32).";
}

// Gap A5-88 / A5-89. The parity document carried the matrix state twice, in two
// different and mutually contradictory forms, both stale.
TEST(F19DocClaims, ParityDocMatrixCountsMatchTheCsv) {
    const MatrixCounts m = matrixCounts();
    ASSERT_GT(m.total, 0) << "ui_test_matrix.csv parsed to zero rows";

    const std::string doc = readFile(repoRoot() / kParityStatus);
    ASSERT_FALSE(doc.empty()) << kParityStatus.string() << " is missing";

    // The header block states the full distribution in one sentence.
    const std::regex re(
        R"(\*\*Matrix state — (\d+) green / (\d+) blocked / (\d+) wont-fix / )"
        R"((\d+) planned /\s*(\d+) xfail / (\d+) skip \((\d+) total rows\)\.\*\*)");
    std::smatch mm;
    ASSERT_TRUE(std::regex_search(doc, mm, re))
        << kParityStatus.string()
        << " no longer carries the machine-checked 'Matrix state — …' line. It "
           "was added in F19 precisely because the two hand-written totals this "
           "document used to carry had drifted apart from each other and from "
           "the CSV (gaps A5-88, A5-89). Do not remove it; update it.";

    EXPECT_EQ(std::stoi(mm[1].str()), m.green)   << "green count drifted";
    EXPECT_EQ(std::stoi(mm[2].str()), m.blocked) << "blocked count drifted";
    EXPECT_EQ(std::stoi(mm[3].str()), m.wontFix) << "wont-fix count drifted";
    EXPECT_EQ(std::stoi(mm[4].str()), m.planned) << "planned count drifted";
    EXPECT_EQ(std::stoi(mm[5].str()), m.xfail)   << "xfail count drifted";
    EXPECT_EQ(std::stoi(mm[6].str()), m.skip)    << "skip count drifted";
    EXPECT_EQ(std::stoi(mm[7].str()), m.total)   << "total row count drifted";

    // The SECOND place this document states the matrix state -- the numbered
    // "What's still open" item -- must agree with the first and with the CSV.
    // That divergence was the original defect: 494/9/1 in the header and
    // 487/16/1 further down, 160 lines apart, both stale.
    //
    // Deliberately NOT a blanket "this string must never appear" check. The
    // document also carries per-pipeline CLOSEOUT RECORDS that legitimately
    // quote what the totals were at the time ("New matrix totals: 494 green
    // …"). Those are history and must survive; forbidding the digits outright
    // would force their deletion, which is how audit trails get destroyed by
    // well-meaning guards.
    const size_t openIdx = doc.find("## What's still open");
    ASSERT_NE(openIdx, std::string::npos)
        << kParityStatus.string() << " has lost its \"What's still open\" section";
    const std::string openSection = stripBlockquotes(doc.substr(openIdx));

    const std::string expected =
        std::to_string(m.green) + " green / " + std::to_string(m.blocked) +
        " blocked / " + std::to_string(m.wontFix) + " wont-fix / " +
        std::to_string(m.planned) + " planned / " + std::to_string(m.xfail) +
        " xfail /\n   " + std::to_string(m.skip) + " skip (" +
        std::to_string(m.total) + " total)";
    EXPECT_TRUE(contains(openSection, expected))
        << kParityStatus.string()
        << " \"What's still open\" no longer states the live matrix totals ("
        << expected
        << "). Before F19 it stated 487/0/16/1 while the header stated "
           "494/0/9/1 -- two contradictory current-state claims in one "
           "document, both three months stale (gaps A5-88, A5-89).";

    const int nonGreen = m.total - m.green;
    EXPECT_TRUE(contains(openSection,
                         "**" + std::to_string(nonGreen) + " rows are non-green.**"))
        << kParityStatus.string()
        << " must state the non-green row count (" << nonGreen
        << "). The section used to enumerate 17 rows and say nothing about the "
           "rest, so 51 rows of coverage debt read as though they did not "
           "exist (gap A5-89).";
}

// ---------------------------------------------------------------------------
// GitHub #175. The matrix distribution is quoted in NINE places across six
// files and, before this test, exactly ONE of them was machine-checked --
// `ParityDocMatrixCountsMatchTheCsv` above, guarding the parity document. The
// other eight would have gone on asserting a stale number indefinitely and
// nothing would have said so. Promoting one row from wont-fix to green moved
// two counts and required thirteen hand edits; that correctness was a
// SNAPSHOT, not a property, and the next status flip re-opened the same hole.
//
// This widens the guard from "the sites I remembered to list" to "every site
// that makes the claim", which is the difference between a check that ratifies
// and one that verifies. Rather than enumerate sites (the enumeration is the
// thing that goes stale), it scans each document for the SHAPE of a live
// distribution claim and requires every occurrence to match the CSV.
//
// A LIVE CLAIM AND A DATED RECORD ARE NOT THE SAME THING, and the guard must
// not confuse them. The parity document also carries per-pipeline CLOSEOUT
// RECORDS that legitimately quote what the totals were at the time ("Matrix
// totals: 487 green / 0 blocked / 16 wont-fix / 1 planned"). Those are history
// and must survive: forbidding the digits outright is how audit trails get
// destroyed by well-meaning guards, as this file already says of the
// non-green check.
//
// The discriminator is the SHAPE, and it is chosen rather than lucky. Every
// live claim states the CURRENT six-way distribution
// (green / wont-fix / planned / xfail / skip, blocked optional). Every dated
// closeout record states the FOUR-way distribution that was current when it
// was written (green / blocked / wont-fix / planned) and names its pipeline
// and date. A record can therefore never be mistaken for a claim, and -- more
// importantly -- a NEW live claim written in the current house style is
// matched automatically, without anyone remembering to add it here.
// ---------------------------------------------------------------------------
TEST(F19DocClaims, EveryLiveMatrixDistributionClaimMatchesTheCsv) {
    const MatrixCounts m = matrixCounts();
    ASSERT_GT(m.total, 0) << "ui_test_matrix.csv parsed to zero rows; every "
                             "comparison below would be against nothing";

    // green / [blocked /] wont-fix / planned / xfail / skip, tolerating the
    // line wrapping every one of these sites actually uses.
    const std::regex shape(
        R"((\d+)\s+green\s*/\s*(?:(\d+)\s+blocked\s*/\s*)?)"
        R"((\d+)\s+wont-fix\s*/\s*(\d+)\s+planned\s*/\s*)"
        R"((\d+)\s+xfail\s*/?\s*(?:\*\*)?\s*(\d+)\s+skip)");

    // Every document that quotes the distribution as a LIVE claim, with the
    // number of such claims each must carry. The count is asserted so that
    // DELETING a claim to silence this guard is as loud as letting one drift:
    // a document that stops making the claim is a different document.
    const struct { fs::path doc; int expected; const char* why; } kSites[] = {
        {kParityStatus, 2,
         "the header 'Matrix state' sentence and the 'What's still open' "
         "restatement -- the two that disagreed with each other AND the CSV "
         "before F19 (gaps A5-88, A5-89)"},
        {fs::path("docs") / "release-readiness-assessment-2026-08-25.md", 1,
         "a PRESENT-TENSE correctness claim ('matching ... exactly'), which "
         "GitHub #175 flags as worse than a stale raw number because it "
         "asserts a property rather than a value"},
        {kManualVerification, 1,
         "the F19 breakdown parenthetical under the row-count precondition"},
        {kMenuWiring, 1,
         "the discharged-banner parenthetical"},
    };

    int totalClaims = 0;
    for (const auto& site : kSites) {
        const std::string raw = readFile(repoRoot() / site.doc);
        ASSERT_FALSE(raw.empty()) << site.doc.string() << " is missing";
        const std::string doc = stripBlockquoteMarkers(raw);

        int found = 0;
        for (std::sregex_iterator it(doc.begin(), doc.end(), shape), end;
             it != end; ++it) {
            const std::smatch& mm = *it;
            ++found;
            ++totalClaims;
            const auto num = [&](int i) { return std::stoi(mm[i].str()); };
            EXPECT_EQ(num(1), m.green)
                << site.doc.string() << " claim #" << found
                << " states " << mm[1].str() << " green; the CSV has "
                << m.green << ". " << site.why;
            if (mm[2].matched) {
                EXPECT_EQ(num(2), m.blocked)
                    << site.doc.string() << " claim #" << found
                    << " states " << mm[2].str() << " blocked; the CSV has "
                    << m.blocked;
            }
            EXPECT_EQ(num(3), m.wontFix)
                << site.doc.string() << " claim #" << found
                << " states " << mm[3].str() << " wont-fix; the CSV has "
                << m.wontFix;
            EXPECT_EQ(num(4), m.planned)
                << site.doc.string() << " claim #" << found
                << " states " << mm[4].str() << " planned; the CSV has "
                << m.planned;
            EXPECT_EQ(num(5), m.xfail)
                << site.doc.string() << " claim #" << found
                << " states " << mm[5].str() << " xfail; the CSV has "
                << m.xfail;
            EXPECT_EQ(num(6), m.skip)
                << site.doc.string() << " claim #" << found
                << " states " << mm[6].str() << " skip; the CSV has "
                << m.skip;
        }

        EXPECT_EQ(found, site.expected)
            << site.doc.string() << " carries " << found
            << " live matrix-distribution claim(s); " << site.expected
            << " expected (" << site.why
            << "). If a claim was deliberately removed, lower the count here "
               "and say why -- do not leave the guard counting a site that no "
               "longer exists, and do not silence a drift by deleting the "
               "sentence that drifted.";
    }

    EXPECT_GE(totalClaims, 5)
        << "this guard checked " << totalClaims
        << " distribution claims. GitHub #175 exists because eight of nine "
           "copies of these numbers sat outside any check; a version of this "
           "test that matches nothing would restore that state while reading "
           "green.";
}

// NEGATIVE CONTROL for the guard above. The per-pipeline closeout records in
// the parity document quote HISTORICAL totals on purpose, and the widened
// guard must leave them alone -- otherwise closing #175 would mean deleting
// the audit trail, which the issue explicitly forbids. This asserts the
// records are still present and still stale, i.e. that the shape
// discriminator really discriminates rather than the records having been
// quietly "fixed" into agreement.
TEST(F19DocClaims, DatedCloseoutRecordsKeepTheirHistoricalTotals) {
    const MatrixCounts m = matrixCounts();
    ASSERT_GT(m.total, 0);

    const std::string doc = readFile(repoRoot() / kParityStatus);
    ASSERT_FALSE(doc.empty());

    // Two real closeout records, quoted from Pipeline G and Pipeline H. Matched
    // wrap-tolerantly: both are line-wrapped mid-record in the document, and a
    // literal substring search reports "absent" for a record that is right
    // there -- which would make this control assert the opposite of the truth.
    const struct { const char* label; const char* re; } kHistorical[] = {
        {"486 green / 11 blocked / 6 wont-fix / 1 planned",
         R"(486\s+green\s*/\s*11\s+blocked\s*/\s*6\s+wont-fix\s*/\s*1\s+planned)"},
        {"487 green / 0 blocked / 16 wont-fix / 1 planned",
         R"(487\s+green\s*/\s*0\s+blocked\s*/\s*16\s+wont-fix\s*/\s*1\s+planned)"},
    };
    for (const auto& rec : kHistorical) {
        EXPECT_TRUE(std::regex_search(doc, std::regex(rec.re)))
            << "the dated closeout record \"" << rec.label
            << "\" has disappeared from " << kParityStatus.string()
            << ". GitHub #175: a guard that forces historical totals to match "
               "today's CSV destroys the audit trail it was supposed to "
               "protect. These must survive being wrong.";
    }

    // And they must genuinely differ from today's counts, or this control is
    // asserting nothing.
    EXPECT_NE(m.green, 486);
    EXPECT_NE(m.green, 487);
}

// Gap A5-13. docs/menu-wiring-gaps.md is a ledger of "29 blocked" matrix rows
// and a target of "29 blocked → ~10-14 blocked". There are no blocked rows.
TEST(F19DocClaims, MenuWiringLedgerHasNoBlockedRowsLeft) {
    const MatrixCounts m = matrixCounts();
    ASSERT_GT(m.total, 0) << "ui_test_matrix.csv parsed to zero rows";

    const std::string doc = readFile(repoRoot() / kMenuWiring);
    ASSERT_FALSE(doc.empty()) << kMenuWiring.string() << " is missing";

    const bool docSaysDischarged = contains(doc, "DISCHARGED 2026-08-03");

    if (m.blocked == 0) {
        EXPECT_TRUE(docSaysDischarged)
            << kMenuWiring.string()
            << " still presents itself as a live ledger of blocked matrix rows, "
               "but ui_test_matrix.csv has ZERO rows with status 'blocked' "
               "(gap A5-13). Its banner must say so.";
    } else {
        EXPECT_FALSE(docSaysDischarged)
            << kMenuWiring.string() << " is banner-marked as discharged, but "
            << m.blocked
            << " matrix rows are 'blocked' again. Re-open the ledger rather "
               "than leaving a false all-clear.";
    }

    // The one item the re-verification found genuinely still open must stay
    // named here, because nothing else in the repo tracks it.
    // Pin the whole statement, not just the identifier. The identifier alone
    // also appears in the surrounding narrative, so a token-presence check
    // survived a mutation that deleted the actual gap statement.
    EXPECT_TRUE(contains(doc, "ONE ITEM IS STILL GENUINELY OPEN") &&
                contains(doc, "Document::createEmpty()") &&
                contains(doc, "tracked\n>   **nowhere else**"))
        << kMenuWiring.string()
        << " no longer carries the File▸New / Document::createEmpty() gap "
           "statement. That is the single item from this ledger that has NOT "
           "landed, and it is tracked nowhere else — there is no file_new "
           "matrix row and no plan entry. Deleting it from here loses it "
           "outright, which is why this document is annotated rather than "
           "archived (gap A5-13).";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test-pointer integrity — gaps A5-91 and A5-17.
// ═══════════════════════════════════════════════════════════════════════════

// A5-91: `test_cert_chain_extractor.cpp` was cited by the parity document and
// has never existed. A5-17: docs/done-row-test-pointer-audit.md certified "all
// 69 file pointers verified" and then aged three months while the document
// grew to 112 pointers. A point-in-time audit is not a control; this is.
TEST(F19DocClaims, EveryParityDocTestPointerResolves) {
    const std::string doc = readFile(repoRoot() / kParityStatus);
    ASSERT_FALSE(doc.empty()) << kParityStatus.string() << " is missing";

    const auto pointers = testPointersIn(stripBlockquotes(doc));
    ASSERT_GE(pointers.size(), 50u)
        << "only " << pointers.size()
        << " test-file pointers found in " << kParityStatus.string()
        << "; the Tests column has evidently been gutted, and this guard would "
           "be checking almost nothing";

    std::vector<std::string> missing;
    for (const auto& p : pointers)
        if (!testFileExistsAnywhere(p)) missing.push_back(p);

    std::string joined;
    for (const auto& s : missing) { joined += "\n    "; joined += s; }
    EXPECT_TRUE(missing.empty())
        << missing.size() << " test-file pointer(s) in "
        << kParityStatus.string() << " do not resolve under tests/:" << joined
        << "\n  A dangling pointer makes a parity row read as test-backed when "
           "it is not (gaps A5-91, A5-17).";
}

// ═══════════════════════════════════════════════════════════════════════════
// Run-record claims — gap A5-97. The comparison document asserted "0 failures"
// for the UI E2E suite in the same sentence that named the artifact recording
// eleven of them.
// ═══════════════════════════════════════════════════════════════════════════

// PDFE-F45-CI-ONLY: F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummaryArtifact
// deferred-backlog row F45, closed in follow-up phase-46. This test was RED IN
// CI ONLY and green locally for six weeks, and it is NOT flaky and NOT new.
// CAUSE: the artifact it reads was matched by `.gitignore`'s `tests/_runtime/*`,
// so it existed only on the machine that produced it (2026-06-24). Every clean
// checkout — and every CI run, since actions/checkout does `git clean -ffdx` —
// hit the ASSERT_FALSE below on an empty read. The counts themselves have
// always agreed (110 files run / 11 failures on both sides).
// FIX: the artifact is now re-included and committed, because the comparison
// document cites it BY NAME as its evidence. Not skipped, not weakened.
// See docs/audit/meta-audit-2026-07/followup/phase-46-dispositions.md.
TEST(F19DocClaims, ComparisonDocUiE2eCountsMatchTheSummaryArtifact) {
    const fs::path summary = repoRoot() / "tests" / "_runtime" /
                             "discovery_phase8" / "ui-suite-summary.json";
    const std::string json = readFile(summary);
    ASSERT_FALSE(json.empty())
        << summary.string()
        << " is missing. The comparison document cites it by name as the "
           "evidence for its UI E2E figures; without it those figures rest on "
           "nothing (gap A5-97).";

    const long failures = firstNumber(json, std::regex(R"("failure_count"\s*:\s*(\d+))"));
    const long filesRun = firstNumber(json, std::regex(R"("files_run"\s*:\s*(\d+))"));
    ASSERT_GE(failures, 0) << "could not parse failure_count from " << summary.string();
    ASSERT_GT(filesRun, 0) << "could not parse files_run from " << summary.string();

    const std::string doc = readFile(repoRoot() / kComparison);
    ASSERT_FALSE(doc.empty()) << kComparison.string() << " is missing";

    const long docFiles =
        firstNumber(doc, std::regex(R"(\*\*(\d+) files run,)"));
    const long docFailures =
        firstNumber(doc, std::regex(R"(\d+ tests, (\d+) failures)"));
    ASSERT_GE(docFiles, 0) << kComparison.string()
                           << " no longer states the UI E2E files-run figure";
    ASSERT_GE(docFailures, 0) << kComparison.string()
                              << " no longer states the UI E2E failure figure";

    EXPECT_EQ(docFiles, filesRun)
        << kComparison.string() << " says " << docFiles
        << " UI E2E files run; the artifact says " << filesRun;
    EXPECT_EQ(docFailures, failures)
        << kComparison.string() << " says " << docFailures
        << " UI E2E failures; the artifact it cites says " << failures
        << ". This is the exact shape of the original defect (gap A5-97): a "
           "ledger contradicting its own named evidence.";

    // The specific false claim, verbatim, must not return as an assertion.
    EXPECT_FALSE(contains(stripBlockquotes(doc),
                          "87 test files / 3015 tests / **0 failures**"))
        << "the stale, artifact-contradicted UI E2E claim is back in "
        << kComparison.string();
}

// The suite-size figures in both ledgers were stale by 1647 and 1035 macros.
// Rather than pin an exact number that reds on every new test, pin the
// property that actually mattered: both documents claimed "0 disabled", and
// that claim is worth something only if it is re-derived.
TEST(F19DocClaims, SuiteReallyHasNoDisabledTests) {
    const fs::path tests = repoRoot() / "tests";
    ASSERT_TRUE(fs::exists(tests)) << tests.string();

    const std::regex disabledMacro(
        R"(^[ \t]*TEST(?:_F|_P)?\([ \t]*[A-Za-z0-9_]+[ \t]*,[ \t]*DISABLED_)");
    std::vector<std::string> offenders;
    int scanned = 0;
    for (const auto& e : fs::directory_iterator(tests)) {
        std::error_code ec;
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".cpp") continue;
        ++scanned;
        std::istringstream in(readFile(e.path()));
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (std::regex_search(line, disabledMacro))
                offenders.push_back(e.path().filename().string() + ":" +
                                    std::to_string(n));
        }
    }
    ASSERT_GT(scanned, 100) << "only " << scanned
                            << " test sources scanned; guard is not meaningful";

    std::string joined;
    for (const auto& s : offenders) { joined += "\n    "; joined += s; }
    EXPECT_TRUE(offenders.empty())
        << offenders.size()
        << " DISABLED_-prefixed test(s) exist, while "
        << kParityStatus.string() << " and " << kManualVerification.string()
        << " both assert the suite has none:" << joined
        << "\n  Either re-enable them or correct both documents. A locked-off "
           "test that the ledger does not disclose is coverage the reader "
           "believes they have.";
}

// Gap A5-45. §4 and §6 of the manual checklist were written around a
// `DISABLED_OcrRecall` case. The test is live under a different name, and the
// checklist's instructions for it could not be followed.
TEST(F19DocClaims, ManualVerificationDocMatchesTheTree) {
    const std::string doc = readFile(repoRoot() / kManualVerification);
    ASSERT_FALSE(doc.empty()) << kManualVerification.string() << " is missing";

    const std::string e2e =
        readFile(repoRoot() / "tests" / "test_e2e_integration.cpp");
    ASSERT_FALSE(e2e.empty()) << "tests/test_e2e_integration.cpp is missing";

    ASSERT_TRUE(contains(e2e, "CleanScanRecallAboveNinetyFivePercent"))
        << "the live OCR-recall test has been renamed again; "
        << kManualVerification.string() << " names it and must be updated";

    EXPECT_TRUE(contains(doc, "CleanScanRecallAboveNinetyFivePercent"))
        << kManualVerification.string()
        << " does not name the live OCR-recall test (gap A5-45)";

    // The instruction that had no referent for months.
    EXPECT_FALSE(contains(doc, "Rename the test from `DISABLED_OcrRecall`"))
        << kManualVerification.string()
        << " is back to telling the reader to rename a test that does not "
           "exist (gap A5-45)";

    // Criterion 5: the per-item automation disposition must be present.
    EXPECT_TRUE(contains(doc, "## 8. Automation disposition (F19)"))
        << kManualVerification.string()
        << " has lost its per-item automation disposition table. Without it "
           "the document is a list of manual items with no record of which "
           "ones are manual for a REASON versus merely unwritten.";

    // Every fixture path the checklist tells the reader to open must exist,
    // apart from ones it explicitly flags as missing. This is what caught
    // tests\fixtures\test_attachments.pdf, which the doc referenced twice.
    const std::regex fixRe(R"(tests\\fixtures\\([A-Za-z0-9_]+\.pdf))");
    std::set<std::string> named;
    for (std::sregex_iterator it(doc.begin(), doc.end(), fixRe), end; it != end; ++it)
        named.insert((*it)[1].str());
    ASSERT_FALSE(named.empty())
        << "no tests\\fixtures\\*.pdf references parsed out of "
        << kManualVerification.string() << "; guard would be vacuous";

    for (const auto& f : named) {
        const bool onDisk = fs::exists(repoRoot() / "tests" / "fixtures" / f);
        const bool flagged =
            contains(doc, "`tests\\fixtures\\" + f + "`**\n> (`ls") ||
            contains(doc, f + "`. **That file does not exist**") ||
            contains(doc, "**That file does not exist**");
        EXPECT_TRUE(onDisk || flagged)
            << kManualVerification.string() << " tells the reader to open "
            << "tests\\fixtures\\" << f
            << ", which does not exist and is not flagged as missing";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Coverage-ledger claims — A4 cluster.
// ═══════════════════════════════════════════════════════════════════════════

// Gaps A4-02 / A4-24. closure.md §C-02 said "the RSS-delta logic is
// cross-platform". The whole test is inside #ifdef _WIN32 with an
// unconditional GTEST_SKIP on the #else branch, so Linux and macOS get zero
// C-02 coverage. The doc must not describe portability the test lacks.
TEST(F19DocClaims, EnduranceLeakIsWindowsOnlyAndDocSaysSo) {
    const std::string src =
        readFile(repoRoot() / "tests" / "test_endurance_leak.cpp");
    ASSERT_FALSE(src.empty()) << "tests/test_endurance_leak.cpp is missing";

    const bool winOnly = contains(src, "#ifdef _WIN32") &&
                         contains(src, "GTEST_SKIP");
    ASSERT_TRUE(contains(src, "GetProcessMemoryInfo"))
        << "the endurance oracle no longer samples Win32 counters; re-check "
           "closure.md §C-02 by hand";

    const std::string doc = readFile(repoRoot() / kClosure);
    ASSERT_FALSE(doc.empty()) << kClosure.string() << " is missing";

    // The exact false sentence.
    EXPECT_FALSE(contains(doc, "the RSS-delta logic is\n  cross-platform"))
        << kClosure.string()
        << " again claims the C-02 RSS-delta logic is cross-platform. It is "
           "Win32-only, and on other platforms nothing runs at all "
           "(gaps A4-02, A4-24).";
    EXPECT_FALSE(contains(doc, "the RSS-delta logic is cross-platform"))
        << kClosure.string() << " (same claim, rewrapped)";

    if (winOnly) {
        EXPECT_TRUE(contains(doc, "ZERO C-02"))
            << kClosure.string()
            << " must state plainly that the Linux and macOS lanes get zero "
               "C-02 coverage while the test is Windows-only (gap A4-02)";
    }
}

// Gaps A4-08 / A4-25. tests/test_golden_reference_breadth.py is baselined but
// selected by no lane: not by the explicit pytest lists in fast-checks.yml or
// governance.yml, not by windows-validation.yml's render glob, and not by the
// three globs in run_ui_suite.py. closure.md called it "COVERED (here)".
TEST(F19DocClaims, BreadthGoldensAreNotSelectedByAnyLane) {
    const fs::path module = repoRoot() / "tests" / "test_golden_reference_breadth.py";
    ASSERT_TRUE(fs::exists(module))
        << module.string()
        << " is gone. If the breadth goldens were deleted, closure.md §C-08 "
           "must stop describing them at all (gap A4-08).";

    // Re-derive reachability: does anything name the module, or glob a
    // directory it lives in?
    bool named = false;
    const fs::path wf = repoRoot() / ".github" / "workflows";
    if (fs::exists(wf)) {
        for (const auto& e : fs::directory_iterator(wf)) {
            std::error_code ec;
            if (!e.is_regular_file(ec)) continue;
            if (contains(readFile(e.path()), "test_golden_reference_breadth"))
                named = true;
        }
    }
    const fs::path runner = repoRoot() / "tests" / "scripts" / "run_ui_suite.py";
    if (fs::exists(runner) &&
        contains(readFile(runner), "test_golden_reference_breadth"))
        named = true;

    const std::string doc = readFile(repoRoot() / kClosure);
    ASSERT_FALSE(doc.empty()) << kClosure.string() << " is missing";

    if (!named) {
        EXPECT_TRUE(contains(doc, "NO LANE RUNS IT"))
            << kClosure.string()
            << " §C-08 does not disclose that nothing selects "
               "tests/test_golden_reference_breadth.py. It was recorded as "
               "'COVERED (here)' while being unreachable from every lane "
               "(gaps A4-08, A4-25).";
        EXPECT_FALSE(contains(doc, "- **COVERED (here):** `tests/test_golden_reference_breadth.py`"))
            << kClosure.string()
            << " has reverted to claiming the breadth goldens are covered";
    } else {
        EXPECT_FALSE(contains(doc, "NO LANE RUNS IT"))
            << "a lane now selects the breadth goldens; " << kClosure.string()
            << " must be updated to stop saying nothing runs them";
    }
}

// Gap A4-33. COVERAGE.md disclaimed shear and rotation ambient fixtures as
// "not enumerated". Six hard-asserting cells for exactly those exist. A
// residual claimed WIDER than reality is the same drift as one claimed
// narrower, and hides work that was actually done.
TEST(F19DocClaims, CtmResetShearAndRotateCellsExist) {
    const std::string src =
        readFile(repoRoot() / "tests" / "test_content_stream_ctm_reset.cpp");
    ASSERT_FALSE(src.empty())
        << "tests/test_content_stream_ctm_reset.cpp is missing";

    const char* shear[] = {
        "AlignmentInjectorUprightUnshearedUnderAmbientShear",
        "HeaderFooterEngineUprightUnshearedUnderAmbientShear",
        "TreWriteTextBlockUprightUnshearedUnderAmbientShear"};
    const char* rotate[] = {
        "AlignmentInjectorUprightUnrotatedUnderAmbientRotate",
        "HeaderFooterEngineUprightUnrotatedUnderAmbientRotate",
        "TreWriteTextBlockUprightUnrotatedUnderAmbientRotate"};

    // Match the DECLARATION, not the bare name. A substring check survives a
    // rename that merely appends (…AmbientShear -> …AmbientShearRenamed),
    // which a mutation run caught: the guard has to pin the exact test that
    // the documentation's claim rests on.
    auto declared = [&src](const char* name) {
        const std::regex re(R"(\bTEST\(\s*CtmReset\s*,\s*)" + std::string(name) +
                            R"(\s*\))");
        return std::regex_search(src, re);
    };

    int found = 0;
    for (const char* n : shear)  if (declared(n)) ++found;
    for (const char* n : rotate) if (declared(n)) ++found;
    EXPECT_EQ(found, 6)
        << "expected 6 shear/rotate ambient CtmReset cells, found " << found
        << ". docs/content-stream-continuity/COVERAGE.md is written on the "
           "basis that all six exist (gap A4-33).";

    const std::string doc = readFile(repoRoot() / kCsCoverage);
    ASSERT_FALSE(doc.empty()) << kCsCoverage.string() << " is missing";
    EXPECT_FALSE(contains(doc, "Other imposed scales\n  / skews / rotations are not enumerated"))
        << kCsCoverage.string()
        << " again disclaims skew and rotation coverage that exists (A4-33)";
    EXPECT_FALSE(contains(doc, "/ skews / rotations are not enumerated"))
        << kCsCoverage.string() << " (same claim, rewrapped)";
}

// Gap A4-36. The doc said the image rotate/crop cells assert "presence +
// not-halved". Both pass expectWidth=0, which switches the width check off, so
// they are presence-only. Overstating an oracle's strength is the failure this
// pipeline exists to catch.
TEST(F19DocClaims, ImageRotateAndCropCellsArePresenceOnly) {
    const std::string src = readFile(
        repoRoot() / "tests" / "test_content_stream_adversarial_ambient.cpp");
    ASSERT_FALSE(src.empty())
        << "tests/test_content_stream_adversarial_ambient.cpp is missing";

    // Count evalImageCell call sites that disable the width check.
    const std::regex kDisabled(R"(evalImageCell\([^;]*expectWidth=\*/\s*0\s*\))");
    const std::regex kAnyCall(R"(evalImageCell\()");

    const auto n = std::distance(
        std::sregex_iterator(src.begin(), src.end(), kDisabled),
        std::sregex_iterator());
    const auto total = std::distance(
        std::sregex_iterator(src.begin(), src.end(), kAnyCall),
        std::sregex_iterator());
    ASSERT_GT(total, 0)
        << "evalImageCell is gone; the A4-36 residual must be re-assessed";

    const std::string doc = readFile(repoRoot() / kCsCoverage);
    ASSERT_FALSE(doc.empty()) << kCsCoverage.string() << " is missing";

    if (n > 0) {
        EXPECT_TRUE(contains(doc, "PRESENCE-ONLY"))
            << kCsCoverage.string() << " describes the image rotate/crop "
            << "oracles as stronger than they are: " << n
            << " evalImageCell call site(s) pass expectWidth=0, disabling the "
               "not-halved check entirely (gap A4-36).";
    } else {
        EXPECT_FALSE(contains(doc, "PRESENCE-ONLY"))
            << "every evalImageCell call site now supplies a width "
               "expectation; " << kCsCoverage.string()
            << " should stop calling the cells presence-only";
    }
}

// Gap A4-41. The register recorded that the pipeline-3f8a1c72 scope comment
// landed on PageOperations only and not on the two compliance converters.
//
// REWRITTEN 2026-08-12 (WIN-87 phase-59) FROM A HAND-COPIED LIST INTO AN
// ENUMERATION, because the list was wrong in BOTH directions at once.
//
//  * It went RED for a correct change. The two converters no longer set these
//    flags at all - they publish through Document::saveAsThroughExternalRewrite
//    and the flags moved with the write. Keeping a scope comment in a file that
//    no longer performs the thing it scopes would be decoration retained purely
//    to satisfy a grep, which is how a suite starts lying.
//  * It was SILENT about a site it never named. src/optimize/OptimizeEngine.cpp
//    has set do_compress_images/do_compress_fonts for as long as this guard has
//    existed and carried no scope note, because it was not one of the three
//    files typed into the list. A class-level claim whose corpus is a
//    hand-copied list is not class-level - this project has now hit that
//    exactly twice.
//
// So the guard now DISCOVERS the sites: every .cpp under src/ that sets either
// flag must carry the scope note. New write sites are covered automatically,
// and a site that stops writing stops being required to explain the write.
TEST(F19DocClaims, EveryRecompressionFlagSiteCarriesTheScopeComment) {
    std::vector<fs::path> sites;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(repoRoot() / "src", ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".cpp") continue;
        const std::string body = readFile(it->path());
        if (contains(body, "do_compress_images")
            || contains(body, "do_compress_fonts")) {
            sites.push_back(it->path());
        }
    }

    // VACUITY GUARD. An empty sweep satisfies every expectation below, and "no
    // site sets these flags" is false about this codebase - so a small result
    // means the walk broke, not that the claim holds.
    ASSERT_GE(sites.size(), 4u)
        << "found only " << sites.size() << " recompression write site(s) under "
        << "src/; this guard has stopped enumerating rather than started "
           "passing";

    for (const auto& site : sites) {
        const std::string body = readFile(site);
        EXPECT_TRUE(contains(body, "3f8a1c72"))
            << site.string()
            << " sets do_compress_images/do_compress_fonts without the "
               "pipeline-3f8a1c72 recompression scope note saying why that is "
               "deliberate and why it cannot be lossy (gap A4-41).";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Process-documentation claims — gaps A5-49, A5-50, A5-69.
// ═══════════════════════════════════════════════════════════════════════════

// A5-49: docs/gate-verification.md named `.claude/hooks/phase-gate.sh` as "the
// repo-local hook" and the enforcement mechanism. That directory does not
// exist; .claude/ is gitignored and only settings.json is tracked, so a clone
// gets no hooks at all. A5-50: the two gate helper scripts live in
// ~/.claude/hooks/, not tools/gate/.
TEST(F19DocClaims, GateVerificationDocDoesNotClaimARepoLocalHook) {
    const std::string doc = readFile(repoRoot() / kGateVerification);
    ASSERT_FALSE(doc.empty()) << kGateVerification.string() << " is missing";

    const bool repoLocalHookExists =
        fs::exists(repoRoot() / ".claude" / "hooks" / "phase-gate.sh");

    if (!repoLocalHookExists) {
        EXPECT_FALSE(contains(doc, "The repo-local hook `.claude/hooks/phase-gate.sh` **never reads"))
            << kGateVerification.string()
            << " again presents a repo-local hook as the enforcement "
               "mechanism, while .claude/hooks/ does not exist in this tree "
               "(gap A5-49).";
        EXPECT_TRUE(contains(doc, "There is no repo-local hook"))
            << kGateVerification.string()
            << " must state that the hook is the user-global "
               "~/.claude/hooks/phase-gate.sh and that the control is "
               "therefore not reproducible from a clone (gap A5-49).";
    } else {
        EXPECT_FALSE(contains(doc, "There is no repo-local hook"))
            << "a repo-local hook exists again; " << kGateVerification.string()
            << " must be updated to describe it";
    }

    // A5-50: no repository document may hand out the tools/gate/ path for the
    // two shell helpers, because nothing resolves there.
    ASSERT_FALSE(fs::exists(repoRoot() / "tools" / "gate" / "emit-gate-verdict.sh"))
        << "emit-gate-verdict.sh now exists under tools/gate/; update this "
           "guard and the A5-50 disposition";
    EXPECT_FALSE(contains(doc, "tools/gate/emit-gate-verdict.sh"))
        << kGateVerification.string()
        << " hands out tools/gate/emit-gate-verdict.sh, which does not exist. "
           "The script lives in ~/.claude/hooks/ (gap A5-50).";
    EXPECT_TRUE(contains(doc, "~/.claude/hooks/emit-gate-verdict.sh"))
        << kGateVerification.string()
        << " must name the path that actually resolves (gap A5-50).";
}

// A5-50, the other half. CLAUDE.md is the project's PRIMARY instruction file,
// and its Gate Reliability Protocol told the reader to run
// `tools/gate/emit-gate-verdict.sh` and `tools/gate/check-hooks-alive.sh`.
// Neither has ever existed — tools/gate/ holds only Python checkers. A gate
// agent following that text verbatim would fail to emit its verdict at all,
// which is the single most load-bearing step in the protocol.
//
// F19 could only escalate this (a subagent may not edit CLAUDE.md); it was
// corrected by the orchestrator in the same commit. This guard exists so the
// correction cannot silently regress, and so the same wrong path cannot be
// reintroduced anywhere in that file.
TEST(F19DocClaims, ClaudeMdNamesGateHelperPathsThatResolve) {
    const fs::path kClaudeMd = fs::path("CLAUDE.md");
    const std::string doc = readFile(repoRoot() / kClaudeMd);
    ASSERT_FALSE(doc.empty()) << "CLAUDE.md is missing";

    // Re-derive rather than assume: these must be true for the guard to mean
    // anything. If either flips, the guard is what should be updated.
    ASSERT_FALSE(fs::exists(repoRoot() / "tools" / "gate" / "emit-gate-verdict.sh"))
        << "emit-gate-verdict.sh now exists under tools/gate/; update this "
           "guard and the A5-50 disposition";
    ASSERT_FALSE(fs::exists(repoRoot() / "tools" / "gate" / "check-hooks-alive.sh"))
        << "check-hooks-alive.sh now exists under tools/gate/; update this "
           "guard and the A5-50 disposition";

    // The correction note in CLAUDE.md necessarily QUOTES the wrong paths in
    // order to explain what was wrong. Scanning the raw file would therefore
    // flag the very note that documents the fix. Strip that one explicitly
    // marked blockquote and scan the rest, so the check still fails on any
    // INSTRUCTIONAL use of the bad path anywhere else in the file.
    std::string instructional = doc;
    const std::string kNoteMarker = "PATH CORRECTION (meta-audit follow-up F19";
    const auto noteAt = instructional.find(kNoteMarker);
    ASSERT_NE(noteAt, std::string::npos)
        << "CLAUDE.md no longer carries the A5-50 PATH CORRECTION note. Either "
           "restore it or update this guard — silently dropping the note would "
           "lose the record of why these paths changed.";
    // The note is a markdown blockquote; it ends at the first line that does
    // not begin with '>'. Cut from the start of that blockquote to its end.
    auto lineStart = instructional.rfind('\n', noteAt);
    lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
    auto scan = lineStart;
    while (scan < instructional.size()) {
        const auto eol = instructional.find('\n', scan);
        const auto len = (eol == std::string::npos)
                             ? instructional.size() - scan : eol - scan;
        const std::string line = instructional.substr(scan, len);
        const bool isQuote = !line.empty() && line[0] == '>';
        if (!isQuote) break;
        scan = (eol == std::string::npos) ? instructional.size() : eol + 1;
    }
    instructional.erase(lineStart, scan - lineStart);

    EXPECT_FALSE(contains(instructional, "tools/gate/emit-gate-verdict.sh"))
        << "CLAUDE.md hands out tools/gate/emit-gate-verdict.sh, which does "
           "not resolve. A gate agent that follows it emits no verdict at all "
           "(gap A5-50). The script lives in ~/.claude/hooks/.";
    EXPECT_FALSE(contains(instructional, "tools/gate/check-hooks-alive.sh"))
        << "CLAUDE.md hands out tools/gate/check-hooks-alive.sh, which does "
           "not resolve (gap A5-50).";

    EXPECT_TRUE(contains(doc, "~/.claude/hooks/emit-gate-verdict.sh"))
        << "CLAUDE.md must name the emitter path that actually resolves "
           "(gap A5-50).";
    EXPECT_TRUE(contains(doc, "~/.claude/hooks/check-hooks-alive.sh"))
        << "CLAUDE.md must name the liveness-check path that actually "
           "resolves (gap A5-50).";
}

// Does `body` mention XFA as a WORD, rather than as three letters inside a
// longer one?
//
// This used to be `contains(body, "XFA") || contains(body, "xfa")`, and in
// p87 phase-4 it went red the moment an unrelated comment in MainWindow.cpp
// used the word "x-f-a-i-l" — which contains "xfa". The guard then reported
// "a user-facing XFA surface now exists in src/", which was false: no XFA
// surface existed, and none had been added.
//
// That is the noisy-guard shape this project has hit before (a path-hygiene
// check once matched `USER=runner` and buried four real failures under 365
// false ones). A guard that cries wolf on an unrelated word gets its finding
// ignored, or worse, gets the real claim edited to silence it.
//
// So: require a non-letter (or a boundary) on each side. "XFA", "XFA-only"
// and "(xfa)" still match; "xfail" and "XFADummy" do not.
static bool mentionsXfaAsAToken(const std::string& body)
{
    std::string hay(body.size(), '\0');
    std::transform(body.begin(), body.end(), hay.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string needle = "xfa";
    for (std::size_t i = hay.find(needle); i != std::string::npos;
         i = hay.find(needle, i + 1)) {
        // Left boundary: start of file, a non-letter, or a camelCase hump --
        // the 'X' of `isXfa()` is upper while the char before it is lower.
        // Without the hump case a real identifier like isXfa()/hasXfaForm()
        // would be missed, which is the failure direction that matters.
        const bool leftOk =
            (i == 0) ||
            !std::isalpha(static_cast<unsigned char>(hay[i - 1])) ||
            (std::isupper(static_cast<unsigned char>(body[i])) &&
             std::islower(static_cast<unsigned char>(body[i - 1])));
        // Right boundary: end of file, a non-letter, or the next word starting
        // (`XFAForm`). What must NOT pass is a lowercase run continuing the
        // same word -- that is `xfail`.
        const std::size_t after = i + needle.size();
        const bool rightOk =
            (after >= hay.size()) ||
            !std::isalpha(static_cast<unsigned char>(hay[after])) ||
            std::isupper(static_cast<unsigned char>(body[after]));
        if (leftOk && rightOk) return true;
    }
    return false;
}

// Pins mentionsXfaAsAToken in BOTH directions. Only the positives would let it
// regress to always-true (the bug above); only the negatives would let it
// regress to always-false, which would silently stop detecting a real XFA
// surface and leave the AGPL doc's claim unguarded. Neither half is padding.
TEST(F19DocClaims, TheXfaScanMatchesTheWordAndNotALongerOne) {
    // The exact regression: an unrelated comment using "xfail".
    EXPECT_FALSE(mentionsXfaAsAToken(
        "// Three UI render probes were xfail(strict=False) for one reason"))
        << "'xfail' contains the letters x-f-a; matching it reported a "
           "user-facing XFA surface that does not exist (p87 phase-4)";
    EXPECT_FALSE(mentionsXfaAsAToken("prefixfab"));
    EXPECT_FALSE(mentionsXfaAsAToken("// see xfailing render probes"));

    // A real surface must still be found, in any of the spellings it would
    // plausibly appear in.
    EXPECT_TRUE(mentionsXfaAsAToken(R"(tr("XFA not supported"))"));
    EXPECT_TRUE(mentionsXfaAsAToken("// xfa forms are not rendered"));
    EXPECT_TRUE(mentionsXfaAsAToken("isXfa()"));      // camelCase hump
    EXPECT_TRUE(mentionsXfaAsAToken("XFADummyWidget"));  // PascalCase prefix
    EXPECT_TRUE(mentionsXfaAsAToken("XFA"));
}

// A5-69. The AGPL re-evaluation records XFA rendering as a gap "see
// mitigation", and the mitigation it points to — a "XFA not supported" dialog
// — was never built. There is no user-facing XFA string anywhere in src/.
TEST(F19DocClaims, XfaMitigationIsLabelledUnimplementedWhileItIs) {
    // Re-derive from the tree: is there any user-facing XFA surface?
    bool userFacing = false;
    for (const char* dir : {"ui", "forms"}) {
        const fs::path d = repoRoot() / "src" / dir;
        if (!fs::exists(d)) continue;
        for (const auto& e : fs::recursive_directory_iterator(d)) {
            std::error_code ec;
            if (!e.is_regular_file(ec)) continue;
            const auto ext = e.path().extension();
            if (ext != ".cpp" && ext != ".h") continue;
            const std::string body = readFile(e.path());
            if (mentionsXfaAsAToken(body)) userFacing = true;
        }
    }

    const std::string doc = readFile(repoRoot() / kAgpl);
    ASSERT_FALSE(doc.empty()) << kAgpl.string() << " is missing";
    ASSERT_TRUE(contains(doc, "XFA not supported"))
        << kAgpl.string()
        << " no longer records the proposed XFA mitigation at all; if it was "
           "withdrawn deliberately, update this guard (gap A5-69)";

    if (!userFacing) {
        EXPECT_TRUE(contains(doc, "The mitigation was never implemented"))
            << kAgpl.string()
            << " presents a 'XFA not supported' dialog as the mitigation for "
               "the XFA gap, but src/ui/ and src/forms/ contain no XFA string "
               "of any kind — the dialog does not exist. Label it unimplemented "
               "(gap A5-69).";
    } else {
        EXPECT_FALSE(contains(doc, "The mitigation was never implemented"))
            << "a user-facing XFA surface now exists in src/; " << kAgpl.string()
            << " should stop saying the mitigation was never implemented";
    }
}
