// -*- coding: utf-8 -*-
// Meta-audit follow-up F22 (gap PDF-38): can a save-path garbage-collection
// pass silently drop /DSS long-term-validation material?
//
// The /DSS dictionary (ISO 32000-2 §12.8.4.3) hangs off /Catalog and holds
// indirect refs to /Certs, /OCSPs and /CRLs streams. Those streams are
// referenced from NOTHING else in the document — not a page, not a resource
// dict, not an annotation. If MuPDF's object-graph sweep (pdf_write_options::
// do_garbage) failed to walk into /Catalog/DSS, every full-rewrite save would
// quietly destroy the material that keeps a signature verifiable after its
// signing certificate expires. That failure is invisible for years.
//
// This file MEASURES the answer rather than assuming it, at every garbage
// level the product can select and across the whole MuPDF 0..3 range:
//
//   level 0 : incremental save (Document::saveIncremental, LtvDssWriter) and
//             the autosave recovery copy — no sweep at all.
//   level 1 : kDefaultFullRewriteGc — Document::save()/saveAs() when the
//             session contains no dedup-worthy op (effectiveGarbageLevel()).
//   level 2 : not currently selectable by the product; swept anyway so a
//             future change of kDefaultFullRewriteGc cannot land unmeasured.
//   level 3 : Document::effectiveGarbageLevel() after markNeedsFullGc() —
//             redaction, page delete/reorder/import, flatten, font embed,
//             encryption. This is the level that also DE-DUPLICATES identical
//             objects, so it renumbers: every assertion below is on stream
//             CONTENT, never on object numbers or xref positions.
//
// CONTROL (the assertion that makes this suite non-vacuous): the fixture also
// carries a deliberately unreferenced "orphan" object. A sweep that actually
// runs MUST collect it. So each level asserts BOTH halves:
//     DSS material survives  AND  the orphan is collected iff the level sweeps.
// A regression that "fixed" a DSS loss by turning GC off, or a fixture that
// silently stopped carrying the orphan, turns this suite red rather than
// green. See docs/audit/meta-audit-2026-07/followup/phase-22-dispositions.md.

#include <gtest/gtest.h>

#include "signatures/LtvDssWriter.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <QRectF>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {

// Key + value of the control orphan. Both are distinctive enough that a
// grep over a saved file, or the object scan below, can find them with no
// chance of a false positive against real PDF content.
constexpr const char* kOrphanKey = "PdfeF22GcControlOrphan";
constexpr const char* kOrphanMarker = "PDFE-F22-GC-CONTROL-ORPHAN-DO-NOT-REFERENCE";

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempWorkDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_ltv_dss_gc";
    std::filesystem::create_directories(d);
    return d;
}

// Everything the suite needs to know about /Catalog/DSS in a saved file,
// captured as stream CONTENT so a de-duplicating/renumbering GC pass cannot
// make the comparison spuriously pass or fail.
struct DssPayload {
    bool hasDss = false;
    std::vector<std::vector<uint8_t>> certs;
    std::vector<std::vector<uint8_t>> ocsps;
    std::vector<std::vector<uint8_t>> crls;
    bool hasVri = false;
    std::vector<std::vector<uint8_t>> vriCerts;
    std::vector<std::vector<uint8_t>> vriOcsps;
    std::vector<std::vector<uint8_t>> vriCrls;
};

// Load every stream referenced from @p parent /@p key into @p out. Must run
// inside MuPdfContext::execute (fz_try scope).
void collectArrayStreams(fz_context* c, pdf_obj* parent, const char* key,
                         std::vector<std::vector<uint8_t>>* out)
{
    pdf_obj* arr = pdf_dict_gets(c, parent, key);
    if (!arr || !pdf_is_array(c, arr)) return;
    const int n = pdf_array_len(c, arr);
    for (int i = 0; i < n; ++i) {
        pdf_obj* ref = pdf_array_get(c, arr, i);
        fz_buffer* buf = pdf_load_stream(c, ref);
        if (!buf) continue;
        unsigned char* data = nullptr;
        const size_t len = fz_buffer_storage(c, buf, &data);
        if (data && len > 0) {
            out->emplace_back(data, data + len);
        }
        fz_drop_buffer(c, buf);
    }
}

