// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F36 — UTF-16 XMP packets.
//
// THE DEFECT (verified, not assumed, before it was fixed)
// ------------------------------------------------------
// `XmpMetadataWriter::detectPdfALevel` parsed the /Metadata XMP packet with
// `pugi::xml_document::load_string(xmpData.c_str())`. `load_string` is a
// NUL-TERMINATED entry point: it stops at the first zero byte and treats the
// buffer as UTF-8. ISO 16684-1 §7.1 permits UTF-8, UTF-16 and UTF-32 XMP
// packets, and a UTF-16LE packet's SECOND byte is already NUL (`<` = `3C 00`),
// so exactly one byte ever reached the parser.
//
// F27 had already hardened `xmlsafety::inspect()` to SEE such a packet
// (it sniffs the encoding the way pugixml does and transcodes before scanning).
// Nothing downstream could CONSUME one. That asymmetry is what F36 closes.
//
// THE BLAST RADIUS IS WIDER THAN "DETECTION RETURNS None"
// ------------------------------------------------------
// `detectPdfALevel` is not only a query. `XmpMetadataWriter::syncFromInfo` and
// `XmpMetadataWriter::writePdfUAIdentification` both call it to learn the
// PDF/A level they must PRESERVE, and then rebuild the packet from that
// answer. On a UTF-16 PDF/A document the answer was always `None`, so an
// ordinary Info-dictionary edit silently STRIPPED the pdfaid identification
// from a conforming file. `PdfAValidator::validate` likewise reported
// "XMP metadata missing PDF/A identification" for a file that had it.
// `SyncFromInfoPreservesPdfALevelOnAUtf16Packet` below pins that consequence.
//
// THE DISCRIMINATING CONTROL (phase criterion 2)
// ----------------------------------------------
// `PreFixLoadStringTruncatesTheUtf16PacketAtItsSecondByte` runs the PRE-FIX
// call — literally `load_string(bytes.c_str())` — against the same committed
// fixture, in the same process, and records what it produces, next to what the
// post-fix call produces. It is a permanent, re-checkable control, not an
// assertion in a document: if someone ever reverts the fix, the production
// assertions fail while this test keeps demonstrating exactly why.
//
// THE SECURITY INVARIANT
// ----------------------
// Before F36 a HOSTILE UTF-16 packet was also "protected" by the accidental
// truncation. That accident is gone, so the F13/F27 `XmlSafety` guard is now
// the only thing refusing it.
// `Utf16HostilePacketIsStillRefusedByXmlSafetyNotByTruncation` proves the
// guard fires on a packet that pugixml, on the new path, parses perfectly
// well — i.e. the refusal is PDFE's control, not a parser accident.

#include <gtest/gtest.h>

#include "compliance/ComplianceTypes.h"
#include "compliance/XmpMetadataWriter.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/XmlSafety.h"

#include <QString>
#include <QTemporaryDir>

#include <pugixml.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using pdfe::compliance::PdfALevel;
using pdfe::compliance::PdfUALevel;
using pdfe::compliance::XmpMetadataWriter;

std::filesystem::path corpusDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / "xmp_encoding";
}

std::string readFixture(const char* name)
{
    const std::filesystem::path p = corpusDir() / name;
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// Decode a fixture to a QString INDEPENDENTLY of pugixml, so the corpus
// integrity check does not lean on the component under test.
QString decodeUtf8Bom(const std::string& b)
{
    if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xEF
        && static_cast<unsigned char>(b[1]) == 0xBB
        && static_cast<unsigned char>(b[2]) == 0xBF) {
        return QString::fromUtf8(b.data() + 3,
                                 static_cast<int>(b.size() - 3));
    }
    return QString::fromUtf8(b.data(), static_cast<int>(b.size()));
}

