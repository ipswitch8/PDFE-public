#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/ProhibitedFeatureScanner.h"
#include "compliance/ComplianceTypes.h"
#include "compliance/XmpMetadataWriter.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::compliance;

namespace {

std::filesystem::path scannerTempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_prohibited_scan";
    std::filesystem::create_directories(d);
    return d;
}

void writePdf(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Whether scanAll() emitted an issue with the given rule id.
bool issuesContainRule(const std::vector<ComplianceIssue>& issues,
                       const std::string& rule)
{
    for (const auto& i : issues)
        if (i.rule == rule) return true;
    return false;
}

} // namespace

class ProhibitedFeatureScannerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_single.pdf not found";
        }
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override {
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
};

TEST_F(ProhibitedFeatureScannerTest, ConstructDestroy) {
    ProhibitedFeatureScanner scanner(context_);
    SUCCEED();
}

TEST_F(ProhibitedFeatureScannerTest, NoEncryption_FreshDoc) {
    ProhibitedFeatureScanner scanner(context_);
    EXPECT_FALSE(scanner.hasEncryption(document_.get()));
}

TEST_F(ProhibitedFeatureScannerTest, NoJavaScript_FreshDoc) {
    ProhibitedFeatureScanner scanner(context_);
    EXPECT_FALSE(scanner.hasJavaScript(document_.get()));
}

TEST_F(ProhibitedFeatureScannerTest, NoExternalReferences_FreshDoc) {
    ProhibitedFeatureScanner scanner(context_);
    EXPECT_FALSE(scanner.hasExternalReferences(document_.get()));
}

TEST_F(ProhibitedFeatureScannerTest, ScanAll_PdfA1b_CleanDoc) {
    ProhibitedFeatureScanner scanner(context_);
    auto issues = scanner.scanAll(document_.get(), PdfALevel::PdfA_1b);
    // A clean PDF should have no prohibited feature issues
    // (transparency/objstreams may or may not be present)
    for (const auto& issue : issues) {
        EXPECT_NE(issue.rule, "6.1.3") << "Fresh doc should not be encrypted";
        EXPECT_NE(issue.rule, "6.6.1") << "Fresh doc should not have JavaScript";
    }
}

TEST_F(ProhibitedFeatureScannerTest, ScanAll_PdfA2b_AllowsTransparency) {
    ProhibitedFeatureScanner scanner(context_);
    auto issues = scanner.scanAll(document_.get(), PdfALevel::PdfA_2b);
    for (const auto& issue : issues) {
        EXPECT_NE(issue.rule, "6.4") << "PDF/A-2b allows transparency";
    }
}

TEST_F(ProhibitedFeatureScannerTest, ScanAll_PdfA2b_AllowsObjStreams) {
    ProhibitedFeatureScanner scanner(context_);
    auto issues = scanner.scanAll(document_.get(), PdfALevel::PdfA_2b);
    for (const auto& issue : issues) {
        EXPECT_NE(issue.rule, "6.1.7") << "PDF/A-2b allows object streams";
    }
}

TEST_F(ProhibitedFeatureScannerTest, NullDoc_NoThrow) {
    ProhibitedFeatureScanner scanner(context_);
    EXPECT_FALSE(scanner.hasEncryption(nullptr));
    EXPECT_FALSE(scanner.hasJavaScript(nullptr));
    EXPECT_FALSE(scanner.hasTransparency(nullptr));
    EXPECT_FALSE(scanner.hasObjectStreams(nullptr));
    EXPECT_FALSE(scanner.hasExternalReferences(nullptr));
}

TEST_F(ProhibitedFeatureScannerTest, ScanAll_IssuesHaveRuleAndDescription) {
    ProhibitedFeatureScanner scanner(context_);
    auto issues = scanner.scanAll(document_.get(), PdfALevel::PdfA_1b);
    for (const auto& issue : issues) {
        EXPECT_FALSE(issue.rule.empty()) << "Each issue should have a rule reference";
        EXPECT_FALSE(issue.description.empty()) << "Each issue should have a description";
        EXPECT_EQ(issue.severity, IssueSeverity::Error);
    }
}

// =====================================================================
// R-22 POSITIVE DETECTION (phase-4 SECURITY-ORACLE).
//
// The pre-existing suite only proved a CLEAN doc passes — a scanner that
// unconditionally returned "no prohibited feature" (the null oracle) would
// pass every test above. These tests feed documents that DO contain each
// prohibited feature and assert the scanner FLAGS it. Remove the detection
// logic in ProhibitedFeatureScanner.cpp and these fail (proof of teeth).
// =====================================================================

// Encryption: reuse the committed AES-256 fixture (opened with its password
// so we have a live Document). hasEncryption() reads the trailer /Encrypt.
TEST_F(ProhibitedFeatureScannerTest, HasEncryption_DetectsEncryptedDoc) {
    auto enc = std::filesystem::path(TEST_FIXTURES_DIR) / "test_encrypted_aes256.pdf";
    if (!std::filesystem::exists(enc))
        GTEST_SKIP() << "test_encrypted_aes256.pdf fixture missing";

    auto encDoc = pdfe::core::Document::open(context_, enc, "test123");
    ASSERT_NE(encDoc, nullptr) << "Encrypted fixture must open with test123";

    ProhibitedFeatureScanner scanner(context_);
    EXPECT_TRUE(scanner.hasEncryption(encDoc.get()))
        << "R-22: scanner must FLAG an encrypted document";

    auto issues = scanner.scanAll(encDoc.get(), PdfALevel::PdfA_1b);
    EXPECT_TRUE(issuesContainRule(issues, "6.1.3"))
        << "R-22: scanAll must emit rule 6.1.3 (encryption) for an encrypted doc";
}