// Open @p path independently (raw MuPDF, no Document bookkeeping) and read
// back the whole /DSS payload including the /VRI entry for @p vriKey.
DssPayload readDssPayload(const std::shared_ptr<MuPdfContext>& ctx,
                          const std::filesystem::path& path,
                          const std::string& vriKey)
{
    DssPayload out;
    const std::string pathStr = path.string();
    ctx->execute([&](fz_context* c) {
        pdf_document* d = pdf_open_document(c, pathStr.c_str());
        if (!d) return;
        pdf_obj* root = pdf_dict_get(c, pdf_trailer(c, d), PDF_NAME(Root));
        pdf_obj* dss = root ? pdf_dict_gets(c, root, "DSS") : nullptr;
        if (dss) {
            out.hasDss = true;
            collectArrayStreams(c, dss, "Certs", &out.certs);
            collectArrayStreams(c, dss, "OCSPs", &out.ocsps);
            collectArrayStreams(c, dss, "CRLs", &out.crls);

            pdf_obj* vri = pdf_dict_gets(c, dss, "VRI");
            pdf_obj* entry = vri ? pdf_dict_gets(c, vri, vriKey.c_str())
                                 : nullptr;
            if (entry) {
                out.hasVri = true;
                collectArrayStreams(c, entry, "Cert", &out.vriCerts);
                collectArrayStreams(c, entry, "OCSP", &out.vriOcsps);
                collectArrayStreams(c, entry, "CRL", &out.vriCrls);
            }
        }
        pdf_drop_document(c, d);
    });
    return out;
}

// Scan every object in @p path for the control orphan's marker key. Uses the
// xref rather than a byte grep so an object-stream-compressed or Flate'd
// layout is still found — the test must not pass merely because the marker
// bytes moved out of plain sight.
bool controlOrphanPresent(const std::shared_ptr<MuPdfContext>& ctx,
                          const std::filesystem::path& path)
{
    bool found = false;
    const std::string pathStr = path.string();
    ctx->execute([&](fz_context* c) {
        pdf_document* d = pdf_open_document(c, pathStr.c_str());
        if (!d) return;
        const int n = pdf_xref_len(c, d);
        for (int i = 1; i < n && !found; ++i) {
            pdf_obj* o = nullptr;
            fz_try(c) {
                o = pdf_load_object(c, d, i);
            }
            fz_catch(c) {
                o = nullptr;   // free / broken entry — not our orphan
            }
            if (!o) continue;
            pdf_obj* v = pdf_dict_gets(c, o, kOrphanKey);
            if (v && pdf_is_string(c, v)) {
                const char* s = pdf_to_text_string(c, v);
                if (s && std::string(s) == kOrphanMarker) found = true;
            }
            pdf_drop_obj(c, o);
        }
        pdf_drop_document(c, d);
    });
    return found;
}

// Append an unreferenced object carrying the control marker to @p path via an
// incremental save, so the fixture's existing bytes (and its signature's
// /ByteRange) are untouched.
void injectControlOrphan(const std::shared_ptr<MuPdfContext>& ctx,
                         const std::filesystem::path& path)
{
    const std::string pathStr = path.string();
    ctx->execute([&](fz_context* c) {
        pdf_document* d = pdf_open_document(c, pathStr.c_str());
        if (!d) return;
        pdf_obj* dict = pdf_new_dict(c, d, 2);
        pdf_dict_puts_drop(c, dict, kOrphanKey,
                           pdf_new_text_string(c, kOrphanMarker));
        // pdf_add_object hands back an indirect ref; NOTHING links to it, which
        // is exactly the point — it is reachable only from the xref.
        pdf_obj* ref = pdf_add_object(c, d, dict);
        pdf_drop_obj(c, ref);
        pdf_drop_obj(c, dict);

        pdf_drop_page_tree(c, d);
        pdf_write_options opts{};
        opts.do_incremental = 1;
        opts.do_preserve_metadata = 1;
        pdf_save_document(c, d, pathStr.c_str(), &opts);
        pdf_drop_document(c, d);
    });
}

