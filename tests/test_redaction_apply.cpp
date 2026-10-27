#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/AtomicFileReplace.h"
#include "core/Page.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionVerifier.h"
#include "redaction/RedactionData.h"
#include "ui/RedactionPanel.h"
#include <QApplication>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QTest>
#include <QSignalSpy>
#include <atomic>
#include <exception>
#include <fstream>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

namespace {

// Process id, portably — part of the unique temp-file token.
inline long pdfeProcessId()
{
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// A per-call-unique temp path: temp_dir / <stem>_<pid>_<counter>.pdf. Fixed
// names collided when the suite ran under --gtest_repeat (iteration >= 2 hit a
// still-locked file) or when two processes shared the temp dir. The pid keeps
// concurrent processes disjoint; the atomic counter keeps repeated calls within
// one process disjoint.
inline std::filesystem::path uniqueRedactTemp(const char* stem)
{
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    std::string name = std::string(stem) + "_" + std::to_string(pdfeProcessId())
                     + "_" + std::to_string(n) + ".pdf";
    return std::filesystem::temp_directory_path() / name;
}

} // namespace

// ============================================================================
// RedactionApplyTest Fixture - For apply workflow tests
// ============================================================================

class RedactionApplyTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto srcPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        if (!std::filesystem::exists(srcPath)) {
            GTEST_SKIP() << "test_redaction.pdf not found";
        }

        // Create a working copy for each test (unique path — see uniqueRedactTemp).
        tmpPath_ = uniqueRedactTemp("test_redaction_apply");
        std::filesystem::copy_file(srcPath, tmpPath_,
            std::filesystem::copy_options::overwrite_existing);

        document_ = pdfe::core::Document::open(context_, tmpPath_);
        ASSERT_NE(document_, nullptr);

        manager_ = std::make_unique<pdfe::redaction::RedactionManager>(context_);
    }

    void TearDown() override {
        pages_.clear();
        document_.reset();   // releases any bake-temp the rewrite reopened onto
        manager_.reset();

        std::error_code ec;
        std::filesystem::remove(tmpPath_, ec);
        // Sweep any derived sibling so --gtest_repeat / concurrent runs don't
        // accumulate them. Non-throwing.
        //
        // WIN-87 phase-57: this used to name ".redact_tmp"/".redact_out"
        // literally. Those names no longer exist — RedactionManager's rewrite
        // now runs entirely behind StagingFile exclusive handles at
        // unpredictable names — so a literal sweep would clean nothing while
        // still LOOKING like hygiene. The legacy names are still swept (a
        // developer switching branches can have real ones on disk) but the
        // sweep is now name-blind for anything this code path can produce
        // today, via the one shared matcher in AtomicFileReplace rather than a
        // second hand-rolled one.
        auto tmpSide = tmpPath_; tmpSide += ".redact_tmp";
        auto outSide = tmpPath_; outSide += ".redact_out";
        std::filesystem::remove(tmpSide, ec);
        std::filesystem::remove(outSide, ec);
        const auto parent = tmpPath_.parent_path();
        const auto stem = tmpPath_.filename();
        std::error_code itEc;
        std::vector<std::filesystem::path> residue;
        for (std::filesystem::directory_iterator it(parent, itEc), end;
             !itEc && it != end; it.increment(itEc)) {
            if (pdfe::core::StagingFile::isStagingResidueName(
                    stem, it->path().filename())) {
                residue.push_back(it->path());
            }
        }
        for (const auto& r : residue) {
            std::filesystem::remove(r, ec);
        }
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::redaction::RedactionManager> manager_;
    std::filesystem::path tmpPath_;
    std::vector<std::unique_ptr<pdfe::core::Page>> pages_;

    pdfe::core::Page* getPage(int index) {
        pages_.push_back(document_->page(index));
        return pages_.back().get();
    }
};

// ============================================================================
// RedactionPanelTest Fixture - For panel UI tests
// ============================================================================

class RedactionPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            GTEST_SKIP() << "QApplication not initialized";
        }
        panel_ = std::make_unique<pdfe::ui::RedactionPanel>();
    }

    void TearDown() override {
        panel_.reset();
    }

    std::unique_ptr<pdfe::ui::RedactionPanel> panel_;
};

// ============================================================================
// Apply Workflow Tests
// ============================================================================

TEST_F(RedactionApplyTest, ApplyNoMarks) {
    // Apply with no marks — should succeed trivially
    auto result = manager_->applyAllRedactions(document_.get());

    EXPECT_TRUE(result.success) << "Apply should succeed even with no marks";
    EXPECT_EQ(result.marksApplied, 0);
    EXPECT_EQ(result.pagesAffected, 0);
    EXPECT_TRUE(result.errorMessage.empty());
}

