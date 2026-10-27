// -*- coding: utf-8 -*-
//
// PDF-05 (meta-audit gap register) / GitHub issue #3 —
// `UnencryptedWrapperDetectedAndReported`.
//
// PRODUCT GAP, confirmed before anything was written. `UnencryptedWrapper` and
// `EncryptMetadata` appeared NOWHERE in src/ (the only `Wrapper` hits were the
// unrelated core/QpdfWrapper.h), and this file did not exist. PDFE had no
// notion of a PDF 2.0 unencrypted wrapper at all, so no test could have been
// written against existing behaviour — the row needed src/core/
// UnencryptedWrapperProbe.{h,cpp} first.
//
// WHY "AND REPORTED" IS LOAD-BEARING. A wrapper is an UNENCRYPTED document
// whose page content is a cover sheet; the real document is an encrypted
// payload attachment. It opens, it renders, and a reader that says nothing
// leaves the user reading a page that is not the document. A detector nobody
// surfaces satisfies the letter of the criterion and none of its intent. So
// the user-facing message is produced by the probe (`summary`) and asserted
// here, not assembled inside a dialog where no test can see it.
//
// FIXTURES ARE BUILT IN-TEST rather than committed. Each case differs from the
// next by one dictionary key, and a reader can see the difference in the
// source; three near-identical committed PDFs would hide exactly the
// distinction the tests turn on.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/UnencryptedWrapperProbe.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::probeUnencryptedWrapper;
using pdfe::core::UnencryptedWrapperInfo;

