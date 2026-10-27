// PHASE5_ROUNDTRIP_METADATA: write -> save -> reopen -> readback
// tests for the metadata subsystem. Closes the audit's gap that
// test_metadata_manager.cpp asserts internal-state-only on the
// writeMetadata side without verifying the bytes round-trip cleanly,
// especially for Unicode (RTL + CJK + emoji) values.

#include <gtest/gtest.h>

#include "metadata/MetadataManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include "helpers/RoundTripHelper.h"

#include <filesystem>
#include <memory>
#include <string>

using pdfe::metadata::DocumentMetadata;
using pdfe::metadata::MetadataManager;

namespace {

class MetadataRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_));
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<MetadataManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_metadata_roundtrip" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        manager_.reset();
        document_.reset();
        ctx_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<MetadataManager> manager_;
};

// ─── ASCII round-trip ───────────────────────────────────────────────

TEST_F(MetadataRoundTripTest, AsciiMetadataExactStringMatchOnReopen) {
    DocumentMetadata m;
    m.title    = "Round-trip ASCII title";
    m.author   = "AsciiAuthor";
    m.subject  = "Round-trip subject line";
    m.keywords = "round, trip, ascii";
    m.creator  = "PdfeRoundTripTest";
    manager_->writeMetadata(document_.get(), m);

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto readBack = manager_->readMetadata(reopened.get());
    EXPECT_EQ(readBack.title,    m.title);
    EXPECT_EQ(readBack.author,   m.author);
    EXPECT_EQ(readBack.subject,  m.subject);
    EXPECT_EQ(readBack.keywords, m.keywords);
    EXPECT_EQ(readBack.creator,  m.creator);
}

// ─── Empty-string clears (not stale value) ──────────────────────────

TEST_F(MetadataRoundTripTest, EmptyStringClearsTheFieldOnReopen) {
    // Stage with a non-empty value, then write an empty string and
    // confirm the saved PDF has the empty string back (not the prior
    // non-empty value).
    DocumentMetadata m;
    m.title  = "stale_value_to_clear";
    m.author = "stale_author";
    manager_->writeMetadata(document_.get(), m);

    DocumentMetadata cleared;
    cleared.title  = "";
    cleared.author = "";
    manager_->writeMetadata(document_.get(), cleared);

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto readBack = manager_->readMetadata(reopened.get());
    EXPECT_EQ(readBack.title,  std::string(""))
        << "Title must be empty after writing empty-string, not stale value";
    EXPECT_EQ(readBack.author, std::string(""))
        << "Author must be empty after writing empty-string";
}

// ─── Unicode (RTL + CJK + emoji) round-trip ─────────────────────────

TEST_F(MetadataRoundTripTest, UnicodeMetadataSurvivesRoundTripWithoutMojibake) {
    DocumentMetadata m;
    m.title    = "مستند تجريبي 测试 テスト 📄";
    m.author   = "أحمد 王 山田";
    m.subject  = "Unicode round-trip 中文 العربية 🌍";
    m.keywords = "unicode, RTL, CJK, emoji";
    manager_->writeMetadata(document_.get(), m);

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto readBack = manager_->readMetadata(reopened.get());
    EXPECT_EQ(readBack.title,    m.title)
        << "Title with RTL+CJK+emoji must round-trip exact-byte";
    EXPECT_EQ(readBack.author,   m.author)
        << "Author with mixed scripts must round-trip exact-byte";
    EXPECT_EQ(readBack.subject,  m.subject);
    EXPECT_EQ(readBack.keywords, m.keywords);
}

// ─── Single-field write (writeField) round-trip ─────────────────────

TEST_F(MetadataRoundTripTest, SingleFieldWriteRoundTrips) {
    // MetadataManager's writeField/readField use the "info:KEY"
    // namespace prefix to disambiguate from XMP keys (verified in
    // src/metadata/MetadataManager.cpp line 100-103). Bare "Title"
    // wouldn't route to the Info dict.
    manager_->writeField(document_.get(), "info:Title",
                         "single_field_test");
    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(manager_->readField(reopened.get(), "info:Title"),
              std::string("single_field_test"));
}

// ─── clearMetadata produces empty Info dict on reopen ───────────────

TEST_F(MetadataRoundTripTest, ClearMetadataLeavesAllFieldsEmpty) {
    DocumentMetadata m;
    m.title    = "T_to_clear";
    m.author   = "A_to_clear";
    m.subject  = "S_to_clear";
    m.keywords = "K_to_clear";
    manager_->writeMetadata(document_.get(), m);

    manager_->clearMetadata(document_.get());

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto readBack = manager_->readMetadata(reopened.get());
    EXPECT_TRUE(readBack.title.empty())
        << "clearMetadata must leave Title empty after reopen";
    EXPECT_TRUE(readBack.author.empty())
        << "clearMetadata must leave Author empty after reopen";
    EXPECT_TRUE(readBack.subject.empty())
        << "clearMetadata must leave Subject empty after reopen";
    EXPECT_TRUE(readBack.keywords.empty())
        << "clearMetadata must leave Keywords empty after reopen";
}

