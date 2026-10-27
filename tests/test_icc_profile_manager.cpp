// -*- coding: utf-8 -*-
//
// GitHub #207 — this file's oracle could not fail.
// ================================================
//
// Every non-trivial test here used to bottom out on the single boolean
// `IccProfileManager::hasOutputIntent()`, whose entire body is
// (`src/compliance/IccProfileManager.cpp:244-245`):
//
//     pdf_obj* intents = pdf_dict_get(ctx, catalog, PDF_NAME(OutputIntents));
//     hasIntent = intents && pdf_is_array(ctx, intents) && pdf_array_len(...) > 0;
//
// "the array is non-empty" is the whole oracle. An /OutputIntents array holding
// one EMPTY dictionary satisfied every test in this file — no /S subtype, no
// /DestOutputProfile, no ICC bytes. `AddSrgbOutputIntent_VerifyStream` was the
// worst of it: its name promised stream verification and its body was byte-for-
// byte identical to `_Roundtrip`, verifying no stream at all. And
// `_Idempotent` never asserted the array length, so the one property the word
// "idempotent" names was untested: adding twice could have appended twice and
// the test would still have been green.
//
// THE FIX IS TO MAKE THE ORACLE CAPABLE OF FAILING, not to downgrade the RTM
// row. `walkRawOutputIntent()` below is a hand descent of the object graph on a
// FRESH fz_context that never constructs an IccProfileManager, lifted from the
// established pattern in tests/test_fidelity_r3_19_pdfa_independent.cpp
// (`walkRawPdfaMarkers`). It re-reads from first principles what
// `addSrgbOutputIntent` physically wrote, so a divergence between "the manager
// says there is an output intent" and "there is a conformant output intent in
// the file" is now observable.
//
// Mutation-confirmed: see the block above AddSrgbOutputIntent_VerifyStream.

#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/IccProfileManager.h"
#include "compliance/srgb_icc_profile.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::compliance;

namespace {

// What `addSrgbOutputIntent` actually writes (IccProfileManager.cpp:280-288):
//   intent: /Type /OutputIntent, /S /GTS_PDFA1 (a NAME),
//           /OutputConditionIdentifier (sRGB IEC61966-2.1) (a TEXT STRING),
//           /RegistryName (http://www.color.org), /DestOutputProfile <stream>
//   stream: /N 3, body = generateSrgbIccProfile()
// Every one of those is read back below. `intentCount` exists so the
// idempotence claim has an oracle at all.
struct RawOutputIntent {
    bool opened = false;              // the file parsed as a PDF at all
    bool intentsKeyPresent = false;   // Root -> /OutputIntents exists
    bool intentsIsArray = false;      // ...and is an array (not a bare dict)
    int  intentCount = -1;            // its length; -1 if not an array
    bool intent0IsDict = false;       // the first entry is a dictionary
    std::string type;                 // observed /Type
    std::string subtype;              // observed /S
    std::string conditionIdentifier;  // observed /OutputConditionIdentifier
    std::string registryName;         // observed /RegistryName
    bool destProfileKeyPresent = false;
    bool destProfileIsStream = false; // /DestOutputProfile is a real stream
    int  destProfileN = -1;           // its /N
    std::vector<uint8_t> destProfileBytes;  // its decoded body
};

RawOutputIntent walkRawOutputIntent(const std::filesystem::path& path) {
    RawOutputIntent m;

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return m;
    fz_register_document_handlers(ctx);

    fz_document* fdoc = nullptr;
    fz_try(ctx) {
        fdoc = fz_open_document(ctx, path.string().c_str());
        pdf_document* doc = pdf_document_from_fz_document(ctx, fdoc);
        if (doc) {
            m.opened = true;

            pdf_obj* trailer = pdf_trailer(ctx, doc);
            pdf_obj* root    = pdf_dict_gets(ctx, trailer, "Root");
            pdf_obj* intents = pdf_dict_gets(ctx, root, "OutputIntents");

            m.intentsKeyPresent = (intents != nullptr);

            pdf_obj* intent0 = nullptr;
            if (pdf_is_array(ctx, intents)) {
                m.intentsIsArray = true;
                m.intentCount = pdf_array_len(ctx, intents);
                if (m.intentCount > 0) intent0 = pdf_array_get(ctx, intents, 0);
            } else if (pdf_is_dict(ctx, intents)) {
                intent0 = intents;  // tolerate a single-dict encoding
            }

            if (intent0 && pdf_is_dict(ctx, intent0)) {
                m.intent0IsDict = true;

                pdf_obj* t = pdf_dict_gets(ctx, intent0, "Type");
                if (pdf_is_name(ctx, t) && pdf_to_name(ctx, t))
                    m.type = pdf_to_name(ctx, t);

                pdf_obj* s = pdf_dict_gets(ctx, intent0, "S");
                if (pdf_is_name(ctx, s) && pdf_to_name(ctx, s))
                    m.subtype = pdf_to_name(ctx, s);

                pdf_obj* oci =
                    pdf_dict_gets(ctx, intent0, "OutputConditionIdentifier");
                if (pdf_is_string(ctx, oci)) {
                    size_t n = 0;
                    const char* p = pdf_to_string(ctx, oci, &n);
                    if (p) m.conditionIdentifier.assign(p, n);
                }

                pdf_obj* rn = pdf_dict_gets(ctx, intent0, "RegistryName");
                if (pdf_is_string(ctx, rn)) {
                    size_t n = 0;
                    const char* p = pdf_to_string(ctx, rn, &n);
                    if (p) m.registryName.assign(p, n);
                }

                pdf_obj* prof = pdf_dict_gets(ctx, intent0, "DestOutputProfile");
                m.destProfileKeyPresent = (prof != nullptr);
                if (prof && pdf_is_stream(ctx, prof)) {
                    m.destProfileIsStream = true;

                    pdf_obj* nObj = pdf_dict_gets(ctx, prof, "N");
                    if (pdf_is_int(ctx, nObj)) m.destProfileN = pdf_to_int(ctx, nObj);

                    fz_buffer* pb = pdf_load_stream(ctx, prof);
                    if (pb) {
                        unsigned char* d = nullptr;
                        size_t n = fz_buffer_storage(ctx, pb, &d);
                        if (d && n) m.destProfileBytes.assign(d, d + n);
                        fz_drop_buffer(ctx, pb);
                    }
                }
            }
        }
    }
    fz_always(ctx) {
        if (fdoc) fz_drop_document(ctx, fdoc);
    }
    fz_catch(ctx) {
        // Leave m as-is; opened==false (or partial) signals the failure.
    }
    fz_drop_context(ctx);
    return m;
}

}  // namespace