// Full-rewrite @p src to @p dst at an explicit garbage level, using exactly
// the option set Document::saveAs() uses (Document.cpp ~line 1873) so the
// sweep under test is the product's sweep, not a stripped-down variant.
void rawFullRewrite(const std::shared_ptr<MuPdfContext>& ctx,
                    const std::filesystem::path& src,
                    const std::filesystem::path& dst,
                    int gcLevel)
{
    const std::string srcStr = src.string();
    const std::string dstStr = dst.string();
    std::filesystem::remove(dst);
    ctx->execute([&](fz_context* c) {
        pdf_document* d = pdf_open_document(c, srcStr.c_str());
        if (!d) return;
        pdf_drop_page_tree(c, d);
        pdf_write_options opts{};
        opts.do_garbage = gcLevel;
        opts.do_compress = 1;
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        opts.do_preserve_metadata = 1;
        pdf_save_document(c, d, dstStr.c_str(), &opts);
        pdf_drop_document(c, d);
    });
}

// Full-rewrite @p src to @p dst at @p gcLevel with /Catalog/DSS UNLINKED first.
// This is the postulated PDF-38 defect, synthesised: with nothing referencing
// them, the cert/OCSP/CRL streams become unreachable and the sweep eats them.
// Used only to prove the survival oracle can actually see that happen.
void rawFullRewriteWithDssUnlinked(const std::shared_ptr<MuPdfContext>& ctx,
                                   const std::filesystem::path& src,
                                   const std::filesystem::path& dst,
                                   int gcLevel)
{
    const std::string srcStr = src.string();
    const std::string dstStr = dst.string();
    std::filesystem::remove(dst);
    ctx->execute([&](fz_context* c) {
        pdf_document* d = pdf_open_document(c, srcStr.c_str());
        if (!d) return;
        pdf_obj* root = pdf_dict_get(c, pdf_trailer(c, d), PDF_NAME(Root));
        if (root) pdf_dict_dels(c, root, "DSS");
        pdf_drop_page_tree(c, d);
        pdf_write_options opts{};
        opts.do_garbage = gcLevel;
        opts.do_compress = 1;
        opts.do_preserve_metadata = 1;
        pdf_save_document(c, d, dstStr.c_str(), &opts);
        pdf_drop_document(c, d);
    });
}

// How many of @p tracers still have at least one byte-identical stream
// somewhere in @p path. Independent of /DSS entirely — it answers "are these
// bytes still in the file at all", which is the question that survives
// renumbering. Distinct-tracer counting (rather than total copies) keeps the
// answer stable whether or not gc=3 chooses to merge the duplicate copies
// LtvDssWriter writes into /DSS/Certs and /DSS/VRI/<key>/Cert.
int distinctTracersInFile(const std::shared_ptr<MuPdfContext>& ctx,
                          const std::filesystem::path& path,
                          const std::vector<std::vector<uint8_t>>& tracers)
{
    std::vector<char> seen(tracers.size(), 0);
    const std::string pathStr = path.string();
    ctx->execute([&](fz_context* c) {
        pdf_document* d = pdf_open_document(c, pathStr.c_str());
        if (!d) return;
        const int n = pdf_xref_len(c, d);
        for (int i = 1; i < n; ++i) {
            fz_buffer* buf = nullptr;
            fz_try(c) {
                if (pdf_obj_num_is_stream(c, d, i))
                    buf = pdf_load_stream_number(c, d, i);
            }
            fz_catch(c) {
                buf = nullptr;
            }
            if (!buf) continue;
            unsigned char* data = nullptr;
            const size_t len = fz_buffer_storage(c, buf, &data);
            for (size_t t = 0; t < tracers.size(); ++t) {
                if (tracers[t].size() == len && data &&
                    std::equal(tracers[t].begin(), tracers[t].end(), data)) {
                    seen[t] = 1;
                    break;
                }
            }
            fz_drop_buffer(c, buf);
        }
        pdf_drop_document(c, d);
    });
    return static_cast<int>(std::count(seen.begin(), seen.end(), '\1'));
}