TEST_F(RedactionApplyTest, ApplySingleMark) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto marks = manager_->getRedactionMarks(page);
    ASSERT_EQ(marks.size(), 1u);

    auto result = manager_->applyAllRedactions(document_.get());

    EXPECT_TRUE(result.success) << "Apply should succeed: " << result.errorMessage;
    EXPECT_EQ(result.marksApplied, 1);
    EXPECT_EQ(result.pagesAffected, 1);

    // Marks should be consumed by apply
    auto remaining = manager_->getRedactionMarks(page);
    EXPECT_EQ(remaining.size(), 0u);
}

TEST_F(RedactionApplyTest, ApplyMultipleMarksOnSamePage) {
    auto* page = getPage(0);

    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    manager_->createRedactionMark(page, QRectF(250, 100, 100, 50));
    manager_->createRedactionMark(page, QRectF(100, 200, 100, 50));
    ASSERT_EQ(manager_->getRedactionMarks(page).size(), 3u);

    auto result = manager_->applyAllRedactions(document_.get());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 3);
    EXPECT_EQ(result.pagesAffected, 1);
}

TEST_F(RedactionApplyTest, ApplyMarksMultiplePages) {
    auto* page0 = getPage(0);
    auto* page1 = getPage(1);

    manager_->createRedactionMark(page0, QRectF(100, 100, 100, 50));
    manager_->createRedactionMark(page0, QRectF(250, 100, 100, 50));
    manager_->createRedactionMark(page1, QRectF(100, 100, 100, 50));

    ASSERT_EQ(manager_->pendingMarkCount(document_.get()), 3);

    auto result = manager_->applyAllRedactions(document_.get());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 3);
    EXPECT_EQ(result.pagesAffected, 2);
}

TEST_F(RedactionApplyTest, ApplyPageRedactions_SinglePage) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 660, 400, 30));

    EXPECT_TRUE(manager_->applyPageRedactions(page));
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 0u);
}

TEST_F(RedactionApplyTest, ApplyWithOptions) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::RedactionApplyOptions options;
    options.blackBoxes = true;
    options.imageMethod = pdfe::redaction::RedactionImageMethod::None;
    options.textMethod = pdfe::redaction::RedactionTextMethod::Remove;
    options.lineArtMethod = pdfe::redaction::RedactionLineArtMethod::RemoveIfTouched;
    options.rewriteDocument = false;

    auto result = manager_->applyAllRedactions(document_.get(), options);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 1);
    EXPECT_FALSE(result.documentRewritten);
}

TEST_F(RedactionApplyTest, ApplyWithRewriteOption) {
    // THE PAGE IS SCOPED, NOT PARKED IN `pages_`, AND THAT IS THE POINT OF THIS
    // TEST NOW. `getPage()` stores the Page in the fixture until TearDown, so it
    // would be alive across the call below; a live Page holds an
    // fz_keep_document reference, the document file therefore stays open, and
    // the publish fails with "Access is denied". That is the same P11-BUG-1
    // constraint MainWindow already honours before Document::save().
    {
        auto page = document_->page(0);
        manager_->createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
    }

    pdfe::redaction::RedactionApplyOptions options;
    options.rewriteDocument = true;

    auto result = manager_->applyAllRedactions(document_.get(), options);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 1);
    // WIN-87 phase-57 — THIS ASSERTION REPLACES A COMMENT THAT TOLERATED THE
    // EXACT FAILURE THIS PHASE FIXED. It used to read: "Note: QPDF rewrite may
    // fail when MuPDF still holds the file open. In production, the document is
    // closed/reopened around the rewrite. We verify that the rewrite attempt
    // doesn't corrupt the apply result." — i.e. the oracle accepted BOTH
    // outcomes, so it could not witness the rewrite working or stopping to
    // work.
    //
    // `Document::saveThroughExternalRewrite` drops the live document handle
    // before the commit precisely so the in-process rewrite CAN succeed, so on
    // the non-hostile path this must now be true. It is asserted rather than
    // tolerated: an oracle that accepts either outcome is not an oracle.
    EXPECT_TRUE(result.documentRewritten)
        << "the in-process rewrite must now succeed on the non-hostile path "
           "(the publish drops the document handle first): "
        << result.errorMessage;
}

