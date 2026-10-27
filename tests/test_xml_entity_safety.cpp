// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F13 (SEC-40 / SEC-41 / SEC-42):
// XXE, entity-expansion and external-DTD REFUSAL for every attacker-reachable
// XML entry point in PDFE.
//
// Phase-4 of the meta audit found ZERO XXE / external-DTD / entity-expansion
// tests anywhere in the tree, against three real XML entry points:
//
//   * pugixml, /Metadata XMP packet   -- src/compliance/XmpMetadataWriter.cpp:396
//   * pugixml, hOCR markup            -- src/tre/HocrParser.cpp:497
//   * QXmlStreamReader, imported XFDF -- src/forms/FormDataIO.cpp:109
//
// LINE NUMBERS ABOVE ARE THE PHASE-4 CITATIONS AND ARE STALE (corrected in
// follow-up phase F36, which needed them and found they no longer resolve).
// As of F36 the three sites are XmpMetadataWriter.cpp:447 (a load_buffer since
// F36 -- it was a load_string, which truncated every UTF-16 packet at its
// second byte), HocrParser.cpp:676, and FormDataIO.cpp:128 (the
// QXmlStreamReader construction). The guard calls that precede them are
// XmpMetadataWriter.cpp:410, HocrParser.cpp:669 and FormDataIO.cpp:118.
// UTF-16 XMP coverage lives in
// tests/test_xmp_encoding_utf16.cpp; this file stays UTF-8-only so the two
// suites do not silently converge.
//
// SIX REQUIRED CASES: {XXE, entity expansion, external DTD} x {pugixml XMP
// path, QXmlStreamReader XFDF path}. Each asserts an EXPLICIT REFUSAL — a
// false acceptance verdict or a thrown exception. None of them is an
// EXPECT_NO_THROW: "it didn't crash" is exactly the proxy oracle this whole
// programme exists to delete, and a successful XXE does not crash.
//
// HONESTY ABOUT WHO PROVIDES THE CONTROL. pugixml has no DTD entity machinery
// at all and never expands external entities; QXmlStreamReader refuses to
// resolve external entities and caps internal expansion. Those are LIBRARY
// defaults, not PDFE code. Phase F13 added src/core/XmlSafety.h so the
// property is PDFE's own and is asserted rather than assumed — the tests below
// assert PDFE's explicit refusal AND, in the disclosure cases, additionally
// assert the classic XXE payoff (file contents surfacing in a parsed value)
// does not occur. See docs/audit/meta-audit-2026-07/followup/
// phase-13-dispositions.md.
//
// NO PAYLOAD FILES ARE COMMITTED. Every hostile document is built at run time,
// and the only "secret" any payload points at is a marker file this test
// creates in its own QTemporaryDir.
#include <gtest/gtest.h>

#include "compliance/XmpMetadataWriter.h"
#include "core/Document.h"
#include "core/Error.h"
#include "core/MuPdfContext.h"
#include "core/XmlSafety.h"
#include "forms/FormDataIO.h"
#include "forms/FormManager.h"
#include "tre/HocrParser.h"
#include "tre/TreTypes.h"

#include <QTemporaryDir>

#include <pugixml.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using pdfe::core::xmlsafety::Reject;

constexpr const char* kMarker = "PDFE_XXE_CANARY_5f3a91";

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// --- payload builders -------------------------------------------------------
// `secretPath` is a file THIS TEST created; a successful XXE would splice its
// contents (the canary marker) into the parsed value.

std::string xxeXmp(const std::string& secretUri)
{
    return std::string(
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<!DOCTYPE x:xmpmeta [\n"
        "  <!ENTITY leak SYSTEM \"") + secretUri + "\">\n"
        "]>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        "  <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "    <rdf:Description rdf:about=\"\" "
        "xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\">\n"
        "      <pdfaid:part>&leak;</pdfaid:part>\n"
        "      <pdfaid:conformance>B</pdfaid:conformance>\n"
        "    </rdf:Description>\n"
        "  </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>";
}

// Nested internal entities ("billion laughs"), kept to a size that is harmless
// if the refusal ever regressed — the point is the DECLARATION, not the blast.
std::string entityBombXmp()
{
    return
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<!DOCTYPE x:xmpmeta [\n"
        "  <!ENTITY a \"aaaaaaaaaa\">\n"
        "  <!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">\n"
        "  <!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">\n"
        "]>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        "  <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "    <rdf:Description rdf:about=\"\" "
        "xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\">\n"
        "      <pdfaid:part>&c;</pdfaid:part>\n"
        "    </rdf:Description>\n"
        "  </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>";
}

std::string externalDtdXmp()
{
    return
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<!DOCTYPE x:xmpmeta SYSTEM \"http://xxe.invalid/evil.dtd\">\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        "  <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "    <rdf:Description rdf:about=\"\" "
        "xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\">\n"
        "      <pdfaid:part>1</pdfaid:part>\n"
        "      <pdfaid:conformance>B</pdfaid:conformance>\n"
        "    </rdf:Description>\n"
        "  </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>";
}

std::string benignXmp()
{
    return
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        "  <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "    <rdf:Description rdf:about=\"\" "
        "xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\">\n"
        "      <pdfaid:part>1</pdfaid:part>\n"
        "      <pdfaid:conformance>B</pdfaid:conformance>\n"
        "    </rdf:Description>\n"
        "  </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>";
}

std::string xxeXfdf(const std::string& secretUri)
{
    return std::string(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE xfdf [\n"
        "  <!ENTITY leak SYSTEM \"") + secretUri + "\">\n"
        "]>\n"
        "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\">\n"
        "  <fields>\n"
        "    <field name=\"Name\"><value>&leak;</value></field>\n"
        "  </fields>\n"
        "</xfdf>\n";
}

std::string entityBombXfdf()
{
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE xfdf [\n"
        "  <!ENTITY a \"aaaaaaaaaa\">\n"
        "  <!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">\n"
        "  <!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">\n"
        "]>\n"
        "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\">\n"
        "  <fields>\n"
        "    <field name=\"Name\"><value>&c;</value></field>\n"
        "  </fields>\n"
        "</xfdf>\n";
}

std::string externalDtdXfdf()
{
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE xfdf SYSTEM \"http://xxe.invalid/evil.dtd\">\n"
        "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\">\n"
        "  <fields>\n"
        "    <field name=\"Name\"><value>Alice</value></field>\n"
        "  </fields>\n"
        "</xfdf>\n";
}

std::string benignXfdf()
{
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\">\n"
        "  <fields>\n"
        "    <field name=\"Name\"><value>Alice</value></field>\n"
        "  </fields>\n"
        "</xfdf>\n";
}

// Minimal 1-page PDF whose catalog carries `xmp` as an uncompressed
// /Metadata stream. No xref offsets are emitted (startxref 0); MuPDF's repair
// path rescans the objects, the same idiom the fuzz builders use.
std::string buildPdfWithXmp(const std::string& xmp)
{
    std::string out;
    out += "%PDF-1.7\n";
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Metadata 5 0 R >>\nendobj\n";
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
           "/Contents 4 0 R /Resources << >> >>\nendobj\n";
    const std::string content = "BT ET\n";
    out += "4 0 obj\n<< /Length " + std::to_string(content.size()) +
           " >>\nstream\n" + content + "endstream\nendobj\n";
    out += "5 0 obj\n<< /Type /Metadata /Subtype /XML /Length " +
           std::to_string(xmp.size()) + " >>\nstream\n" + xmp +
           "\nendstream\nendobj\n";
    out += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

} // namespace

// ===========================================================================
// Fixture
// ===========================================================================
class XmlEntitySafetyTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        secretFile_ = dir() / "xxe_canary.txt";
        std::ofstream f(secretFile_, std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f << kMarker << "\n";
        f.close();
        ASSERT_TRUE(std::filesystem::exists(secretFile_));

        // file:///C:/... form MuPDF/pugixml/Qt would use if they resolved it.
        std::string p = secretFile_.generic_string();
        secretUri_ = "file:///" + p;
    }

    std::filesystem::path dir() const
    {
        return std::filesystem::path(tempDir_->path().toStdString());
    }

    std::filesystem::path writePdf(const std::string& name, const std::string& xmp)
    {
        const std::filesystem::path p = dir() / name;
        std::ofstream f(p, std::ios::binary);
        const std::string bytes = buildPdfWithXmp(xmp);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.close();
        return p;
    }

    std::unique_ptr<QTemporaryDir> tempDir_;
    std::filesystem::path secretFile_;
    std::string secretUri_;
};

// ===========================================================================
// CASE 1/6 — pugixml XMP path, XXE (external general entity).
// ===========================================================================
TEST_F(XmlEntitySafetyTest, XmpXxeExternalEntityIsRefused)
{
    const std::string packet = xxeXmp(secretUri_);

    // (a) EXPLICIT REFUSAL: PDFE's own acceptance check says no, and names why.
    std::string reason;
    EXPECT_FALSE(
        pdfe::compliance::XmpMetadataWriter::isXmpPacketAcceptable(packet, &reason))
        << "an XMP packet declaring an external entity must be refused";
    EXPECT_FALSE(reason.empty());
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(packet).reason,
              Reject::ExternalIdentifier);

    // (b) THE XXE PAYOFF DOES NOT OCCUR: end-to-end through a real document,
    //     the canary never reaches a parsed value and the forged PDF/A claim
    //     ("&leak;" as pdfaid:part) is not honoured.
    const std::filesystem::path pdf = writePdf("xmp_xxe.pdf", packet);
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    pdfe::compliance::XmpMetadataWriter writer(ctx);

    const std::string raw = writer.readXmpPacket(doc.get());
    ASSERT_FALSE(raw.empty()) << "the hostile packet must actually be reachable, "
                                 "otherwise this test is vacuous";
    EXPECT_EQ(raw.find(kMarker), std::string::npos)
        << "the raw packet must not already contain the canary";

    EXPECT_EQ(writer.detectPdfALevel(doc.get()), pdfe::compliance::PdfALevel::None)
        << "a DTD-bearing XMP packet must yield no PDF/A determination at all";
}

// ===========================================================================
// CASE 2/6 — pugixml XMP path, internal entity expansion (billion laughs).
// ===========================================================================
TEST_F(XmlEntitySafetyTest, XmpEntityExpansionBombIsRefused)
{
    const std::string packet = entityBombXmp();

    std::string reason;
    EXPECT_FALSE(
        pdfe::compliance::XmpMetadataWriter::isXmpPacketAcceptable(packet, &reason))
        << "an XMP packet declaring nested entities must be refused";
    EXPECT_FALSE(reason.empty());
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(packet).reason,
              Reject::EntityDeclaration);

    const std::filesystem::path pdf = writePdf("xmp_bomb.pdf", packet);
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    pdfe::compliance::XmpMetadataWriter writer(ctx);
    ASSERT_FALSE(writer.readXmpPacket(doc.get()).empty());
    EXPECT_EQ(writer.detectPdfALevel(doc.get()), pdfe::compliance::PdfALevel::None);
}

// ===========================================================================
// CASE 3/6 — pugixml XMP path, external DTD subset.
// ===========================================================================
TEST_F(XmlEntitySafetyTest, XmpExternalDtdIsRefused)
{
    const std::string packet = externalDtdXmp();

    std::string reason;
    EXPECT_FALSE(
        pdfe::compliance::XmpMetadataWriter::isXmpPacketAcceptable(packet, &reason))
        << "an XMP packet pointing at an external DTD must be refused";
    EXPECT_FALSE(reason.empty());
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(packet).reason,
              Reject::ExternalIdentifier);

    // The body of this packet is a perfectly well-formed PDF/A-1b claim. The
    // ONLY difference from the positive control below is the external DTD, so
    // a None verdict here proves the DTD is what caused the refusal.
    const std::filesystem::path pdf = writePdf("xmp_extdtd.pdf", packet);
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    pdfe::compliance::XmpMetadataWriter writer(ctx);
    ASSERT_FALSE(writer.readXmpPacket(doc.get()).empty());
    EXPECT_EQ(writer.detectPdfALevel(doc.get()), pdfe::compliance::PdfALevel::None);
}