QString decodeUtf16(const std::string& b, bool bigEndian)
{
    // Skip the leading BOM (2 bytes) and re-order to host (little-endian x86)
    // order explicitly rather than relying on QString::fromUtf16's assumption.
    if (b.size() < 2) {
        return {};
    }
    std::vector<char16_t> units;
    units.reserve((b.size() - 2) / 2);
    for (std::size_t i = 2; i + 1 < b.size(); i += 2) {
        const auto lo = static_cast<unsigned char>(b[i]);
        const auto hi = static_cast<unsigned char>(b[i + 1]);
        units.push_back(bigEndian
            ? static_cast<char16_t>((lo << 8) | hi)
            : static_cast<char16_t>((hi << 8) | lo));
    }
    return QString::fromUtf16(units.data(), static_cast<int>(units.size()));
}

// Encode a UTF-8 string as UTF-16, optionally with a leading BOM. Used only to
// build the HOSTILE packets, which are never committed (same policy as
// tests/test_xml_entity_safety.cpp).
std::string toUtf16(const std::string& utf8, bool bigEndian, bool leadingBom)
{
    const QString s = QString::fromUtf8(utf8.c_str(),
                                        static_cast<int>(utf8.size()));
    std::string out;
    if (leadingBom) {
        out += bigEndian ? "\xFE\xFF" : "\xFF\xFE";
    }
    const char16_t* u = reinterpret_cast<const char16_t*>(s.utf16());
    for (int i = 0; i < s.size(); ++i) {
        const char16_t c = u[i];
        const char hi = static_cast<char>((c >> 8) & 0xFF);
        const char lo = static_cast<char>(c & 0xFF);
        if (bigEndian) { out += hi; out += lo; }
        else           { out += lo; out += hi; }
    }
    return out;
}

// The values a caller cares about, however the packet was encoded.
struct Extracted {
    bool parsed = false;
    std::string part;
    std::string conformance;
    std::string title;
    std::string creatorTool;
    std::string producer;

    bool operator==(const Extracted& o) const
    {
        return parsed == o.parsed && part == o.part
            && conformance == o.conformance && title == o.title
            && creatorTool == o.creatorTool && producer == o.producer;
    }
};

Extracted harvest(const pugi::xml_document& d)
{
    Extracted e;
    const pugi::xml_node desc =
        d.child("x:xmpmeta").child("rdf:RDF").child("rdf:Description");
    if (!desc) {
        return e;
    }
    e.part        = desc.child("pdfaid:part").child_value();
    e.conformance = desc.child("pdfaid:conformance").child_value();
    e.title       = desc.child("dc:title").child("rdf:Alt")
                        .child("rdf:li").child_value();
    e.creatorTool = desc.child("xmp:CreatorTool").child_value();
    e.producer    = desc.child("pdf:Producer").child_value();
    return e;
}

// THE POST-FIX PATH — byte-identical to src/compliance/XmpMetadataWriter.cpp.
Extracted extractPostFix(const std::string& bytes)
{
    pugi::xml_document d;
    const pugi::xml_parse_result r = d.load_buffer(
        bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_auto);
    Extracted e = harvest(d);
    e.parsed = static_cast<bool>(r);
    return e;
}

// THE PRE-FIX PATH — byte-identical to what the file contained before F36.
Extracted extractPreFix(const std::string& bytes)
{
    pugi::xml_document d;
    const pugi::xml_parse_result r = d.load_string(bytes.c_str());
    Extracted e = harvest(d);
    e.parsed = static_cast<bool>(r);
    return e;
}

// Minimal 1-page PDF whose catalog carries `xmp` as an uncompressed /Metadata
// stream. Same idiom as tests/test_xml_entity_safety.cpp:200 — no xref offsets
// (startxref 0), MuPDF's repair path rescans. /Length is declared explicitly,
// so a stream full of NUL bytes is carried verbatim.
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

// A hostile packet whose BODY is a perfectly good PDF/A-1b claim; the only
// thing wrong with it is the DTD. Mirrors externalDtdXmp() in
// tests/test_xml_entity_safety.cpp so the two surfaces stay comparable.
std::string externalDtdXmpUtf8()
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