// Regression for #46 — RE-POINTED BY WIN-87 PHASE-57, AND THE RE-POINTING IS
// THE WHOLE VALUE OF THIS TEST NOW.
//
// WHAT IT USED TO ASSERT. RedactionManager's rewriteDocument=true path baked the
// redaction by saving to "<path>.redact_tmp", which reopened the live Document
// ONTO that temp; the Document then held it open, so nothing could remove it
// during the call, and #46 fixed the leak by having the Document adopt it
// (registerScratchTempForCleanup) and delete it when it released its handle.
// The oracle was `!exists(<path>.redact_tmp) && !exists(<path>.redact_out)`.
//
// WHY THAT ORACLE IS NOW WORTHLESS AND MUST NOT SIMPLY BE LEFT IN PLACE.
// Phase-57 removed both names: the bake and the rewrite output each live behind
// a StagingFile exclusive handle at a CSPRNG name, and the bake is never
// published at all. Those two paths can therefore NEVER exist, so the old
// assertions pass VACUOUSLY — a check for a name nothing produces reads as
// coverage while testing nothing, which this programme has repeatedly found to
// be worse than a deleted test. Deleting it would also be wrong: the underlying
// property (a rewrite leaves NOTHING behind once the Document releases its
// handles) is still real and still worth pinning.
//
// SO IT ASSERTS ON THE DIRECTORY, name-blind, exactly as
// NoFilesystemResidueFromAnyApplyPath does — the final listing must equal the
// expected set EXACTLY, which catches residue of ANY name including the new
// staging names and including one nobody has thought of yet. What keeps this
// distinct from the class-level test is the LIFECYCLE it pins: the listing is
// taken only after the Document has been destroyed, because #46 was specifically
// about an artifact whose cleanup is deferred to the handle release.
TEST_F(RedactionApplyTest, RewriteBakeTempCleanedWhenDocumentDestroyed) {
    // Its own otherwise-empty directory: the oracle is the whole listing, so it
    // cannot share a directory with anything else.
    auto dir = std::filesystem::temp_directory_path()
             / ("pdfe_bake46_" + std::to_string(pdfeProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);

    const auto workPath = dir / "work.pdf";
    auto srcPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
    std::filesystem::copy_file(srcPath, workPath,
        std::filesystem::copy_options::overwrite_existing);

    auto listing = [&]() {
        std::set<std::string> names;
        std::error_code lec;
        for (const auto& e : std::filesystem::directory_iterator(dir, lec)) {
            names.insert(e.path().filename().string());
        }
        return names;
    };
    ASSERT_EQ(listing(), std::set<std::string>{"work.pdf"})
        << "the isolated directory was not clean before the run";

    {
        auto doc = pdfe::core::Document::open(context_, workPath);
        ASSERT_NE(doc, nullptr);
        {
            auto page = doc->page(0);
            manager_->createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
        }
        // THE PAGE IS RELEASED BEFORE THE CALL, AND THAT IS A REAL PRECONDITION,
        // NOT TIDINESS. A live Page holds an fz_keep_document reference, so the
        // fz_drop_document inside the rewrite does not actually close the file,
        // and the publish then fails with "Access is denied" — the same
        // P11-BUG-1 constraint MainWindow already honours before Document::save
        // (see the comment at MainWindow::onApplyRedactions). Holding the page
        // across the call would make this a FAILURE-path test wearing a
        // success-path name, which is what the assertion below now prevents.

        pdfe::redaction::RedactionApplyOptions options;
        options.rewriteDocument = true;  // exercise the bake path
        auto result = manager_->applyAllRedactions(doc.get(), options);
        EXPECT_TRUE(result.success);
        // VACUITY GUARD. Without this the rewrite could silently not happen and
        // "no residue" would be a statement about a path that never ran.
        EXPECT_TRUE(result.documentRewritten) << result.errorMessage;
        doc.reset();  // #46: cleanup deferred to the handle release happens here
    }

    const auto after = listing();
    EXPECT_EQ(after, std::set<std::string>{"work.pdf"})
        << "the redaction rewrite left something behind after the Document was "
           "destroyed (#46 / DCA 0010). This oracle compares the WHOLE "
           "directory, so it does not matter what the leftover is called.";
    for (const auto& n : after) {
        if (n != "work.pdf") ADD_FAILURE() << "  residue: " << n;
    }

    std::filesystem::remove_all(dir, ec);
}

// INSTANCE test for DCA 0007 (added in follow-up phase 42).
//
// DCA 0007 recorded the side-file cleanup leak but never had a case-level
// reproducing test: its "Reproducing coverage" field named the suite and the
// fixture TEARDOWN sweep, and the strengthened check_dca.py (phase 41) rightly
// refuses a citation that does not resolve to a real Suite.Case. The teardown
// sweep is not an oracle — it deletes the evidence rather than asserting on it,
// so it passes identically whether or not the product cleaned up.
//
// This pins the 0007 defect exactly: the RedactionManager rewrite path removed
// `.redact_tmp` only on the SUCCESS path (a throwing `remove`) and did no
// cleanup at all when the bake threw, so a failure left both side-files behind.
//
// WIN-87 PHASE-57 CHANGED BOTH HALVES OF THIS TEST, AND NEITHER CHANGE IS
// COSMETIC.
//
//   1. THE FORCING MECHANISM STOPPED FORCING ANYTHING. This row used to make
//      the bake fail by planting a non-empty DIRECTORY at
//      "<work>.redact_tmp" — the name the bake was about to write. The bake no
//      longer has that name, or any predictable one, so the blocker blocks
//      nothing: the redaction would SUCCEED and every assertion below would
//      hold trivially. A failure-path test that no longer reaches the failure
//      path is decoration. It is re-pointed at a failure that still exists and
//      is still deterministic: an open handle THIS process holds on the target
//      blocks the replace-existing rename that publishes the result (see
//      openDenyWriteReadStream's header and DCA 0021's closing note), so the
//      publish fails AFTER both staging artifacts have been created and
//      written — which is precisely the moment DCA 0007 is about.
//   2. THE ASSERTIONS STOPPED BEING ABLE TO FAIL. `!exists(".redact_out")` and
//      `!is_regular_file(".redact_tmp")` are now true no matter what the
//      product does, because nothing can ever create those names. They are
//      KEPT — as a regression guard against reintroducing a predictable
//      intermediate — but they can no longer carry the record, so the real
//      oracle is the whole-directory comparison beside them.
//
// The suffixes are still named on purpose: that is what makes this the INSTANCE
// test. NoFilesystemResidueFromAnyApplyPath is the class-level counterpart and
// names nothing.
TEST_F(RedactionApplyTest, RewriteExceptionPathRemovesBothSideFiles) {
    auto dir = std::filesystem::temp_directory_path()
             / ("pdfe_dca7_" + std::to_string(pdfeProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);

    const auto workPath = dir / "work.pdf";
    auto srcPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
    std::filesystem::copy_file(srcPath, workPath,
        std::filesystem::copy_options::overwrite_existing);

    const std::filesystem::path tmpSide = workPath.string() + ".redact_tmp";
    const std::filesystem::path outSide = workPath.string() + ".redact_out";

    pdfe::redaction::RedactionApplyResult result;
    {
        auto doc = pdfe::core::Document::open(context_, workPath);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        auto* rawPage = page.get();
        pages_.push_back(std::move(page));
        manager_->createRedactionMark(rawPage, QRectF(100, 660, 400, 30));

        // Force the PUBLISH to fail: hold a handle on the target that denies
        // DELETE, so the rename-over cannot land. Taken after the marks are
        // created and released before the listing, so it is not itself residue.
        std::error_code pinEc;
        std::FILE* pin = pdfe::core::openDenyWriteReadStream(workPath, pinEc);
        ASSERT_NE(pin, nullptr)
            << "could not pin the target, so this row cannot drive the failure "
               "path it exists for: " << pinEc.message();

        pdfe::redaction::RedactionApplyOptions options;
        options.rewriteDocument = true;
        try {
            result = manager_->applyAllRedactions(doc.get(), options);
        } catch (const std::exception&) {
            // The failure is the point; the cleanup is what is under test.
        }
        std::fclose(pin);
        pages_.clear();
        doc.reset();
    }

    // VACUITY GUARD. Without this, a change that made the publish succeed would
    // turn every assertion below into a statement about the success path while
    // the test name still promised the exception path.
    EXPECT_FALSE(result.documentRewritten)
        << "the publish was expected to FAIL (the target is pinned), so this "
           "row did not exercise the error path at all";
    EXPECT_FALSE(result.errorMessage.empty())
        << "a failed rewrite must say why";

    EXPECT_FALSE(std::filesystem::exists(outSide))
        << "a failed rewrite left .redact_out behind (DCA 0007): "
        << outSide.string();
    EXPECT_FALSE(std::filesystem::exists(tmpSide))
        << "a failed rewrite left a .redact_tmp behind (DCA 0007): "
        << tmpSide.string();
    // THE ORACLE THAT CAN ACTUALLY FAIL: nothing of ANY name may survive the
    // error path except the user's untouched document.
    std::set<std::string> after;
    std::error_code lec;
    for (const auto& e : std::filesystem::directory_iterator(dir, lec)) {
        after.insert(e.path().filename().string());
    }
    EXPECT_EQ(after, std::set<std::string>{"work.pdf"})
        << "the failed rewrite left residue behind (DCA 0007)";
    for (const auto& n : after) {
        if (n != "work.pdf") ADD_FAILURE() << "  residue: " << n;
    }

    std::filesystem::remove_all(dir, ec);
}

// ═════════════════════════════════════════════════════════════════════
// CLASS-LEVEL TEST for DCA 0007 and DCA 0010 (follow-up phase 42).
//
// Both records are the same family and share this test. 0007 is the leak of
// `.redact_tmp`/`.redact_out` on the error path; 0010 is the doc-held
// `.redact_tmp` that 0007 explicitly left behind as "known remaining
// limitation #46". 0007's own causal analysis names the class: *the redaction
// tests proved the secret is gone from the OUTPUT but never checked what temp
// files were left* — an end-artifact oracle with no filesystem oracle beside it.
//
// The INSTANCE tests were both keyed on the two suffixes someone already knew
// about, and the fixture TearDown swept exactly those two names. A third
// derived file — a new suffix, a scratch file in the same directory, a QPDF
// spill — was invisible to every one of them. That is a sweep keyed on
// PHRASING, and this project has watched that species come back four times
// (WIN-45).
//
// WIN-87 phase-57 PROVED that point at this site's expense: it removed both
// names outright, which would have left every suffix-keyed oracle passing
// vacuously. Both instance tests are re-pointed at whole-directory comparisons
// (see their own headers); this class-level form needed no change at all,
// because it never named anything. That asymmetry is the argument for writing
// the class-level form first.
//
// THE CLASS-LEVEL FORM asserts on the DIRECTORY, not on names. Each row runs
// one mutating path inside its own otherwise-empty directory and requires the
// final directory listing to equal the expected set EXACTLY. Any residue, of
// any name, on any path, fails — including a file nobody has thought of yet.
//
// Rows cover the success paths AND a forced-failure path, because 0007 was
// specifically a leak on the exception path: the pre-fix code removed
// `.redact_tmp` only on success.
//
// RED-ness, restated for the phase-57 shape: make either StagingFile in
// Document::saveThroughExternalRewrite outlive its scope without being
// committed — or leak one of the duplicate streams handed to the rewrite
// callback, which blocks the publish and strands the output staging file — and
// the rewrite rows leave residue. Restore the by-path relay and the rows leave
// `.redact_tmp`/`.redact_out`. None of that needs the test to know what the
// leaked file is called, which is the entire point.
TEST_F(RedactionApplyTest, NoFilesystemResidueFromAnyApplyPath) {
    auto srcPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
    ASSERT_TRUE(std::filesystem::exists(srcPath));

    // Everything present in `dir`, by filename.
    auto listing = [](const std::filesystem::path& dir) {
        std::set<std::string> names;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            names.insert(e.path().filename().string());
        }
        return names;
    };

    // Runs `body` against a fresh working copy alone in its own directory,
    // then requires the directory to contain exactly `expected` afterwards.
    // `body` receives the working path; it must release every handle it takes.
    auto expectNoResidue = [&](const char* rowName,
                               const std::set<std::string>& expected,
                               auto&& setup,
                               auto&& body) {
        auto dir = std::filesystem::temp_directory_path()
                 / ("pdfe_dca42_residue_" + std::to_string(pdfeProcessId())
                    + "_" + rowName);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);

        const auto work = dir / "work.pdf";
        std::filesystem::copy_file(srcPath, work,
            std::filesystem::copy_options::overwrite_existing);
        ASSERT_EQ(listing(dir), std::set<std::string>{"work.pdf"})
            << "[" << rowName << "] the isolated directory was not clean before "
               "the run — the oracle would report someone else's residue";

        setup(work);
        body(work);

        const auto after = listing(dir);
        EXPECT_EQ(after, expected)
            << "[" << rowName << "] the redaction path left files behind. This "
               "oracle compares the WHOLE directory, so it does not matter what "
               "the leftovers are called — every derived file must be cleaned "
               "up on every exit path (DCA 0007 / 0010).";
        if (after != expected) {
            for (const auto& n : after) {
                if (!expected.count(n)) ADD_FAILURE() << "  residue: " << n;
            }
        }
        std::filesystem::remove_all(dir, ec);
    };

    const std::set<std::string> onlyTheDocument{"work.pdf"};
    auto noSetup = [](const std::filesystem::path&) {};

    // ROW 1 — applyAllRedactions with DEFAULT options. DCA 0010 notes the API
    // default is rewriteDocument=true, i.e. the bake-temp path a future caller
    // gets without asking for it.
    expectNoResidue("default", onlyTheDocument, noSetup,
                    [&](const std::filesystem::path& work) {
        auto doc = pdfe::core::Document::open(context_, work);
        ASSERT_NE(doc, nullptr);
        {
            auto page = doc->page(0);
            manager_->createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
        }
        // The page is released BEFORE the call on purpose — a live Page holds
        // an fz_keep_document reference, the file therefore stays open, and the
        // publish fails. This row is about the SUCCESS path, so it must not
        // quietly become a failure-path row; the assertion below enforces that.
        const auto r = manager_->applyAllRedactions(doc.get());
        EXPECT_TRUE(r.documentRewritten)
            << "[default] the rewrite did not happen, so this row is no longer "
               "about the path it names: " << r.errorMessage;
        doc.reset();
    });

    // ROW 2 — explicit rewriteDocument=true (the #46 bake-temp path).
    expectNoResidue("rewrite_true", onlyTheDocument, noSetup,
                    [&](const std::filesystem::path& work) {
        auto doc = pdfe::core::Document::open(context_, work);
        ASSERT_NE(doc, nullptr);
        {
            auto page = doc->page(0);
            manager_->createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
        }
        pdfe::redaction::RedactionApplyOptions opts;
        opts.rewriteDocument = true;
        const auto r = manager_->applyAllRedactions(doc.get(), opts);
        EXPECT_TRUE(r.documentRewritten)
            << "[rewrite_true] the rewrite did not happen: " << r.errorMessage;
        doc.reset();
    });

    // ROW 3 — rewriteDocument=false (the path MainWindow/BatchEngine use).
    expectNoResidue("rewrite_false", onlyTheDocument, noSetup,
                    [&](const std::filesystem::path& work) {
        auto doc = pdfe::core::Document::open(context_, work);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        manager_->createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
        pdfe::redaction::RedactionApplyOptions opts;
        opts.rewriteDocument = false;
        manager_->applyAllRedactions(doc.get(), opts);
        page.reset();
        doc.reset();
    });

    // ROW 4 — the per-page entry point.
    expectNoResidue("apply_page", onlyTheDocument, noSetup,
                    [&](const std::filesystem::path& work) {
        auto doc = pdfe::core::Document::open(context_, work);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        manager_->createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
        manager_->applyPageRedactions(page.get());
        page.reset();
        doc.reset();
    });

    // ROW 5 — the no-marks path. It should touch nothing at all; a temp created
    // before the "nothing to do" check would show up here and nowhere else.
    expectNoResidue("no_marks", onlyTheDocument, noSetup,
                    [&](const std::filesystem::path& work) {
        auto doc = pdfe::core::Document::open(context_, work);
        ASSERT_NE(doc, nullptr);
        manager_->applyAllRedactions(doc.get());
        doc.reset();
    });

    // ROW 6 — THE FAILURE PATH, which is what DCA 0007 actually was: pre-fix,
    // `.redact_tmp` was removed only on success and the exception path removed
    // nothing.
    //
    // WIN-87 PHASE-57 RE-POINTED THE FORCING MECHANISM, AND LEAVING IT ALONE
    // WOULD HAVE BEEN THE WORSE MISTAKE. This row used to plant a non-empty
    // DIRECTORY at "work.pdf.redact_tmp" — the name the bake was about to
    // write — and listed that blocker in the expected set. The bake has no
    // predictable name any more, so the blocker would block NOTHING: the
    // redaction would succeed, the row would go green, and it would have
    // stopped being a failure-path row without saying so. Worse, the blocker
    // itself was in the expected set, so the row would keep passing while
    // asserting about a file only the test created.
    //
    // It now forces the PUBLISH to fail instead — an open handle this process
    // holds on the target blocks the replace-existing rename — which happens
    // AFTER both staging artifacts exist, so it exercises strictly more of the
    // error path than the old blocker did. Nothing but the user's document may
    // survive.
    expectNoResidue("publish_blocked",
                    std::set<std::string>{"work.pdf"},
                    noSetup,
                    [&](const std::filesystem::path& work) {
        auto doc = pdfe::core::Document::open(context_, work);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        manager_->createRedactionMark(page.get(), QRectF(100, 660, 400, 30));

        std::error_code pinEc;
        std::FILE* pin = pdfe::core::openDenyWriteReadStream(work, pinEc);
        ASSERT_NE(pin, nullptr)
            << "could not pin the target, so this row cannot drive the failure "
               "path it exists for: " << pinEc.message();

        pdfe::redaction::RedactionApplyOptions opts;
        opts.rewriteDocument = true;
        pdfe::redaction::RedactionApplyResult r;
        // Must not escape as an exception either — but the residue assertion
        // is the point, so swallow whatever the failure reports.
        try {
            r = manager_->applyAllRedactions(doc.get(), opts);
        } catch (const std::exception&) {
        }
        std::fclose(pin);
        // Vacuity guard: if the publish succeeded, this row is silently no
        // longer a failure-path row.
        EXPECT_FALSE(r.documentRewritten)
            << "[publish_blocked] the publish was expected to fail";
        page.reset();
        doc.reset();
    });

    // ROW 7 — THE SAVE ITSELF FAILING (phase-42 remediation). Rows 1-6 all
    // reach a SUCCESSFUL pdf_save_document; the only failure they drive is the
    // atomic replace. That is why they stayed green against a fix that merely
    // MOVED a bare remove() inside the replace-failure branch: two throwing
    // calls remain above it (pdf_save_document itself, and the fz_drop_document
    // that follows), and on those paths nothing cleaned up at all.
    //
    // Blocking `work.pdf.pdfe_tmp` with a non-empty DIRECTORY makes
    // pdf_save_document fail to open its output — the earliest throwing point
    // after the temp path is decided. The blocker is ours and expected to
    // survive (a non-empty directory cannot be removed); anything ELSE in the
    // folder is a leak on a path that previously had no cleanup whatsoever.
    expectNoResidue("save_target_blocked",
                    std::set<std::string>{
                        "work.pdf",
                        // The blocker's own name — the FULL-REWRITE temp of
                        // "work.pdf", because that is the path this row plants
                        // a directory on to make pdf_save_document fail.
                        pdfe::core::fullRewriteTempPathFor(
                            std::filesystem::path("work.pdf")).string()},
                    [](const std::filesystem::path& work) {
                        const auto blocker =
                            pdfe::core::fullRewriteTempPathFor(work);
                        std::filesystem::create_directories(blocker);
                        std::ofstream(std::filesystem::path(blocker) / "pin.txt")
                            << "blocks pdf_save_document's output path";
                    },
                    [&](const std::filesystem::path& work) {
        auto doc = pdfe::core::Document::open(context_, work);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        // Force the full-rewrite path (not incremental) so save() goes through
        // the temp-file strategy this row is about.
        manager_->createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
        manager_->applyPageRedactions(page.get());
        page.reset();
        try {
            doc->save(work);
        } catch (const std::exception&) {
            // Expected: the temp target is unusable. The residue assertion is
            // the point.
        }
        doc.reset();
    });
}

