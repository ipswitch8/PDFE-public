// -*- coding: utf-8 -*-
//
// PDF-49 (meta-audit-2026-07 gap register) — the /ImportData action.
//
// /ImportData is the read-side twin of /SubmitForm: the document names a file
// and asks the viewer to populate the form from it. A viewer that honours it
// can be pointed at any path the user can read, and the values it loads then
// become exfiltratable through any submit route. PDFE does not implement it —
// AnnotationManager::getLinkAction parses /URI, /GoTo, /Named and /Launch and
// nothing else, so an /ImportData subtype falls through to the inert default.
//
// That is an ABSENCE, and the register's point is that an absence with no
// test in front of it is one commit deep. These cases make "we do not do
// this" a property the suite enforces rather than a fact about today's code.
//
// THE TRAP THIS FILE IS BUILT TO AVOID. The easy version of this test names an
// FDF that is not there, opens the document, and finds no imported data. It
// passes on a viewer that implements /ImportData flawlessly — the import
// simply failed for want of a file. So tests/fixtures/gen_form_importdata_action.py
// emits a real FDF alongside the PDF, and setUp copies BOTH into the working
// directory so the relative reference resolves. The only remaining reason for
// the data not to arrive is that PDFE declined to fetch it, and
// TheFdfPayloadIsRealAndReachable below refuses to let that premise rot.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "annotations/LinkAction.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"
#include "forms/JavaScriptEngine.h"

#include "security_oracles.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace pdfe::core;
using namespace pdfe::annotations;

namespace {

// Must match gen_form_importdata_action.py.
constexpr const char* kSentinel = "PDFE_IMPORTDATA_EXECUTED";
constexpr const char* kPdfName  = "form_importdata_action.pdf";
constexpr const char* kFdfName  = "form_importdata_payload.fdf";

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

class ImportDataActionTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!std::filesystem::exists(fixturesDir() / kPdfName) ||
            !std::filesystem::exists(fixturesDir() / kFdfName)) {
            GTEST_SKIP() << "run tests/fixtures/gen_form_importdata_action.py";
        }

        work_ = std::filesystem::temp_directory_path() / "pdfe_importdata_action";
        std::filesystem::create_directories(work_);

        // BOTH files, into the SAME directory — the FDF's reference from the
        // PDF is relative, so this is what makes the import genuinely
        // possible rather than merely attempted.
        pdf_ = work_ / kPdfName;
        fdf_ = work_ / kFdfName;
        std::filesystem::copy_file(fixturesDir() / kPdfName, pdf_,
            std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(fixturesDir() / kFdfName, fdf_,
            std::filesystem::copy_options::overwrite_existing);

        ctx_ = std::make_shared<MuPdfContext>();
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path work_, pdf_, fdf_;
};

// The premise guard. If this goes red, every other case in the file is
// passing for the wrong reason and must not be believed.
TEST_F(ImportDataActionTest, TheFdfPayloadIsRealAndReachable)
{
    ASSERT_TRUE(std::filesystem::exists(fdf_))
        << "the FDF must sit beside the PDF or the import cannot even be "
           "attempted, and 'no data imported' would prove nothing";

    const std::string fdf = readAll(fdf_);
    EXPECT_NE(fdf.find(kSentinel), std::string::npos)
        << "the FDF no longer carries the sentinel the other cases look for";
    EXPECT_NE(fdf.find("%FDF-"), std::string::npos)
        << "the payload is not an FDF, so a viewer would reject it on format "
           "grounds rather than on policy grounds";
    EXPECT_NE(fdf.find("ImportTarget"), std::string::npos)
        << "the FDF's field names must match the PDF's or the import would be "
           "a no-op even on a viewer that performs it";

    // And the sentinel must NOT already be in the document, or asserting its
    // absence from the fields would be vacuous.
    EXPECT_EQ(readAll(pdf_).find(kSentinel), std::string::npos)
        << "the sentinel is already present in the PDF itself";
}