namespace {

namespace fs = std::filesystem;

fs::path tempDir()
{
    auto d = fs::temp_directory_path() / "pdfe_pdf20_features";
    fs::create_directories(d);
    return d;
}

void writeBytes(const fs::path& p, const std::string& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A one-page PDF 2.0 document.
//
//   `collection`  emit a catalog /Collection (portfolio marker)
//   `payload`     emit an /AF entry + embedded file whose /AFRelationship is
//                 /EncryptedPayload, carrying an /EP dictionary
//   `viaNameTree` declare the payload ONLY through /Names /EmbeddedFiles, not
//                 through /AF — the reachability path a reader that looked at
//                 /AF alone would miss
//
// No xref table: MuPDF reconstructs, exactly as the neighbouring malicious-PDF
// generators rely on. Computing offsets here would make the generator the
// thing under test.
std::string wrapperPdf(bool collection, bool payload, bool viaNameTree)
{
    const std::string cover =
        "BT /F1 14 Tf 60 700 Td (This document is protected.) Tj ET\n";

    std::string catalog = "1 0 obj\n<< /Type /Catalog /Pages 2 0 R";
    if (collection) {
        catalog += " /Collection << /Type /Collection /View /H >>";
    }
    if (payload && !viaNameTree) {
        catalog += " /AF [5 0 R]";
    }
    if (payload && viaNameTree) {
        catalog += " /Names << /EmbeddedFiles << /Names "
                   "[(payload.pdf) 5 0 R] >> >>";
    }
    catalog += " >>\nendobj\n";

    std::string out = "%PDF-2.0\n";
    out += catalog;
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 "
           "/BaseFont /Helvetica >> >> >> /Contents 4 0 R >>\nendobj\n";
    out += "4 0 obj\n<< /Length " + std::to_string(cover.size()) +
           " >>\nstream\n" + cover + "endstream\nendobj\n";

    if (payload) {
        out += "5 0 obj\n<< /Type /Filespec /F (payload.pdf) /UF (payload.pdf) "
               "/AFRelationship /EncryptedPayload /EF << /F 6 0 R >> "
               ">>\nendobj\n";
        const std::string blob = "ENCRYPTED-PAYLOAD-BYTES";
        out += "6 0 obj\n<< /Type /EmbeddedFile /Subtype /application#2Fpdf "
               "/EP << /Type /EncryptedPayload /Name /AcmeSecure /V 2 >> "
               "/Length " + std::to_string(blob.size()) + " >>\nstream\n" +
               blob + "\nendstream\nendobj\n";
    }

    out += "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

// ── /AF (associated files) snapshot, read from the BYTES ON DISK ────────────
//
// GitHub #2 / gap-register PDF-04.
//
// setjmp/longjmp discipline: everything written inside the fz_try below is a
// PLAIN-OLD-DATA buffer. Not one std::string, std::vector or map is
// constructed in a frame a longjmp could abandon — the STL conversions happen
// in the caller, after the fz_try has returned. (MuPdfContext::execute() wraps
// its lambda in an fz_try too, so the same rule would apply there.)
constexpr int kMaxAf = 16;
constexpr size_t kAfNameMax = 160;
constexpr size_t kAfRelMax = 64;

struct AfEntryRaw {
    char name[kAfNameMax];
    char rel[kAfRelMax];
    bool relPresent;
};

struct AfSnapshotRaw {
    bool opened = false;
    int catalogCount = 0;
    int pageCount = 0;
    AfEntryRaw catalog[kMaxAf]{};
    AfEntryRaw page0[kMaxAf]{};
    int embeddedFilesNameCount = 0;  // /Names /EmbeddedFiles entries
};

// Copy `src` (may be null) into a fixed buffer, always NUL-terminated.
void copyFixed(char* dst, size_t cap, const char* src)
{
    dst[0] = '\0';
    if (src == nullptr) return;
    std::strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void readAfArray(fz_context* c, pdf_obj* arr, AfEntryRaw* out, int* count)
{
    *count = 0;
    if (arr == nullptr || !pdf_is_array(c, arr)) return;
    const int n = pdf_array_len(c, arr);
    for (int i = 0; i < n && *count < kMaxAf; ++i) {
        pdf_obj* spec = pdf_array_get(c, arr, i);
        if (spec == nullptr) continue;
        AfEntryRaw& e = out[*count];
        // /UF is the PDF 2.0-preferred name; /F is the legacy one. Prefer /UF,
        // fall back to /F, so a writer that keeps only one is still readable.
        pdf_obj* uf = pdf_dict_get(c, spec, PDF_NAME(UF));
        pdf_obj* f = pdf_dict_get(c, spec, PDF_NAME(F));
        const char* nm = nullptr;
        if (uf != nullptr && pdf_is_string(c, uf)) nm = pdf_to_text_string(c, uf);
        else if (f != nullptr && pdf_is_string(c, f)) nm = pdf_to_text_string(c, f);
        copyFixed(e.name, kAfNameMax, nm);

        pdf_obj* rel = pdf_dict_gets(c, spec, "AFRelationship");
        e.relPresent = (rel != nullptr && pdf_is_name(c, rel));
        copyFixed(e.rel, kAfRelMax, e.relPresent ? pdf_to_name(c, rel) : nullptr);
        ++(*count);
    }
}

AfSnapshotRaw readAssociatedFiles(fz_context* c, const char* path)
{
    AfSnapshotRaw snap;
    fz_document* fzDoc = nullptr;
    fz_try(c) {
        fzDoc = fz_open_document(c, path);
        pdf_document* pdfDoc = pdf_document_from_fz_document(c, fzDoc);
        if (pdfDoc != nullptr) {
            snap.opened = true;
            pdf_obj* catalog =
                pdf_dict_get(c, pdf_trailer(c, pdfDoc), PDF_NAME(Root));
            readAfArray(c, pdf_dict_gets(c, catalog, "AF"), snap.catalog,
                        &snap.catalogCount);

            if (pdf_count_pages(c, pdfDoc) > 0) {
                pdf_obj* pg = pdf_lookup_page_obj(c, pdfDoc, 0);
                readAfArray(c, pdf_dict_gets(c, pg, "AF"), snap.page0,
                            &snap.pageCount);
            }

            pdf_obj* names = pdf_dict_get(c, catalog, PDF_NAME(Names));
            pdf_obj* ef = names ? pdf_dict_gets(c, names, "EmbeddedFiles")
                                : nullptr;
            pdf_obj* efNames = ef ? pdf_dict_get(c, ef, PDF_NAME(Names))
                                  : nullptr;
            if (efNames != nullptr && pdf_is_array(c, efNames)) {
                snap.embeddedFilesNameCount = pdf_array_len(c, efNames) / 2;
            }
        }
    }
    fz_always(c) {
        if (fzDoc != nullptr) fz_drop_document(c, fzDoc);
    }
    fz_catch(c) {
        snap.opened = false;
    }
    return snap;
}

// name -> /AFRelationship, built OUTSIDE the fz_try. A missing relationship
// maps to the sentinel "<no /AFRelationship>" rather than to an absent key, so
// "the relationship was dropped" is a VALUE DIFFERENCE in the comparison below
// and not merely a smaller map — the two failures read differently and both
// must be caught.
std::map<std::string, std::string> afMap(const AfEntryRaw* e, int n)
{
    std::map<std::string, std::string> m;
    for (int i = 0; i < n; ++i) {
        m[std::string(e[i].name)] =
            e[i].relPresent ? std::string(e[i].rel)
                            : std::string("<no /AFRelationship>");
    }
    return m;
}

std::string describe(const std::map<std::string, std::string>& m)
{
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (const auto& kv : m) {
        if (!first) os << ", ";
        first = false;
        os << kv.first << " -> /" << kv.second;
    }
    os << "}";
    return os.str();
}

class Pdf20FeaturesTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<MuPdfContext>();
        td_ = tempDir();
    }
    std::shared_ptr<MuPdfContext> ctx_;
    fs::path td_;
};

}  // namespace

// THE ROW. A wrapper must be DETECTED and the detection must carry something
// the UI can SAY.
//
// The three controls are in the two tests below, not here, because they are
// separate claims: a plain document, and a portfolio that is not a wrapper.
// Without both, "return isWrapper = true unconditionally" passes this test.
TEST_F(Pdf20FeaturesTest, UnencryptedWrapperDetectedAndReported)
{
    const auto path = td_ / "wrapper.pdf";
    writeBytes(path, wrapperPdf(/*collection=*/true, /*payload=*/true,
                                /*viaNameTree=*/false));

    // PRECONDITION, asserted rather than assumed: the document really does
    // open as an ordinary, unencrypted, renderable PDF. That is the whole
    // hazard — if it failed to open, the user would already know something was
    // wrong and the advisory would be redundant.
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr) << "the wrapper fixture does not open at all, so "
                               "it is not the silent-cover-sheet case this "
                               "test is about";
    ASSERT_EQ(doc->pageCount(), 1);
    doc.reset();