TEST_F(RedactionApplyTest, PendingMarkCount) {
    EXPECT_EQ(manager_->pendingMarkCount(document_.get()), 0);

    auto* page0 = getPage(0);
    auto* page1 = getPage(1);

    manager_->createRedactionMark(page0, QRectF(100, 100, 100, 50));
    EXPECT_EQ(manager_->pendingMarkCount(document_.get()), 1);

    manager_->createRedactionMark(page0, QRectF(200, 100, 100, 50));
    manager_->createRedactionMark(page1, QRectF(100, 100, 100, 50));
    EXPECT_EQ(manager_->pendingMarkCount(document_.get()), 3);

    auto result = manager_->applyAllRedactions(document_.get());
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 3);
}

TEST_F(RedactionApplyTest, VerifyAfterApply) {
    auto* page = getPage(0);

    // Create mark over text area and apply
    auto created = manager_->createRedactionMark(page, QRectF(100, 660, 400, 30));
    manager_->applyPageRedactions(page);

    // Save to a separate file (original is still locked by MuPDF)
    auto verifyPath = uniqueRedactTemp("test_redaction_verify_apply");
    document_->save(verifyPath);

    // Close the document before verification (releases file handles)
    pages_.clear();
    document_.reset();

    // Verify the saved file
    auto verifier = std::make_unique<pdfe::redaction::RedactionVerifier>(context_);
    std::vector<pdfe::redaction::RedactionData> regions;
    regions.push_back(created);

    auto vResult = verifier->verify(verifyPath, regions);
    EXPECT_TRUE(vResult.success) << "Post-apply verification should succeed: " << vResult.errorMessage;

    std::filesystem::remove(verifyPath);
}