// Order-insensitive byte-exact comparison. gc=3 de-duplicates identical
// streams and renumbers everything, so array ORDER is not a property the
// product guarantees — content membership is.
bool sameBlobSet(std::vector<std::vector<uint8_t>> a,
                 std::vector<std::vector<uint8_t>> b)
{
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

// Distinctive synthetic revocation blobs. LtvDssWriter::write() embeds the
// bytes verbatim (structural parsing happens in writeWithFiles(), which is
// not the path under test), so these are ideal survival tracers: unique,
// incompressible-looking and trivially identifiable byte-for-byte.
std::vector<uint8_t> tracerBlob(uint8_t tag, size_t len)
{
    std::vector<uint8_t> b(len);
    for (size_t i = 0; i < len; ++i) {
        b[i] = static_cast<uint8_t>((i * 31u + tag * 7u + 0xA5u) & 0xFFu);
    }
    // Front-load a fixed signature so a partially-truncated survivor is not
    // mistaken for an intact one.
    b[0] = 0x30; b[1] = 0x82; b[2] = tag; b[3] = 0x5A;
    return b;
}

// Sign tests/fixtures/test_single.pdf with tests/fixtures/test_signer.pfx into
// @p outPath. Mirrors LtvDssWriterTest::buildSignedFixture (which is known
// good) — the F22 question is about GC, but it must be asked of a genuinely
// signed document, since that is the only state in which losing /DSS matters.
bool buildSignedFixture(const std::shared_ptr<MuPdfContext>& ctx,
                        const std::filesystem::path& outPath)
{
    auto fixtures = std::filesystem::path(fixturesDir());
    auto pfx = fixtures / "test_signer.pfx";
    auto srcPdf = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(srcPdf))
        return false;

    SignatureManager mgr(ctx);
    {
        auto doc = Document::open(ctx, srcPdf);
        if (!doc) return false;
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "LtvGcSig",
                                 QRectF(100, 500, 200, 50));
        std::filesystem::remove(outPath);
        doc->saveAs(outPath);
    }
    {
        auto doc = Document::open(ctx, outPath);
        if (!doc) return false;
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfx.string();
        opts.pfxPassword = "test123";
        opts.reason = "LTV GC survival test";
        opts.location = "PDFE test suite";
        mgr.signField(doc.get(), page.get(), 0, opts);
    }
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------

class LtvDssGcSurvivalTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        // One context for the whole suite: re-opening a freshly-signed file in
        // a different fz_context trips MuPDF's xref cache ("object out of
        // range"). Same constraint LtvDssWriterTest works under.
        ctx_ = std::make_shared<MuPdfContext>();
        ltvFixture_ = tempWorkDir() / "ltv_gc_source.pdf";
        try {
            if (!buildSignedFixture(ctx_, ltvFixture_)) return;

            // Keep a signed-but-LTV-less copy so one test can write /DSS and
            // full-rewrite within a SINGLE session, exercising the in-memory
            // object graph rather than a reloaded one.
            signedOnlyFixture_ = tempWorkDir() / "ltv_gc_signed_only.pdf";
            std::filesystem::remove(signedOnlyFixture_);
            std::filesystem::copy_file(ltvFixture_, signedOnlyFixture_);

            // Extract the real signature inputs, then embed LTV material:
            // the genuine cert chain from the PKCS#7 container plus tracer
            // OCSP/CRL blobs whose bytes we can compare exactly afterwards.
            auto doc = Document::open(ctx_, ltvFixture_);
            if (!doc) return;
            SignatureInputs inputs;
            if (!LtvDssWriter::extractFirstSignatureInputs(doc.get(), inputs))
                return;

            certs_ = inputs.certChain;
            ocsps_ = { tracerBlob(0x11, 512), tracerBlob(0x12, 700) };
            crls_  = { tracerBlob(0x21, 640), tracerBlob(0x22, 333) };
            vriKey_ = LtvDssWriter::sha1HexUpper(inputs.contentsHex);

            LtvDssWriter writer;
            if (!writer.write(doc.get(), inputs.contentsHex,
                              certs_, ocsps_, crls_)) {
                return;
            }
            doc.reset();

            // Add the control orphan LAST so it is a plain unreferenced object
            // in the final revision, not something the DSS write could link.
            injectControlOrphan(ctx_, ltvFixture_);
            ready_ = true;
        } catch (const std::exception&) {
            ready_ = false;
        }
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        std::filesystem::remove_all(tempWorkDir(), ec);
        ctx_.reset();
    }

    void SetUp() override
    {
        // Conditional only: fires if the on-the-fly signing / PKCS#7 bridge is
        // unavailable, never in a working build. Every assertion below runs in
        // CI. (Not a scope reduction — the same guard LtvDssWriterTest uses.)
        if (!ready_) {
            GTEST_SKIP() << "Could not build the signed+LTV fixture; check "
                            "OpenSSL/Pkcs7 bridge and test_signer.pfx";
        }
    }

    std::filesystem::path workCopy(const char* name) const
    {
        auto dst = tempWorkDir() / name;
        std::filesystem::remove(dst);
        std::filesystem::copy_file(ltvFixture_, dst);
        return dst;
    }

    // Assert that every byte of LTV material written into the fixture is still
    // present in @p path, top-level arrays and /VRI entry alike.
    void expectDssIntact(const std::filesystem::path& path,
                         const char* what) const
    {
        DssPayload p = readDssPayload(ctx_, path, vriKey_);
        EXPECT_TRUE(p.hasDss) << what << ": /Catalog/DSS is gone";
        EXPECT_TRUE(sameBlobSet(p.certs, certs_))
            << what << ": /DSS/Certs content changed ("
            << p.certs.size() << " vs " << certs_.size() << " entries)";
        EXPECT_TRUE(sameBlobSet(p.ocsps, ocsps_))
            << what << ": /DSS/OCSPs content changed ("
            << p.ocsps.size() << " vs " << ocsps_.size() << " entries)";
        EXPECT_TRUE(sameBlobSet(p.crls, crls_))
            << what << ": /DSS/CRLs content changed ("
            << p.crls.size() << " vs " << crls_.size() << " entries)";
        EXPECT_TRUE(p.hasVri) << what << ": /DSS/VRI/" << vriKey_ << " is gone";
        EXPECT_TRUE(sameBlobSet(p.vriCerts, certs_))
            << what << ": /VRI/Cert content changed";
        EXPECT_TRUE(sameBlobSet(p.vriOcsps, ocsps_))
            << what << ": /VRI/OCSP content changed";
        EXPECT_TRUE(sameBlobSet(p.vriCrls, crls_))
            << what << ": /VRI/CRL content changed";
    }

    static std::shared_ptr<MuPdfContext> ctx_;
    static std::filesystem::path ltvFixture_;
    static std::filesystem::path signedOnlyFixture_;
    static std::vector<std::vector<uint8_t>> certs_;
    static std::vector<std::vector<uint8_t>> ocsps_;
    static std::vector<std::vector<uint8_t>> crls_;
    static std::string vriKey_;
    static bool ready_;
};

std::shared_ptr<MuPdfContext> LtvDssGcSurvivalTest::ctx_;
std::filesystem::path LtvDssGcSurvivalTest::ltvFixture_;
std::filesystem::path LtvDssGcSurvivalTest::signedOnlyFixture_;
std::vector<std::vector<uint8_t>> LtvDssGcSurvivalTest::certs_;
std::vector<std::vector<uint8_t>> LtvDssGcSurvivalTest::ocsps_;
std::vector<std::vector<uint8_t>> LtvDssGcSurvivalTest::crls_;
std::string LtvDssGcSurvivalTest::vriKey_;
bool LtvDssGcSurvivalTest::ready_ = false;

// --- fixture sanity: without these two, every level assertion below is
// --- vacuous, so they are asserted explicitly rather than assumed.

TEST_F(LtvDssGcSurvivalTest, FixtureCarriesLtvMaterialAndControlOrphan)
{
    ASSERT_FALSE(certs_.empty()) << "cert chain extraction produced nothing";
    ASSERT_EQ(ocsps_.size(), 2u);
    ASSERT_EQ(crls_.size(), 2u);
    expectDssIntact(ltvFixture_, "fixture");
    EXPECT_TRUE(controlOrphanPresent(ctx_, ltvFixture_))
        << "the control orphan is not in the fixture — every 'GC collected "
           "the orphan' assertion in this file would then be unfalsifiable";
}

// --- the product's own selectable levels ------------------------------------

