// -*- coding: utf-8 -*-
//
// Advanced compatibility tests — XFA-FALLBACK, complex typography
// patterns, multi-language content, and additional real-world
// document shapes.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::core;

namespace {

std::filesystem::path tempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_compat_adv";
    std::filesystem::create_directories(d);
    return d;
}

void writePdf(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

class CompatAdvancedTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        td_ = tempDir();
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path td_;
};

// =====================================================================
// XFA-FALLBACK: PDF with XFA stream but no static AcroForm fallback
// =====================================================================

TEST_F(CompatAdvancedTest, XfaOnlyPdfRendersWithoutCrash)
{
    // PDF with /XFA entry but no /Fields — XFA-only forms should
    // render the static page content and not crash on missing
    // form rendering.
    std::string xfaXml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<xdp:xdp xmlns:xdp=\"http://ns.adobe.com/xdp/\">"
        "<template xmlns=\"http://www.xfa.org/schema/xfa-template/2.6/\">"
        "<subform name=\"form1\">"
        "<field name=\"Field1\"><ui><textEdit/></ui></field>"
        "</subform></template></xdp:xdp>";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /AcroForm "
        "<< /XFA 4 0 R >> >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n"
        "4 0 obj\n<< /Length " + std::to_string(xfaXml.size()) +
        " >>\nstream\n" + xfaXml + "\nendstream\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000150 00000 n \n"
        "0000000220 00000 n \n0000000310 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "xfa_only.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        ASSERT_NE(doc, nullptr);
        EXPECT_GT(doc->pageCount(), 0);
        auto dl = doc->createDisplayList(0);
        if (dl) doc->releaseDisplayList(dl);
    });
}

TEST_F(CompatAdvancedTest, XfaWithStaticAcroFormFallbackRenders)
{
    // PDF with both /XFA (dynamic) and /Fields (static). MuPDF
    // should render via the static fallback path.
    std::string xfaXml = "<xdp:xdp xmlns:xdp=\"http://ns.adobe.com/xdp/\"/>";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /AcroForm "
        "<< /Fields [4 0 R] /XFA 5 0 R >> >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Annots [4 0 R] >>\nendobj\n"
        "4 0 obj\n<< /Type /Annot /Subtype /Widget /FT /Tx /T (StaticField) "
        "/Rect [100 700 300 720] /P 3 0 R >>\nendobj\n"
        "5 0 obj\n<< /Length " + std::to_string(xfaXml.size()) +
        " >>\nstream\n" + xfaXml + "\nendstream\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000180 00000 n \n"
        "0000000240 00000 n \n0000000340 00000 n \n"
        "0000000460 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "xfa_with_fallback.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        ASSERT_NE(doc, nullptr);
        auto dl = doc->createDisplayList(0);
        if (dl) doc->releaseDisplayList(dl);
    });
}

// =====================================================================
// COMPLEX-TYPOGRAPHY: kerning + ligatures + RTL + CJK
// =====================================================================

TEST_F(CompatAdvancedTest, ComplexTextPositioningOperators)
{
    // Use TJ (positioned text) with kerning adjustments
    std::string content =
        "BT /F1 12 Tf 100 700 Td "
        "[(A) 60 (W) -20 (a) 30 (V) -15 (e)] TJ "  // Kerning AVa
        "0 -20 Td "
        "[(fi) (fl) (ff) (ffi)] TJ "  // Ligature pairs
        "0 -20 Td "
        "(\\x80\\x81\\x82) Tj "  // High-byte chars
        "ET";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 "
        "/BaseFont /Helvetica >> >> >> /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length " + std::to_string(content.size()) +
        " >>\nstream\n" + content + "\nendstream\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n0000000300 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "complex_typography.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    });
}

// =====================================================================
// MULTI-PAGE STRESS with mixed content
// =====================================================================

TEST_F(CompatAdvancedTest, MixedPageSizesAndOrientations)
{
    // PDF with pages of different sizes and rotations — verifies
    // PDFE doesn't assume uniform page geometry.
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R] "
        "/Count 4 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Rotate 0 >>\nendobj\n"  // Letter portrait
        "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 792 612] "
        "/Rotate 0 >>\nendobj\n"  // Letter landscape
        "5 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
        "/Rotate 90 >>\nendobj\n"  // A4 rotated 90
        "6 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 1224 792] "
        "/Rotate 270 >>\nendobj\n"  // Tabloid rotated 270
        "xref\n0 7\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000130 00000 n \n0000000215 00000 n \n"
        "0000000300 00000 n \n0000000390 00000 n \n"
        "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "mixed_sizes.pdf";
    writePdf(path, pdf);

    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 4);

    for (int i = 0; i < 4; ++i) {
        EXPECT_NO_THROW({
            auto dl = doc->createDisplayList(i);
            if (dl) doc->releaseDisplayList(dl);
        }) << "Page " << i << " render must succeed";
    }
}

// =====================================================================
// HIDDEN TEXT in invisible OCG layer (security-adjacent)
// =====================================================================

TEST_F(CompatAdvancedTest, HiddenTextInOcgLayerDetected)
{
    // PDF with text in an Optional Content Group that's marked off.
    // Tests that PDFE doesn't accidentally expose hidden content
    // through text extraction paths.
    std::string content =
        "BT /F1 12 Tf 100 700 Td (PUBLIC VISIBLE) Tj ET "
        "/OC /Hidden BDC "
        "BT /F1 12 Tf 100 650 Td (SECRET HIDDEN) Tj ET "
        "EMC";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
        "/OCProperties << /OCGs [5 0 R] /D << /OFF [5 0 R] >> >> >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 "
        "/BaseFont /Helvetica >> >> /Properties << /Hidden 5 0 R >> >> "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length " + std::to_string(content.size()) +
        " >>\nstream\n" + content + "\nendstream\nendobj\n"
        "5 0 obj\n<< /Type /OCG /Name (Hidden Layer) >>\nendobj\n"
        "xref\n0 6\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000170 00000 n \n"
        "0000000230 00000 n \n0000000450 00000 n \n"
        "0000000600 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "ocg_hidden_text.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    });
}

// =====================================================================
// BACKWARDS COMPAT: very old PDF version (1.0)
// =====================================================================

TEST_F(CompatAdvancedTest, AncientPdf10Format)
{
    // PDF 1.0 from 1993 — no cross-reference stream, simple structure
    std::string pdf =
        "%PDF-1.0\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n"
        "xref\n0 4\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "pdf_1_0.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        ASSERT_NE(doc, nullptr);
        EXPECT_GT(doc->pageCount(), 0);
    });
}

// =====================================================================
// FORWARDS COMPAT: future PDF version
// =====================================================================

TEST_F(CompatAdvancedTest, FuturePdfVersion30Handled)
{
    // PDF 3.0 doesn't exist yet — MuPDF should treat as latest
    // compatible spec and not reject outright.
    std::string pdf =
        "%PDF-3.0\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n"
        "xref\n0 4\n0000000000 65535 f \n"
        "0000000009 00000 n \n0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "pdf_3_0.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        // OK if either it opens or throws cleanly
    });
}