// ===========================================================================
// POSITIVE CONTROL for the XMP path — without this, all three refusals above
// could be satisfied by a parser that rejects everything.
// ===========================================================================
TEST_F(XmlEntitySafetyTest, BenignXmpIsAcceptedAndParsed)
{
    const std::string packet = benignXmp();
    EXPECT_TRUE(pdfe::compliance::XmpMetadataWriter::isXmpPacketAcceptable(packet));

    const std::filesystem::path pdf = writePdf("xmp_benign.pdf", packet);
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    pdfe::compliance::XmpMetadataWriter writer(ctx);
    EXPECT_EQ(writer.detectPdfALevel(doc.get()),
              pdfe::compliance::PdfALevel::PdfA_1b)
        << "the identical packet WITHOUT a DTD must still be parsed normally";
}

// ===========================================================================
// CASE 4/6 — QXmlStreamReader XFDF path, XXE (external general entity).
// ===========================================================================
TEST_F(XmlEntitySafetyTest, XfdfXxeExternalEntityIsRefused)
{
    const std::string xfdf = xxeXfdf(secretUri_);
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(xfdf).reason,
              Reject::ExternalIdentifier);

    ASSERT_TRUE(std::filesystem::exists(fixturesDir() / "test_form.pdf"));
    const std::filesystem::path work = dir() / "xfdf_xxe_form.pdf";
    std::filesystem::copy_file(fixturesDir() / "test_form.pdf", work,
                               std::filesystem::copy_options::overwrite_existing);

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    pdfe::forms::FormManager manager(ctx);
    pdfe::forms::FormDataIO io(ctx);

    EXPECT_THROW(io.importFromXfdf(doc.get(), &manager, xfdf),
                 pdfe::core::PdfeException)
        << "XFDF declaring an external entity must be refused with an error";
}

// ===========================================================================
// CASE 5/6 — QXmlStreamReader XFDF path, entity expansion.
// ===========================================================================
TEST_F(XmlEntitySafetyTest, XfdfEntityExpansionBombIsRefused)
{
    const std::string xfdf = entityBombXfdf();
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(xfdf).reason,
              Reject::EntityDeclaration);

    const std::filesystem::path work = dir() / "xfdf_bomb_form.pdf";
    std::filesystem::copy_file(fixturesDir() / "test_form.pdf", work,
                               std::filesystem::copy_options::overwrite_existing);

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    pdfe::forms::FormManager manager(ctx);
    pdfe::forms::FormDataIO io(ctx);

    EXPECT_THROW(io.importFromXfdf(doc.get(), &manager, xfdf),
                 pdfe::core::PdfeException)
        << "XFDF declaring nested entities must be refused with an error";
}

// ===========================================================================
// CASE 6/6 — QXmlStreamReader XFDF path, external DTD.
// ===========================================================================
TEST_F(XmlEntitySafetyTest, XfdfExternalDtdIsRefused)
{
    const std::string xfdf = externalDtdXfdf();
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(xfdf).reason,
              Reject::ExternalIdentifier);

    const std::filesystem::path work = dir() / "xfdf_extdtd_form.pdf";
    std::filesystem::copy_file(fixturesDir() / "test_form.pdf", work,
                               std::filesystem::copy_options::overwrite_existing);

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    pdfe::forms::FormManager manager(ctx);
    pdfe::forms::FormDataIO io(ctx);

    EXPECT_THROW(io.importFromXfdf(doc.get(), &manager, xfdf),
                 pdfe::core::PdfeException)
        << "XFDF pointing at an external DTD must be refused with an error";
}

// ===========================================================================
// POSITIVE CONTROL for the XFDF path.
// ===========================================================================
TEST_F(XmlEntitySafetyTest, BenignXfdfIsAcceptedAndImported)
{
    const std::string xfdf = benignXfdf();
    EXPECT_TRUE(pdfe::core::xmlsafety::inspect(xfdf).safe());

    const std::filesystem::path work = dir() / "xfdf_benign_form.pdf";
    std::filesystem::copy_file(fixturesDir() / "test_form.pdf", work,
                               std::filesystem::copy_options::overwrite_existing);

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    pdfe::forms::FormManager manager(ctx);
    pdfe::forms::FormDataIO io(ctx);

    // The identical document WITHOUT a DTD must import without throwing, so
    // the three refusals above are not a blanket rejection of all XFDF.
    EXPECT_NO_THROW(io.importFromXfdf(doc.get(), &manager, xfdf));
}

// ===========================================================================
// Third entry point (SEC-41): hOCR, same guard.
// ===========================================================================
TEST_F(XmlEntitySafetyTest, HocrWithDtdIsRefusedAndBenignHocrParses)
{
    const char* kBenign =
        "<html><body>"
        "<div class=\"ocr_page\" title=\"bbox 0 0 800 600; scan_res 300 300\">"
        "<div class=\"ocr_carea\" title=\"bbox 10 10 200 40\">"
        "<p class=\"ocr_par\">"
        "<span class=\"ocr_line\" title=\"bbox 10 10 200 40\">"
        "<span class=\"ocrx_word\" title=\"bbox 10 10 90 40; x_wconf 95\">Hello</span>"
        "</span></p></div></div></body></html>";

    const pdfe::tre::HocrDocument good = pdfe::tre::HocrParser::parse(kBenign);
    ASSERT_EQ(good.pageWidth, 800) << "positive control must actually parse";
    ASSERT_FALSE(good.blocks.empty());

    // A REAL Tesseract hOCR document begins with an XHTML DOCTYPE. It must
    // still parse: refusing it would break OCR on valid input, which is how
    // security controls end up switched off.
    const std::string realWorld =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\""
        " \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">" +
        std::string(kBenign);
    EXPECT_TRUE(pdfe::core::xmlsafety::inspect(
        realWorld, pdfe::core::xmlsafety::Policy::AllowDoctype).safe())
        << "the XHTML doctype Tesseract emits must not be refused";
    EXPECT_EQ(pdfe::tre::HocrParser::parse(realWorld).pageWidth, 800);

    // ... but an ENTITY declaration with an external identifier — the XXE
    // primitive itself — is refused even under the lenient hOCR policy.
    const std::string hostile =
        std::string("<!DOCTYPE html [ <!ENTITY leak SYSTEM \"") + secretUri_ +
        "\"> ]>" + kBenign;
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                  hostile, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
              Reject::ExternalIdentifier);

    const pdfe::tre::HocrDocument bad = pdfe::tre::HocrParser::parse(hostile);
    EXPECT_EQ(bad.pageWidth, 0)
        << "entity-declaring hOCR must be refused, not parsed";
    EXPECT_TRUE(bad.blocks.empty());
}