// Pins WHICH levels the product can reach, so the sweep below is provably
// exhaustive rather than exhaustive-by-assertion-of-the-author. If
// kDefaultFullRewriteGc ever moves, this fails and forces the matrix to be
// re-derived.
TEST_F(LtvDssGcSurvivalTest, ProductSelectableGarbageLevelsAreOneAndThree)
{
    auto work = workCopy("gc_levels_probe.pdf");
    auto doc = Document::open(ctx_, work);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->effectiveGarbageLevel(), 1)
        << "default full-rewrite GC level changed; re-derive the F22 matrix";
    doc->markNeedsFullGc();
    EXPECT_EQ(doc->effectiveGarbageLevel(), 3)
        << "dedup-worthy full-rewrite GC level changed; re-derive the matrix";
}

// The gap register's own proposed test name for PDF-38, verbatim, so the row's
// evidence column cites a test that exists. Asserts the headline property once
// over BOTH product-selectable full-rewrite levels.
TEST_F(LtvDssGcSurvivalTest, DssSurvivesFullRewriteSaveWithGc)
{
    for (bool dedup : { false, true }) {
        const char* label = dedup ? "gc=3" : "gc=1";
        SCOPED_TRACE(label);
        auto work = workCopy(dedup ? "gc_named_in_3.pdf" : "gc_named_in_1.pdf");
        auto out = tempWorkDir() /
                   (dedup ? "gc_named_out_3.pdf" : "gc_named_out_1.pdf");
        std::filesystem::remove(out);
        {
            auto doc = Document::open(ctx_, work);
            ASSERT_NE(doc, nullptr);
            if (dedup) doc->markNeedsFullGc();
            ASSERT_EQ(doc->effectiveGarbageLevel(), dedup ? 3 : 1);
            // GitHub #19 / PDF-50: this fixture IS signed, and a full rewrite
            // destroys the signature's /ByteRange — which the new
            // signed-rewrite guard now refuses by default. The SUBJECT of this
            // test is what garbage collection does to the /DSS, not what the
            // guard does to the save, so the loss is explicitly accepted here.
            // Opting in is the honest way to keep measuring the GC behaviour;
            // removing the guard, or asserting a refusal instead, would stop
            // measuring it.
            doc->setSignedRewritePolicy(
                Document::SignedRewritePolicy::AcceptLoss);
            doc->saveAs(out);
        }
        expectDssIntact(out, label);
        EXPECT_FALSE(controlOrphanPresent(ctx_, out))
            << label << ": the sweep did not run, so survival proves nothing";
    }
}

// Document::saveAs() with no dedup-worthy op → do_garbage = 1.
TEST_F(LtvDssGcSurvivalTest, ProductSaveAsDefaultGc_DssSurvives_OrphanCollected)
{
    auto work = workCopy("gc_product_default_in.pdf");
    auto out = tempWorkDir() / "gc_product_default_out.pdf";
    std::filesystem::remove(out);
    {
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(doc->effectiveGarbageLevel(), 1);
        doc->saveAs(out);
    }
    expectDssIntact(out, "Document::saveAs (gc=1)");
    EXPECT_FALSE(controlOrphanPresent(ctx_, out))
        << "gc=1 did not collect an unreferenced object — the sweep is not "
           "running, so DSS survival at this level proves nothing";
}

// Document::saveAs() after a dedup-worthy op → do_garbage = 3 (renumbers and
// merges identical objects; the /Certs and /VRI/Cert streams ARE identical, so
// this level genuinely exercises the dedup path over LTV material).
TEST_F(LtvDssGcSurvivalTest, ProductSaveAsFullGc_DssSurvives_OrphanCollected)
{
    auto work = workCopy("gc_product_full_in.pdf");
    auto out = tempWorkDir() / "gc_product_full_out.pdf";
    std::filesystem::remove(out);
    {
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        doc->markNeedsFullGc();
        ASSERT_EQ(doc->effectiveGarbageLevel(), 3);
        // GitHub #19 / PDF-50 — see DssSurvivesFullRewriteSaveWithGc above for
        // why the signed-rewrite loss is accepted rather than avoided.
        doc->setSignedRewritePolicy(Document::SignedRewritePolicy::AcceptLoss);
        doc->saveAs(out);
    }
    expectDssIntact(out, "Document::saveAs (gc=3, dedup)");
    EXPECT_FALSE(controlOrphanPresent(ctx_, out))
        << "gc=3 did not collect an unreferenced object";
}

