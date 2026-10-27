// PDF version handling — meta-audit follow-up F14.
//
// Gaps closed here: PDF-01 (no version handling existed at all), PDF-07 (the
// one PDF 2.0 fixture in the tree had no C++ consumer), PDF-08 (the
// header/catalog mismatch fixture was only asserted to open).
//
// WHAT WAS WRONG. Before F14, `grep -rn 'pdfVersion|/Version|header version'
// src/core/Document.cpp` returned nothing: neither the `%PDF-x.y` header nor
// `/Root/Version` was read or written anywhere in PDFE. Two consequences were
// invisible to the suite:
//
//   1. Nothing reported the document's version, so nothing could notice a
//      change to it.
//   2. MuPDF overwrites `pdf_document::version` with the catalog `/Version`
//      UNCONDITIONALLY, but ISO 32000-1 §7.5.5 makes that override apply only
//      when the catalog value is LATER than the header. A file authored with
//      header 1.7 and `/Version /1.4` is a 1.7 document that MuPDF both reports
//      AND RE-EMITS as 1.4 — a silent downgrade of a saved file.
//
// `SaveDoesNotDowngradeWhenCatalogVersionIsOlder` is the regression test for
// (2); it fails against unmodified MuPDF behaviour.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Error.h"
#include "core/MuPdfContext.h"

#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::PdfVersion;

namespace {

fs::path fixture(const std::string& name) {
    return fs::path(TEST_FIXTURES_DIR) / name;
}

/// The `%PDF-x.y` version in a file's leading bytes, read independently of
/// PDFE so the oracle does not use the code under test.
std::string rawHeaderVersion(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    char buf[64] = {0};
    in.read(buf, sizeof(buf) - 1);
    const std::string s(buf, static_cast<size_t>(in.gcount()));
    const auto at = s.find("%PDF-");
    if (at == std::string::npos) return {};
    std::string v;
    for (size_t i = at + 5; i < s.size() && v.size() < 8; ++i) {
        const char c = s[i];
        if ((c >= '0' && c <= '9') || c == '.') v += c;
        else break;
    }
    return v;
}

/// The catalog `/Version` name in a file's bytes, read with a byte scan rather
/// than through PDFE's object model. Empty when absent.
std::string rawCatalogVersion(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    const std::string s((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    // Only meaningful for the uncompressed fixtures this test writes/reads.
    auto at = s.find("/Version");
    while (at != std::string::npos) {
        size_t i = at + 8;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) ++i;
        if (i < s.size() && s[i] == '/') {
            ++i;
            std::string v;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) {
                v += s[i++];
            }
            if (!v.empty()) return v;
        }
        at = s.find("/Version", at + 1);
    }
    return {};
}

class PdfVersionHandlingTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        ASSERT_TRUE(tmp_.isValid());
    }
    void TearDown() override { ctx_.reset(); }

    fs::path out(const std::string& name) const {
        return fs::path(tmp_.path().toStdString()) / name;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    QTemporaryDir tmp_;
};

} // namespace

// ===========================================================================
// PdfVersion value type
// ===========================================================================

TEST(PdfVersionValue, ParsesAndRoundTrips) {
    const auto v17 = PdfVersion::parse("1.7");
    ASSERT_TRUE(v17.has_value());
    EXPECT_EQ(v17->major, 1);
    EXPECT_EQ(v17->minor, 7);
    EXPECT_EQ(v17->toString(), "1.7");
    EXPECT_EQ(v17->key(), 17);

    const auto v20 = PdfVersion::parse("%PDF-2.0");
    ASSERT_TRUE(v20.has_value());
    EXPECT_EQ(v20->toString(), "2.0");

    EXPECT_TRUE(*v17 < *v20);
    EXPECT_TRUE(*v20 > *v17);
    EXPECT_TRUE(*v17 == PdfVersion({1, 7}));
    EXPECT_TRUE(*v17 != *v20);

    // Malformed input is rejected rather than silently defaulted.
    EXPECT_FALSE(PdfVersion::parse("").has_value());
    EXPECT_FALSE(PdfVersion::parse("1").has_value());
    EXPECT_FALSE(PdfVersion::parse("1.").has_value());
    EXPECT_FALSE(PdfVersion::parse(".7").has_value());
    EXPECT_FALSE(PdfVersion::parse("x.y").has_value());
    EXPECT_FALSE(PdfVersion::parse("0.0").has_value());
}