// ===========================================================================
// F27 — the two HYPOTHESIS items from deferred-backlog.md, converted to
// MEASURED results.
//
// HYPOTHESIS 1 (BOM / UTF-16 encoding-mismatch bypass) was MEASURED AND
// CONFIRMED as a real bypass of PDFE's own control, not merely hypothetical:
// before the F27 fix, a UTF-16LE hOCR document carrying
// `<!ENTITY leak SYSTEM "file:///...">` produced verdict `accepted` from
// inspect() and was then decoded and PARSED by pugixml (pageWidth 800, one
// block). It bypassed the guard WITHOUT a BOM as well, via pugixml's
// `guess_buffer_encoding` NUL-pattern heuristic — wider than the backlog
// claimed. What did NOT happen: pugixml expanded nothing (the reference
// surfaced verbatim as the literal text "&leak;") and fetched nothing (the
// canary never appeared). So the payoff was inert — but the inertness was the
// LIBRARY's property, not PDFE's, which is exactly the "latent regression
// waiting on a library swap" the backlog named. Fixed in XmlSafety.cpp by
// sniffing the encoding the way the parser does and transcoding to UTF-8
// before the scan.
//
// The tests below assert the fixed behaviour AND carry the discriminating
// controls that stop them passing by way of a blanket rejection: benign UTF-16
// hOCR must still decode and parse.
//
// MUTATIONS KILLED (each applied to src/core/XmlSafety.cpp, confirmed RED,
// then reverted -- full table in docs/audit/meta-audit-2026-07/followup/
// phase-27-dispositions.md):
//   M1 the F27 encoding handling reverted wholesale
//        -> HocrUtf16EncodedEntityDeclarationIsSeenByTheGuardAndRefused RED,
//           BenignUtf16Hocr... and the subset test GREEN
//   M5 BOM sniffed but pugixml's no-BOM NUL-pattern guess dropped
//        -> HocrUtf16Encoded... RED (its BOM-less rows)
//   M3 every non-UTF-8 document refused (detection made UNIVERSAL)
//        -> BenignUtf16HocrIsStillDecodedParsedAndAccepted RED -- the proof
//           that the detection is SELECTIVE and not a blanket refusal
//   M2 subset-scan comment/PI skips disabled
//        -> HocrInternalSubsetParameterEntityRecallIsRefusedBehindCommentsAndPis
//           RED, every encoding test GREEN
//   M4 subset '%' fires on a bare percent sign
//        -> the same test RED via its CONTROL arm
//
// REMEDIATION MUTATIONS (after the security-audit gate's FAIL; full table in
// the dispositions file):
//   M6 the two-byte fallback deleted -- the gate's exact defect, restored
//        -> HocrUtf16WithNonLatinSecondCharacterIsStillRefused,
//           HocrUtf16EncodedEntityDeclarationIsSeenByTheGuardAndRefused and
//           BOTH XmlSafetyPugixmlDifferential tests RED; every benign control
//           and both subset tests GREEN
//   M7 the fallback hoisted ABOVE the four-byte patterns (ordering inverted)
//        -> Utf32WithoutBomIsDetectedAndOrderedBeforeTheUtf16Fallback RED,
//           the UTF-16 tests GREEN -- the suite tells mis-ordered from missing
//   M8 sniffEncoding() returns Utf16LE for everything
//        -> 36 of 44 RED, every CONTROL among them
//   M9 a speculative latin1 branch added
//        -> Latin1IsByteCompatibleWithTheAsciiScan RED
//
// SECOND-REMEDIATION MUTATIONS (after the gate's SECOND FAIL):
//   M10 the CDATA context-awareness alone reverted (inert everywhere again)
//        -> CdataInsideTheInternalSubsetIsScannedNotSkipped and both
//           CoreXmlSafetyCdataContext tests RED; every comment-inertness and
//           element-content CDATA control GREEN
//   M11 CDATA treated as markup EVERYWHERE (over-broad, the opposite error)
//        -> the element-content CONTROLS RED -- this is the assertion pair
//           that shows the fix is CONTEXT-AWARE and not merely stricter
//   M12 a nested marked section allowed to shrink the tracked subset extent
//        -> only SubsetExtentIsRobustToUnterminationNestingAndLiterals RED
// ===========================================================================
namespace {

// Encode ASCII source as UTF-16 / UTF-32, optionally BOM-prefixed. Built here
// rather than committed as a binary fixture (see the file header).
std::string toUtf16(const std::string& ascii, bool littleEndian, bool bom)
{
    std::string out;
    if (bom) {
        out += littleEndian ? '\xFF' : '\xFE';
        out += littleEndian ? '\xFE' : '\xFF';
    }
    for (char c : ascii) {
        if (littleEndian) { out += c; out += '\0'; }
        else              { out += '\0'; out += c; }
    }
    return out;
}

std::string toUtf32Le(const std::string& ascii, bool bom)
{
    std::string out;
    if (bom) { out += '\xFF'; out += '\xFE'; out += '\0'; out += '\0'; }
    for (char c : ascii) { out += c; out += '\0'; out += '\0'; out += '\0'; }
    return out;
}

// A well-formed hOCR page whose single word is `wordText`.
std::string hocrPage(const std::string& wordText)
{
    return
        "<html><body>"
        "<div class=\"ocr_page\" title=\"bbox 0 0 800 600; scan_res 300 300\">"
        "<div class=\"ocr_carea\" title=\"bbox 10 10 200 40\">"
        "<p class=\"ocr_par\">"
        "<span class=\"ocr_line\" title=\"bbox 10 10 200 40\">"
        "<span class=\"ocrx_word\" title=\"bbox 10 10 90 40; x_wconf 95\">" +
        wordText +
        "</span>"
        "</span></p></div></div></body></html>";
}

int firstLineWordCount(const pdfe::tre::HocrDocument& d)
{
    if (d.blocks.empty() || d.blocks[0].paragraphs.empty() ||
        d.blocks[0].paragraphs[0].lines.empty()) {
        return 0;
    }
    return static_cast<int>(d.blocks[0].paragraphs[0].lines[0].words.size());
}

} // namespace