std::string entityBombXmpUtf8()
{
    return
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<!DOCTYPE x:xmpmeta [\n"
        "  <!ENTITY a \"aaaaaaaaaa\">\n"
        "  <!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">\n"
        "]>\n"
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

constexpr const char* kUtf8Fixture    = "xmp_pdfa2b_utf8.xmp";
constexpr const char* kUtf16LeFixture = "xmp_pdfa2b_utf16le.xmp";
constexpr const char* kUtf16BeFixture = "xmp_pdfa2b_utf16be.xmp";

} // namespace

// ===========================================================================
class XmpEncodingTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();

        utf8_    = readFixture(kUtf8Fixture);
        utf16le_ = readFixture(kUtf16LeFixture);
        utf16be_ = readFixture(kUtf16BeFixture);
        ASSERT_FALSE(utf8_.empty())    << kUtf8Fixture    << " missing";
        ASSERT_FALSE(utf16le_.empty()) << kUtf16LeFixture << " missing";
        ASSERT_FALSE(utf16be_.empty()) << kUtf16BeFixture << " missing";
    }

    void TearDown() override
    {
        ctx_.reset();
        tempDir_.reset();
    }

    std::filesystem::path writePdf(const std::string& name,
                                   const std::string& xmp)
    {
        const std::filesystem::path p =
            std::filesystem::path(tempDir_->path().toStdString()) / name;
        std::ofstream f(p, std::ios::binary);
        const std::string bytes = buildPdfWithXmp(xmp);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.close();
        return p;
    }

    std::unique_ptr<QTemporaryDir> tempDir_;
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::string utf8_, utf16le_, utf16be_;
};

// ===========================================================================
// CORPUS INTEGRITY (phase criterion 4).
//
// The three fixtures are transcodes of ONE source string. Decoded here with Qt
// — not with pugixml — they must yield the identical QString. If `core.autocrlf`
// ever line-ending-mangles one of them on checkout (every `0A 00` becoming
// `0D 00 0A 00` in the UTF-16 files, or `0A` -> `0D 0A` in the UTF-8 one),
// they stop agreeing and this fails. That is what makes `*.xmp binary` in
// .gitattributes a CHECKED control rather than a hopeful comment.
// ===========================================================================
TEST_F(XmpEncodingTest, FixtureCorpusIsAnExactTranscodeSet)
{
    ASSERT_EQ(static_cast<unsigned char>(utf16le_[0]), 0xFFu);
    ASSERT_EQ(static_cast<unsigned char>(utf16le_[1]), 0xFEu);
    ASSERT_EQ(static_cast<unsigned char>(utf16be_[0]), 0xFEu);
    ASSERT_EQ(static_cast<unsigned char>(utf16be_[1]), 0xFFu);
    ASSERT_EQ(static_cast<unsigned char>(utf8_[0]), 0xEFu);
    ASSERT_EQ(static_cast<unsigned char>(utf8_[1]), 0xBBu);
    ASSERT_EQ(static_cast<unsigned char>(utf8_[2]), 0xBFu);

    EXPECT_EQ(utf16le_.size() % 2u, 0u) << "UTF-16LE fixture has an odd byte "
        "count — it has been rewritten by something (autocrlf?)";
    EXPECT_EQ(utf16be_.size(), utf16le_.size());

    const QString a = decodeUtf8Bom(utf8_);
    const QString b = decodeUtf16(utf16le_, /*bigEndian=*/false);
    const QString c = decodeUtf16(utf16be_, /*bigEndian=*/true);

    EXPECT_EQ(a, b) << "UTF-8 and UTF-16LE fixtures no longer decode alike";
    EXPECT_EQ(a, c) << "UTF-8 and UTF-16BE fixtures no longer decode alike";

    // The astral character must have survived; an all-BMP fixture would let a
    // surrogate-pair bug pass unnoticed.
    EXPECT_TRUE(a.contains(QString::fromUcs4(
        []{ static const char32_t s[] = {0x1D11E, 0}; return s; }(), 1)))
        << "the U+1D11E surrogate pair is gone from the corpus";

    // No CR anywhere: the generator emits LF only.
    EXPECT_FALSE(a.contains(QChar('\r')))
        << "a CR appeared in the corpus — .gitattributes `*.xmp binary` lost?";
}

