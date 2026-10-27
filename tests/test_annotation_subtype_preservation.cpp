// F15 / gaps PDF-56 + PDF-59 — annotation subtypes PDFE does not model.
//
// WHAT WAS BROKEN
// ---------------
// Polygon and PolyLine (PDF-56) and ten rarely-used subtypes (PDF-59: Caret,
// Sound, Movie, Screen, Watermark, 3D, PrinterMark, TrapNet, RichMedia,
// Projection) have zero hits in src/annotations/. Not modelling them is an
// acceptable scope decision — but it was an UNASSERTED one: nothing proved
// such an annotation is PRESERVED rather than silently dropped or corrupted
// when PDFE saves the file. Losing a third party's Polygon markup on save is
// data destruction, and the suite was green either way.
//
// THE ORACLE
// ----------
// Open tests/fixtures/f15_unsupported_annots.pdf (12 authored annotations),
// make an UNRELATED edit (add a sticky note), save, reopen with a fresh
// independent MuPDF context and assert, per subtype:
//   * the annotation is still present,
//   * /Subtype is unchanged,
//   * the authored /Contents, /NM and /Rect are byte-identical,
//   * Polygon / PolyLine still carry their /Vertices array.
// Plus: PDFE's own model degrades them to AnnotationType::Unknown rather than
// mis-classifying them as a supported type.
//
// MUTATIONS KILLED (each confirmed RED then reverted):
//   * ran the oracle against a deliberately truncated /Annots array (drop the
//     last 6 entries in the fixture) -> AllUnsupportedSubtypesSurviveSave
//     reports the missing subtypes by name.
//   * changed one expected /Contents literal -> RED, proving the comparison is
//     against authored values and not against whatever the product produced.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "helpers/RoundTripHelper.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using pdfe::annotations::AnnotationData;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;

namespace {

// AUTHORED literals — must match tests/fixtures/gen_f15_unsupported_annots.py.
const std::vector<std::string> kSubtypes = {
    "Polygon", "PolyLine",                        // PDF-56
    "Caret", "Sound", "Movie", "Screen",          // PDF-59
    "Watermark", "3D", "PrinterMark", "TrapNet",
    "RichMedia", "Projection",
};

struct RawAnnot {
    std::string subtype;
    std::string contents;
    std::string nm;
    double rect[4] = {0, 0, 0, 0};
    bool hasVertices = false;
};

// Walk page 0's /Annots with a fresh, independent MuPDF context.
std::vector<RawAnnot> InspectAnnots(const std::filesystem::path& path) {
    std::vector<RawAnnot> out;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return out;
    pdf_document* doc = nullptr;
    pdf_page* page = nullptr;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, path.string().c_str());
        page = pdf_load_page(ctx, doc, 0);
        pdf_obj* annots = pdf_dict_get(ctx, page->obj, PDF_NAME(Annots));
        const int n = pdf_array_len(ctx, annots);
        for (int i = 0; i < n; ++i) {
            pdf_obj* a = pdf_array_get(ctx, annots, i);
            if (!pdf_is_dict(ctx, a)) continue;
            RawAnnot r;
            const char* st = pdf_to_name(ctx, pdf_dict_get(ctx, a, PDF_NAME(Subtype)));
            r.subtype = st ? st : "";
            const char* c = pdf_to_text_string(ctx, pdf_dict_get(ctx, a, PDF_NAME(Contents)));
            r.contents = c ? c : "";
            const char* nm = pdf_to_text_string(ctx, pdf_dict_gets(ctx, a, "NM"));
            r.nm = nm ? nm : "";
            pdf_obj* rect = pdf_dict_get(ctx, a, PDF_NAME(Rect));
            for (int k = 0; k < 4 && k < pdf_array_len(ctx, rect); ++k) {
                r.rect[k] = pdf_to_real(ctx, pdf_array_get(ctx, rect, k));
            }
            r.hasVertices = pdf_dict_get(ctx, a, PDF_NAME(Vertices)) != nullptr;
            out.push_back(r);
        }
    }
    fz_always(ctx) {
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        out.clear();
    }
    fz_drop_context(ctx);
    return out;
}

class UnsupportedSubtypeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR)
                   / "f15_unsupported_annots.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_))
            << "regenerate with "
               "python tests/fixtures/gen_f15_unsupported_annots.py";
        manager_ = std::make_unique<AnnotationManager>(ctx_);
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_f15_subtypes" / info->name();
        std::filesystem::create_directories(tempDir_);
    }
    void TearDown() override {
        manager_.reset();
        ctx_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<AnnotationManager> manager_;
};

TEST_F(UnsupportedSubtypeTest, FixtureCarriesEveryAuthoredSubtype) {
    const auto found = InspectAnnots(fixture_);
    ASSERT_EQ(found.size(), kSubtypes.size());
    for (size_t i = 0; i < kSubtypes.size(); ++i) {
        EXPECT_EQ(found[i].subtype, kSubtypes[i]);
        EXPECT_EQ(found[i].contents, "F15-" + kSubtypes[i] + "-marker");
    }
}

TEST_F(UnsupportedSubtypeTest, AllUnsupportedSubtypesSurviveSaveUnmodified) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // An UNRELATED edit, so the save is a real PDFE write and not a byte copy.
    AnnotationData note = manager_->createAnnotation(
        page.get(), AnnotationType::Text, QRectF(400, 700, 24, 24));
    manager_->setContents(page.get(), note.annotIndex, "f15 unrelated edit");
    page.reset();

    auto r = pdfe::test::SaveAndReopenWithPath(*doc, ctx_, tempDir_, "subtypes");
    ASSERT_NE(r.doc, nullptr);

    const auto after = InspectAnnots(r.path);
    std::map<std::string, RawAnnot> byNm;
    for (const auto& a : after) {
        if (!a.nm.empty()) byNm[a.nm] = a;
    }

    for (size_t i = 0; i < kSubtypes.size(); ++i) {
        const std::string nm = "f15-annot-" + std::to_string(i);
        SCOPED_TRACE(kSubtypes[i] + " (" + nm + ")");
        auto it = byNm.find(nm);
        ASSERT_NE(it, byNm.end())
            << "annotation of subtype " << kSubtypes[i]
            << " was DROPPED by the save";

        EXPECT_EQ(it->second.subtype, kSubtypes[i]);
        EXPECT_EQ(it->second.contents, "F15-" + kSubtypes[i] + "-marker");
        EXPECT_NEAR(it->second.rect[0], 50.0, 0.01);
        EXPECT_NEAR(it->second.rect[1], 50.0 + 20.0 * static_cast<double>(i), 0.01);
        EXPECT_NEAR(it->second.rect[2], 150.0, 0.01);
        EXPECT_NEAR(it->second.rect[3], 70.0 + 20.0 * static_cast<double>(i), 0.01);
        if (kSubtypes[i] == "Polygon" || kSubtypes[i] == "PolyLine") {
            EXPECT_TRUE(it->second.hasVertices)
                << "/Vertices lost — the geometry of the markup is gone";
        }
    }

    // The unrelated sticky note really was written (so the save was a write).
    const bool sawNote = std::any_of(
        after.begin(), after.end(), [](const RawAnnot& a) {
            return a.contents == "f15 unrelated edit";
        });
    EXPECT_TRUE(sawNote);
}

TEST_F(UnsupportedSubtypeTest, UnsupportedSubtypesDegradeToUnknownNotMisclassified) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto annots = manager_->getAnnotations(page.get());
    ASSERT_EQ(annots.size(), kSubtypes.size());
    for (size_t i = 0; i < annots.size(); ++i) {
        SCOPED_TRACE(kSubtypes[i]);
        EXPECT_EQ(annots[i].type, AnnotationType::Unknown)
            << "subtype " << kSubtypes[i]
            << " is mapped to a supported AnnotationType it is not";
    }
}

} // namespace