// ===========================================================================
// F27 REMEDIATION — raised by the `security-audit` gate, which FAILED the
// first cut of F27 with a working, measured bypass.
//
// WHAT THE GATE FOUND (finding 1). `sniff_encoding()` was advertised as
// mirroring pugixml's `guess_buffer_encoding`. It did not. It implemented the
// four-byte patterns (pugixml.cpp:2038-2043) and MISSED the two-byte fallback
// at :2046-2047 —
//
//     if (d0 == 0 && d1 == 0x3c) return encoding_utf16_be;
//     if (d0 == 0x3c && d1 == 0 ) return encoding_utf16_le;
//
// — which constrains bytes 2-3 not at all. The four-byte patterns additionally
// require the document's SECOND character to encode with a zero high byte,
// i.e. codepoint < 0x100. Any document whose second character is Greek,
// Cyrillic, CJK, Hebrew or Arabic therefore fell through to `Utf8` while
// pugixml decoded the entire buffer as UTF-16.
//
// REPRODUCED HERE BEFORE FIXING, against the project's real vcpkg pugixml
// 1.15 with the exact flags HocrParser uses:
//
//   UTF-16BE `<λ/><!DOCTYPE evil [<!ENTITY xxe SYSTEM "http://…">]>` (λ =
//   U+03BB, chosen ONLY because it forces bytes 2-3 non-zero)
//     - pugi status 0, encoding utf16be, root element decoded as `λ`
//     - inspect(…, AllowDoctype)  ->  ACCEPTED
//   UTF-16LE: symmetric, same result.
//   The ASCII-second-character forms were caught, which is exactly why the
//   original tests passed and the hole survived.
//
// This re-opened F27's own central claim — "anything pugixml can decode, this
// must also be able to read" — and it is the reason the tests below exist in
// this shape. `sniffEncoding()` is now a faithful port in pugixml's order, and
// `XmlSafetyPugixmlDifferential` pins it against what pugixml ACTUALLY reports
// rather than against a hand-typed table, because a hand-typed table is what
// produced the defect in the first place.
// ===========================================================================
namespace {

using pdfe::core::xmlsafety::Encoding;
using pdfe::core::xmlsafety::sniffEncoding;

// --- encoders. The corpus is GENERATED, never a typed byte table. -----------

std::string encodeUtf16(std::string_view utf8Ascii, bool littleEndian, bool bom);

// Encode a sequence of code points (so the corpus can carry a second character
// >= U+0100, which is the whole point of the gate's finding).
std::string encodeUtf16Cp(const std::vector<unsigned>& cps, bool littleEndian,
                          bool bom)
{
    std::string out;
    auto unit = [&](unsigned u) {
        if (littleEndian) { out += char(u & 0xFF); out += char((u >> 8) & 0xFF); }
        else              { out += char((u >> 8) & 0xFF); out += char(u & 0xFF); }
    };
    if (bom) unit(0xFEFF);
    for (unsigned cp : cps) {
        if (cp < 0x10000u) {
            unit(cp);
        } else {
            const unsigned v = cp - 0x10000u;
            unit(0xD800u + (v >> 10));
            unit(0xDC00u + (v & 0x3FFu));
        }
    }
    return out;
}

std::string encodeUtf32Cp(const std::vector<unsigned>& cps, bool littleEndian,
                          bool bom)
{
    std::string out;
    auto unit = [&](unsigned u) {
        if (littleEndian) {
            out += char(u & 0xFF);         out += char((u >> 8) & 0xFF);
            out += char((u >> 16) & 0xFF); out += char((u >> 24) & 0xFF);
        } else {
            out += char((u >> 24) & 0xFF); out += char((u >> 16) & 0xFF);
            out += char((u >> 8) & 0xFF);  out += char(u & 0xFF);
        }
    };
    if (bom) unit(0xFEFF);
    for (unsigned cp : cps) unit(cp);
    return out;
}

// ASCII source -> code points (every char is its own code point below 0x80).
std::vector<unsigned> cps(std::string_view ascii)
{
    std::vector<unsigned> v;
    v.reserve(ascii.size());
    for (char c : ascii) v.push_back(static_cast<unsigned char>(c));
    return v;
}

std::vector<unsigned> operator+(std::vector<unsigned> a,
                                const std::vector<unsigned>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

constexpr unsigned kLambda = 0x03BBu;  // U+03BB GREEK SMALL LETTER LAMDA

const char* pugiEncName(pugi::xml_encoding e)
{
    switch (e) {
    case pugi::encoding_auto:     return "auto";
    case pugi::encoding_utf8:     return "utf8";
    case pugi::encoding_utf16_le: return "utf16_le";
    case pugi::encoding_utf16_be: return "utf16_be";
    case pugi::encoding_utf16:    return "utf16";
    case pugi::encoding_utf32_le: return "utf32_le";
    case pugi::encoding_utf32_be: return "utf32_be";
    case pugi::encoding_utf32:    return "utf32";
    case pugi::encoding_wchar:    return "wchar";
    case pugi::encoding_latin1:   return "latin1";
    }
    return "<unknown>";
}

const char* pdfeEncName(Encoding e)
{
    switch (e) {
    case Encoding::Utf8:    return "Utf8";
    case Encoding::Utf16LE: return "Utf16LE";
    case Encoding::Utf16BE: return "Utf16BE";
    case Encoding::Utf32LE: return "Utf32LE";
    case Encoding::Utf32BE: return "Utf32BE";
    }
    return "<unknown>";
}

// pugixml's detected encoding, mapped onto the classification `inspect()` acts
// on. latin1 maps to Utf8 BECAUSE THAT IS MEASURED to be byte-compatible with
// the ASCII scan — see CoreXmlSafetyEncoding.Latin1IsByteCompatibleWithTheAsciiScan
// — not because the two are the same encoding.
Encoding pugiToPdfe(pugi::xml_encoding e)
{
    switch (e) {
    case pugi::encoding_utf16_le: return Encoding::Utf16LE;
    case pugi::encoding_utf16_be: return Encoding::Utf16BE;
    case pugi::encoding_utf32_le: return Encoding::Utf32LE;
    case pugi::encoding_utf32_be: return Encoding::Utf32BE;
    default:                      return Encoding::Utf8;
    }
}

struct EncodedCase {
    std::string name;
    std::string bytes;
    bool carriesDtd = false;
};

// Every detection branch of guess_buffer_encoding, plus the ones that fall
// THROUGH it. Documents are generated by the encoders above.
std::vector<EncodedCase> differentialCorpus()
{
    // A document whose second character is ASCII (`r`) -- the shape the
    // four-byte patterns catch -- and one whose second character is U+03BB,
    // which ONLY the two-byte fallback catches. Both are well-formed XML.
    const std::vector<unsigned> asciiRoot = cps("<r/>");
    const std::vector<unsigned> lambdaRoot =
        std::vector<unsigned>{'<', kLambda, '/', '>'};
    const std::vector<unsigned> decl = cps("<?xml version=\"1.0\"?><r/>");
    const std::vector<unsigned> dtd =
        cps("<!DOCTYPE evil [<!ENTITY xxe SYSTEM \"http://example.invalid/x\">]>");

    std::vector<EncodedCase> out;
    auto add16 = [&](const char* tag, const std::vector<unsigned>& body, bool le,
                     bool bom, bool hasDtd) {
        out.push_back({tag, encodeUtf16Cp(body, le, bom), hasDtd});
    };
    auto add32 = [&](const char* tag, const std::vector<unsigned>& body, bool le,
                     bool bom, bool hasDtd) {
        out.push_back({tag, encodeUtf32Cp(body, le, bom), hasDtd});
    };

    // --- BOM branches (pugixml :2031-2035)
    add16("bom/utf16le", asciiRoot + dtd, true,  true,  true);
    add16("bom/utf16be", asciiRoot + dtd, false, true,  true);
    add32("bom/utf32le", asciiRoot + dtd, true,  true,  true);
    add32("bom/utf32be", asciiRoot + dtd, false, true,  true);
    out.push_back({"bom/utf8",
                   std::string("\xEF\xBB\xBF") +
                       "<r/><!DOCTYPE evil [<!ENTITY xxe SYSTEM \"http://h/\">]>",
                   true});

    // --- four-byte content branches (pugixml :2038-2043)
    add32("4byte/utf32be <", asciiRoot + dtd, false, false, true);
    add32("4byte/utf32le <", asciiRoot + dtd, true,  false, true);
    add16("4byte/utf16be <?", decl + dtd,     false, false, true);
    add16("4byte/utf16le <?", decl + dtd,     true,  false, true);

    // --- TWO-BYTE FALLBACK (pugixml :2046-2047) -- the branch the first cut
    //     of F27 omitted, and the gate's exploit.
    add16("2byte/utf16be lambda", lambdaRoot + dtd, false, false, true);
    add16("2byte/utf16le lambda", lambdaRoot + dtd, true,  false, true);
    add16("2byte/utf16be lambda benign", lambdaRoot, false, false, false);
    add16("2byte/utf16le lambda benign", lambdaRoot, true,  false, false);
    // An astral second character (U+1F600) -- a surrogate pair, so bytes 2-3
    // are D8 3D / 3D D8: still only the two-byte fallback can see it.
    add16("2byte/utf16be astral",
          (std::vector<unsigned>{'<', 0x1F600u, '/', '>'}) + dtd, false, false, true);
    add16("2byte/utf16le astral",
          (std::vector<unsigned>{'<', 0x1F600u, '/', '>'}) + dtd, true, false, true);

    // --- fall-through to UTF-8 / latin1
    out.push_back({"plain/utf8",
                   "<r/><!DOCTYPE evil [<!ENTITY xxe SYSTEM \"http://h/\">]>", true});
    out.push_back({"plain/utf8 non-ascii",
                   "<r>caf\xC3\xA9 \xCE\xBB</r>"
                   "<!DOCTYPE evil [<!ENTITY xxe SYSTEM \"http://h/\">]>", true});
    out.push_back({"decl/latin1",
                   "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>"
                   "<!DOCTYPE evil [<!ENTITY xxe SYSTEM \"http://h/\">]>"
                   "<r>caf\xE9</r>", true});
    out.push_back({"decl/latin1 alias",
                   "<?xml version=\"1.0\" encoding=\"latin1\"?>"
                   "<!DOCTYPE evil [<!ENTITY xxe SYSTEM \"http://h/\">]>"
                   "<r>caf\xE9</r>", true});
    out.push_back({"decl/utf8",
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                   "<!DOCTYPE evil [<!ENTITY xxe SYSTEM \"http://h/\">]>"
                   "<r/>", true});
    out.push_back({"benign/utf8", "<r>100% SYSTEM PUBLIC</r>", false});
    return out;
}

std::string encodeUtf16(std::string_view utf8Ascii, bool littleEndian, bool bom)
{
    return encodeUtf16Cp(cps(utf8Ascii), littleEndian, bom);
}

// hOCR page with a caller-chosen root element name, so the corpus can put a
// non-ASCII character in the second byte position. HocrParser walks the tree
// for class="ocr_page" and does not care what the root is called.
std::vector<unsigned> hocrPageCps(unsigned rootCp, std::string_view word)
{
    std::vector<unsigned> v{'<', rootCp, '>'};
    v = v + cps("<body>"
                "<div class=\"ocr_page\" title=\"bbox 0 0 800 600; scan_res 300 300\">"
                "<div class=\"ocr_carea\" title=\"bbox 10 10 200 40\">"
                "<p class=\"ocr_par\">"
                "<span class=\"ocr_line\" title=\"bbox 10 10 200 40\">"
                "<span class=\"ocrx_word\" title=\"bbox 10 10 90 40; x_wconf 95\">");
    v = v + cps(word);
    v = v + cps("</span></span></p></div></div></body>");
    v.push_back('<'); v.push_back('/'); v.push_back(rootCp); v.push_back('>');
    return v;
}

} // namespace

// ===========================================================================
// F27 REMEDIATION 2 — the `security-audit` gate's SECOND finding.
//
// An `<!ENTITY … SYSTEM …>` wrapped in `<![CDATA[ … ]]>` INSIDE a DOCTYPE
// internal subset was seen by no detection logic at all. Measured through the
// real production path (`HocrParser::parse` -> pugixml `load_buffer`,
// `Policy::AllowDoctype`) before the fix:
//
//   <!DOCTYPE html [ <![CDATA[<!ENTITY xxe SYSTEM "http://…">]]> ]> + hOCR page
//     inspect()  -> accepted        pugixml status 0     pageWidth 800
//   <!DOCTYPE html [ <![CDATA[ %pe; ]]> ]> + hOCR page
//     inspect()  -> accepted        pugixml status 0     pageWidth 800
//
// ROOT CAUSE — an ELEMENT-CONTENT rule applied in a DTD context. The outer
// scan skipped `<![CDATA[…]]>` unconditionally, under a comment that read
// "Comments and CDATA are inert: their contents are text". That is true in
// element content (XML 1.0 sec. 2.7) and FALSE in the internal subset, which
// sec. 2.8 does not admit CDATA sections into at all. The bytes discarded as
// "text" were bytes no conforming parser would ever treat as text.
//
// The asymmetry that proved the diagnosis: `<![INCLUDE[` and `<![IGNORE[`
// wrapping the same declaration WERE caught, because they do not match the
// literal `<![CDATA[` and so fell through to the declaration path. The
// incidental catch worked; the designed-looking one did not.
//
// THE DURABLE LESSON, and the reason both of this phase's gate findings are
// written up together: each was a CLAIM IN A COMMENT that was true in one
// context and silently assumed to hold in another — "mirrors pugixml's
// guess_buffer_encoding" (true of eight branches of ten) and "CDATA contents
// are text" (true in element content, false in a DTD). Neither bug was in the
// logic the author was thinking about; both were in the scope of a sentence.
// ===========================================================================

// The bypass, through the REAL loader, plus the control that distinguishes a
// context-aware fix from a blanket one.
TEST_F(XmlEntitySafetyTest, CdataInsideTheInternalSubsetIsScannedNotSkipped)
{
    const std::string page = hocrPage("Hello");

    // (a) HOSTILE — a CDATA-wrapped declaration inside the subset. Under
    //     XML 1.0 sec. 2.8 this construct cannot legally appear here at all,
    //     so there is no "its contents are text" rule to apply to it.
    const std::string hostile[] = {
        "<!DOCTYPE html [ <![CDATA[<!ENTITY xxe SYSTEM \"http://example.invalid/x\">]]> ]>" + page,
        "<!DOCTYPE html [<![CDATA[<!ENTITY xxe SYSTEM 'http://h/'>]]>]>" + page,
        // Wrapped alongside a legitimate declaration.
        "<!DOCTYPE html [ <!ELEMENT a (#PCDATA)> <![CDATA[<!ENTITY xxe SYSTEM \"http://h/\">]]> ]>" + page,
        // A `]` inside a nested declaration's quoted literal must not end the
        // subset early and hand the CDATA back to the element-content rules.
        "<!DOCTYPE html [ <!ENTITY ok \"]\"> <![CDATA[<!ENTITY xxe SYSTEM \"http://h/\">]]> ]>" + page,
        // Unterminated subset: the CDATA is still inside it.
        "<!DOCTYPE html [ <![CDATA[<!ENTITY xxe SYSTEM \"http://h/\">]]> " + page,
    };
    for (const std::string& h : hostile) {
        EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                      h, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
                  Reject::ExternalIdentifier)
            << "CDATA-wrapped external entity unseen in: " << h.substr(0, 90);
        EXPECT_EQ(pdfe::tre::HocrParser::parse(h).pageWidth, 0)
            << "refused input must not reach the parser: " << h.substr(0, 90);
    }