// ============================================================================
// Panel UI Tests
// ============================================================================

TEST_F(RedactionPanelTest, ConstructDestroy) {
    EXPECT_NE(panel_.get(), nullptr);
    panel_->show();
    QTest::qWait(10);
    panel_->hide();
}

TEST_F(RedactionPanelTest, InitialState) {
    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    EXPECT_EQ(treeWidget->topLevelItemCount(), 0);

    auto* applyButton = panel_->findChild<QPushButton*>("applyRedactionsButton");
    auto* deleteButton = panel_->findChild<QPushButton*>("deleteRedactionButton");

    ASSERT_NE(applyButton, nullptr);
    ASSERT_NE(deleteButton, nullptr);

    EXPECT_FALSE(applyButton->isEnabled());
    EXPECT_FALSE(deleteButton->isEnabled());
}

TEST_F(RedactionPanelTest, SetMarks) {
    std::vector<pdfe::redaction::RedactionData> marks;

    pdfe::redaction::RedactionData mark1;
    mark1.pageIndex = 0;
    mark1.annotIndex = 0;
    mark1.rect = QRectF(100, 100, 200, 150);
    mark1.fillColor = QColor(Qt::black);
    marks.push_back(mark1);

    pdfe::redaction::RedactionData mark2;
    mark2.pageIndex = 1;
    mark2.annotIndex = 0;
    mark2.rect = QRectF(300, 200, 150, 100);
    mark2.fillColor = QColor(Qt::red);
    mark2.overlayText = "CONFIDENTIAL";
    marks.push_back(mark2);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    EXPECT_EQ(treeWidget->topLevelItemCount(), 2);
}