// ===========================================================================
// Criterion 1a — the three header versions are read and reported
// ===========================================================================

TEST_F(PdfVersionHandlingTest, OpensAndReportsHeaderVersion_1_4) {
    const auto p = fixture("version_header_1_4.pdf");
    ASSERT_TRUE(fs::exists(p)) << p;
    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->headerVersion().toString(), "1.4");
    EXPECT_EQ(doc->pdfVersion().toString(), "1.4");
    EXPECT_FALSE(doc->catalogVersion().has_value());
}

TEST_F(PdfVersionHandlingTest, OpensAndReportsHeaderVersion_1_7) {
    const auto p = fixture("version_header_1_7.pdf");
    ASSERT_TRUE(fs::exists(p)) << p;
    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->headerVersion().toString(), "1.7");
    EXPECT_EQ(doc->pdfVersion().toString(), "1.7");
}

TEST_F(PdfVersionHandlingTest, OpensAndReportsHeaderVersion_2_0) {
    const auto p = fixture("version_header_2_0.pdf");
    ASSERT_TRUE(fs::exists(p)) << p;
    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->headerVersion().toString(), "2.0");
    EXPECT_EQ(doc->pdfVersion().toString(), "2.0");
}

// PDF-07: the pre-existing PDF 2.0 fixture had no C++ consumer at all.
TEST_F(PdfVersionHandlingTest, Pdf20HeaderFixtureOpensAndReportsVersion) {
    const auto p = fixture("phase3_v20_minimal.pdf");
    ASSERT_TRUE(fs::exists(p)) << p;
    ASSERT_EQ(rawHeaderVersion(p), "2.0") << "fixture is no longer PDF 2.0";
    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->headerVersion().toString(), "2.0");
    EXPECT_EQ(doc->pdfVersion().toString(), "2.0");
    EXPECT_GE(doc->pageCount(), 1);
}

// ===========================================================================
// Criterion 1b — /Root/Version precedence (ISO 32000-1 §7.5.5)
// ===========================================================================

// The catalog value is LATER than the header, so it wins.
TEST_F(PdfVersionHandlingTest, CatalogVersionOverridesHeaderWhenLater) {
    const auto p = fixture("version_catalog_raises_2_0.pdf");
    ASSERT_TRUE(fs::exists(p)) << p;
    ASSERT_EQ(rawHeaderVersion(p), "1.4");
    ASSERT_EQ(rawCatalogVersion(p), "2.0");

    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->headerVersion().toString(), "1.4")
        << "the AUTHORED header must still be reportable";
    ASSERT_TRUE(doc->catalogVersion().has_value());
    EXPECT_EQ(doc->catalogVersion()->toString(), "2.0");
    EXPECT_EQ(doc->pdfVersion().toString(), "2.0")
        << "a LATER catalog /Version overrides the header";
}

// The catalog value is EARLIER than the header, so it is IGNORED. Raw MuPDF
// reports 1.4 here; PDFE must report 1.7.
TEST_F(PdfVersionHandlingTest, CatalogVersionIsIgnoredWhenEarlierThanHeader) {
    const auto p = fixture("version_catalog_older_1_4.pdf");
    ASSERT_TRUE(fs::exists(p)) << p;
    ASSERT_EQ(rawHeaderVersion(p), "1.7");
    ASSERT_EQ(rawCatalogVersion(p), "1.4");

    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->headerVersion().toString(), "1.7");
    ASSERT_TRUE(doc->catalogVersion().has_value());
    EXPECT_EQ(doc->catalogVersion()->toString(), "1.4");
    EXPECT_EQ(doc->pdfVersion().toString(), "1.7")
        << "ISO 32000-1 §7.5.5: the catalog override applies only when the "
           "catalog version is LATER than the header. MuPDF applies it "
           "unconditionally and would report 1.4 here.";
}