    // A CDATA-wrapped parameter-entity recall, likewise.
    const std::string recall = "<!DOCTYPE html [ <![CDATA[ %pe; ]]> ]>" + page;
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                  recall, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
              Reject::ParameterEntityRef);
    EXPECT_EQ(pdfe::tre::HocrParser::parse(recall).pageWidth, 0);

    // (b) THE DISCRIMINATING CONTROL — a CDATA section in ORDINARY ELEMENT
    //     CONTENT is still inert. Without this the test cannot tell a
    //     context-aware fix from "scan CDATA everywhere", which would refuse
    //     legitimate documents that quote markup as text.
    const std::string benign[] = {
        // No DOCTYPE at all.
        "<html><body><![CDATA[<!ENTITY e SYSTEM \"http://h/\">]]>"
        "<div class=\"ocr_page\" title=\"bbox 0 0 800 600; scan_res 300 300\">"
        "<div class=\"ocr_carea\" title=\"bbox 10 10 200 40\"><p class=\"ocr_par\">"
        "<span class=\"ocr_line\" title=\"bbox 10 10 200 40\">"
        "<span class=\"ocrx_word\" title=\"bbox 10 10 90 40; x_wconf 95\">Hi</span>"
        "</span></p></div></div></body></html>",
        // A DOCTYPE with a CLOSED, empty subset, then element-content CDATA.
        // This is the sharp one: subset tracking must have ENDED at the `]`.
        "<!DOCTYPE html [ ]><html><body><![CDATA[<!ENTITY e SYSTEM \"http://h/\">]]>"
        "<div class=\"ocr_page\" title=\"bbox 0 0 800 600; scan_res 300 300\">"
        "<div class=\"ocr_carea\" title=\"bbox 10 10 200 40\"><p class=\"ocr_par\">"
        "<span class=\"ocr_line\" title=\"bbox 10 10 200 40\">"
        "<span class=\"ocrx_word\" title=\"bbox 10 10 90 40; x_wconf 95\">Hi</span>"
        "</span></p></div></div></body></html>",
        // A DOCTYPE with a subset containing a comment, then element-content
        // CDATA — the subset must still end at its `]`.
        "<!DOCTYPE html [ <!-- note --> ]><html><body>"
        "<![CDATA[<!DOCTYPE x SYSTEM 'y'> %pe; <!ENTITY q \"z\">]]>"
        "<div class=\"ocr_page\" title=\"bbox 0 0 800 600; scan_res 300 300\">"
        "<div class=\"ocr_carea\" title=\"bbox 10 10 200 40\"><p class=\"ocr_par\">"
        "<span class=\"ocr_line\" title=\"bbox 10 10 200 40\">"
        "<span class=\"ocrx_word\" title=\"bbox 10 10 90 40; x_wconf 95\">Hi</span>"
        "</span></p></div></div></body></html>",
    };
    for (const std::string& b : benign) {
        const auto v = pdfe::core::xmlsafety::inspect(
            b, pdfe::core::xmlsafety::Policy::AllowDoctype);
        EXPECT_TRUE(v.safe())
            << "CDATA in ELEMENT CONTENT is character data (XML 1.0 sec. 2.7) "
               "and must stay inert; refused as "
            << pdfe::core::xmlsafety::describe(v.reason) << " -- "
            << b.substr(0, 90);
        EXPECT_EQ(pdfe::tre::HocrParser::parse(b).pageWidth, 800)
            << "the control must actually parse: " << b.substr(0, 90);
    }

    // (c) The asymmetry that diagnosed the bug must now be gone: CDATA,
    //     INCLUDE and IGNORE inside a subset all take the same path.
    for (const char* kw : {"CDATA", "INCLUDE", "IGNORE", "ANYTHING"}) {
        const std::string doc = std::string("<!DOCTYPE html [ <![") + kw +
            "[<!ENTITY xxe SYSTEM \"http://h/\">]]> ]>" + page;
        EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                      doc, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
                  Reject::ExternalIdentifier)
            << "marked-section keyword `" << kw << "` takes a different path";
    }
}


// ===========================================================================
// F27 REMEDIATION 3+4 — the `security-audit` gate's THIRD and FOURTH findings,
// fixed together by mirroring pugixml's structure instead of patching a
// fourth symptom.
//
// FINDING 3 — `skip_marked_section()` returned at the FIRST `]]>`.
// pugixml's `parse_doctype_ignore` (:3017-3046) DEPTH-COUNTS: a nested `<![`
// raises the depth, and only a `]]>` at depth zero ends the section. For
// `<![INCLUDE[ <![IGNORE[ x ]]> ]]>` the hand-written version stopped at the
// inner `]]>`, handed the leftover ` ]]> ` back to the subset walker, whose
// top-level loop read that leading `]` as the subset's own close. `dtdEnd` was
// then set before the following CDATA, the element-content rule re-engaged and
// the entity declaration inside it was never scanned.
//
// FINDING 4 — the plain-declaration stepper walked to the first unquoted `>`.
// pugixml's `parse_doctype_group` (:3048-3090) DEPTH-COUNTS nested `<!...`
// control groups too: `<!ELEMENT x <!FOO> >` is ONE group, ending at the
// SECOND `>`. Stopping at the first resumes subset scanning inside bytes the
// parser still considers part of the declaration — and a `]` sitting there is
// read as the subset's close, which is finding 3's hole reached by a different
// route.
//
// MEASURED BEFORE FIXING, through the real production path:
//
//   <!DOCTYPE html [ <![INCLUDE[ <![IGNORE[ x ]]> ]]> <![CDATA[<!ENTITY e
//     SYSTEM "http://h/">]]> ]>            accepted / pugi 0 / pageWidth 800
//   ... three levels of nesting            accepted / pugi 0 / pageWidth 800
//   <!DOCTYPE html [ <!1 <!2> ] > <![CDATA[<!ENTITY e SYSTEM "http://h/">]]> ]>
//                                          accepted / pugi 0 / pageWidth 800
//
// A NOTE ON THE GATE'S FINDING-4 REPRO, because it did not reproduce as
// written and the difference matters. Its document used `<!ELEMENT x <!FOO> >`,
// which WAS refused — but for the wrong reason (`DoctypeDeclaration`, from
// `<!ELEMENT` itself, which the lenient policy refuses on its own merits). The
// extent was still computed wrong; the verdict merely happened not to change.
// Substituting a NON-ALPHABETIC group name (`<!1 <!2>`), which sets no flag in
// the outer scan but which pugixml still depth-counts, isolates it to a clean
// bypass. Both forms are pinned below: the isolated one for the hole, the
// gate's own for the reason-escalation, which is the sharper assertion.
// ===========================================================================

namespace {

// pugixml's OWN notion of where a DOCTYPE ends, read back from the parser
// rather than computed here. With `parse_doctype`, pugixml pushes a
// `node_doctype` whose value is the declaration's body: everything after
// `<!DOCTYPE` and its leading whitespace, up to the `>` that closes it
// (pugixml.cpp:3186-3193). So the exclusive end offset is
//
//     start + strlen("<!DOCTYPE") + leadingSpaces + value.length() + 1
//
// Returns npos when pugixml refuses the document (status_bad_doctype), which
// is exactly when PDFE is entitled to answer "the rest of the buffer".
std::size_t pugiDoctypeEnd(const std::string& doc, std::size_t start)
{
    pugi::xml_document d;
    const pugi::xml_parse_result res = d.load_buffer(
        doc.data(), doc.size(), pugi::parse_default | pugi::parse_doctype);
    if (!res) return std::string::npos;

    pugi::xml_node dt;
    for (pugi::xml_node c = d.first_child(); c; c = c.next_sibling()) {
        if (c.type() == pugi::node_doctype) { dt = c; break; }
    }
    if (!dt) return std::string::npos;

    std::size_t mark = start + 9;  // past "<!DOCTYPE"
    while (mark < doc.size() &&
           (doc[mark] == ' ' || doc[mark] == '\t' || doc[mark] == '\r' ||
            doc[mark] == '\n')) {
        ++mark;
    }
    return mark + std::char_traits<char>::length(dt.value()) + 1;
}

} // namespace