TEST_F(RedactionPanelTest, SetMarksColumns) {
    std::vector<pdfe::redaction::RedactionData> marks;

    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 5;
    mark.annotIndex = 0;
    mark.rect = QRectF(150.5, 200.25, 300.75, 100.0);
    mark.fillColor = QColor(Qt::red);
    mark.overlayText = "REDACTED";
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    auto* item = treeWidget->topLevelItem(0);
    ASSERT_NE(item, nullptr);

    // Column 0: Page number (expecting "Page 6" since pageIndex is 0-indexed)
    QString pageText = item->text(0);
    EXPECT_TRUE(pageText.contains("6") || pageText.contains("5"))
        << "Page column should show page number (actual: " << pageText.toStdString() << ")";

    // Column 1: Region coordinates
    QString regionText = item->text(1);
    EXPECT_FALSE(regionText.isEmpty());

    // Column 2: Color or overlay text
    QString colorText = item->text(2);
    EXPECT_FALSE(colorText.isEmpty());
}

TEST_F(RedactionPanelTest, ClearMarks) {
    std::vector<pdfe::redaction::RedactionData> marks;

    for (int i = 0; i < 3; ++i) {
        pdfe::redaction::RedactionData mark;
        mark.pageIndex = i;
        mark.annotIndex = 0;
        mark.rect = QRectF(100, 100, 200, 150);
        mark.fillColor = QColor(Qt::black);
        marks.push_back(mark);
    }

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 3);

    panel_->clear();

    EXPECT_EQ(treeWidget->topLevelItemCount(), 0);
}