// ─── Real-world unicode_metadata.pdf fixture ────────────────────────

TEST_F(MetadataRoundTripTest, UnicodeRealworldFixtureReadsCleanly) {
    auto rwPath = std::filesystem::path(TEST_FIXTURES_DIR) / "realworld"
                  / "unicode_metadata" / "unicode_metadata.pdf";
    ASSERT_TRUE(std::filesystem::exists(rwPath));

    auto rwDoc = pdfe::core::Document::open(ctx_, rwPath);
    ASSERT_NE(rwDoc, nullptr);
    auto md = manager_->readMetadata(rwDoc.get());

    // Phase-16 (pipeline a7c2e541) closeout: previously this test
    // accepted any non-ASCII byte. We now verify each known script
    // range is present in the title — Arabic (U+0600-06FF), CJK
    // Unified (U+4E00-9FFF), Hiragana/Katakana (U+30A0-30FF),
    // emoji (U+1F300+). UTF-16LE/BE mojibake would still produce
    // high bytes but would not match these specific Unicode ranges.
    //
    // Decode UTF-8 -> codepoints for range checks.
    auto utf8ToCodepoints = [](const std::string& s) {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = s[i];
            uint32_t cp = 0;
            if (c < 0x80) { cp = c; ++i; }
            else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
                cp = (c & 0x1F) << 6;
                cp |= (s[i + 1] & 0x3F);
                i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
                cp = (c & 0x0F) << 12;
                cp |= (s[i + 1] & 0x3F) << 6;
                cp |= (s[i + 2] & 0x3F);
                i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
                cp = (c & 0x07) << 18;
                cp |= (s[i + 1] & 0x3F) << 12;
                cp |= (s[i + 2] & 0x3F) << 6;
                cp |= (s[i + 3] & 0x3F);
                i += 4;
            } else {
                ++i;
            }
            if (cp) out.push_back(cp);
        }
        return out;
    };

    auto cps = utf8ToCodepoints(md.title);
    bool hasArabic = false, hasCjk = false, hasKana = false, hasEmoji = false;
    for (uint32_t cp : cps) {
        if (cp >= 0x0600 && cp <= 0x06FF) hasArabic = true;
        if (cp >= 0x4E00 && cp <= 0x9FFF) hasCjk = true;
        if (cp >= 0x30A0 && cp <= 0x30FF) hasKana = true;
        if (cp >= 0x1F300) hasEmoji = true;
    }
    EXPECT_TRUE(hasArabic)
        << "unicode_metadata.pdf title missing Arabic (U+0600-06FF) "
           "codepoints; got '" << md.title << "'";
    EXPECT_TRUE(hasCjk)
        << "unicode_metadata.pdf title missing CJK Unified "
           "(U+4E00-9FFF) codepoints; got '" << md.title << "'";
    EXPECT_TRUE(hasKana)
        << "unicode_metadata.pdf title missing Kana (U+30A0-30FF) "
           "codepoints; got '" << md.title << "'";
    EXPECT_TRUE(hasEmoji)
        << "unicode_metadata.pdf title missing emoji (U+1F300+) "
           "codepoints; got '" << md.title << "'";
}

// ─── Writing on top of unicode_metadata fixture round-trips ─────────

TEST_F(MetadataRoundTripTest, WriteOverwritesUnicodeFixtureMetadata) {
    auto rwPath = std::filesystem::path(TEST_FIXTURES_DIR) / "realworld"
                  / "unicode_metadata" / "unicode_metadata.pdf";
    ASSERT_TRUE(std::filesystem::exists(rwPath));

    auto rwDoc = pdfe::core::Document::open(ctx_, rwPath);
    ASSERT_NE(rwDoc, nullptr);

    DocumentMetadata m;
    m.title  = "Replaced 替换 🔁";
    m.author = "Replaced author";
    manager_->writeMetadata(rwDoc.get(), m);

    auto reopened = pdfe::test::SaveAndReopen(*rwDoc, ctx_, tempDir_,
                                               "rw_md");
    ASSERT_NE(reopened, nullptr);
    auto readBack = manager_->readMetadata(reopened.get());
    EXPECT_EQ(readBack.title,  m.title)
        << "Overwrite must replace unicode_metadata.pdf's original "
           "title with our new mixed-script string";
    EXPECT_EQ(readBack.author, m.author);
}

} // namespace