// ===========================================================================
// CRITERION 2 — THE DISCRIMINATING CONTROL.
//
// Runs the literal pre-fix call against the committed UTF-16 fixtures and
// records what it produced, beside the post-fix call on the same bytes. This
// is the necessity proof for the fix: without it, "the old code truncated"
// would be an assertion in a document rather than a measurement.
// ===========================================================================
TEST_F(XmpEncodingTest, PreFixLoadStringTruncatesTheUtf16PacketAtItsSecondByte)
{
    // (a) THE MECHANISM. `.c_str()` hands pugixml a NUL-terminated view; the
    //     UTF-16LE packet's first NUL is at offset 3 (BOM `FF FE`, then
    //     `3C 00`), so 1595 of its 1598 bytes are simply not there.
    ASSERT_GT(utf16le_.size(), 8u);
    const std::size_t leVisible = std::string(utf16le_.c_str()).size();
    const std::size_t beVisible = std::string(utf16be_.c_str()).size();
    EXPECT_LT(leVisible, utf16le_.size())
        << "the UTF-16LE fixture must contain an embedded NUL, else this "
           "control is vacuous";
    EXPECT_LT(beVisible, utf16be_.size());
    EXPECT_LE(leVisible, 4u) << "measured NUL-terminated prefix length (LE)";
    EXPECT_LE(beVisible, 4u) << "measured NUL-terminated prefix length (BE)";

    // (b) THE PRE-FIX RESULT: nothing at all comes back.
    for (const auto& [label, bytes] :
         std::vector<std::pair<const char*, const std::string*>>{
             {"utf16le", &utf16le_}, {"utf16be", &utf16be_}}) {
        const Extracted before = extractPreFix(*bytes);
        EXPECT_FALSE(before.parsed)
            << label << ": load_string() must FAIL on the truncated prefix";
        EXPECT_TRUE(before.part.empty())
            << label << ": pre-fix pdfaid:part was unexpectedly non-empty";
        EXPECT_TRUE(before.title.empty());

        // (c) THE POST-FIX RESULT ON THE SAME BYTES: everything comes back.
        const Extracted after = extractPostFix(*bytes);
        EXPECT_TRUE(after.parsed) << label << ": load_buffer() must succeed";
        EXPECT_EQ(after.part, "2");
        EXPECT_EQ(after.conformance, "B");
        EXPECT_FALSE(after.title.empty());
        EXPECT_NE(before, after)
            << label << ": pre- and post-fix agreed, so the fix changed "
                        "nothing and this control proves nothing";
    }

    // (d) AND THE CONTROL IS NOT "load_string is simply broken": on the UTF-8
    //     fixture the two paths agree exactly. Truncation is encoding-specific.
    EXPECT_EQ(extractPreFix(utf8_), extractPostFix(utf8_))
        << "UTF-8 must behave identically on both paths — otherwise the "
           "difference above is not about encoding at all";
}

// ===========================================================================
// CRITERION 1 (values) — the same metadata comes out of all three encodings.
// ===========================================================================
TEST_F(XmpEncodingTest, Utf16LeAndUtf16BeYieldTheSameValuesAsUtf8)
{
    const Extracted u8 = extractPostFix(utf8_);
    const Extracted le = extractPostFix(utf16le_);
    const Extracted be = extractPostFix(utf16be_);

    ASSERT_TRUE(u8.parsed);
    ASSERT_TRUE(le.parsed);
    ASSERT_TRUE(be.parsed);

    // Pin the UTF-8 baseline to literals first, so "all three agree" cannot be
    // satisfied by all three returning nothing.
    EXPECT_EQ(u8.part, "2");
    EXPECT_EQ(u8.conformance, "B");
    EXPECT_EQ(u8.creatorTool, "PDFE F36 fixture generator");
    EXPECT_EQ(u8.producer, "PDFE");
    EXPECT_FALSE(u8.title.empty());
    EXPECT_NE(u8.title.find("nicode"), std::string::npos);

    EXPECT_EQ(u8, le) << "UTF-16LE extraction diverged from UTF-8";
    EXPECT_EQ(u8, be) << "UTF-16BE extraction diverged from UTF-8";

    // The astral character round-trips through pugixml's transcoder as the
    // 4-byte UTF-8 sequence F0 9D 84 9E.
    EXPECT_NE(le.title.find("\xF0\x9D\x84\x9E"), std::string::npos)
        << "U+1D11E did not survive the UTF-16LE -> UTF-8 transcode";
    EXPECT_NE(be.title.find("\xF0\x9D\x84\x9E"), std::string::npos)
        << "U+1D11E did not survive the UTF-16BE -> UTF-8 transcode";
}