TEST_F(RedactionPanelTest, ApplyButtonEnabled) {
    auto* applyButton = panel_->findChild<QPushButton*>("applyRedactionsButton");
    ASSERT_NE(applyButton, nullptr);

    EXPECT_FALSE(applyButton->isEnabled());

    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0;
    mark.annotIndex = 0;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    marks.push_back(mark);

    panel_->setMarks(marks);

    EXPECT_TRUE(applyButton->isEnabled());

    panel_->clear();
    EXPECT_FALSE(applyButton->isEnabled());
}

TEST_F(RedactionPanelTest, DeleteButtonDisabled) {
    auto* deleteButton = panel_->findChild<QPushButton*>("deleteRedactionButton");
    ASSERT_NE(deleteButton, nullptr);

    EXPECT_FALSE(deleteButton->isEnabled());

    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0;
    mark.annotIndex = 0;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    marks.push_back(mark);

    panel_->setMarks(marks);

    // Still disabled — no item selected
    EXPECT_FALSE(deleteButton->isEnabled());
}

TEST_F(RedactionPanelTest, MarkCountLabel) {
    auto* countLabel = panel_->findChild<QLabel*>("markCountLabel");
    ASSERT_NE(countLabel, nullptr);

    QString initialText = countLabel->text();
    EXPECT_TRUE(initialText.contains("No") || initialText.contains("0"));

    std::vector<pdfe::redaction::RedactionData> marks;
    for (int i = 0; i < 5; ++i) {
        pdfe::redaction::RedactionData mark;
        mark.pageIndex = 0;
        mark.annotIndex = i;
        mark.rect = QRectF(100.0 + i * 50.0, 100.0, 200.0, 150.0);
        mark.fillColor = QColor(Qt::black);
        marks.push_back(mark);
    }

    panel_->setMarks(marks);

    QString updatedText = countLabel->text();
    EXPECT_TRUE(updatedText.contains("5"))
        << "Label should show 5 marks (actual: " << updatedText.toStdString() << ")";

    panel_->clear();
    QString clearedText = countLabel->text();
    EXPECT_TRUE(clearedText.contains("No") || clearedText.contains("0"));
}

TEST_F(RedactionPanelTest, SignalEmitted) {
    QSignalSpy spy(panel_.get(), &pdfe::ui::RedactionPanel::applyAllRequested);
    ASSERT_TRUE(spy.isValid());

    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0;
    mark.annotIndex = 0;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* applyButton = panel_->findChild<QPushButton*>("applyRedactionsButton");
    ASSERT_NE(applyButton, nullptr);
    ASSERT_TRUE(applyButton->isEnabled());

    QTest::mouseClick(applyButton, Qt::LeftButton);

    EXPECT_EQ(spy.count(), 1);
    // Phase-16 fix (qt_test_gap_audit finding #22): lock the signal
    // signature explicitly by inspecting the first emission. The
    // applyAllRequested signal is parameterless on the current
    // contract; if it grows arguments, downstream wiring (using
    // QObject::connect with default-arg matching) would silently
    // break — this assertion forces a deliberate update if the
    // signature changes.
    ASSERT_GE(spy.count(), 1);
    EXPECT_EQ(spy.at(0).size(), 0)
        << "applyAllRequested signature drift: expected 0 args, got "
        << spy.at(0).size();
}