// The core case named by the register.
TEST_F(ImportDataActionTest, ImportDataActionIsIgnoredAndDoesNotReadLocalFiles)
{
    // Nothing may be spawned, and nothing in the working directory may be
    // created, modified or deleted — an import that wrote a cache, or a
    // handler that consumed and unlinked the FDF, would show here.
    pdfe_test::ProcessSpawnGuard spawnGuard;
    pdfe_test::FileWriteGuard writeGuard(work_, "pdfe_importdata");

    auto doc = Document::open(ctx_, pdf_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::forms::FormManager fm(ctx_);
    const auto fields = fm.getFormFields(page.get());
    ASSERT_FALSE(fields.empty())
        << "vacuity guard: the fixture must have form fields for their "
           "values to be worth asserting about";

    // The whole-form statement, rather than one field the test remembered.
    for (const auto& f : fields) {
        EXPECT_EQ(f.value.find(kSentinel), std::string::npos)
            << "field [" << f.fieldName << "] was populated from the FDF — "
               "/ImportData executed";
        EXPECT_TRUE(f.value.empty())
            << "field [" << f.fieldName << "] arrived non-empty: ["
            << f.value << "]";
    }

    // The document-open placement fires with no interaction; the on-focus and
    // click placements need the events below. Give the action every route it
    // has before concluding it was ignored.
    pdfe::forms::JavaScriptEngine engine(ctx_);
    engine.enable(page.get());
    EXPECT_NO_THROW({
        engine.fireFocusEvent(page.get(), 0);
        engine.fireKeystrokeEvent(page.get(), 0, "", "x", 0, 0, false);
        engine.fireBlurEvent(page.get(), 0);
    });

    const auto after = fm.getFormFields(page.get());
    for (const auto& f : after) {
        EXPECT_EQ(f.value.find(kSentinel), std::string::npos)
            << "field [" << f.fieldName << "] was populated after firing the "
               "widget's own events — the on-focus /ImportData executed";
    }

    EXPECT_EQ(spawnGuard.newlySpawned(), "")
        << "handling /ImportData spawned a process";
    EXPECT_EQ(writeGuard.violation(), "")
        << "handling /ImportData touched the filesystem: "
        << writeGuard.violation();

    // And the FDF must be exactly as it was — not consumed, not truncated.
    EXPECT_NE(readAll(fdf_).find(kSentinel), std::string::npos)
        << "the FDF was modified or emptied while the document was open";
}

// /ImportData must arrive at the action reader as the inert default, the same
// place /GoToR lands (see LinkActionRemoteRefusal in test_link_actions.cpp).
// If someone adds an ImportData branch to getLinkAction, this is what goes
// red — before the dispatcher gets one.
TEST_F(ImportDataActionTest, ImportDataIsNotReportedAsAnActionableLinkType)
{
    auto doc = Document::open(ctx_, pdf_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    AnnotationManager mgr(ctx_);
    const auto annots = mgr.getAnnotations(page.get());
    ASSERT_FALSE(annots.empty());

    int linkCount = 0;
    for (const auto& a : annots) {
        if (a.type != AnnotationType::Link) continue;
        ++linkCount;
        const LinkAction act = mgr.getLinkAction(page.get(), a.annotIndex);
        EXPECT_EQ(act.type, LinkActionType::Uri)
            << "/ImportData was given an actionable type";
        EXPECT_TRUE(act.uri.empty())
            << "the FDF path leaked into the URI field: [" << act.uri
            << "] — MainWindow::followLinkAt dispatches a non-empty URI";
        EXPECT_TRUE(act.launchPath.empty());
    }
    EXPECT_GT(linkCount, 0)
        << "vacuity guard: the fixture's Link annotation carrying the "
           "/ImportData action is gone, so nothing was actually checked";
}

// A saved copy must not acquire the imported values either — the round trip
// is where a partially-implemented import would become persistent.
TEST_F(ImportDataActionTest, SaveAndReopenDoesNotMaterialiseImportedValues)
{
    auto doc = Document::open(ctx_, pdf_);
    ASSERT_NE(doc, nullptr);

    const auto out = work_ / "importdata_roundtrip.pdf";
    ASSERT_NO_THROW(doc->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    EXPECT_EQ(readAll(out).find(kSentinel), std::string::npos)
        << "the saved document contains the FDF's sentinel value";

    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr);
    auto page = reopened->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::forms::FormManager fm(ctx_);
    for (const auto& f : fm.getFormFields(page.get())) {
        EXPECT_EQ(f.value.find(kSentinel), std::string::npos)
            << "field [" << f.fieldName << "] carries imported data after a "
               "save/reopen round trip";
    }
}