// FINDING 3+4, end to end through the REAL loader.
TEST_F(XmlEntitySafetyTest, NestedDoctypeSectionsAndGroupsDoNotEndTheDtdEarly)
{
    const std::string page = hocrPage("Hello");
    const std::string ext =
        "<![CDATA[<!ENTITY xxe SYSTEM \"http://example.invalid/x\">]]>";

    // Each of these puts a CDATA-wrapped external entity AFTER a nested
    // construct that a non-depth-counting scanner terminates early.
    const std::string hostile[] = {
        // finding 3: nested marked sections, two and three deep.
        "<!DOCTYPE html [ <![INCLUDE[ <![IGNORE[ x ]]> ]]> " + ext + " ]>" + page,
        "<!DOCTYPE html [ <![A[ <![B[ <![C[ x ]]> ]]> ]]> " + ext + " ]>" + page,
        // finding 4: nested control group with a NON-ALPHABETIC name, so the
        // outer scan sets no flag of its own and the bypass is isolated.
        "<!DOCTYPE html [ <!1 <!2> ] > " + ext + " ]>" + page,
        "<!DOCTYPE html [ <!1 <!2 <!3> > ] > " + ext + " ]>" + page,
        // finding 4 combined with finding 3.
        "<!DOCTYPE html [ <!1 <!2> ] > <![INCLUDE[ <![IGNORE[ y ]]> ]]> " +
            ext + " ]>" + page,
    };
    for (const std::string& h : hostile) {
        EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                      h, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
                  Reject::ExternalIdentifier)
            << "DTD ended early, so the CDATA after it was treated as element "
               "content: " << h.substr(0, 100);
        EXPECT_EQ(pdfe::tre::HocrParser::parse(h).pageWidth, 0)
            << "refused input must not reach the parser: " << h.substr(0, 100);
    }

    // The `%pe;` variants of the same shapes.
    const std::string recalls[] = {
        "<!DOCTYPE html [ <![INCLUDE[ <![IGNORE[ x ]]> ]]> <![CDATA[ %pe; ]]> ]>" + page,
        "<!DOCTYPE html [ <!1 <!2> ] > <![CDATA[ %pe; ]]> ]>" + page,
    };
    for (const std::string& r : recalls) {
        EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                      r, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
                  Reject::ParameterEntityRef)
            << r.substr(0, 100);
        EXPECT_EQ(pdfe::tre::HocrParser::parse(r).pageWidth, 0);
    }

    // The GATE'S OWN finding-4 document. It was already refused before the
    // fix — but as `DoctypeDeclaration`, for its `<!ELEMENT`. The reason must
    // now ESCALATE, which is what proves the CDATA is being scanned rather
    // than that the document merely happens to be refused.
    const std::string gateRepro =
        "<!DOCTYPE html [ <!ELEMENT x <!FOO> ] > " + ext + " ]>" + page;
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                  gateRepro, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
              Reject::ExternalIdentifier)
        << "still refused only for the <!ELEMENT — the entity inside the CDATA "
           "is not being seen";

    // UNTERMINATED nesting must fail toward a LONGER DTD, never fall back to
    // the first `]]>`. pugixml rejects these outright; PDFE refuses them.
    const std::string unterminated[] = {
        "<!DOCTYPE html [ <![INCLUDE[ <![IGNORE[ x ]]> " + ext + " ]>" + page,
        "<!DOCTYPE html [ <![INCLUDE[ " + ext + " ]>" + page,
        "<!DOCTYPE html [ <!1 <!2 " + ext + " ]>" + page,
    };
    for (const std::string& u : unterminated) {
        EXPECT_FALSE(pdfe::core::xmlsafety::inspect(
                         u, pdfe::core::xmlsafety::Policy::AllowDoctype).safe())
            << "unterminated nesting must extend the DTD to EOF, not truncate "
               "it: " << u.substr(0, 100);
        EXPECT_EQ(pdfe::tre::HocrParser::parse(u).pageWidth, 0);
    }

    // CONTROL — element-content CDATA is still inert, and still parses.
    const std::string benign =
        "<!DOCTYPE html [ <![IGNORE[ <![IGNORE[ x ]]> ]]> ]><html><body>"
        "<![CDATA[<!ENTITY e SYSTEM \"http://h/\">]]>"
        "<div class=\"ocr_page\" title=\"bbox 0 0 800 600; scan_res 300 300\">"
        "<div class=\"ocr_carea\" title=\"bbox 10 10 200 40\"><p class=\"ocr_par\">"
        "<span class=\"ocr_line\" title=\"bbox 10 10 200 40\">"
        "<span class=\"ocrx_word\" title=\"bbox 10 10 90 40; x_wconf 95\">Hi</span>"
        "</span></p></div></div></body></html>";
    const auto v = pdfe::core::xmlsafety::inspect(
        benign, pdfe::core::xmlsafety::Policy::AllowDoctype);
    EXPECT_TRUE(v.safe())
        << "element-content CDATA after a CLOSED nested DTD must stay inert; "
           "refused as " << pdfe::core::xmlsafety::describe(v.reason);
    EXPECT_EQ(pdfe::tre::HocrParser::parse(benign).pageWidth, 800);
}

// THE STRUCTURAL GUARD. `doctypeExtent()` is a hand-maintained mirror of
// `parse_doctype_group`; four gate findings came from such mirrors drifting.
// This pins it against the extent pugixml ITSELF reports, over a corpus of the
// nesting shapes that produced findings 3 and 4.
TEST(XmlSafetyPugixmlDoctypeDifferential, ExtentAgreesWithPugixmlOnNestedDoctypes)
{
    // Bodies are the DOCTYPE only; a trailing `<r/>` gives pugixml a document
    // element so the parse succeeds and the doctype node is reachable.
    const char* doctypes[] = {
        "<!DOCTYPE html>",
        "<!DOCTYPE html []>",
        "<!DOCTYPE html [ ]>",
        "<!DOCTYPE html SYSTEM \"x.dtd\">",
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\""
        " \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">",
        "<!DOCTYPE html [ <!ENTITY e \"x\"> ]>",
        "<!DOCTYPE html [ <!ELEMENT a (#PCDATA)> ]>",
        // marked sections, nested to three levels
        "<!DOCTYPE html [ <![IGNORE[ x ]]> ]>",
        "<!DOCTYPE html [ <![INCLUDE[ <![IGNORE[ x ]]> ]]> ]>",
        "<!DOCTYPE html [ <![A[ <![B[ <![C[ x ]]> ]]> ]]> ]>",
        // nested control groups
        "<!DOCTYPE html [ <!1 <!2> ] > ]>",
        "<!DOCTYPE html [ <!1 <!2 <!3> > ] > ]>",
        "<!DOCTYPE html [ <!ELEMENT x <!FOO> ] > ]>",
        // primitives that may contain the terminators of other constructs
        "<!DOCTYPE html [ <!-- > ] --> ]>",
        "<!DOCTYPE html [ <?pi > ] ?> ]>",
        "<!DOCTYPE html [ <!ENTITY a \"]]>\"> ]>",
        "<!DOCTYPE html [ <!ENTITY a '>'> ]>",
        "<!DOCTYPE html [ <!ENTITY a \"[\"> ]>",
        // a marked section holding a declaration with a '>'
        "<!DOCTYPE html [ <![INCLUDE[ <!ELEMENT z (#PCDATA)> ]]> ]>",
        // combinations
        "<!DOCTYPE html [ <!1 <!2> ] > <![INCLUDE[ <![IGNORE[ y ]]> ]]> ]>",
        "<!DOCTYPE html [ <!-- c --> <![IGNORE[ <!-- d --> ]]> %pe; ]>",
    };

    int agreed = 0;
    for (const char* dt : doctypes) {
        const std::string doc = std::string(dt) + "<r/>";
        const std::size_t mine = pdfe::core::xmlsafety::doctypeExtent(doc, 0);
        const std::size_t theirs = pugiDoctypeEnd(doc, 0);

        ASSERT_NE(theirs, std::string::npos)
            << "pugixml refused this document, so it cannot pin an extent — "
               "rewrite the case: " << dt;
        EXPECT_EQ(mine, theirs)
            << "doctypeExtent disagrees with pugixml.\n  document: " << dt
            << "\n  PDFE says the DOCTYPE ends at " << mine
            << ", pugixml at " << theirs
            << ".\n  A SHORTER extent is the bypass class that failed this "
               "phase four times.";
        if (mine == theirs) ++agreed;

        // The extent must also be a sane offset into the buffer.
        EXPECT_LE(mine, doc.size()) << dt;
        EXPECT_GT(mine, 0u) << dt;
    }
    EXPECT_GE(agreed, 20)
        << "the corpus must actually exercise the nesting shapes";

    // Malformed / unterminated: pugixml throws status_bad_doctype, and PDFE
    // must answer with the LONGEST extent rather than a partial one.
    const char* malformed[] = {
        "<!DOCTYPE html [ <![INCLUDE[ <![IGNORE[ x ]]> ",
        "<!DOCTYPE html [ <!1 <!2 ",
        "<!DOCTYPE html [ <!-- unterminated ",
        "<!DOCTYPE html [ <!ENTITY a \"unterminated ",
        "<!DOCTYPE html",
        "<!DOCTYPE html [",
    };
    for (const char* m : malformed) {
        const std::string doc(m);
        EXPECT_EQ(pdfe::core::xmlsafety::doctypeExtent(doc, 0), doc.size())
            << "an unterminated DOCTYPE must extend to EOF, never truncate: "
            << m;
        pugi::xml_document d;
        EXPECT_FALSE(d.load_buffer(doc.data(), doc.size(),
                                   pugi::parse_default | pugi::parse_doctype))
            << "pugixml is expected to refuse this; if it ever accepted one, "
               "PDFE's fail-long answer would need re-reading: " << m;
    }
}