// Document::save() to the same path after markRequiresFullRewrite() — the
// File>Save route a user actually takes after editing an LTV document.
TEST_F(LtvDssGcSurvivalTest, ProductSaveInPlaceFullRewrite_DssSurvives_OrphanCollected)
{
    auto work = workCopy("gc_product_inplace.pdf");
    {
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        doc->markRequiresFullRewrite();
        ASSERT_EQ(doc->effectiveGarbageLevel(), 1);
        doc->save(work);
    }
    expectDssIntact(work, "Document::save in place (full rewrite, gc=1)");
    EXPECT_FALSE(controlOrphanPresent(ctx_, work))
        << "in-place full rewrite did not collect an unreferenced object";
}

// Document::saveIncremental() — do_garbage = 0 by construction. Both the DSS
// AND the orphan must survive: an incremental append that dropped either would
// mean it was not appending.
TEST_F(LtvDssGcSurvivalTest, ProductSaveIncremental_DssAndOrphanBothSurvive)
{
    auto work = workCopy("gc_product_incremental.pdf");
    {
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        doc->saveIncremental();
    }
    expectDssIntact(work, "Document::saveIncremental (gc=0)");
    EXPECT_TRUE(controlOrphanPresent(ctx_, work))
        << "incremental save must not sweep anything";
}

// Write /DSS and then full-rewrite at the dedup level WITHOUT closing and
// reopening in between. The streams are built by pdf_add_stream and only
// afterwards linked into the /DSS arrays, so this is the ordering under which
// a "created but not yet reachable when the sweep runs" defect would show up —
// the reloaded-document tests above could never see it.
TEST_F(LtvDssGcSurvivalTest, SameSessionDssWriteThenFullGcRewrite_DssSurvives)
{
    auto work = tempWorkDir() / "gc_same_session_in.pdf";
    auto out = tempWorkDir() / "gc_same_session_out.pdf";
    std::filesystem::remove(work);
    std::filesystem::remove(out);
    std::filesystem::copy_file(signedOnlyFixture_, work);

    std::string vriKey;
    {
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        SignatureInputs inputs;
        ASSERT_TRUE(
            LtvDssWriter::extractFirstSignatureInputs(doc.get(), inputs));
        vriKey = LtvDssWriter::sha1HexUpper(inputs.contentsHex);

        LtvDssWriter writer;
        ASSERT_TRUE(writer.write(doc.get(), inputs.contentsHex,
                                 inputs.certChain, ocsps_, crls_));
        // Same live Document, no reopen: a dedup-worthy edit now forces the
        // full-rewrite + gc=3 path over the just-built object graph.
        doc->markNeedsFullGc();
        ASSERT_EQ(doc->effectiveGarbageLevel(), 3);
        // GitHub #19 / PDF-50 — see DssSurvivesFullRewriteSaveWithGc above.
        doc->setSignedRewritePolicy(Document::SignedRewritePolicy::AcceptLoss);
        doc->saveAs(out);
    }

    DssPayload p = readDssPayload(ctx_, out, vriKey);
    EXPECT_TRUE(p.hasDss) << "/DSS lost by a same-session gc=3 rewrite";
    EXPECT_TRUE(p.hasVri) << "/VRI lost by a same-session gc=3 rewrite";
    EXPECT_TRUE(sameBlobSet(p.ocsps, ocsps_));
    EXPECT_TRUE(sameBlobSet(p.crls, crls_));
    EXPECT_TRUE(sameBlobSet(p.vriOcsps, ocsps_));
    EXPECT_TRUE(sameBlobSet(p.vriCrls, crls_));
    EXPECT_FALSE(p.certs.empty());
    EXPECT_TRUE(sameBlobSet(p.certs, p.vriCerts));
}

// --- the full MuPDF 0..3 range ---------------------------------------------
// Same option set as Document::saveAs(), only do_garbage varies, so the levels
// the product does not currently select are still measured.