// JavaScript via catalog /Names /JavaScript. startxref 0 forces MuPDF repair,
// which rebuilds the xref and links the catalog → the JS name tree.
TEST_F(ProhibitedFeatureScannerTest, HasJavaScript_DetectsNamesJavaScript) {
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Names 4 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n"
        "4 0 obj\n<< /JavaScript << /Names [(hostile) 5 0 R] >> >>\nendobj\n"
        "5 0 obj\n<< /S /JavaScript /JS (app.alert('pwn')) >>\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000078 00000 n \n"
        "0000000135 00000 n \n0000000220 00000 n \n"
        "0000000290 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    auto path = scannerTempDir() / "js_names.pdf";
    writePdf(path, pdf);

    auto jsDoc = pdfe::core::Document::open(context_, path);
    ASSERT_NE(jsDoc, nullptr);

    ProhibitedFeatureScanner scanner(context_);
    EXPECT_TRUE(scanner.hasJavaScript(jsDoc.get()))
        << "R-22: scanner must FLAG a doc carrying /Names /JavaScript";

    auto issues = scanner.scanAll(jsDoc.get(), PdfALevel::PdfA_1b);
    EXPECT_TRUE(issuesContainRule(issues, "6.6.1"))
        << "R-22: scanAll must emit rule 6.6.1 (JavaScript)";
}

// JavaScript via catalog /OpenAction.
TEST_F(ProhibitedFeatureScannerTest, HasJavaScript_DetectsOpenActionJs) {
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
        "/OpenAction << /S /JavaScript /JS (this.print()) >> >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n"
        "xref\n0 4\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000110 00000 n \n"
        "0000000167 00000 n \n"
        "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    auto path = scannerTempDir() / "js_openaction.pdf";
    writePdf(path, pdf);

    auto jsDoc = pdfe::core::Document::open(context_, path);
    ASSERT_NE(jsDoc, nullptr);

    ProhibitedFeatureScanner scanner(context_);
    EXPECT_TRUE(scanner.hasJavaScript(jsDoc.get()))
        << "R-22: scanner must FLAG a doc with an /OpenAction JavaScript action";
}

// External reference via a page-level annotation with a /GoToR action.
TEST_F(ProhibitedFeatureScannerTest, HasExternalReferences_DetectsGoToR) {
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Annots [4 0 R] >>\nendobj\n"
        "4 0 obj\n<< /Type /Annot /Subtype /Link /Rect [100 100 200 120] "
        "/A << /S /GoToR /F (\\\\\\\\attacker.example\\\\share\\\\x) /D [0 /Fit] >> "
        ">>\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000210 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    auto path = scannerTempDir() / "extref_gotor.pdf";
    writePdf(path, pdf);

    auto refDoc = pdfe::core::Document::open(context_, path);
    ASSERT_NE(refDoc, nullptr);

    ProhibitedFeatureScanner scanner(context_);
    EXPECT_TRUE(scanner.hasExternalReferences(refDoc.get()))
        << "R-22: scanner must FLAG a doc with a /GoToR external-reference action";

    auto issues = scanner.scanAll(refDoc.get(), PdfALevel::PdfA_1b);
    EXPECT_TRUE(issuesContainRule(issues, "6.6.3"))
        << "R-22: scanAll must emit rule 6.6.3 (external references)";
}

// =====================================================================
// R-22 pdfaid part/conformance must be read at the correct XML NODE, not
// via a naive substring scan of the whole XMP packet. The packet below
// carries a DECOY "pdfaid:part 3 conformance A" inside dc:description that
// appears BEFORE the real pdfaid attributes (part=1/conformance=B). A
// substring reader would report PDF/A-3a; the node reader must report
// PDF/A-1b.
// =====================================================================
TEST_F(ProhibitedFeatureScannerTest, PdfaidReadFromNodeNotSubstring) {
    const std::string xmp =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "   <dc:description>Migrated from pdfaid:part 3 conformance A legacy "
        "note</dc:description>\n"
        "  </rdf:Description>\n"
        "  <rdf:Description rdf:about=\"\" "
        "xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\" "
        "pdfaid:part=\"1\" pdfaid:conformance=\"B\"/>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Metadata 4 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n"
        "4 0 obj\n<< /Type /Metadata /Subtype /XML /Length " +
        std::to_string(xmp.size()) + " >>\nstream\n" + xmp +
        "\nendstream\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000078 00000 n \n"
        "0000000135 00000 n \n0000000210 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    auto path = scannerTempDir() / "pdfaid_decoy.pdf";
    writePdf(path, pdf);

    auto metaDoc = pdfe::core::Document::open(context_, path);
    ASSERT_NE(metaDoc, nullptr);

    XmpMetadataWriter xmp_writer(context_);
    // Sanity: the packet must have actually been embedded/round-tripped.
    ASSERT_FALSE(xmp_writer.readXmpPacket(metaDoc.get()).empty())
        << "Crafted /Metadata stream must be readable back";

    PdfALevel level = xmp_writer.detectPdfALevel(metaDoc.get());
    EXPECT_EQ(level, PdfALevel::PdfA_1b)
        << "R-22: pdfaid must be read from the pdfaid:part/conformance NODE "
           "(=1/B → PDF/A-1b), NOT the decoy substring '3 conformance A'. "
           "Got level index " << static_cast<int>(level);
}