// FINDING 3 — the structural fix. The sniff is a hand-written mirror of a
// library function; without this test, nothing in the suite could tell that it
// was wrong, and nothing would tell us when pugixml changes. The expectation is
// DERIVED from `xml_parse_result::encoding` — what the parser actually did —
// never from a table typed out here.
TEST(XmlSafetyPugixmlDifferential, SniffAgreesWithPugixmlOnEveryDetectionBranch)
{
    const auto corpus = differentialCorpus();
    ASSERT_GE(corpus.size(), 20u) << "corpus must cover every detection branch";

    int sawUtf16 = 0, sawUtf32 = 0, sawUtf8 = 0;
    for (const EncodedCase& c : corpus) {
        ASSERT_GE(c.bytes.size(), 4u)
            << c.name << ": short buffers diverge deliberately and are covered "
                         "by their own test";

        pugi::xml_document doc;
        const pugi::xml_parse_result res = doc.load_buffer(
            c.bytes.data(), c.bytes.size(),
            pugi::parse_default | pugi::parse_ws_pcdata_single);

        const Encoding expected = pugiToPdfe(res.encoding);
        const Encoding actual = sniffEncoding(c.bytes);
        EXPECT_EQ(actual, expected)
            << c.name << ": pugixml detected " << pugiEncName(res.encoding)
            << " but PDFE's sniff said " << pdfeEncName(actual)
            << ". The guard scans bytes the parser reads differently, which is "
               "precisely the bypass the F27 security-audit gate demonstrated.";

        switch (expected) {
        case Encoding::Utf16LE: case Encoding::Utf16BE: ++sawUtf16; break;
        case Encoding::Utf32LE: case Encoding::Utf32BE: ++sawUtf32; break;
        case Encoding::Utf8:                            ++sawUtf8;  break;
        }
    }

    // The corpus must actually exercise all three families, or agreement is
    // cheap: a sniff that returned Utf8 unconditionally would "agree" on a
    // corpus of UTF-8 documents.
    EXPECT_GE(sawUtf16, 8) << "corpus does not exercise the UTF-16 branches";
    EXPECT_GE(sawUtf32, 4) << "corpus does not exercise the UTF-32 branches";
    EXPECT_GE(sawUtf8, 5)  << "corpus does not exercise the fall-through";
}

// The invariant that actually matters, asserted WITHOUT depending on the
// encoding mapping above being right: if pugixml parsed the document, and the
// document declares an entity with an external identifier, PDFE must have
// refused it. This is the property the gate's exploit violated.
TEST(XmlSafetyPugixmlDifferential, AnythingPugixmlParsesWithADtdIsRefused)
{
    int parsedAndHostile = 0;
    for (const EncodedCase& c : differentialCorpus()) {
        pugi::xml_document doc;
        const pugi::xml_parse_result res = doc.load_buffer(
            c.bytes.data(), c.bytes.size(),
            pugi::parse_default | pugi::parse_ws_pcdata_single);

        const auto verdict = pdfe::core::xmlsafety::inspect(
            c.bytes, pdfe::core::xmlsafety::Policy::AllowDoctype);

        if (c.carriesDtd) {
            EXPECT_EQ(verdict.reason, Reject::ExternalIdentifier)
                << c.name << ": carries <!ENTITY … SYSTEM …> but PDFE said "
                << pdfe::core::xmlsafety::describe(verdict.reason)
                << " (pugixml: status " << static_cast<int>(res.status)
                << ", encoding " << pugiEncName(res.encoding) << ")";
            if (res) ++parsedAndHostile;
        } else {
            EXPECT_TRUE(verdict.safe())
                << c.name << ": DTD-free document refused as "
                << pdfe::core::xmlsafety::describe(verdict.reason)
                << " — the guard must not be a blanket rejection";
        }
    }
    // Without this the test could pass vacuously on a corpus pugixml rejects.
    EXPECT_GE(parsedAndHostile, 15)
        << "the hostile corpus must actually PARSE under pugixml, or the "
           "refusals above prove nothing about a reachable attack";
}

// FINDING 1, end to end through the REAL loader. The four-byte patterns cannot
// see this document; only the two-byte fallback can.
TEST_F(XmlEntitySafetyTest, HocrUtf16WithNonLatinSecondCharacterIsStillRefused)
{
    const std::vector<unsigned> dtd =
        cps(std::string("<!DOCTYPE html [ <!ENTITY leak SYSTEM \"") + secretUri_ +
            "\"> ]>");

    // The gate's exact construction: a `λ`-named element FIRST (U+03BB is a
    // legal XML NameStartChar), so the second character's bytes land at offsets
    // 2-3 — exactly where the four-byte patterns demand a zero high byte and
    // this has none. Putting the DOCTYPE first instead would place `!` there,
    // which the OLD four-byte pattern caught; that mistake was made writing
    // this test and is why the byte-level assertions below exist.
    const std::vector<unsigned> lambdaLead{'<', kLambda, '/', '>'};

    for (bool le : {true, false}) {
        const char* tag = le ? "UTF-16LE" : "UTF-16BE";

        const std::string hostile = encodeUtf16Cp(
            lambdaLead + dtd + hocrPageCps('h', "&leak;"), le, false);

        // The premise, asserted: neither the ASCII literals NOR the four-byte
        // detection pattern is present. Without the second check this test
        // would still pass against the pre-remediation sniff.
        ASSERT_EQ(hostile.find("ENTITY"), std::string::npos) << tag;
        ASSERT_EQ(hostile.find("SYSTEM"), std::string::npos) << tag;
        ASSERT_GE(hostile.size(), 4u);
        const auto b = reinterpret_cast<const unsigned char*>(hostile.data());
        if (le) {
            ASSERT_EQ(b[0], 0x3C); ASSERT_EQ(b[1], 0x00);
            ASSERT_NE(b[2], 0x00) << "byte 2 must be non-zero or the OLD "
                                     "four-byte pattern would catch this";
            ASSERT_NE(b[3], 0x00) << "byte 3 must be non-zero, likewise";
        } else {
            ASSERT_EQ(b[0], 0x00); ASSERT_EQ(b[1], 0x3C);
            ASSERT_NE(b[2], 0x00);
            ASSERT_NE(b[3], 0x00);
        }

        EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                      hostile, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
                  Reject::ExternalIdentifier)
            << tag << ": the two-byte fallback branch of guess_buffer_encoding "
                      "must be mirrored, or this document is scanned as UTF-8 "
                      "while pugixml decodes it as UTF-16";
        EXPECT_EQ(pdfe::tre::HocrParser::parse(hostile).pageWidth, 0) << tag;

        // CONTROL — the identical document WITHOUT the DTD must still decode
        // and parse. pugixml really does read these bytes; that is what made
        // the bypass reachable, and it must keep working.
        const std::string benign =
            encodeUtf16Cp(lambdaLead + hocrPageCps('h', "Hello"), le, false);
        const auto v = pdfe::core::xmlsafety::inspect(
            benign, pdfe::core::xmlsafety::Policy::AllowDoctype);
        EXPECT_TRUE(v.safe())
            << tag << ": benign non-Latin UTF-16 hOCR refused as "
            << pdfe::core::xmlsafety::describe(v.reason);
        EXPECT_EQ(pdfe::tre::HocrParser::parse(benign).pageWidth, 800)
            << tag << ": the control must actually parse, or the refusal above "
                      "proves nothing";
    }
}

// The UTF-32 BOM-less branches, which the gate flagged as unprobed rather than
// clean. `3C 00 00 00` is UTF-32LE and ALSO matches the two-byte UTF-16LE
// fallback, so this is the ordering hazard the remediation had to respect.
TEST_F(XmlEntitySafetyTest, Utf32WithoutBomIsDetectedAndOrderedBeforeTheUtf16Fallback)
{
    const std::vector<unsigned> dtd =
        cps("<!DOCTYPE html [ <!ENTITY leak SYSTEM \"http://example.invalid/x\"> ]>");

    for (bool le : {true, false}) {
        const char* tag = le ? "UTF-32LE" : "UTF-32BE";
        const std::string hostile =
            encodeUtf32Cp(dtd + hocrPageCps('h', "&leak;"), le, false);

        EXPECT_EQ(sniffEncoding(hostile),
                  le ? Encoding::Utf32LE : Encoding::Utf32BE)
            << tag << ": a UTF-32 document must not be classified as UTF-16 by "
                      "the two-byte fallback";
        EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                      hostile, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
                  Reject::ExternalIdentifier) << tag;
        EXPECT_EQ(pdfe::tre::HocrParser::parse(hostile).pageWidth, 0) << tag;

        // CONTROL — benign UTF-32 still decodes and parses.
        const std::string benign =
            encodeUtf32Cp(hocrPageCps('h', "Hello"), le, false);
        EXPECT_TRUE(pdfe::core::xmlsafety::inspect(
            benign, pdfe::core::xmlsafety::Policy::AllowDoctype).safe()) << tag;
        EXPECT_EQ(pdfe::tre::HocrParser::parse(benign).pageWidth, 800) << tag;
    }
}


// HYPOTHESIS 1 — the bypass itself, through the REAL loader (HocrParser ->
// pugixml load_buffer). Five encodings, each of which pugixml auto-detects.
TEST_F(XmlEntitySafetyTest, HocrUtf16EncodedEntityDeclarationIsSeenByTheGuardAndRefused)
{
    const std::string hostile =
        std::string("<!DOCTYPE html [ <!ENTITY leak SYSTEM \"") + secretUri_ +
        "\"> ]>" + hocrPage("&leak;");

    struct Case { const char* name; std::string bytes; };
    const Case cases[] = {
        {"UTF-16LE + BOM",  toUtf16(hostile, true,  true)},
        {"UTF-16BE + BOM",  toUtf16(hostile, false, true)},
        {"UTF-16LE no BOM", toUtf16(hostile, true,  false)},
        {"UTF-16BE no BOM", toUtf16(hostile, false, false)},
        {"UTF-32LE + BOM",  toUtf32Le(hostile, true)},
    };

    for (const Case& c : cases) {
        // The bypass PREMISE, asserted rather than restated: none of the ASCII
        // literals the raw-byte scan looks for is present in these bytes. If a
        // future encoder change made them present, this test would be passing
        // for the wrong reason, and this assertion says so.
        ASSERT_EQ(c.bytes.find("ENTITY"), std::string::npos) << c.name;
        ASSERT_EQ(c.bytes.find("SYSTEM"), std::string::npos) << c.name;
        ASSERT_EQ(c.bytes.find("<!DOCTYPE"), std::string::npos) << c.name;

        EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                      c.bytes, pdfe::core::xmlsafety::Policy::AllowDoctype)
                      .reason,
                  Reject::ExternalIdentifier)
            << c.name << ": the guard must normalise the encoding before "
                         "scanning, not depend on the payload being UTF-8";

        const pdfe::tre::HocrDocument d = pdfe::tre::HocrParser::parse(c.bytes);
        EXPECT_EQ(d.pageWidth, 0) << c.name << ": refused input must not parse";
        EXPECT_TRUE(d.blocks.empty()) << c.name;
    }

    // CONTROL A — the same document as well-formed UTF-8 is still refused with
    // the same reason, so the fix did not move detection into an
    // encoding-only special case.
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                  hostile, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(pdfe::tre::HocrParser::parse(hostile).pageWidth, 0);

    // CONTROL B — a UTF-8 BOM is not a UTF-16 BOM: an ordinary BOM-prefixed
    // UTF-8 hOCR page must still parse.
    const std::string utf8Bom = std::string("\xEF\xBB\xBF") + hocrPage("Hello");
    EXPECT_TRUE(pdfe::core::xmlsafety::inspect(
        utf8Bom, pdfe::core::xmlsafety::Policy::AllowDoctype).safe());
    EXPECT_EQ(pdfe::tre::HocrParser::parse(utf8Bom).pageWidth, 800);
}