// PDF-08: the flaw-corpus header/catalog mismatch fixture was previously
// asserted only to open. Assert the resolution rule instead.
TEST_F(PdfVersionHandlingTest, HeaderVersionMismatchCorpusFileResolvesPerSpec) {
    const auto p = fs::path(TEST_FIXTURES_DIR) / "flaw_corpus" / "corrupt"
                 / "header_version_mismatch.pdf";
    ASSERT_TRUE(fs::exists(p)) << p;

    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);

    const PdfVersion header = doc->headerVersion();
    ASSERT_TRUE(header.isValid()) << "no readable %PDF- header";
    EXPECT_EQ(header.toString(), rawHeaderVersion(p))
        << "reported header version must match the authored bytes";

    const auto cat = doc->catalogVersion();
    const PdfVersion effective = doc->pdfVersion();

    // The resolution rule, asserted rather than assumed.
    if (cat && *cat > header) {
        EXPECT_EQ(effective, *cat) << "later catalog /Version must win";
    } else {
        EXPECT_EQ(effective, header)
            << "absent or earlier catalog /Version must not lower the header";
    }
}

// ===========================================================================
// Criterion 1c — a save must not downgrade the version
// ===========================================================================

TEST_F(PdfVersionHandlingTest, SaveAsDoesNotDowngradeHeaderVersion) {
    for (const char* name : {"version_header_1_4.pdf",
                             "version_header_1_7.pdf",
                             "version_header_2_0.pdf"}) {
        SCOPED_TRACE(name);
        const auto src = fixture(name);
        ASSERT_TRUE(fs::exists(src)) << src;

        PdfVersion before;
        {
            auto doc = Document::open(ctx_, src);
            ASSERT_NE(doc, nullptr);
            before = doc->pdfVersion();
            ASSERT_TRUE(before.isValid());
            doc->saveAs(out(std::string("saved_") + name));
        }

        const auto dst = out(std::string("saved_") + name);
        ASSERT_TRUE(fs::exists(dst));

        // Byte-level oracle: the emitted header itself.
        const auto savedHeader = PdfVersion::parse(rawHeaderVersion(dst));
        ASSERT_TRUE(savedHeader.has_value())
            << "saved file has no readable %PDF- header";
        EXPECT_GE(*savedHeader, before)
            << "saveAs DOWNGRADED the header version from " << before.toString()
            << " to " << savedHeader->toString();

        // And through the product's own resolution.
        auto reopened = Document::open(ctx_, dst);
        ASSERT_NE(reopened, nullptr);
        EXPECT_GE(reopened->pdfVersion(), before)
            << "effective version dropped across saveAs";
    }
}

// The regression that raw MuPDF exhibits: header 1.7 + `/Version /1.4` is
// re-emitted with a `%PDF-1.4` header.
TEST_F(PdfVersionHandlingTest, SaveDoesNotDowngradeWhenCatalogVersionIsOlder) {
    const auto src = fixture("version_catalog_older_1_4.pdf");
    ASSERT_TRUE(fs::exists(src)) << src;
    ASSERT_EQ(rawHeaderVersion(src), "1.7");

    const auto dst = out("saved_catalog_older.pdf");
    {
        auto doc = Document::open(ctx_, src);
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(doc->pdfVersion().toString(), "1.7");
        doc->saveAs(dst);
    }
    ASSERT_TRUE(fs::exists(dst));

    const auto savedHeader = PdfVersion::parse(rawHeaderVersion(dst));
    ASSERT_TRUE(savedHeader.has_value());
    EXPECT_GE(*savedHeader, PdfVersion({1, 7}))
        << "saveAs emitted a %PDF-" << savedHeader->toString()
        << " header for a 1.7 document (MuPDF copies the catalog /Version into "
           "its writer header field unconditionally). This is a silent "
           "downgrade of the user's file.";

    auto reopened = Document::open(ctx_, dst);
    ASSERT_NE(reopened, nullptr);
    EXPECT_GE(reopened->pdfVersion(), PdfVersion({1, 7}));
}