    const UnencryptedWrapperInfo info = probeUnencryptedWrapper(ctx_, path);

    EXPECT_TRUE(info.isWrapper)
        << "a document carrying an /AFRelationship /EncryptedPayload "
           "attachment was not recognised as an unencrypted wrapper";
    EXPECT_TRUE(info.hasEncryptedPayload);
    EXPECT_TRUE(info.hasCollection);

    // DETAIL, not just a boolean. A warning that cannot name the payload or
    // the handler leaves the user with nothing to act on.
    EXPECT_EQ(info.payloadFileName, "payload.pdf");
    EXPECT_EQ(info.cryptoFilterName, "AcmeSecure");
    EXPECT_EQ(info.payloadVersion, 2);

    // ---- THE "REPORTED" HALF. ----
    ASSERT_FALSE(info.summary.empty())
        << "the wrapper was detected but the probe produced nothing to tell "
           "the user -- that meets the letter of the criterion and none of "
           "its intent";
    // The message has to say the thing that matters: what is on screen is not
    // the document. Asserting the presence of a string is weak, so assert the
    // three facts a user needs, each of which a placeholder message would miss.
    EXPECT_NE(info.summary.find("cover sheet"), std::string::npos)
        << "the advisory does not tell the user the visible page is a cover "
           "sheet; message was: " << info.summary;
    EXPECT_NE(info.summary.find("payload.pdf"), std::string::npos)
        << "the advisory does not name the payload; message was: "
        << info.summary;
    EXPECT_NE(info.summary.find("AcmeSecure"), std::string::npos)
        << "the advisory does not name the handler the payload needs; message "
           "was: " << info.summary;
}

// CONTROL 1. An ordinary document must NOT be reported, and must carry NO
// message. Without this, an unconditional "true" passes the test above.
//
// The empty-summary half matters on its own: if `summary` were populated
// regardless of the flag, a caller that forgot to check `isWrapper` would warn
// about every document, and the warning would stop meaning anything.
TEST_F(Pdf20FeaturesTest, OrdinaryDocumentIsNotReportedAsAWrapper)
{
    const auto path = td_ / "plain.pdf";
    writeBytes(path, wrapperPdf(/*collection=*/false, /*payload=*/false,
                                /*viaNameTree=*/false));

    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 1);
    doc.reset();

    const UnencryptedWrapperInfo info = probeUnencryptedWrapper(ctx_, path);
    EXPECT_FALSE(info.isWrapper);
    EXPECT_FALSE(info.hasEncryptedPayload);
    EXPECT_FALSE(info.hasCollection);
    EXPECT_TRUE(info.summary.empty())
        << "an ordinary document produced a wrapper advisory: " << info.summary;
}