// CONTROL for hypothesis 1, and the measurement that makes the test above
// non-vacuous: pugixml really does decode UTF-16 from this same buffer, so the
// pre-fix bypass was reachable — and the fix must NOT have degenerated into
// "refuse everything that is not UTF-8".
TEST_F(XmlEntitySafetyTest, BenignUtf16HocrIsStillDecodedParsedAndAccepted)
{
    const std::string benign = hocrPage("Hello");

    struct Case { const char* name; std::string bytes; };
    const Case cases[] = {
        {"UTF-16LE + BOM",  toUtf16(benign, true,  true)},
        {"UTF-16BE + BOM",  toUtf16(benign, false, true)},
        {"UTF-16LE no BOM", toUtf16(benign, true,  false)},
        {"UTF-16BE no BOM", toUtf16(benign, false, false)},
        {"UTF-32LE + BOM",  toUtf32Le(benign, true)},
    };

    for (const Case& c : cases) {
        const auto v = pdfe::core::xmlsafety::inspect(
            c.bytes, pdfe::core::xmlsafety::Policy::AllowDoctype);
        EXPECT_TRUE(v.safe())
            << c.name << ": DTD-free UTF-16/32 hOCR must not be refused — "
            << pdfe::core::xmlsafety::describe(v.reason);

        const pdfe::tre::HocrDocument d = pdfe::tre::HocrParser::parse(c.bytes);
        EXPECT_EQ(d.pageWidth, 800)
            << c.name << ": pugixml decodes this buffer, which is precisely why "
                         "the guard has to as well";
        EXPECT_EQ(firstLineWordCount(d), 1) << c.name;
    }
}

// HYPOTHESIS 2 — "a bare `%pe;` in the internal subset is not flagged".
// MEASURED: the premise is STALE for the plain form — XmlSafety.cpp already
// carries an internal-subset scan that flags it. But that scan stepped over a
// nested `<...>` by running to the first unquoted '>', which is NOT a
// comment's or a PI's terminator; a '>' inside one ended the step early, and a
// ']' later in the same comment then ended the whole subset scan. Measured
// pre-fix: `<!DOCTYPE html [ <!-- > ] --> %pe; ]>` -> verdict `accepted`.
// That residual is what this test pins.
TEST_F(XmlEntitySafetyTest, HocrInternalSubsetParameterEntityRecallIsRefusedBehindCommentsAndPis)
{
    const std::string page = hocrPage("Hello");

    // Each of these carries a bare `%pe;` DIRECTLY in the internal subset —
    // valid DTD syntax, and the recall half of the standard blind-XXE pair.
    const std::string hostile[] = {
        "<!DOCTYPE html [ %pe; ]>" + page,                    // plain form
        "<!DOCTYPE html [ <!-- > ] --> %pe; ]>" + page,        // the F27 residual
        "<!DOCTYPE html [ <?pi > ] ?> %pe; ]>" + page,         // same, via a PI
        "<!DOCTYPE html [ <!-- ]> --> %pe; ]>" + page,
        "<!DOCTYPE html [ <![INCLUDE[ %pe; ]]> ]>" + page,
        "<!DOCTYPE html [ <!-- a --><!-- > ] --> %pe; ]>" + page,
    };
    for (const std::string& h : hostile) {
        EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                      h, pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
                  Reject::ParameterEntityRef)
            << "unflagged parameter-entity recall in: " << h.substr(0, 60);
        EXPECT_EQ(pdfe::tre::HocrParser::parse(h).pageWidth, 0)
            << "refused input must not reach the parser: " << h.substr(0, 60);
    }

    // CONTROL A — a '%' must still need a name AND a ';', and a comment or PI
    // in the subset is not by itself a reason to refuse anything. Without this,
    // the assertions above could be satisfied by refusing every subset.
    const std::string benignSubsets[] = {
        "<!DOCTYPE html [ <!-- just a comment --> ]>" + page,
        "<!DOCTYPE html [ <!-- > ] --> ]>" + page,
        "<!DOCTYPE html [ <?pi > ] ?> ]>" + page,
        "<!DOCTYPE html [ <!-- > ] --> % ]>" + page,
        "<!DOCTYPE html [ <!-- > ] --> %noterminator ]>" + page,
    };
    for (const std::string& b : benignSubsets) {
        const auto v = pdfe::core::xmlsafety::inspect(
            b, pdfe::core::xmlsafety::Policy::AllowDoctype);
        EXPECT_TRUE(v.safe())
            << "false positive on " << b.substr(0, 60) << " -- "
            << pdfe::core::xmlsafety::describe(v.reason);
        EXPECT_EQ(pdfe::tre::HocrParser::parse(b).pageWidth, 800)
            << "a comment/PI in the subset must not break OCR on valid input";
    }

    // CONTROL B — a `%pe;` properly INSIDE a declaration body is still caught
    // (the pre-existing path, which the F27 change must not have disturbed).
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                  "<!DOCTYPE html %pe; >" + page,
                  pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
              Reject::ParameterEntityRef);
    // ... and a '%' inside a quoted literal is still NOT a reference.
    EXPECT_EQ(pdfe::core::xmlsafety::inspect(
                  "<!DOCTYPE html [<!ATTLIST x y CDATA \"%notaref;\">]>" + page,
                  pdfe::core::xmlsafety::Policy::AllowDoctype).reason,
              Reject::DoctypeDeclaration);
}


// ===========================================================================
// Guard unit behaviour: the scanner must not fire on inert look-alikes, or the
// refusals above would be indistinguishable from a broken parser.
// ===========================================================================
TEST(XmlSafetyScanner, DoesNotFireOnInertLookalikes)
{
    using pdfe::core::xmlsafety::inspect;

    EXPECT_TRUE(inspect("<a>plain text, no markup declarations</a>").safe());
    EXPECT_TRUE(inspect("<a><!-- <!DOCTYPE x> --></a>").safe())
        << "a DOCTYPE inside a comment is text, not a declaration";
    EXPECT_TRUE(inspect("<a><![CDATA[<!ENTITY e \"x\">]]></a>").safe())
        << "a declaration inside CDATA is text, not a declaration";
    EXPECT_TRUE(inspect("<a sysid=\"MYSYSTEMID\">SYSTEM PUBLIC</a>").safe())
        << "SYSTEM/PUBLIC as element text or inside an attribute is not an "
           "external identifier";
    EXPECT_TRUE(inspect("<a>50% off; 100%</a>").safe());
    EXPECT_TRUE(inspect("").safe());

    // ... and it does fire on the real thing.
    EXPECT_EQ(inspect("<!DOCTYPE a><a/>").reason, Reject::DoctypeDeclaration);
    EXPECT_EQ(inspect("<!DOCTYPE a [<!ENTITY e \"x\">]><a/>").reason,
              Reject::EntityDeclaration);
    EXPECT_EQ(inspect("<!DOCTYPE a SYSTEM \"x.dtd\"><a/>").reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE a [<!ELEMENT a (#PCDATA)>]><a/>").reason,
              Reject::DoctypeDeclaration);
}

// The lenient policy exists only for hOCR (see XmlSafety.h). It must permit a
// plain XHTML doctype and NOTHING else — a policy that let entities through
// would silently disarm the hOCR arm of the guard.
TEST(XmlSafetyScanner, LenientPolicyAllowsOnlyThePlainDoctype)
{
    using pdfe::core::xmlsafety::inspect;
    using pdfe::core::xmlsafety::Policy;

    EXPECT_TRUE(inspect("<!DOCTYPE html><html/>", Policy::AllowDoctype).safe());
    EXPECT_TRUE(inspect("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 "
                        "Transitional//EN\" \"http://www.w3.org/TR/xhtml1/DTD/"
                        "xhtml1-transitional.dtd\"><html/>",
                        Policy::AllowDoctype).safe());

    // Still refused under the lenient policy:
    EXPECT_EQ(inspect("<!DOCTYPE a [<!ENTITY e \"x\">]><a/>",
                      Policy::AllowDoctype).reason,
              Reject::EntityDeclaration);
    EXPECT_EQ(inspect("<!DOCTYPE a [<!ENTITY e SYSTEM \"file:///x\">]><a/>",
                      Policy::AllowDoctype).reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE a [<!ELEMENT a (#PCDATA)>]><a/>",
                      Policy::AllowDoctype).reason,
              Reject::DoctypeDeclaration);

    // ... and the same PUBLIC doctype IS refused under the strict policy used
    // for XMP and XFDF, where the format specs forbid a DTD outright.
    EXPECT_EQ(inspect("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 "
                      "Transitional//EN\" \"http://x/y.dtd\"><html/>").reason,
              Reject::ExternalIdentifier);
}