TEST_F(PdfVersionHandlingTest, InPlaceSaveDoesNotDowngradeHeaderVersion) {
    // Copy the 2.0 fixture into the temp dir and save IN PLACE (the routing
    // path that reopens the document), not saveAs.
    const auto work = out("inplace_2_0.pdf");
    std::error_code ec;
    fs::copy_file(fixture("version_header_2_0.pdf"), work,
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    {
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(doc->pdfVersion().toString(), "2.0");
        doc->markRequiresFullRewrite();   // force the full-rewrite route
        doc->save(work);
    }

    const auto savedHeader = PdfVersion::parse(rawHeaderVersion(work));
    ASSERT_TRUE(savedHeader.has_value());
    EXPECT_GE(*savedHeader, PdfVersion({2, 0}))
        << "in-place save downgraded the header to " << savedHeader->toString();
}

TEST_F(PdfVersionHandlingTest, RecoveryCopyDoesNotDowngradeHeaderVersion) {
    const auto src = fixture("version_header_2_0.pdf");
    const auto dst = out("recovery_2_0.pdf");
    {
        auto doc = Document::open(ctx_, src);
        ASSERT_NE(doc, nullptr);
        doc->writeRecoveryCopy(dst);
    }
    ASSERT_TRUE(fs::exists(dst));
    const auto savedHeader = PdfVersion::parse(rawHeaderVersion(dst));
    ASSERT_TRUE(savedHeader.has_value());
    EXPECT_GE(*savedHeader, PdfVersion({2, 0}))
        << "autosave/recovery snapshot downgraded the header";
}

// ===========================================================================
// setMinimumPdfVersion — raises, never lowers
// ===========================================================================

TEST_F(PdfVersionHandlingTest, SetMinimumPdfVersionRaisesTheVersion) {
    const auto src = fixture("version_header_1_4.pdf");
    const auto dst = out("raised_to_2_0.pdf");
    {
        auto doc = Document::open(ctx_, src);
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(doc->pdfVersion().toString(), "1.4");
        doc->setMinimumPdfVersion(PdfVersion({2, 0}));
        EXPECT_EQ(doc->pdfVersion().toString(), "2.0");
        doc->saveAs(dst);
    }
    ASSERT_TRUE(fs::exists(dst));

    auto reopened = Document::open(ctx_, dst);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pdfVersion().toString(), "2.0")
        << "the raised version did not survive the save";

    const auto savedHeader = PdfVersion::parse(rawHeaderVersion(dst));
    ASSERT_TRUE(savedHeader.has_value());
    EXPECT_GE(*savedHeader, PdfVersion({2, 0}));
}

TEST_F(PdfVersionHandlingTest, SetMinimumPdfVersionNeverLowersTheVersion) {
    auto doc = Document::open(ctx_, fixture("version_header_2_0.pdf"));
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pdfVersion().toString(), "2.0");

    doc->setMinimumPdfVersion(PdfVersion({1, 4}));
    EXPECT_EQ(doc->pdfVersion().toString(), "2.0")
        << "setMinimumPdfVersion must be a floor, not an assignment";

    doc->setMinimumPdfVersion(PdfVersion({2, 0}));
    EXPECT_EQ(doc->pdfVersion().toString(), "2.0");

    EXPECT_THROW(doc->setMinimumPdfVersion(PdfVersion({0, 0})),
                 pdfe::core::PdfeException);
}