// CONTROL 2, and the sharper one. A PDF PORTFOLIO has a /Collection and is not
// a wrapper. Detecting on /Collection alone would flag every portfolio, and a
// warning shown on documents that are fine is one users learn to dismiss —
// which disarms it for the documents that are not.
TEST_F(Pdf20FeaturesTest, PortfolioWithoutEncryptedPayloadIsNotAWrapper)
{
    const auto path = td_ / "portfolio.pdf";
    writeBytes(path, wrapperPdf(/*collection=*/true, /*payload=*/false,
                                /*viaNameTree=*/false));

    const UnencryptedWrapperInfo info = probeUnencryptedWrapper(ctx_, path);
    // The precondition is what makes this a control rather than a repeat of
    // the one above: the marker IS present, and is deliberately not enough.
    ASSERT_TRUE(info.hasCollection)
        << "the portfolio fixture carries no /Collection, so it does not "
           "distinguish 'detects on /Collection' from 'detects on payload'";
    EXPECT_FALSE(info.isWrapper)
        << "an ordinary PDF portfolio was reported as an encrypted-payload "
           "wrapper";
    EXPECT_TRUE(info.summary.empty());
}

// REACHABILITY. A payload declared only through /Names /EmbeddedFiles, with no
// /AF array, must still be found. A probe that walked /AF alone would miss the
// document it exists to warn about — the same "union of two reachability
// paths" argument PdfSignatureProbe makes about signature widgets, and the
// same way the F26 defect it documents came about.
TEST_F(Pdf20FeaturesTest, PayloadReachableOnlyViaNameTreeIsStillDetected)
{
    const auto path = td_ / "wrapper_nametree.pdf";
    writeBytes(path, wrapperPdf(/*collection=*/true, /*payload=*/true,
                                /*viaNameTree=*/true));

    // PRECONDITION: this fixture must genuinely lack /AF, otherwise it is a
    // duplicate of the first test and proves nothing about the second path.
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    ASSERT_EQ(bytes.find("/AF ["), std::string::npos)
        << "the name-tree fixture also declares /AF, so it cannot show that "
           "the name-tree path is walked";
    ASSERT_NE(bytes.find("/EmbeddedFiles"), std::string::npos);

    const UnencryptedWrapperInfo info = probeUnencryptedWrapper(ctx_, path);
    EXPECT_TRUE(info.isWrapper)
        << "a payload declared only in /Names /EmbeddedFiles was missed";
    EXPECT_EQ(info.payloadFileName, "payload.pdf");
}

// The probe is ADVISORY and must never be the reason a file fails. A path that
// is not a PDF at all answers "not a wrapper" instead of throwing.
TEST_F(Pdf20FeaturesTest, UnreadableFileAnswersNotAWrapperRatherThanThrowing)
{
    const auto path = td_ / "not_a_pdf.bin";
    writeBytes(path, "this is not a PDF at all\n");

    UnencryptedWrapperInfo info;
    EXPECT_NO_THROW({ info = probeUnencryptedWrapper(ctx_, path); });
    EXPECT_FALSE(info.isWrapper);
    EXPECT_TRUE(info.summary.empty());

    // And a path that does not exist.
    EXPECT_NO_THROW({
        info = probeUnencryptedWrapper(ctx_, td_ / "definitely_absent.pdf");
    });
    EXPECT_FALSE(info.isWrapper);
}

