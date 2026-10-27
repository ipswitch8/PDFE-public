// -*- coding: utf-8 -*-
// F6 / PDF-100 — the document-SPLIT tests the suite did not have.
//
// Split is `extractPagesToFile` in a loop, and before this file no test drove
// that loop at all: the four `*Split*` hits in tests/ were a compliance-dialog
// widget name and three text-run helpers. The gap is not "split is untested in
// depth", it is "split is untested".
//
// The oracle here is per-page CONTENT-MARKER IDENTITY, not page arithmetic: a
// split that produced N one-page files all containing page 0 would satisfy any
// count-based check and fails every test below.

#include <gtest/gtest.h>

#include "support/PreservationOracle.h"
#include "support/PdfContentOracle.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PageOperations.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pdfe::testsupport;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

constexpr int kSplitPages = 5;

class DocumentSplitTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code ec;
        dir_ = fs::temp_directory_path() /
               ("pdfe_f6_split_" + std::to_string(static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(dir_, ec);
        src_ = dir_ / "split_source.pdf";
        names_ = buildArtifactRichPdf(src_, kSplitPages, "S");
        ASSERT_FALSE(names_.tag.empty()) << "fixture construction failed";
        ASSERT_TRUE(fs::exists(src_));
        ctx_ = std::make_shared<MuPdfContext>();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);   // best effort; a held handle must not fail the test
    }

    /// Split the source into one file per page. Returns the part paths in order.
    std::vector<fs::path> splitAll() {
        std::vector<fs::path> parts;
        auto doc = Document::open(ctx_, src_);
        EXPECT_NE(doc, nullptr);
        if (!doc) return parts;
        EXPECT_EQ(doc->pageCount(), kSplitPages);
        for (int i = 0; i < kSplitPages; ++i) {
            const fs::path out = dir_ / ("part_" + std::to_string(i) + ".pdf");
            std::vector<int> pages = {i};
            pdfe::core::extractPagesToFile(ctx_, doc.get(), pages, out);
            parts.push_back(out);
        }
        return parts;
    }

    fs::path dir_;
    fs::path src_;
    ArtifactFixtureNames names_;
    std::shared_ptr<MuPdfContext> ctx_;
};

TEST_F(DocumentSplitTest, SplitIntoNFilesCoversEveryPageExactlyOnce)
{
    const auto parts = splitAll();
    ASSERT_EQ(parts.size(), static_cast<size_t>(kSplitPages));

    std::multiset<std::string> seenMarkers;
    for (int i = 0; i < kSplitPages; ++i) {
        SCOPED_TRACE("part " + std::to_string(i));
        ASSERT_TRUE(fs::exists(parts[(size_t)i]));
        const DocArtifacts a = readArtifacts(parts[(size_t)i]);
        ASSERT_TRUE(DocReadable(a));
        EXPECT_EQ(a.pageCount, 1);

        const std::string content = decompressedPageContent(parts[(size_t)i], 0);
        // Identity, not arithmetic: this part must hold page i's marker and no
        // other page's marker.
        EXPECT_NE(content.find(names_.contentMarker(i)), std::string::npos)
            << "part " << i << " does not contain page " << i << "'s content marker";
        for (int j = 0; j < kSplitPages; ++j) {
            if (j == i) continue;
            EXPECT_EQ(content.find(names_.contentMarker(j)), std::string::npos)
                << "part " << i << " also contains page " << j << "'s content";
        }
        seenMarkers.insert(names_.contentMarker(i));
    }
    // Every source page appears exactly once across the whole split.
    for (int i = 0; i < kSplitPages; ++i) {
        EXPECT_EQ(seenMarkers.count(names_.contentMarker(i)), 1u);
    }
    EXPECT_EQ(seenMarkers.size(), static_cast<size_t>(kSplitPages));
}

TEST_F(DocumentSplitTest, SplitPartsKeepTheirOwnPageArtifacts)
{
    const auto parts = splitAll();
    ASSERT_EQ(parts.size(), static_cast<size_t>(kSplitPages));

    for (int i = 0; i < kSplitPages; ++i) {
        SCOPED_TRACE("part " + std::to_string(i));
        const DocArtifacts a = readArtifacts(parts[(size_t)i]);
        ASSERT_TRUE(DocReadable(a));

        EXPECT_TRUE(HasOutlineTitles(a, {names_.bookmark(i)}));
        EXPECT_TRUE(HasFieldValues(a, {names_.fieldValue(i)}));
        EXPECT_TRUE(HasAnnotContents(a, {names_.annotContents(i)}));
        EXPECT_TRUE(a.hasNamedDest(names_.destName(i)));
        EXPECT_TRUE(a.hasOcg(names_.ocgName()));
        EXPECT_TRUE(a.hasEmbeddedFile(names_.embeddedFile()));
        EXPECT_TRUE(a.hasPageLabelsKey);
        EXPECT_TRUE(a.hasStructTreeRoot);
        EXPECT_EQ(a.structPageRoles.size(), 1u);
    }
}

TEST_F(DocumentSplitTest, SplitPartsCarryNoReferenceToPagesTheyDoNotContain)
{
    const auto parts = splitAll();
    ASSERT_EQ(parts.size(), static_cast<size_t>(kSplitPages));

    for (int i = 0; i < kSplitPages; ++i) {
        SCOPED_TRACE("part " + std::to_string(i));
        const DocArtifacts a = readArtifacts(parts[(size_t)i]);
        ASSERT_TRUE(DocReadable(a));

        // A bookmark or a named destination that survived pointing at a page
        // that is not in this part is a dangling reference -- its own defect.
        EXPECT_TRUE(NoDanglingDestinations(a));
        for (int j = 0; j < kSplitPages; ++j) {
            if (j == i) continue;
            EXPECT_FALSE(a.hasOutlineTitle(names_.bookmark(j)));
            EXPECT_FALSE(a.hasNamedDest(names_.destName(j)));
            EXPECT_FALSE(a.hasFieldValue(names_.fieldValue(j)));
            EXPECT_FALSE(a.hasAnnotContents(names_.annotContents(j)));
        }
    }
}

} // namespace
