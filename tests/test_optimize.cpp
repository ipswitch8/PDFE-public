// -*- coding: utf-8 -*-
// Phase 5b: OptimizeEngine tests.
// Verifies byte reporting, option toggles affect output size, default-options
// run successfully, duplicate-object merging reduces size on duplicated-content
// fixtures, and error paths surface cleanly.
#include <gtest/gtest.h>
#include "optimize/OptimizeEngine.h"
#include "optimize/OptimizeTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "support/PreservationOracle.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

// GitHub #144: QPDF::isLinearized() is the truth oracle for the linearization
// tests at the end of this file.
#include <qpdf/QPDF.hh>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace pdfe::optimize;
using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path tempOut(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "pdfe_opt_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

/// Build a single-page PDF whose content stream is LARGE and HIGHLY REPETITIVE,
/// written with `do_compress = 0` so the source streams are stored raw.
///
/// Why this fixture rather than tests/fixtures/test_3page.pdf: that file is 2395
/// bytes and its content streams are a few dozen bytes each, so flate's
/// per-stream overhead (zlib header + Adler-32 + the /Filter /FlateDecode dict
/// entry, ~30 bytes/stream) is the same order as the savings. The
/// "compressed <= uncompressed" assertion therefore sat on a coin-flip boundary
/// and flipped run to run (observed 2307 vs 2299, and 2307 vs 2306).
///
/// Here the payload is `repeatCount` copies of one identical ~64-byte operator
/// line. That is the best case for LZ77 back-references: after the first line
/// every subsequent one is a single long match, so flate reduces it by three
/// orders of magnitude. The margin between compressed and uncompressed output is
/// then hundreds of kilobytes, not bytes, and cannot be crossed by xref-length
/// or object-numbering jitter.
///
/// Returns false on failure (caller asserts).
bool buildRepetitivePdf(const std::filesystem::path& out, int repeatCount)
{
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return false;
    fz_register_document_handlers(ctx);

    // Every std:: object is constructed BEFORE the fz_try frame (setjmp/longjmp
    // gives no C++ RAII unwinding on this stack).
    std::string content;
    content.reserve(static_cast<size_t>(repeatCount) * 64 + 64);
    for (int i = 0; i < repeatCount; ++i) {
        content += "BT /F1 12 Tf 72 700 Td (REPEATED COMPRESSIBLE PAYLOAD LINE) Tj ET\n";
    }
    const std::string outStr = out.string();

    bool ok = false;
    pdf_document* doc = nullptr;
    fz_try(ctx) {
        doc = pdf_create_document(ctx);

        fz_buffer* buf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(content.data()), content.size());
        pdf_obj* res = pdf_new_dict(ctx, doc, 1);
        pdf_obj* fonts = pdf_new_dict(ctx, doc, 1);
        pdf_obj* f1 = pdf_new_dict(ctx, doc, 3);
        pdf_dict_put(ctx, f1, PDF_NAME(Type), PDF_NAME(Font));
        pdf_dict_put(ctx, f1, PDF_NAME(Subtype), PDF_NAME(Type1));
        pdf_dict_put_drop(ctx, f1, PDF_NAME(BaseFont), pdf_new_name(ctx, "Helvetica"));
        pdf_dict_puts_drop(ctx, fonts, "F1", f1);
        pdf_dict_put_drop(ctx, res, PDF_NAME(Font), fonts);

        pdf_obj* pageObj =
            pdf_add_page(ctx, doc, fz_make_rect(0, 0, 612, 792), 0, res, buf);
        fz_drop_buffer(ctx, buf);
        pdf_drop_obj(ctx, res);
        pdf_insert_page(ctx, doc, -1, pageObj);
        pdf_drop_obj(ctx, pageObj);

        pdf_write_options wo = pdf_default_write_options;
        wo.do_garbage  = 0;
        wo.do_compress = 0;   // store the payload RAW so the later runs differ
        pdf_save_document(ctx, doc, outStr.c_str(), &wo);
        ok = true;
    }
    fz_always(ctx) { if (doc) pdf_drop_document(ctx, doc); }
    fz_catch(ctx) { ok = false; }
    fz_drop_context(ctx);
    return ok;
}

} // namespace

class OptimizeEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx;
};

TEST_F(OptimizeEngineTest, DefaultOptionsSucceedOnSimpleFile)
{
    auto src = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto out = tempOut("default.pdf");
    std::filesystem::remove(out);

    OptimizeEngine eng(ctx);
    OptimizeOptions opts;  // defaults: all-on except sanitize/linearize
    auto r = eng.run(src, out, opts);

    EXPECT_TRUE(r.success()) << r.errorMessage;
    EXPECT_GT(r.sizeBeforeBytes, 0u);
    EXPECT_GT(r.sizeAfterBytes, 0u);
    EXPECT_TRUE(std::filesystem::exists(out));
}

TEST_F(OptimizeEngineTest, OutputOpensAndPageCountPreserved)
{
    auto src = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_3page.pdf not available";
    }
    auto out = tempOut("pagecount.pdf");
    std::filesystem::remove(out);

    OptimizeEngine eng(ctx);
    auto r = eng.run(src, out, {});
    ASSERT_TRUE(r.success()) << r.errorMessage;

    auto srcDoc = Document::open(ctx, src);
    auto outDoc = Document::open(ctx, out);
    ASSERT_NE(srcDoc, nullptr);
    ASSERT_NE(outDoc, nullptr);
    EXPECT_EQ(srcDoc->pageCount(), outDoc->pageCount());
}

TEST_F(OptimizeEngineTest, MissingInputReturnsErrorNotCrash)
{
    auto bogus = fixturesDir() / "does_not_exist.pdf";
    auto out = tempOut("bogus.pdf");
    OptimizeEngine eng(ctx);
    auto r = eng.run(bogus, out, {});
    EXPECT_FALSE(r.success());
    EXPECT_FALSE(r.errorMessage.empty());
}

TEST_F(OptimizeEngineTest, SameInputOutputRejected)
{
    auto src = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    OptimizeEngine eng(ctx);
    auto r = eng.run(src, src, {});
    EXPECT_FALSE(r.success());
}

TEST_F(OptimizeEngineTest, AllOptionsOffStillProducesValidFile)
{
    auto src = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto out = tempOut("no_opts.pdf");
    std::filesystem::remove(out);

    OptimizeOptions opts;
    opts.compressStreams = false;
    opts.recompressImages = false;
    opts.subsetFonts = false;
    opts.removeUnused = false;
    opts.mergeDuplicates = false;

    OptimizeEngine eng(ctx);
    auto r = eng.run(src, out, opts);
    EXPECT_TRUE(r.success()) << r.errorMessage;
    EXPECT_GT(r.sizeAfterBytes, 0u);
}

namespace {

/// The compression oracle's margin, shared by the guarded test
/// (CompressOnSmallerThanAllOptionsOff) and its control
/// (CompressRatioOracleRejectsANonCompressingWriter) — meta-audit follow-up
/// F28. Keeping it in ONE place means the control re-validates whatever factor
/// the guarded test currently uses, rather than a copy that can drift.
constexpr unsigned kMinCompressionFactor = 10u;

/// The two runs the compression oracle compares. Everything except
/// `compressStreams` is off in both, so the only variable is stream compression.
struct CompressPair {
    OptimizeResult uncompressed;
    OptimizeResult compressed;
};

CompressPair runCompressPair(const std::shared_ptr<MuPdfContext>& ctx,
                             const std::filesystem::path& src,
                             const std::filesystem::path& outUncompressed,
                             const std::filesystem::path& outCompressed)
{
    std::filesystem::remove(outUncompressed);
    std::filesystem::remove(outCompressed);

    OptimizeOptions opts;
    opts.compressStreams  = false;
    opts.recompressImages = false;
    opts.subsetFonts      = false;
    opts.removeUnused     = false;
    opts.mergeDuplicates  = false;

    OptimizeOptions optsC = opts;
    optsC.compressStreams = true;

    OptimizeEngine eng(ctx);
    CompressPair p;
    p.uncompressed = eng.run(src, outUncompressed, opts);
    p.compressed   = eng.run(src, outCompressed, optsC);
    return p;
}

} // namespace

