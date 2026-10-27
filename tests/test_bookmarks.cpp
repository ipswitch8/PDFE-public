// PHASE1_HARDENING: skip=0 expect_ge=1 no_throw=1  (2026-05-08)
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/BookmarkNode.h"
#include "core/BookmarkExtractor.h"

#include <filesystem>
#include <memory>

namespace {
std::filesystem::path getFixturePath(const std::string& filename) {
    std::filesystem::path fixturesDir(TEST_FIXTURES_DIR);
    return fixturesDir / filename;
}

bool fixtureExists(const std::string& filename) {
    return std::filesystem::exists(getFixturePath(filename));
}
} // anonymous namespace

class BookmarkTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
    }

    void TearDown() override {
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
};

TEST_F(BookmarkTest, ExtractBookmarks) {
    // This test requires a PDF with bookmarks
    // For now, we'll test with available fixtures and handle gracefully
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    // Extract bookmarks - may be empty if document has no bookmarks
    auto bookmarks = doc->extractBookmarks();

    // bookmarks.size() is unsigned; >= 0 is always true. The real
    // contract is that the call returned without crashing AND every
    // bookmark in the result is well-formed. Drop the always-true
    // size check; keep the loop's structural validation.
    for (const auto& bookmark : bookmarks) {
        ASSERT_NE(bookmark, nullptr) << "extractBookmarks must not return null entries";
        // Title is allowed to be empty; .size() on a string is
        // always >= 0 (unsigned), so the assertion below is also
        // always-true and is removed. The real check is reachability.
        (void)bookmark->title;
    }
}

TEST_F(BookmarkTest, NoBookmarks) {
    if (!fixtureExists("test_single.pdf")) {
        GTEST_SKIP() << "test_single.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_single.pdf"));
    ASSERT_NE(doc, nullptr);

    // Extract bookmarks from document that likely has none
    auto bookmarks = doc->extractBookmarks();

    // A3-19/A3-41 (over-tolerant-threshold): `EXPECT_GE(size(), 0u)` cannot
    // fail. test_single.pdf is a committed fixture with NO /Outlines at all, so
    // the extraction result is knowable exactly.
    EXPECT_TRUE(bookmarks.empty())
        << "test_single.pdf has no /Outlines; extraction invented "
        << bookmarks.size() << " bookmark(s)";
    for (const auto& bm : bookmarks) {
        EXPECT_NE(bm, nullptr);
    }
}

TEST_F(BookmarkTest, BookmarkHierarchy) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    auto bookmarks = doc->extractBookmarks();

    // Verify hierarchical structure is accessible
    for (const auto& bookmark : bookmarks) {
        ASSERT_NE(bookmark, nullptr);

        // Verify children are accessible and structurally valid
        for (const auto& child : bookmark->children) {
            ASSERT_NE(child, nullptr);
            // A3-19/A3-41: `title.size() >= 0u` is always true. A bookmark that
            // renders as a blank row in the outline panel is a defect, so the
            // real contract is that a child carries a title at all.
            EXPECT_FALSE(child->title.empty())
                << "a child bookmark with an empty title is unusable in the "
                   "outline panel";
            EXPECT_TRUE(
                child->destination.type == pdfe::core::BookmarkDestType::Page ||
                child->destination.type == pdfe::core::BookmarkDestType::URI ||
                child->destination.type == pdfe::core::BookmarkDestType::NamedDest ||
                child->destination.type == pdfe::core::BookmarkDestType::Unknown
            );
        }
    }
}

TEST_F(BookmarkTest, PageDestinations) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    auto bookmarks = doc->extractBookmarks();

    int pageCount = doc->pageCount();

    // Verify page destinations
    std::function<void(const std::shared_ptr<pdfe::core::BookmarkNode>&)> checkNode;
    checkNode = [&](const std::shared_ptr<pdfe::core::BookmarkNode>& node) {
        if (!node) return;

        if (node->destination.type == pdfe::core::BookmarkDestType::Page) {
            // Page index should be valid
            EXPECT_GE(node->destination.pageIndex, -1);
            if (node->destination.pageIndex >= 0) {
                EXPECT_LT(node->destination.pageIndex, pageCount);
            }
        }

        // Check children recursively
        for (const auto& child : node->children) {
            checkNode(child);
        }
    };

    for (const auto& bookmark : bookmarks) {
        checkNode(bookmark);
    }
}

TEST_F(BookmarkTest, BookmarkDestinationTypes) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    auto bookmarks = doc->extractBookmarks();

    // Verify destination types are valid
    std::function<void(const std::shared_ptr<pdfe::core::BookmarkNode>&)> checkNode;
    checkNode = [&](const std::shared_ptr<pdfe::core::BookmarkNode>& node) {
        if (!node) return;

        // Destination type should be one of the valid enum values
        EXPECT_TRUE(
            node->destination.type == pdfe::core::BookmarkDestType::Page ||
            node->destination.type == pdfe::core::BookmarkDestType::URI ||
            node->destination.type == pdfe::core::BookmarkDestType::NamedDest ||
            node->destination.type == pdfe::core::BookmarkDestType::Unknown
        );

        // Check children recursively
        for (const auto& child : node->children) {
            checkNode(child);
        }
    };

    for (const auto& bookmark : bookmarks) {
        checkNode(bookmark);
    }
}

TEST_F(BookmarkTest, ExtractorWithNullDocument) {
    pdfe::core::BookmarkExtractor extractor(context);

    // Should throw on null document
    EXPECT_THROW(extractor.extract(nullptr), pdfe::core::PdfeException);
}

TEST_F(BookmarkTest, URIDestinations) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    auto bookmarks = doc->extractBookmarks();

    // Check for URI destinations
    std::function<void(const std::shared_ptr<pdfe::core::BookmarkNode>&)> checkNode;
    checkNode = [&](const std::shared_ptr<pdfe::core::BookmarkNode>& node) {
        if (!node) return;

        if (node->destination.type == pdfe::core::BookmarkDestType::URI) {
            // URI should not be empty
            EXPECT_FALSE(node->destination.uri.empty());
        }

        // Check children recursively
        for (const auto& child : node->children) {
            checkNode(child);
        }
    };

    for (const auto& bookmark : bookmarks) {
        checkNode(bookmark);
    }
}

TEST_F(BookmarkTest, BookmarkOpenState) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    auto bookmarks = doc->extractBookmarks();

    // Verify isOpen flag is accessible
    std::function<void(const std::shared_ptr<pdfe::core::BookmarkNode>&)> checkNode;
    checkNode = [&](const std::shared_ptr<pdfe::core::BookmarkNode>& node) {
        if (!node) return;

        // isOpen is a bool member; reading it cannot throw, so a bare
        // EXPECT_NO_THROW is meaningless. The contract being tested is
        // "the value is reachable without UB". A simple read+ignore
        // exercises that without dressing it up as a throw guard.
        const bool open = node->isOpen;
        (void)open;

        // Check children recursively
        for (const auto& child : node->children) {
            checkNode(child);
        }
    };

    for (const auto& bookmark : bookmarks) {
        checkNode(bookmark);
    }
}