// ===========================================================================
// CRITERION 1 (production path) — end-to-end through a real Document, the
// public API PdfAValidator and syncFromInfo actually depend on.
// ===========================================================================
TEST_F(XmpEncodingTest, DetectPdfALevelAgreesAcrossEncodings)
{
    struct Case { const char* name; const std::string* bytes; };
    const std::vector<Case> cases{
        {"utf8.pdf", &utf8_}, {"utf16le.pdf", &utf16le_},
        {"utf16be.pdf", &utf16be_}};

    for (const Case& c : cases) {
        const std::filesystem::path pdf = writePdf(c.name, *c.bytes);
        auto doc = pdfe::core::Document::open(ctx_, pdf);
        ASSERT_NE(doc, nullptr) << c.name;
        XmpMetadataWriter writer(ctx_);

        // The packet must really be reachable, byte-for-byte, or the detection
        // assertion below would be vacuous.
        const std::string raw = writer.readXmpPacket(doc.get());
        ASSERT_EQ(raw.size(), c.bytes->size())
            << c.name << ": /Metadata stream did not round-trip";
        ASSERT_EQ(raw, *c.bytes) << c.name;

        EXPECT_TRUE(XmpMetadataWriter::isXmpPacketAcceptable(raw))
            << c.name << ": XmlSafety must accept a benign packet in any "
                         "encoding (F27), else detection never runs";
        EXPECT_EQ(writer.detectPdfALevel(doc.get()), PdfALevel::PdfA_2b)
            << c.name << ": a PDF/A-2b packet must be detected whatever "
                         "encoding it is written in";
    }
}

// ===========================================================================
// SCOPE DECISION, MADE EXPLICIT — BOM-less UTF-16 is IN scope and works.
//
// ISO 16684-1 puts the U+FEFF inside the `<?xpacket begin="…"?>` header, in the
// packet's own encoding — so a real-world UTF-16 packet frequently has NO BOM
// at byte offset 0; it starts `3C 00 3F 00`. pugixml's `guess_buffer_encoding`
// falls through from the BOM check to the first-four-bytes NUL pattern, which
// classifies exactly that. Rather than assume it, measure it: the BOM-less
// variants are DERIVED from the committed fixtures here (drop 2 bytes), so
// they cannot drift from them and need no fourth file.
// ===========================================================================
TEST_F(XmpEncodingTest, BomlessUtf16PacketsAreAlsoDetected)
{
    const std::string leNoBom = utf16le_.substr(2);
    const std::string beNoBom = utf16be_.substr(2);

    ASSERT_EQ(static_cast<unsigned char>(leNoBom[0]), 0x3Cu);
    ASSERT_EQ(static_cast<unsigned char>(leNoBom[1]), 0x00u);
    ASSERT_EQ(static_cast<unsigned char>(beNoBom[0]), 0x00u);
    ASSERT_EQ(static_cast<unsigned char>(beNoBom[1]), 0x3Cu);

    const Extracted le = extractPostFix(leNoBom);
    const Extracted be = extractPostFix(beNoBom);
    EXPECT_TRUE(le.parsed);
    EXPECT_TRUE(be.parsed);
    EXPECT_EQ(le.part, "2");
    EXPECT_EQ(be.part, "2");
    EXPECT_EQ(le, extractPostFix(utf16le_))
        << "dropping the leading BOM changed the extracted values";
    EXPECT_EQ(be, extractPostFix(utf16be_));

    // And end-to-end, through the production API.
    for (const auto& [name, bytes] :
         std::vector<std::pair<const char*, const std::string*>>{
             {"le_nobom.pdf", &leNoBom}, {"be_nobom.pdf", &beNoBom}}) {
        const std::filesystem::path pdf = writePdf(name, *bytes);
        auto doc = pdfe::core::Document::open(ctx_, pdf);
        ASSERT_NE(doc, nullptr) << name;
        XmpMetadataWriter writer(ctx_);
        EXPECT_EQ(writer.detectPdfALevel(doc.get()), PdfALevel::PdfA_2b)
            << name;
    }
}