TEST_F(OptimizeEngineTest, CompressOnSmallerThanAllOptionsOff)
{
    // This test used to run over tests/fixtures/test_3page.pdf (2395 bytes),
    // where flate's per-stream overhead is the same order as its savings -- the
    // assertion sat on a 1-to-8-byte boundary and flaked. It now runs over a
    // generated document whose single content stream is ~256 KB of one repeated
    // operator line, so "does stream compression actually work" is a question
    // with a three-orders-of-magnitude answer instead of a coin flip.
    auto dir = std::filesystem::temp_directory_path() /
               ("pdfe_opt_compress_" + std::to_string(static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);

    const auto src = dir / "repetitive_src.pdf";
    constexpr int kRepeats = 4000;              // ~256 KB of payload
    ASSERT_TRUE(buildRepetitivePdf(src, kRepeats));
    ASSERT_TRUE(std::filesystem::exists(src));

    const auto p = runCompressPair(ctx, src, dir / "uncompressed.pdf",
                                   dir / "compressed.pdf");
    ASSERT_TRUE(p.uncompressed.success()) << p.uncompressed.errorMessage;
    ASSERT_TRUE(p.compressed.success())   << p.compressed.errorMessage;
    ASSERT_GT(p.uncompressed.sizeAfterBytes, 0u);
    ASSERT_GT(p.compressed.sizeAfterBytes, 0u);

    // Control: the uncompressed run really did keep the payload raw. Without
    // this the ratio assertion could pass because BOTH runs compressed, or
    // because the payload was never written.
    ASSERT_GT(p.uncompressed.sizeAfterBytes,
              static_cast<uint64_t>(kRepeats) * 32u)
        << "uncompressed output is far smaller than the raw payload -- the "
           "fixture or the do_compress=0 path is not doing what this test "
           "assumes, so the ratio below would be vacuous";

    // Margin: the payload is one 66-byte line repeated 4000 times, which flate
    // encodes as one literal line plus long back-references. MEASURED on this
    // fixture: 1535 compressed vs 264537 uncompressed = 0.58%, a 172x ratio, and
    // the 1535 is almost entirely fixed PDF scaffolding rather than payload.
    // Requiring only 10x therefore keeps a ~17x safety factor over the measured
    // value -- far outside anything zlib-tuning or xref-width jitter can move --
    // while still being a claim no non-compressing writer can satisfy (a writer
    // that did nothing lands at exactly 1x; the mutation run confirmed it
    // reports 264537 vs 264537 and fails). This is a real oracle, not a
    // tolerance: it fails the moment stream compression stops working.
    EXPECT_LT(p.compressed.sizeAfterBytes * kMinCompressionFactor,
              p.uncompressed.sizeAfterBytes)
        << "stream compression must make a highly compressible document at "
           "least " << kMinCompressionFactor
        << "x smaller; got compressed=" << p.compressed.sizeAfterBytes
        << " uncompressed=" << p.uncompressed.sizeAfterBytes;

    // F28: emit the measured byte counts into the JUnit XML so the margin is
    // EVIDENCE across runs rather than a number frozen in a comment. Any future
    // reader can diff these attributes between runs instead of re-deriving them.
    RecordProperty("f28_uncompressedBytes",
                   static_cast<int>(p.uncompressed.sizeAfterBytes));
    RecordProperty("f28_compressedBytes",
                   static_cast<int>(p.compressed.sizeAfterBytes));
    RecordProperty("f28_ratioX100",
                   static_cast<int>((p.uncompressed.sizeAfterBytes * 100u) /
                                    p.compressed.sizeAfterBytes));

    // The compressed output must still be a readable one-page PDF -- "smaller"
    // is worthless if it was achieved by dropping content.
    auto outDoc = Document::open(ctx, dir / "compressed.pdf");
    ASSERT_NE(outDoc, nullptr);
    EXPECT_EQ(outDoc->pageCount(), 1);
    outDoc.reset();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(OptimizeEngineTest, CompressRatioOracleRejectsANonCompressingWriter)
{
    // CONTROL for CompressOnSmallerThanAllOptionsOff (meta-audit follow-up
    // F28). The risk with any "fix a flaky size comparison" change is that the
    // assertion gets widened until nothing can fail it. This test proves the
    // guarded assertion is not in that state, in two independent ways:
    //
    //  (a) It applies the SAME predicate, with the SAME shared factor, to a
    //      pair of runs where stream compression is DISABLED on both sides —
    //      the stand-in for "compression silently stopped working" — and
    //      asserts the predicate does NOT hold. So the guarded test really
    //      does go red on that regression rather than passing vacuously.
    //
    //  (b) It measures the ratio a non-compressing writer actually achieves
    //      (~1.00x, since both runs are byte-identical work) and asserts the
    //      oracle's factor stays well clear of it. Widening
    //      kMinCompressionFactor toward 1 to silence a failure therefore fails
    //      HERE, which is the check (a) alone cannot make: at factor 1 the
    //      predicate 1*x < x is still false, so (a) would stay green.
    auto dir = std::filesystem::temp_directory_path() /
               ("pdfe_opt_control_" + std::to_string(static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);

    const auto src = dir / "repetitive_src.pdf";
    constexpr int kRepeats = 4000;   // identical fixture to the guarded test
    ASSERT_TRUE(buildRepetitivePdf(src, kRepeats));
    ASSERT_TRUE(std::filesystem::exists(src));

    OptimizeOptions off;
    off.compressStreams  = false;
    off.recompressImages = false;
    off.subsetFonts      = false;
    off.removeUnused     = false;
    off.mergeDuplicates  = false;

    OptimizeEngine eng(ctx);
    const auto a = eng.run(src, dir / "control_a.pdf", off);
    const auto b = eng.run(src, dir / "control_b.pdf", off);
    ASSERT_TRUE(a.success()) << a.errorMessage;
    ASSERT_TRUE(b.success()) << b.errorMessage;
    ASSERT_GT(a.sizeAfterBytes, 0u);
    ASSERT_GT(b.sizeAfterBytes, 0u);

    // Sanity: both runs really did write the raw payload, so the predicate
    // below is being evaluated on real, comparable outputs.
    ASSERT_GT(a.sizeAfterBytes, static_cast<uint64_t>(kRepeats) * 32u);
    ASSERT_GT(b.sizeAfterBytes, static_cast<uint64_t>(kRepeats) * 32u);

    EXPECT_FALSE(a.sizeAfterBytes * kMinCompressionFactor < b.sizeAfterBytes)
        << "the " << kMinCompressionFactor
        << "x compression oracle is satisfied even with compression DISABLED "
           "on both runs — it no longer discriminates and would pass for a "
           "writer that compresses nothing; a=" << a.sizeAfterBytes
        << " b=" << b.sizeAfterBytes;

    // (b) Anti-widening. Measure what a non-compressing writer scores on this
    // fixture and require the oracle's factor to sit far above it.
    const unsigned disabledRatioX100 =
        static_cast<unsigned>((b.sizeAfterBytes * 100u) / a.sizeAfterBytes);
    EXPECT_LT(disabledRatioX100, 200u)
        << "two identical no-compression runs differ by more than 2x — the "
           "fixture is not deterministic and the margin reasoning below is "
           "unsound; a=" << a.sizeAfterBytes << " b=" << b.sizeAfterBytes;
    EXPECT_GE(kMinCompressionFactor, 4u)
        << "the compression oracle's factor has been widened to "
        << kMinCompressionFactor << "x, which is no longer meaningfully above "
           "the " << (disabledRatioX100 / 100.0)
        << "x a writer that compresses NOTHING achieves on this fixture; the "
           "assertion has become a tolerance instead of an oracle";

    RecordProperty("f28_controlABytes", static_cast<int>(a.sizeAfterBytes));
    RecordProperty("f28_controlBBytes", static_cast<int>(b.sizeAfterBytes));
    RecordProperty("f28_disabledRatioX100", static_cast<int>(disabledRatioX100));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(OptimizeEngineTest, CompressOnTinyFileSucceedsAndStaysBounded)
{
    // Companion to the test above, kept deliberately: the tiny-file case is the
    // one that used to flake, and dropping it would lose coverage of a real
    // behaviour -- on a document with negligible compressible content, enabling
    // compression must still SUCCEED and must not bloat the file. It may
    // legitimately end a few bytes LARGER, because each stream pays a zlib
    // header + Adler-32 + a /Filter /FlateDecode dict entry (~30 bytes) that
    // exceeds the savings on a 50-byte stream. So the assertion here is a hard
    // BOUND on that overhead, not a size ordering: it would still fail if
    // compression corrupted, dropped, or meaningfully inflated the document.
    auto src = fixturesDir() / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();

    auto dir = std::filesystem::temp_directory_path() /
               ("pdfe_opt_tiny_" + std::to_string(static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);
    const auto outC = dir / "tiny_compressed.pdf";

    const auto p = runCompressPair(ctx, src, dir / "tiny_uncompressed.pdf", outC);
    ASSERT_TRUE(p.uncompressed.success()) << p.uncompressed.errorMessage;
    ASSERT_TRUE(p.compressed.success())   << p.compressed.errorMessage;
    ASSERT_GT(p.compressed.sizeAfterBytes, 0u);

    // Bound: 3 pages => 3 content streams, each paying at most ~30 bytes of
    // flate + dict overhead; 64 bytes per stream also absorbs xref-offset width
    // changes. Anything beyond this is inflation, not per-stream overhead.
    auto srcDoc = Document::open(ctx, src);
    ASSERT_NE(srcDoc, nullptr);
    const auto pages = static_cast<uint64_t>(srcDoc->pageCount());
    srcDoc.reset();
    ASSERT_GT(pages, 0u);

    EXPECT_LE(p.compressed.sizeAfterBytes,
              p.uncompressed.sizeAfterBytes + 64u * pages)
        << "compressing a negligible-content file inflated it beyond per-stream "
           "flate overhead; compressed=" << p.compressed.sizeAfterBytes
        << " uncompressed=" << p.uncompressed.sizeAfterBytes;

    // F34 REGRESSION CONTROL for the fixture replacement made in F13 (849d3af).
    // The backlog row that motivated that change cited an observed 2307-vs-2299
    // inversion on THIS fixture -- an 8-byte margin, which is why the old
    // `EXPECT_LE(compressed, uncompressed)` flipped run to run. Emit the actual
    // numbers so a reviewer can see the boundary condition rather than take the
    // 8 bytes on trust, and so it stays observable if MuPDF's writer changes.
    //
    // `f34_oldDeltaBytes` is compressed MINUS uncompressed, SIGNED: a positive
    // value is the inversion that made the old ordering assertion a coin flip;
    // a negative one means compression happened to win on this run. Either sign
    // is consistent with the finding -- that the old assertion's outcome was
    // decided by a margin of single-digit bytes. Nothing here asserts a sign;
    // the real assertion is the overhead BOUND above, which holds either way.
    std::error_code fsEc;
    const auto fixtureBytes = std::filesystem::file_size(src, fsEc);
    RecordProperty("f34_oldFixtureBytes",
                   fsEc ? -1 : static_cast<int>(fixtureBytes));
    RecordProperty("f34_oldUncompressedBytes",
                   static_cast<int>(p.uncompressed.sizeAfterBytes));
    RecordProperty("f34_oldCompressedBytes",
                   static_cast<int>(p.compressed.sizeAfterBytes));
    RecordProperty("f34_oldDeltaBytes",
                   static_cast<int>(static_cast<int64_t>(p.compressed.sizeAfterBytes) -
                                    static_cast<int64_t>(p.uncompressed.sizeAfterBytes)));

    // And the result is still a valid document with every page intact.
    auto outDoc = Document::open(ctx, outC);
    ASSERT_NE(outDoc, nullptr);
    EXPECT_EQ(outDoc->pageCount(), static_cast<int>(pages));
    outDoc.reset();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(OptimizeEngineTest, SavedBytesAndPercentConsistent)
{
    OptimizeResult r;
    r.sizeBeforeBytes = 1000;
    r.sizeAfterBytes  = 250;
    EXPECT_EQ(r.savedBytes(), 750);
    EXPECT_NEAR(r.savedPercent(), 75.0, 1e-6);

    OptimizeResult empty;
    EXPECT_EQ(empty.savedBytes(), 0);
    EXPECT_EQ(empty.savedPercent(), 0.0);
}

namespace {

/// Raw byte search over a file. Used for the confidentiality assertion below:
/// "the value is not in the output" is a claim about BYTES, and a reopen-and-
/// count check cannot make it.
bool fileContainsBytes(const std::filesystem::path& p, const std::string& needle)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    const std::string data((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return data.find(needle) != std::string::npos;
}

/// Detach field `index` from /AcroForm /Fields and from its page's /Annots,
/// leaving the field object itself in the file but UNREFERENCED -- exactly the
/// state a "delete this form field" edit leaves behind before a garbage pass.
void detachFieldLeavingOrphan(const std::filesystem::path& in,
                              const std::filesystem::path& out,
                              int index)
{
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    ASSERT_NE(ctx, nullptr);
    fz_register_document_handlers(ctx);
    const std::string inStr = in.string();
    const std::string outStr = out.string();
    pdf_document* doc = nullptr;
    fz_try(ctx) {
        doc = pdf_open_document(ctx, inStr.c_str());
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* acro = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        pdf_obj* fields = pdf_dict_get(ctx, acro, PDF_NAME(Fields));
        pdf_obj* victim = pdf_array_get(ctx, fields, index);
        const int victimNum = pdf_to_num(ctx, victim);
        pdf_array_delete(ctx, fields, index);
        const int pages = pdf_count_pages(ctx, doc);
        for (int p = 0; p < pages; ++p) {
            pdf_obj* page = pdf_lookup_page_obj(ctx, doc, p);
            pdf_obj* annots = pdf_dict_get(ctx, page, PDF_NAME(Annots));
            for (int a = pdf_array_len(ctx, annots) - 1; a >= 0; --a) {
                if (pdf_to_num(ctx, pdf_array_get(ctx, annots, a)) == victimNum) {
                    pdf_array_delete(ctx, annots, a);
                }
            }
        }
        pdf_write_options wo = pdf_default_write_options;
        wo.do_garbage = 0;      // deliberately keep the orphan in the file
        wo.do_compress = 0;
        pdf_save_document(ctx, doc, outStr.c_str(), &wo);
    }
    fz_always(ctx) { if (doc) pdf_drop_document(ctx, doc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
}

} // namespace

TEST_F(OptimizeEngineTest, OptimizeDoesNotLeakDeletedFormFieldValues)
{
    // F6 / PDF-98. This test used to assert only "reopens and has >0 pages",
    // which is true of literally any PDF -- a security-named false green. It now
    // asserts the property its name claims: the DELETED field's value string is
    // absent from the OUTPUT BYTES, while a surviving field's value still reads
    // back.
    auto dir = std::filesystem::temp_directory_path() /
               ("pdfe_opt_leak_" + std::to_string(static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);

    const auto src = dir / "form_src.pdf";
    const auto orphaned = dir / "form_field_deleted.pdf";
    const auto out = dir / "form_opt.pdf";
    const auto names = pdfe::testsupport::buildArtifactRichPdf(src, 2, "OPTLEAK");
    ASSERT_FALSE(names.tag.empty());

    const std::string deletedValue  = names.fieldValue(0);
    const std::string survivingValue = names.fieldValue(1);

    detachFieldLeavingOrphan(src, orphaned, 0);
    ASSERT_TRUE(std::filesystem::exists(orphaned));

    // CONTROL: before the optimize, the deleted field's value IS still in the
    // bytes. Without this the byte-absence assertion below could pass simply
    // because the value was never written.
    ASSERT_TRUE(fileContainsBytes(orphaned, deletedValue))
        << "control failed: the orphaned value is not in the pre-optimize file, "
           "so the post-optimize absence assertion would be vacuous";
    ASSERT_TRUE(fileContainsBytes(orphaned, survivingValue));

    OptimizeOptions opts;
    opts.mergeDuplicates = true;   // garbage level 4
    opts.removeUnused = true;
    OptimizeEngine eng(ctx);
    auto r = eng.run(orphaned, out, opts);
    ASSERT_TRUE(r.success()) << r.errorMessage;
    ASSERT_TRUE(std::filesystem::exists(out));

    EXPECT_FALSE(fileContainsBytes(out, deletedValue))
        << "the deleted form field's value is still present in the optimized "
           "output bytes";

    // ... and the optimize did not achieve that by throwing away live data.
    const auto a = pdfe::testsupport::readArtifacts(out);
    ASSERT_TRUE(pdfe::testsupport::DocReadable(a));
    EXPECT_TRUE(pdfe::testsupport::HasFieldValues(a, {survivingValue}));
    EXPECT_TRUE(fileContainsBytes(out, survivingValue));
    EXPECT_EQ(a.pageCount, 2);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(OptimizeEngineTest, OptimizePreservesDocumentArtifacts)
{
    // F6 / PDF-102: garbage=4 plus stream/object dedup is the setting most
    // likely to prune a live outline, OCG or name-tree entry. Assert the
    // document-level artifacts across each option combination, not just size.
    auto dir = std::filesystem::temp_directory_path() /
               ("pdfe_opt_artifacts_" + std::to_string(static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);
    const auto src = dir / "artifacts_src.pdf";
    const auto names = pdfe::testsupport::buildArtifactRichPdf(src, 3, "OPTART");
    ASSERT_FALSE(names.tag.empty());

    struct Combo { const char* label; bool removeUnused; bool mergeDuplicates; bool compress; };
    const Combo combos[] = {
        {"none",        false, false, false},
        {"removeUnused", true, false, false},
        {"merge",        true, true,  false},
        {"merge+compress", true, true, true},
    };
    int n = 0;
    for (const Combo& c : combos) {
        SCOPED_TRACE(c.label);
        const auto out = dir / ("opt_" + std::to_string(n++) + ".pdf");
        OptimizeOptions opts;
        opts.removeUnused = c.removeUnused;
        opts.mergeDuplicates = c.mergeDuplicates;
        opts.compressStreams = c.compress;
        OptimizeEngine eng(ctx);
        auto r = eng.run(src, out, opts);
        ASSERT_TRUE(r.success()) << r.errorMessage;

        const auto a = pdfe::testsupport::readArtifacts(out);
        ASSERT_TRUE(pdfe::testsupport::DocReadable(a));
        EXPECT_EQ(a.pageCount, 3);
        EXPECT_TRUE(pdfe::testsupport::HasOutlineTitles(
            a, {names.bookmark(0), names.bookmark(1), names.bookmark(2)}));
        EXPECT_TRUE(a.hasPageLabelsKey);
        EXPECT_TRUE(pdfe::testsupport::HasFieldValues(
            a, {names.fieldValue(0), names.fieldValue(1), names.fieldValue(2)}));
        EXPECT_TRUE(a.hasOcg(names.ocgName()));
        EXPECT_TRUE(a.hasStructTreeRoot);
        EXPECT_TRUE(a.hasEmbeddedFile(names.embeddedFile()));
        EXPECT_TRUE(a.hasNamedDest(names.destName(0)));
        EXPECT_TRUE(pdfe::testsupport::HasAnnotContents(a, {names.annotContents(0)}));
        EXPECT_TRUE(pdfe::testsupport::NoDanglingDestinations(a));
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// GitHub #144: the SHIPPING linearization path had zero coverage.
//
// Two distinct code paths can linearize, and until now only the unreachable one
// was tested:
//
//   QpdfWrapper::save(path, linearize=true)   - 4 tests, NO production caller
//   OptimizeDialog -> opts.linearize
//     -> OptimizeEngine -> wo.do_linear       - 0 tests, THE user-reachable one
//
// The RTM marked linearization COVERED, so the coverage record actively
// concealed the gap rather than merely omitting it. These two tests drive
// OptimizeEngine itself.
//
// Oracle: QPDF::isLinearized(), which reads the linearization parameter
// dictionary and the first-page hint tables. That is an authoritative check,
// not a byte-grep proxy for the literal name -- the same oracle
// test_linearization.cpp uses, for the same reason.
//
// The pair is deliberate. The positive test alone could pass vacuously if the
// fixture were somehow already linearized, or if isLinearized() were to return
// true unconditionally; the negative control fails in both of those cases. The
// two differ in EXACTLY ONE input -- opts.linearize -- so between them they pin
// the flag to the observable effect, rather than pinning one output against
// another output that might share the same defect.
//
// Note these ASSERT the fixture exists rather than GTEST_SKIP on it, unlike the
// older tests above. A skip here would restore precisely the condition the
// issue reports: a green suite with the shipping path unexercised.

TEST_F(OptimizeEngineTest, LinearizeOptionProducesLinearizedOutput)
{
    auto src = fixturesDir() / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(src))
        << "test_3page.pdf is required; skipping would silently restore the "
           "zero-coverage state this test exists to close (GitHub #144)";

    auto out = tempOut("linearized_on.pdf");
    std::filesystem::remove(out);

    OptimizeEngine eng(ctx);
    OptimizeOptions opts;
    opts.linearize = true;          // the only difference from the control below
    auto r = eng.run(src, out, opts);

    ASSERT_TRUE(r.success()) << r.errorMessage;
    ASSERT_TRUE(std::filesystem::exists(out));

    QPDF q;
    q.processFile(out.string().c_str());
    EXPECT_TRUE(q.isLinearized())
        << "OptimizeEngine ran with opts.linearize = true but the output "
           "carries no linearization dictionary";
}

TEST_F(OptimizeEngineTest, LinearizeOffLeavesOutputNonLinearized)
{
    auto src = fixturesDir() / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(src))
        << "test_3page.pdf is required (GitHub #144 negative control)";

    auto out = tempOut("linearized_off.pdf");
    std::filesystem::remove(out);

    OptimizeEngine eng(ctx);
    OptimizeOptions opts;
    opts.linearize = false;         // the default, stated explicitly
    auto r = eng.run(src, out, opts);

    ASSERT_TRUE(r.success()) << r.errorMessage;
    ASSERT_TRUE(std::filesystem::exists(out));

    QPDF q;
    q.processFile(out.string().c_str());
    EXPECT_FALSE(q.isLinearized())
        << "output is linearized with opts.linearize = false, so the positive "
           "test above cannot distinguish the flag from the default behaviour";
}