// ───────────────────────────────────────────────────────────────────────────
// GitHub #2 / gap-register PDF-04 — ASSOCIATED FILES SURVIVE A SAVE/REOPEN.
//
// ISO 32000-2 §14.13. An associated file is an embedded file plus a statement
// of WHAT IT IS to the document that carries it: /Source (the file the PDF was
// produced from), /Data (the machine-readable payload the page renders),
// /Supplement, /Alternative, /EncryptedPayload, /FormData, /Schema,
// /Unspecified.
//
// BOTH HALVES ARE THE TEST. Asserting only that the /AF array survived would
// pass an implementation that carried four attachments across the save and
// dropped every /AFRelationship — the document would still hold the bytes and
// would have lost the only thing that says a ZUGFeRD/Factur-X invoice XML is
// the invoice rather than a stray attachment. So the oracle compares
// name -> relationship MAPS, and a dropped relationship shows up as a changed
// VALUE (the "<no /AFRelationship>" sentinel), not as a shorter map.
//
// The expectation is DERIVED FROM THE SOURCE FILE at run time, not written
// here as a literal, so the fixture and the oracle cannot drift apart. What IS
// written here — and what makes the comparison non-vacuous — are the
// preconditions: the source must really carry three catalog entries, one
// page-level entry, and FOUR DISTINCT relationship values. Without those, an
// empty-vs-empty comparison would pass.
TEST_F(Pdf20FeaturesTest, AssociatedFilesSurviveRoundtrip)
{
    const fs::path fixture =
        fs::path(TEST_FIXTURES_DIR) / "pdf20_associated_files.pdf";
    ASSERT_TRUE(fs::exists(fixture))
        << "the PDF 2.0 associated-files fixture is missing; regenerate with "
           "python tests/fixtures/gen_pdf20_associated_files.py -- "
        << fixture.string();

    fz_context* c = ctx_->get();
    const std::string fixtureStr = fixture.string();
    const AfSnapshotRaw before = readAssociatedFiles(c, fixtureStr.c_str());
    ASSERT_TRUE(before.opened) << "the fixture did not open as a PDF at all";

    const auto beforeCatalog = afMap(before.catalog, before.catalogCount);
    const auto beforePage = afMap(before.page0, before.pageCount);

    // ── PRECONDITIONS. These are what stop the EXPECT_EQs below from being a
    // comparison of two empty maps.
    ASSERT_EQ(before.catalogCount, 3)
        << "the fixture no longer carries three catalog-level /AF entries, so "
           "this test would compare two near-empty maps and pass regardless of "
           "what the save did";
    ASSERT_EQ(before.pageCount, 1)
        << "the fixture no longer carries a PAGE-level /AF entry. Page-level "
           "/AF is a SEPARATE carrier from the catalog's and a writer can "
           "preserve one while losing the other";
    ASSERT_EQ(before.embeddedFilesNameCount, 4)
        << "the fixture's /Names /EmbeddedFiles name tree no longer lists all "
           "four filespecs, so the second reachability path is untested";
    {
        std::set<std::string> rels;
        for (const auto& kv : beforeCatalog) rels.insert(kv.second);
        for (const auto& kv : beforePage) rels.insert(kv.second);
        ASSERT_EQ(rels.size(), 4u)
            << "the fixture's four associated files no longer carry four "
               "DISTINCT /AFRelationship values, so an implementation that "
               "flattened every relationship to one name could still pass. "
               "Saw: " << describe(beforeCatalog) << " / "
            << describe(beforePage);
        ASSERT_EQ(rels.count("<no /AFRelationship>"), 0u)
            << "an /AFRelationship is already missing IN THE FIXTURE, so this "
               "test could not detect the save dropping one";
    }

    // ── THE ROUNDTRIP.
    const auto out = td_ / "af_roundtrip.pdf";
    std::error_code rmEc;
    fs::remove(out, rmEc);
    {
        auto doc = Document::open(ctx_, fixture);
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(doc->pageCount(), 1);
        ASSERT_NO_THROW(doc->saveAs(out));
    }
    ASSERT_TRUE(fs::exists(out)) << "saveAs produced no file";

    const std::string outStr = out.string();
    const AfSnapshotRaw after = readAssociatedFiles(c, outStr.c_str());
    ASSERT_TRUE(after.opened) << "the saved document does not reopen as a PDF";

    const auto afterCatalog = afMap(after.catalog, after.catalogCount);
    const auto afterPage = afMap(after.page0, after.pageCount);

    EXPECT_EQ(afterCatalog, beforeCatalog)
        << "the catalog /AF entries did not survive save/reopen intact.\n"
           "  before: " << describe(beforeCatalog) << "\n"
           "  after : " << describe(afterCatalog) << "\n"
           "A '<no /AFRelationship>' on the right-hand side means the "
           "attachment survived and its RELATIONSHIP was dropped -- the file "
           "is still there and no longer says what it is to the document.";
    EXPECT_EQ(afterPage, beforePage)
        << "the PAGE-level /AF entry did not survive save/reopen intact.\n"
           "  before: " << describe(beforePage) << "\n"
           "  after : " << describe(afterPage);

    // The relationship half again, stated on its own so a failure names it
    // directly rather than being read out of a map diff.
    for (int i = 0; i < after.catalogCount; ++i) {
        EXPECT_TRUE(after.catalog[i].relPresent)
            << "catalog associated file '" << after.catalog[i].name
            << "' came back from the save with no /AFRelationship key at all";
    }
    for (int i = 0; i < after.pageCount; ++i) {
        EXPECT_TRUE(after.page0[i].relPresent)
            << "page-level associated file '" << after.page0[i].name
            << "' came back from the save with no /AFRelationship key at all";
    }

    // The name tree is the second reachability path; a writer that rebuilt
    // /AF and forgot /Names would leave the document inconsistent.
    EXPECT_EQ(after.embeddedFilesNameCount, before.embeddedFilesNameCount)
        << "the /Names /EmbeddedFiles name tree lost entries across the save "
           "(before=" << before.embeddedFilesNameCount
        << ", after=" << after.embeddedFilesNameCount << ")";
}