// ===========================================================================
// CRITERION 3 — UTF-8 is not regressed, asserted here as well as by the
// pre-existing XmpMetadataWriterTest / XmlEntitySafetyTest suites, which were
// NOT modified for this phase.
// ===========================================================================
TEST_F(XmpEncodingTest, Utf8PacketsWithAndWithoutABomStillParse)
{
    const std::string noBom = utf8_.substr(3);
    ASSERT_EQ(noBom[0], '<');

    const Extracted withBom = extractPostFix(utf8_);
    const Extracted sansBom = extractPostFix(noBom);
    EXPECT_TRUE(withBom.parsed);
    EXPECT_TRUE(sansBom.parsed);
    EXPECT_EQ(withBom, sansBom);

    for (const auto& [name, bytes] :
         std::vector<std::pair<const char*, const std::string*>>{
             {"u8_bom.pdf", &utf8_}, {"u8_nobom.pdf", &noBom}}) {
        const std::filesystem::path pdf = writePdf(name, *bytes);
        auto doc = pdfe::core::Document::open(ctx_, pdf);
        ASSERT_NE(doc, nullptr) << name;
        XmpMetadataWriter writer(ctx_);
        EXPECT_EQ(writer.detectPdfALevel(doc.get()), PdfALevel::PdfA_2b)
            << name;
    }
}

// ===========================================================================
// THE CONSEQUENCE THE FINDING DID NOT NAME — silent PDF/A strip on Info edit.
//
// `syncFromInfo` rebuilds the packet from `detectPdfALevel`'s answer. Before
// F36 that answer was `None` for a UTF-16 packet, so the rebuilt packet lost
// its pdfaid identification: a plain metadata edit downgraded a conforming
// document. This asserts the level SURVIVES, and asserts the rebuilt packet is
// really a rebuild (UTF-8, per buildXmpPacket) so the test cannot pass by
// syncFromInfo having quietly done nothing.
// ===========================================================================
TEST_F(XmpEncodingTest, SyncFromInfoPreservesPdfALevelOnAUtf16Packet)
{
    const std::filesystem::path pdf = writePdf("sync_le.pdf", utf16le_);
    auto doc = pdfe::core::Document::open(ctx_, pdf);
    ASSERT_NE(doc, nullptr);
    XmpMetadataWriter writer(ctx_);

    ASSERT_EQ(writer.readXmpPacket(doc.get()), utf16le_);
    ASSERT_TRUE(writer.syncFromInfo(doc.get()))
        << "syncFromInfo must act — the document does have an XMP packet";

    const std::string rebuilt = writer.readXmpPacket(doc.get());
    ASSERT_NE(rebuilt, utf16le_) << "the packet was not actually rebuilt";
    EXPECT_NE(rebuilt.find("pdfaid:part"), std::string::npos)
        << "syncFromInfo STRIPPED the PDF/A identification from a UTF-16 "
           "packet — this is the F36 data-loss consequence";
    EXPECT_NE(rebuilt.find("<pdfaid:part>2</pdfaid:part>"), std::string::npos);
    EXPECT_NE(rebuilt.find("<pdfaid:conformance>B</pdfaid:conformance>"),
              std::string::npos);
    EXPECT_EQ(writer.detectPdfALevel(doc.get()), PdfALevel::PdfA_2b);

    // RESIDUAL, ASSERTED SO IT CANNOT BE MISTAKEN FOR AN OVERSIGHT: the
    // rebuilt packet is UTF-8, because `buildXmpPacket` only ever emits UTF-8
    // (`doc.save(..., pugi::encoding_utf8)`, XmpMetadataWriter.cpp:584). That
    // is legal — ISO 16684-1 permits UTF-8 — and the PDF/A level, which is
    // what the F36 data-loss defect was about, survives. The packet's original
    // ENCODING does not. F36 is about reading; re-encoding on write is a
    // separate question, recorded in phase-36-dispositions.md §11.
    EXPECT_EQ(rebuilt.find('\0'), std::string::npos)
        << "the rebuilt packet is expected to be UTF-8, with no NUL bytes";
    EXPECT_NE(rebuilt.find("<x:xmpmeta"), std::string::npos)
        << "rebuilt packet is not UTF-8 element markup";
}

