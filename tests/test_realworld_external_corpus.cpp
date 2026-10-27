// -*- coding: utf-8 -*-
//
// External real-world PDF corpus tests (REAL-WORLD-CORPUS phase).
// Complements test_realworld_corpus.cpp (which uses synthetic fixtures)
// by exercising PDFE against PDFs downloaded from public sources:
// IRS forms, USCIS forms, arXiv papers, Project Gutenberg books.
//
// The corpus is assembled by tests/scripts/assemble_realworld_corpus.py
// into tests/fixtures/realworld/. Each PDF is from a public-domain or
// government source.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {

std::filesystem::path corpusDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / "realworld";
}

// Collect PDFs from the external corpus only — we look for filenames
// matching the external-source naming convention (irs_*, uscis_*,
// arxiv_*, ssa_*, gutenberg_*). This avoids overlap with the
// synthetic test_realworld_*.pdf fixtures used by test_realworld_corpus.cpp.
std::vector<std::filesystem::path> collectExternalCorpus()
{
    std::vector<std::filesystem::path> pdfs;
    auto dir = corpusDir();
    if (!std::filesystem::exists(dir)) return pdfs;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".pdf") continue;
        auto stem = entry.path().stem().string();
        // Skip synthetic fixtures (start with "test_")
        if (stem.rfind("test_", 0) == 0) continue;
        pdfs.push_back(entry.path());
    }
    return pdfs;
}

} // namespace

class ExternalRealWorldTest
    : public ::testing::TestWithParam<std::filesystem::path> {};

struct ExternalNameGen {
    std::string operator()(
        const ::testing::TestParamInfo<std::filesystem::path>& info) const
    {
        std::string name = info.param.stem().string();
        for (auto& c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        }
        return name;
    }
};

TEST_P(ExternalRealWorldTest, OpensRendersFirstAndLastPage)
{
    auto path = GetParam();
    auto ctx = std::make_shared<MuPdfContext>();

    std::unique_ptr<Document> doc;
    ASSERT_NO_THROW(doc = Document::open(ctx, path))
        << "Failed to open " << path.filename();
    ASSERT_NE(doc, nullptr) << path.filename();

    int pageCount = doc->pageCount();
    // ASSERT, not EXPECT: both renders below index with pageCount, so a
    // zero-page document must stop the test rather than continue into
    // createDisplayList(0) and createDisplayList(-1). EXPECT_ continues past
    // a failure, which left that reachable — latent before GitHub #273 made
    // the last-page render unconditional, and a real out-of-range call after.
    ASSERT_GT(pageCount, 0) << "Document must have at least one page";

    // Render first page
    EXPECT_NO_THROW({
        auto firstDl = doc->createDisplayList(0);
        if (firstDl) doc->releaseDisplayList(firstDl);
    }) << "First page render failed for " << path.filename();

    // Render the last page. GitHub #273: this was gated on `pageCount > 1`,
    // which made it assert nothing for every single-page document in the
    // corpus. No condition is needed — for a one-page file `pageCount - 1` is
    // 0, which is a valid page index and simply renders page 0 a second time.
    // Unconditional is both simpler and strictly more coverage.
    EXPECT_NO_THROW({
        auto lastDl = doc->createDisplayList(pageCount - 1);
        if (lastDl) doc->releaseDisplayList(lastDl);
    }) << "Last page (index " << (pageCount - 1) << " of " << pageCount
       << ") render failed for " << path.filename();
}

TEST_P(ExternalRealWorldTest, SaveRoundTrip)
{
    auto path = GetParam();

    // PDFE-LIMITATION-SAVEAS-LARGE-XFA — RESOLVED (pipeline b2e47d91, Tier 2).
    // These large XFA forms were skipped because saveAs at do_garbage=3 took
    // 5-17 min each. Tier 2 lowered the default full-rewrite GC level to 1 for
    // sessions with no dedup-worthy op, so this round-trip (open + saveAs, no
    // structural edit) now saves+reopens in ~1-3 s on Release. They therefore
    // run as normal regression coverage on Release. Debug MuPDF is still
    // materially slower on these multi-MB files, so keep the opt-in gate there
    // to avoid bloating the Debug CI run. See tests/docs/known_issues.md.
    auto stemStr = path.stem().string();
    bool knownLargeXfa = (
        stemStr == "ssa_sf86" ||      // 8 MB XFA form
        stemStr == "irs_i1040" ||     // 126-page IRS instructions
        stemStr == "irs_p17"          // 142-page IRS Publication 17
    );
#ifndef NDEBUG
    if (knownLargeXfa) {
        const char* optIn = std::getenv("PDFE_RUN_SLOW_SAVE_TESTS");
        if (!optIn || optIn[0] == '\0' || optIn[0] == '0') {
            GTEST_SKIP() << "PDFE-LIMITATION-SAVEAS-LARGE-XFA: Debug build is "
                         << "slow on large XFA forms; set "
                         << "PDFE_RUN_SLOW_SAVE_TESTS=1 to run. Runs "
                         << "unconditionally on Release (Tier 2 conditional GC).";
        }
    }
#else
    (void)knownLargeXfa;  // Release: run unconditionally (gc=1, ~1-3 s)
#endif

    static int counter = 0;
    auto tmpDir = std::filesystem::temp_directory_path() /
                  "pdfe_realworld_rt" / path.stem();
    std::filesystem::create_directories(tmpDir);
    auto outPath = tmpDir / (
        path.stem().string() + "_resaved_" +
        std::to_string(++counter) + ".pdf");
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, path);
        ASSERT_NE(doc, nullptr);

        EXPECT_NO_THROW(doc->saveAs(outPath))
            << "Save round-trip threw for " << path.filename();
    }

    ASSERT_TRUE(std::filesystem::exists(outPath));
    auto savedSize = std::filesystem::file_size(outPath);
    EXPECT_GT(savedSize, 100u) << "Resaved file too small";

    EXPECT_NO_THROW({
        auto ctx2 = std::make_shared<MuPdfContext>();
        auto reopened = Document::open(ctx2, outPath);
        EXPECT_NE(reopened, nullptr);
        if (reopened) {
            EXPECT_GT(reopened->pageCount(), 0);
        }
    });
}

INSTANTIATE_TEST_SUITE_P(
    ExternalCorpus,
    ExternalRealWorldTest,
    ::testing::ValuesIn(collectExternalCorpus()),
    ExternalNameGen());

TEST(ExternalRealWorldMeta, CorpusAssembled)
{
    auto pdfs = collectExternalCorpus();
    if (pdfs.empty()) {
        GTEST_SKIP() << "External corpus not assembled. Run: "
                     << "python tests/scripts/assemble_realworld_corpus.py";
    }
    EXPECT_GE(pdfs.size(), 5u)
        << "External corpus should have at least 5 PDFs";
}