class IccProfileManagerTest : public ::testing::Test {
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

TEST_F(IccProfileManagerTest, ConstructDestroy) {
    IccProfileManager manager(context_);
    SUCCEED();
}

TEST_F(IccProfileManagerTest, HasOutputIntent_FreshDoc_False) {
    IccProfileManager manager(context_);
    bool hasIntent = manager.hasOutputIntent(document_.get());
    EXPECT_FALSE(hasIntent);
}

TEST_F(IccProfileManagerTest, AddSrgbOutputIntent) {
    IccProfileManager manager(context_);

    EXPECT_FALSE(manager.hasOutputIntent(document_.get()));

    manager.addSrgbOutputIntent(document_.get());

    auto tempPath = std::filesystem::temp_directory_path() / "test_icc_add.pdf";
    document_->saveAs(tempPath);

    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    EXPECT_TRUE(manager.hasOutputIntent(doc2.get()));

    doc2.reset();
    std::filesystem::remove(tempPath);
}

TEST_F(IccProfileManagerTest, AddSrgbOutputIntent_Roundtrip) {
    IccProfileManager manager(context_);

    manager.addSrgbOutputIntent(document_.get());

    auto tempPath = std::filesystem::temp_directory_path() / "test_icc_roundtrip.pdf";
    document_->saveAs(tempPath);

    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    EXPECT_TRUE(manager.hasOutputIntent(doc2.get()));

    doc2.reset();
    std::filesystem::remove(tempPath);
}

// GitHub #207. "Idempotent" is a claim about the array LENGTH, and the length
// was never asserted: `hasOutputIntent` returns true for a 1-entry array and
// for a 2-entry one alike, so a second `addSrgbOutputIntent` appending a
// duplicate intent — the exact defect the word rules out — left this test green.
//
// MUTATION RECORD M0 (measured 2026-09-18): neutralising the
// `if (hasOutputIntent(doc)) return;` early-out in
// IccProfileManager::addSrgbOutputIntent reddens this test on
// `EXPECT_EQ(m.intentCount, 1)` — observed intentCount == 2 — while the old
// oracle in this same test, `EXPECT_TRUE(manager.hasOutputIntent(doc2.get()))`,
// still PASSED. Reverted; source tree left clean.
TEST_F(IccProfileManagerTest, AddSrgbOutputIntent_Idempotent) {
    IccProfileManager manager(context_);

    manager.addSrgbOutputIntent(document_.get());
    manager.addSrgbOutputIntent(document_.get());

    auto tempPath = std::filesystem::temp_directory_path() / "test_icc_idempotent.pdf";
    document_->saveAs(tempPath);

    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    EXPECT_TRUE(manager.hasOutputIntent(doc2.get()));

    doc2.reset();

    const RawOutputIntent m = walkRawOutputIntent(tempPath);
    ASSERT_TRUE(m.opened) << tempPath.string() << " did not reopen as a PDF";
    ASSERT_TRUE(m.intentsIsArray)
        << "/OutputIntents is not an array; cannot judge idempotence";
    EXPECT_EQ(m.intentCount, 1)
        << "adding the sRGB output intent twice must leave exactly ONE entry; "
           "observed " << m.intentCount
        << ". A second entry means the early-out in addSrgbOutputIntent() is "
           "not holding — and hasOutputIntent() cannot see the difference.";

    std::filesystem::remove(tempPath);
}

TEST_F(IccProfileManagerTest, HasOutputIntent_NullDoc) {
    IccProfileManager manager(context_);
    EXPECT_FALSE(manager.hasOutputIntent(nullptr));
}

// GitHub #207 — THE row's oracle. This test's name promised stream
// verification; until 2026-09-18 its body was byte-for-byte identical to
// `_Roundtrip` and verified no stream whatsoever.
//
// MUTATION RECORD — measured 2026-09-18, not asserted.
// Each mutation was applied to src/compliance/IccProfileManager.cpp ALONE, the
// marker grepped back out of the file BEFORE building (a patch that silently
// failed to apply is indistinguishable from a surviving mutant), pdfe_tests
// rebuilt, `--gtest_filter=IccProfileManagerTest.*:PdfUAConverterTest.*` run,
// the JUnit XML parsed for failures (gtest exits 0 with failures in the XML),
// then reverted by direct write and the marker's absence re-checked.
//
// THE LAST COLUMN IS THE POINT. In every case the pre-#207 oracle — the
// `EXPECT_TRUE(manager.hasOutputIntent(doc2.get()))` still present in this test
// — PASSED. The mutants are invisible to it and visible only to the assertions
// added below it. (Assertions are named, not cited by line number, because a
// line citation goes stale the next time anything above it is edited.)
//
//   id  mutation                                  killed by          old oracle
//   --  ----------------------------------------  -----------------  ----------
//   M1  drop `pdf_dict_puts(.., "DestOutputProfile", iccStream)`
//                                                 destProfileKey-    PASSED
//                                                 Present
//   M2  `/S` name "GTS_PDFA1" -> "GTS_PDFX"       m.subtype          PASSED
//   M3  `/N` on the ICC stream 3 -> 4             m.destProfileN     PASSED
//   M6  `profileData.resize(64)` before embed     destProfileBytes   PASSED
//                                                 .size() AND ==
//                                                 expected
//
// (M0 is recorded on _Idempotent above; M4/M5 on the PDF/UA converter.)
// 7 mutants applied across #207, 7 killed, 0 survived. Afterwards
// `git diff src/compliance/` was empty and the clean tree ran 17/17 green.
//
// M6 is why the profile assertion is a byte EQUALITY against
// generateSrgbIccProfile() rather than a size check or a header sniff: a
// truncated-but-still-ICC-looking profile keeps `acsp` at offset 36 and
// `RGB ` at offset 16, so a header sniff alone would have let M6 live. An
// oracle that accepts any non-empty buffer is the same decoration this issue
// is about, one level down.
TEST_F(IccProfileManagerTest, AddSrgbOutputIntent_VerifyStream) {
    IccProfileManager manager(context_);

    manager.addSrgbOutputIntent(document_.get());

    auto tempPath = std::filesystem::temp_directory_path() / "test_icc_verify.pdf";
    document_->saveAs(tempPath);

    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    EXPECT_TRUE(manager.hasOutputIntent(doc2.get()));

    // Release the handle before the independent walk opens the same path.
    doc2.reset();

    // --- the independent oracle: what is PHYSICALLY in the file -------------
    const RawOutputIntent m = walkRawOutputIntent(tempPath);
    ASSERT_TRUE(m.opened) << tempPath.string() << " did not reopen as a PDF";
    ASSERT_TRUE(m.intentsKeyPresent) << "Root has no /OutputIntents at all";
    ASSERT_TRUE(m.intentsIsArray) << "/OutputIntents is not an array";
    ASSERT_EQ(m.intentCount, 1) << "expected exactly one output intent";
    ASSERT_TRUE(m.intent0IsDict)
        << "/OutputIntents[0] is not a dictionary — this is precisely the shape "
           "hasOutputIntent() cannot distinguish from a correct intent";

    EXPECT_EQ(m.type, "OutputIntent") << "observed /Type '" << m.type << "'";
    EXPECT_EQ(m.subtype, "GTS_PDFA1")
        << "observed /S '" << m.subtype
        << "'; the subtype is what tells a consumer which conformance regime "
           "this intent belongs to";
    EXPECT_EQ(m.conditionIdentifier, "sRGB IEC61966-2.1")
        << "observed /OutputConditionIdentifier '" << m.conditionIdentifier << "'";
    EXPECT_EQ(m.registryName, "http://www.color.org")
        << "observed /RegistryName '" << m.registryName << "'";

    ASSERT_TRUE(m.destProfileKeyPresent) << "/DestOutputProfile key is absent";
    ASSERT_TRUE(m.destProfileIsStream)
        << "/DestOutputProfile is present but is not a stream — a dangling ref "
           "or a name would satisfy a presence-only check and carry no profile";
    EXPECT_EQ(m.destProfileN, 3)
        << "/N must be 3 for an RGB profile; observed " << m.destProfileN;

    const std::vector<uint8_t> expected = generateSrgbIccProfile();
    ASSERT_FALSE(expected.empty()) << "generateSrgbIccProfile() returned nothing";
    EXPECT_EQ(m.destProfileBytes.size(), expected.size())
        << "embedded profile is " << m.destProfileBytes.size()
        << " bytes, generator produced " << expected.size();

    // Compare every byte EXCEPT the ICC creation date/time.
    //
    // generateSrgbIccProfile() is NOT deterministic: IccProfileManager.cpp's
    // writeDateTime(profile, 24) stamps std::time(nullptr) into the 12-byte
    // dateTime field at offset 24 (ICC.1 header). The profile embedded during
    // the save above and the one generated for `expected` here are therefore
    // produced at two different instants, and a bare equality holds only while
    // both land inside the same second.
    //
    // That is exactly what happened: this assertion failed in-shard and passed
    // on a fresh rerun, and run_gtest.py's heal-on-rerun then cleared it from
    // the merged XML (GitHub #215) — an intermittent red that reports as green.
    // Timing, not the property under test.
    //
    // Excluding the field costs nothing the mutants care about: truncation is
    // caught by the size check above, and a substituted or corrupted profile
    // differs far outside 12 bytes at a fixed offset. What it buys is an
    // assertion whose verdict depends on the profile and not on the clock.
    constexpr size_t kIccDateTimeOffset = 24;
    constexpr size_t kIccDateTimeLength = 12;
    ASSERT_GE(expected.size(), kIccDateTimeOffset + kIccDateTimeLength)
        << "profile is too short to contain an ICC header";
    if (m.destProfileBytes.size() == expected.size()) {
        std::vector<uint8_t> gotMasked = m.destProfileBytes;
        std::vector<uint8_t> wantMasked = expected;
        std::fill_n(gotMasked.begin() + kIccDateTimeOffset, kIccDateTimeLength,
                    uint8_t{0});
        std::fill_n(wantMasked.begin() + kIccDateTimeOffset, kIccDateTimeLength,
                    uint8_t{0});
        EXPECT_TRUE(gotMasked == wantMasked)
            << "the embedded /DestOutputProfile stream is not the sRGB profile "
               "this build generates — the intent names sRGB IEC61966-2.1 while "
               "carrying different bytes (compared with the creation date/time "
               "field masked out; a difference here is a real content "
               "difference, not a clock difference)";
    }

    // Independent sanity on the profile itself: ICC v2/v4 header carries the
    // 'acsp' signature at offset 36 and the data colour space at offset 16.
    ASSERT_GE(m.destProfileBytes.size(), 40u) << "profile too short to be ICC";
    EXPECT_EQ(0, std::memcmp(m.destProfileBytes.data() + 36, "acsp", 4))
        << "no 'acsp' ICC profile signature at offset 36";
    EXPECT_EQ(0, std::memcmp(m.destProfileBytes.data() + 16, "RGB ", 4))
        << "ICC data colour space at offset 16 is not 'RGB '";

    std::filesystem::remove(tempPath);
}

TEST_F(IccProfileManagerTest, MultipleDocuments) {
    auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
    auto doc1 = pdfe::core::Document::open(context_, pdfPath);
    auto doc2 = pdfe::core::Document::open(context_, pdfPath);
    ASSERT_NE(doc1, nullptr);
    ASSERT_NE(doc2, nullptr);

    IccProfileManager manager1(context_);
    IccProfileManager manager2(context_);

    manager1.addSrgbOutputIntent(doc1.get());

    auto tempPath1 = std::filesystem::temp_directory_path() / "test_icc_multi1.pdf";
    auto tempPath2 = std::filesystem::temp_directory_path() / "test_icc_multi2.pdf";

    doc1->saveAs(tempPath1);
    doc2->saveAs(tempPath2);

    auto reopen1 = pdfe::core::Document::open(context_, tempPath1);
    auto reopen2 = pdfe::core::Document::open(context_, tempPath2);

    EXPECT_TRUE(manager1.hasOutputIntent(reopen1.get()));
    EXPECT_FALSE(manager2.hasOutputIntent(reopen2.get()));

    reopen1.reset();
    reopen2.reset();
    std::filesystem::remove(tempPath1);
    std::filesystem::remove(tempPath2);
}

// ============================================================================
// GitHub #228 -- generateSrgbIccProfile() must be byte-deterministic.
//
// The header's DateTime field at offset 24 used to carry the wall clock, so two
// calls returned different bytes whenever they straddled a second boundary.
// That is an intermittent non-determinism in something that is otherwise a
// fixed constant: it defeats byte-comparison of output PDFs, content-addressed
// caching and any reproducible-build claim, and it fails only sometimes, which
// is the hardest kind to notice.
//
// ICC.1:2010 section 7.2.8 sanctions zero -- "if the date and time are not
// known, the field shall be set to zeros" -- and for a profile synthesised
// identically on every call there is no creation moment to record.
// ============================================================================

TEST(IccProfileDeterminism, TwoCallsProduceByteIdenticalProfiles) {
    const auto a = pdfe::compliance::generateSrgbIccProfile();
    const auto b = pdfe::compliance::generateSrgbIccProfile();

    // Non-vacuity: comparing two empty vectors would pass while measuring
    // nothing. A real sRGB v2 profile is several hundred bytes.
    ASSERT_GT(a.size(), 300u)
        << "the generated profile is implausibly small, so a byte-equality "
           "assertion below would not be exercising a real profile";
    ASSERT_EQ(a.size(), b.size()) << "two calls produced different lengths";

    // The whole point: EVERY byte, not a prefix or a hash of part of it.
    EXPECT_EQ(a, b)
        << "generateSrgbIccProfile() is not deterministic. If this reddens "
           "again, the first suspect is a wall-clock or counter value written "
           "into the header -- offset 24 is the DateTime field.";
}

TEST(IccProfileDeterminism, DateTimeFieldIsZeroAndTheProfileStaysSpecValid) {
    const auto p = pdfe::compliance::generateSrgbIccProfile();
    ASSERT_GT(p.size(), 128u) << "profile is shorter than its own header";

    // The twelve DateTime bytes at offset 24 are all zero. Asserted directly
    // rather than inferred from the equality test above, because two calls
    // within the same second would ALSO be byte-identical with a live clock --
    // that is exactly how this defect hid. This assertion cannot pass for that
    // reason.
    for (size_t i = 24; i < 36; ++i) {
        EXPECT_EQ(p[i], 0u)
            << "DateTime byte at offset " << i << " is non-zero; the profile "
               "is carrying a timestamp again";
    }

    // Still a well-formed ICC profile, so determinism was not bought by
    // corrupting the header. Three independent structural facts:
    //   - the 'acsp' file signature at offset 36 (ICC.1:2010 7.2.9)
    //   - the size field at offset 0 agreeing with the actual byte count
    //   - the declared colour space and PCS
    EXPECT_EQ(std::memcmp(&p[36], "acsp", 4), 0)
        << "the ICC file signature 'acsp' is missing or damaged";

    const uint32_t declaredSize = (static_cast<uint32_t>(p[0]) << 24) |
                                  (static_cast<uint32_t>(p[1]) << 16) |
                                  (static_cast<uint32_t>(p[2]) << 8) |
                                   static_cast<uint32_t>(p[3]);
    EXPECT_EQ(declaredSize, p.size())
        << "the header's profile-size field disagrees with the real size";

    EXPECT_EQ(std::memcmp(&p[16], "RGB ", 4), 0) << "colour space is not RGB";
    EXPECT_EQ(std::memcmp(&p[20], "XYZ ", 4), 0) << "PCS is not XYZ";
}