// ===========================================================================
// SECURITY — the widened input path did NOT drop a safety check.
//
// Three assertions, and the third is the one that matters:
//   (a) XmlSafety refuses the hostile UTF-16 packet and names why;
//   (b) end-to-end, detectPdfALevel yields None (the forged claim is not
//       honoured);
//   (c) pugixml, on the NEW path, parses those exact bytes and would have
//       surfaced `pdfaid:part` = 1 — proving the refusal in (a)/(b) comes from
//       PDFE's guard and not from the truncation accident F36 removed.
// Both leading-BOM and BOM-less forms, both byte orders, both DTD shapes.
// ===========================================================================
TEST_F(XmpEncodingTest, Utf16HostilePacketIsStillRefusedByXmlSafetyNotByTruncation)
{
    struct Shape { const char* name; std::string utf8; pdfe::core::xmlsafety::Reject expect; };
    const std::vector<Shape> shapes{
        {"extdtd", externalDtdXmpUtf8(),
         pdfe::core::xmlsafety::Reject::ExternalIdentifier},
        {"bomb", entityBombXmpUtf8(),
         pdfe::core::xmlsafety::Reject::EntityDeclaration},
    };

    int caseIndex = 0;
    for (const Shape& shape : shapes) {
        for (bool bigEndian : {false, true}) {
            for (bool leadingBom : {true, false}) {
                const std::string bytes =
                    toUtf16(shape.utf8, bigEndian, leadingBom);
                const std::string label = std::string(shape.name)
                    + (bigEndian ? "/BE" : "/LE")
                    + (leadingBom ? "/bom" : "/nobom");

                // (c) FIRST — establish the packet is genuinely parseable on
                //     the new path, so (a)/(b) are not passing by accident.
                const Extracted asParsed = extractPostFix(bytes);
                ASSERT_TRUE(asParsed.parsed)
                    << label << ": pugixml did NOT parse these bytes, so this "
                                "case cannot demonstrate anything about the "
                                "guard being load-bearing";
                ASSERT_EQ(asParsed.part, "1")
                    << label << ": the forged PDF/A-1b claim must be visible "
                                "to the parser for the refusal to mean "
                                "anything";

                // (a) PDFE's own guard refuses it, and says why.
                std::string reason;
                EXPECT_FALSE(XmpMetadataWriter::isXmpPacketAcceptable(
                                 bytes, &reason))
                    << label << ": a DTD-bearing UTF-16 XMP packet must be "
                                "refused";
                EXPECT_FALSE(reason.empty()) << label;
                EXPECT_EQ(pdfe::core::xmlsafety::inspect(bytes).reason,
                          shape.expect) << label;

                // (b) end-to-end: the forged claim is not honoured.
                const std::string pdfName =
                    "hostile_" + std::to_string(caseIndex++) + ".pdf";
                const std::filesystem::path pdf = writePdf(pdfName, bytes);
                auto doc = pdfe::core::Document::open(ctx_, pdf);
                ASSERT_NE(doc, nullptr) << label;
                XmpMetadataWriter writer(ctx_);
                ASSERT_EQ(writer.readXmpPacket(doc.get()), bytes) << label;
                EXPECT_EQ(writer.detectPdfALevel(doc.get()), PdfALevel::None)
                    << label << ": the DTD-bearing packet must yield no PDF/A "
                                "determination at all";
            }
        }
    }
}