TEST_F(LtvDssGcSurvivalTest, RawGcLevel0_DssAndOrphanBothSurvive)
{
    auto out = tempWorkDir() / "gc_raw_0.pdf";
    rawFullRewrite(ctx_, ltvFixture_, out, 0);
    expectDssIntact(out, "raw full rewrite gc=0");
    EXPECT_TRUE(controlOrphanPresent(ctx_, out))
        << "gc=0 must not sweep — if the orphan is gone here the control is "
           "measuring something other than the garbage collector";
}

TEST_F(LtvDssGcSurvivalTest, RawGcLevel1_DssSurvives_OrphanCollected)
{
    auto out = tempWorkDir() / "gc_raw_1.pdf";
    rawFullRewrite(ctx_, ltvFixture_, out, 1);
    expectDssIntact(out, "raw full rewrite gc=1");
    EXPECT_FALSE(controlOrphanPresent(ctx_, out));
}

TEST_F(LtvDssGcSurvivalTest, RawGcLevel2_DssSurvives_OrphanCollected)
{
    auto out = tempWorkDir() / "gc_raw_2.pdf";
    rawFullRewrite(ctx_, ltvFixture_, out, 2);
    expectDssIntact(out, "raw full rewrite gc=2");
    EXPECT_FALSE(controlOrphanPresent(ctx_, out));
}

TEST_F(LtvDssGcSurvivalTest, RawGcLevel3_DssSurvives_OrphanCollected)
{
    auto out = tempWorkDir() / "gc_raw_3.pdf";
    rawFullRewrite(ctx_, ltvFixture_, out, 3);
    expectDssIntact(out, "raw full rewrite gc=3");
    EXPECT_FALSE(controlOrphanPresent(ctx_, out));
}

// --- oracle proof ----------------------------------------------------------
// Every "DSS survived" assertion above is only worth what the oracle can
// detect. Synthesise the exact failure PDF-38 postulates — /DSS unreferenced
// when the sweep runs — and assert the oracle turns red on it, and that the
// LTV bytes really are gone from the file rather than merely re-homed.
TEST_F(LtvDssGcSurvivalTest, OracleDetectsDssLossWhenTheSweepReallyDropsIt)
{
    auto intact = tempWorkDir() / "gc_oracle_intact.pdf";
    auto lost = tempWorkDir() / "gc_oracle_lost.pdf";
    rawFullRewrite(ctx_, ltvFixture_, intact, 3);
    rawFullRewriteWithDssUnlinked(ctx_, ltvFixture_, lost, 3);

    std::vector<std::vector<uint8_t>> tracers;
    tracers.insert(tracers.end(), ocsps_.begin(), ocsps_.end());
    tracers.insert(tracers.end(), crls_.begin(), crls_.end());

    // Same GC level, same fixture: the only difference is reachability.
    EXPECT_EQ(distinctTracersInFile(ctx_, intact, tracers),
              static_cast<int>(tracers.size()))
        << "reachable LTV streams must all survive gc=3";
    EXPECT_EQ(distinctTracersInFile(ctx_, lost, tracers), 0)
        << "unreachable LTV streams must be collected — if they are not, the "
           "control below proves nothing about the sweep";

    DssPayload p = readDssPayload(ctx_, lost, vriKey_);
    EXPECT_FALSE(p.hasDss);
    EXPECT_FALSE(sameBlobSet(p.ocsps, ocsps_))
        << "expectDssIntact() would have passed on a document whose LTV "
           "material was destroyed — the oracle is vacuous";
    EXPECT_FALSE(sameBlobSet(p.crls, crls_));
}

// A GC pass is not a one-shot risk: an LTV document that is opened and saved
// repeatedly (the realistic archive workflow) must not shed material a little
// at a time. Three successive full rewrites at the dedup level.
TEST_F(LtvDssGcSurvivalTest, RepeatedFullGcRewritesDoNotErodeDss)
{
    auto a = tempWorkDir() / "gc_chain_1.pdf";
    auto b = tempWorkDir() / "gc_chain_2.pdf";
    auto c = tempWorkDir() / "gc_chain_3.pdf";
    rawFullRewrite(ctx_, ltvFixture_, a, 3);
    rawFullRewrite(ctx_, a, b, 3);
    rawFullRewrite(ctx_, b, c, 3);
    expectDssIntact(c, "three chained gc=3 rewrites");
    EXPECT_FALSE(controlOrphanPresent(ctx_, c));
}
