#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/Error.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace pdfe::core;

class DocumentTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
    }

    std::shared_ptr<MuPdfContext> context;
};

TEST_F(DocumentTest, OpenNonExistentFileThrows) {
    std::filesystem::path invalidPath = "C:\\nonexistent\\file.pdf";

    EXPECT_THROW(
        Document::open(context, invalidPath),
        PdfeException
    );
}

TEST_F(DocumentTest, OpenWithNullContextThrows) {
    std::filesystem::path somePath = "test.pdf";

    EXPECT_THROW(
        Document::open(nullptr, somePath),
        PdfeException
    );
}

// Regression test for phase-3 svg_polyglot/test14.svg: PDFE used to
// hand any input to MuPDF for format-sniffing, which let polyglot
// SVG/MSL files trigger Qt/MuPDF SVG handling that resolved
// attacker-controlled hostnames via the OS network stack. Refusing
// non-PDF inputs at Document::open closes that vector.
TEST_F(DocumentTest, OpenNonPdfMagicBytesThrows) {
    auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_test_magic";
    std::filesystem::create_directories(tmpDir);
    auto svgPath = tmpDir / "fake.pdf";
    {
        std::ofstream f(svgPath, std::ios::binary);
        f << "<svg xmlns=\"http://www.w3.org/2000/svg\"><image href=\"x\"/></svg>";
    }
    try {
        EXPECT_THROW(
            Document::open(context, svgPath),
            PdfeException
        );
    } catch (...) {}
    std::filesystem::remove(svgPath);
}

TEST_F(DocumentTest, OpenEmptyFileThrows) {
    auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_test_magic";
    std::filesystem::create_directories(tmpDir);
    auto emptyPath = tmpDir / "empty.pdf";
    {
        std::ofstream f(emptyPath, std::ios::binary);
    }
    try {
        EXPECT_THROW(
            Document::open(context, emptyPath),
            PdfeException
        );
    } catch (...) {}
    std::filesystem::remove(emptyPath);
}

TEST_F(DocumentTest, OpenAcceptsPdfWithSmallPreamble) {
    // PDF spec allows a small preamble before the %PDF- magic
    // (legacy SMTP/MIME-encoded PDFs). Our magic-byte scan walks
    // the first 1024 bytes so this case continues to work.
    auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_test_magic";
    std::filesystem::create_directories(tmpDir);
    auto preamblePath = tmpDir / "preamble.pdf";
    auto fixturesDir = std::filesystem::path(TEST_FIXTURES_DIR);
    auto realPdf = fixturesDir / "test_blank.pdf";
    if (!std::filesystem::exists(realPdf)) {
        GTEST_SKIP() << "fixture missing";
    }
    // Concatenate "junk\n" + real PDF bytes.
    std::ifstream src(realPdf, std::ios::binary);
    std::ofstream dst(preamblePath, std::ios::binary);
    dst << "MIME-Version: 1.0\r\nContent-Type: application/pdf\r\n\r\n";
    dst << src.rdbuf();
    dst.close();
    EXPECT_NO_THROW({
        auto doc = Document::open(context, preamblePath);
        EXPECT_GE(doc->pageCount(), 1);
    });
    std::filesystem::remove(preamblePath);
}

TEST_F(DocumentTest, OpenValidPdfDocument) {
    // Check if test fixture exists
    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path testPdf = fixturesDir / "test_3page.pdf";

    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto doc = Document::open(context, testPdf);

    ASSERT_NE(doc, nullptr);
    EXPECT_GT(doc->pageCount(), 0);
    EXPECT_EQ(doc->filePath(), testPdf);
    EXPECT_FALSE(doc->isModified());
}

TEST_F(DocumentTest, GetPageInfo) {
    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path testPdf = fixturesDir / "test_3page.pdf";

    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto doc = Document::open(context, testPdf);

    ASSERT_GT(doc->pageCount(), 0);

    PageInfo info = doc->pageInfo(0);

    EXPECT_EQ(info.index, 0);
    EXPECT_GT(info.widthPt, 0.0f);
    EXPECT_GT(info.heightPt, 0.0f);
    EXPECT_GE(info.rotation, 0);
    EXPECT_LE(info.rotation, 270);
}

TEST_F(DocumentTest, GetPageInfoInvalidIndexThrows) {
    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path testPdf = fixturesDir / "test_3page.pdf";

    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto doc = Document::open(context, testPdf);

    EXPECT_THROW(
        doc->pageInfo(-1),
        PdfeException
    );

    EXPECT_THROW(
        doc->pageInfo(doc->pageCount()),
        PdfeException
    );
}

TEST_F(DocumentTest, LoadPage) {
    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path testPdf = fixturesDir / "test_3page.pdf";

    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto doc = Document::open(context, testPdf);

    ASSERT_GT(doc->pageCount(), 0);

    auto page = doc->page(0);

    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->index(), 0);
}

TEST_F(DocumentTest, LoadPageInvalidIndexThrows) {
    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path testPdf = fixturesDir / "test_3page.pdf";

    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto doc = Document::open(context, testPdf);

    EXPECT_THROW(
        doc->page(-1),
        PdfeException
    );

    EXPECT_THROW(
        doc->page(doc->pageCount()),
        PdfeException
    );
}

TEST_F(DocumentTest, DocumentIsMovable) {
    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path testPdf = fixturesDir / "test_3page.pdf";

    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto doc1 = Document::open(context, testPdf);
    int pageCount = doc1->pageCount();

    // Move constructor
    auto doc2 = std::move(doc1);
    EXPECT_EQ(doc2->pageCount(), pageCount);

    // Move assignment
    auto doc3 = Document::open(context, testPdf);
    doc3 = std::move(doc2);
    EXPECT_EQ(doc3->pageCount(), pageCount);
}

TEST_F(DocumentTest, ContextAccessor) {
    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path testPdf = fixturesDir / "test_3page.pdf";

    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto doc = Document::open(context, testPdf);

    EXPECT_EQ(doc->context(), context);
}

// Phase-3 (option-1, J investigation): UNC and remote-scheme inputs
// must be refused at Document::open before any std::filesystem call
// or MuPDF tokenization. The kernel resolver fires NetBIOS / LLMNR /
// mDNS broadcasts the moment a UNC is canonicalized, attributed to
// PDFE.exe. These tests verify the gate is in place.
TEST_F(DocumentTest, OpenRefusesUncBackslash) {
    EXPECT_THROW(
        Document::open(context, std::filesystem::path("\\\\server\\share\\x.pdf")),
        PdfeException
    );
}

TEST_F(DocumentTest, OpenRefusesUncForwardSlash) {
    EXPECT_THROW(
        Document::open(context, std::filesystem::path("//server/share/x.pdf")),
        PdfeException
    );
}

TEST_F(DocumentTest, OpenRefusesFileSchemeUnc) {
    EXPECT_THROW(
        Document::open(context, std::filesystem::path("file://attacker.example/share/x.pdf")),
        PdfeException
    );
}

TEST_F(DocumentTest, OpenRefusesFileSchemeFourSlashUnc) {
    EXPECT_THROW(
        Document::open(context, std::filesystem::path("file:////attacker.example/share/x.pdf")),
        PdfeException
    );
}

TEST_F(DocumentTest, OpenRefusesHttpScheme) {
    EXPECT_THROW(
        Document::open(context, std::filesystem::path("http://attacker.example/x.pdf")),
        PdfeException
    );
}

TEST_F(DocumentTest, OpenRefusesSmbScheme) {
    EXPECT_THROW(
        Document::open(context, std::filesystem::path("smb://attacker.example/share/x.pdf")),
        PdfeException
    );
}

// ── Issue #125: markPageDirty() actually notifies ────────────────────────────
//
// It used to insert into a set nothing consulted. ~14 call sites called it
// believing they had signalled a repaint; the only thing that stopped a stale
// canvas was a hand-written PdfCanvas::refreshPage() at each MainWindow call
// site. "Tile cache not invalidated -> content edits not visually shown" is a
// bug this project has shipped four times
// (docs/audit/seed-catalog-git.md G-02/G-03/G-10/G-11).
//
// These pin the Document half of the fix. The PdfCanvas half is covered
// separately; here the question is only "does the signal exist and behave".

namespace {
std::filesystem::path pageDirtyFixture() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
}
}  // namespace

TEST_F(DocumentTest, MarkPageDirtyNotifiesEveryRegisteredListener) {
    auto doc = Document::open(context, pageDirtyFixture());
    ASSERT_NE(doc, nullptr);

    std::vector<int> first, second;
    const auto t1 = doc->addPageDirtyListener([&](int p) { first.push_back(p); });
    const auto t2 = doc->addPageDirtyListener([&](int p) { second.push_back(p); });
    ASSERT_NE(t1, 0u);
    ASSERT_NE(t2, 0u);
    ASSERT_NE(t1, t2) << "each registration must get its own token";

    doc->markPageDirty(2);
    doc->markPageDirty(0);

    // Both listeners, in order, with the page index the caller named. The index
    // is the whole point: a subscriber that cannot tell WHICH page changed has
    // to invalidate everything.
    EXPECT_EQ(first, (std::vector<int>{2, 0}));
    EXPECT_EQ(second, (std::vector<int>{2, 0}));

    doc->removePageDirtyListener(t1);
    doc->removePageDirtyListener(t2);
}

TEST_F(DocumentTest, RemovingAPageDirtyListenerStopsOnlyThatOne) {
    auto doc = Document::open(context, pageDirtyFixture());
    ASSERT_NE(doc, nullptr);

    int removedCalls = 0, keptCalls = 0;
    const auto removed = doc->addPageDirtyListener([&](int) { ++removedCalls; });
    const auto kept    = doc->addPageDirtyListener([&](int) { ++keptCalls; });

    doc->markPageDirty(1);
    ASSERT_EQ(removedCalls, 1);
    ASSERT_EQ(keptCalls, 1);

    doc->removePageDirtyListener(removed);
    doc->markPageDirty(1);

    EXPECT_EQ(removedCalls, 1)
        << "a removed listener still received a notification — removal is what "
           "PdfCanvas relies on to not dangle across a document swap";
    EXPECT_EQ(keptCalls, 2)
        << "removing one listener silenced another";

    // Removal must be idempotent: teardown paths call it unconditionally.
    EXPECT_NO_THROW(doc->removePageDirtyListener(removed));
    EXPECT_NO_THROW(doc->removePageDirtyListener(0));
    doc->removePageDirtyListener(kept);
}

// The listener must run with docMutex RELEASED. A repaint listener naturally
// wants to ask the document questions, and std::mutex is not recursive, so
// firing under the lock is a hang rather than an error.
//
// The probe is deliberately shaped so the BAD case fails instead of hanging the
// suite: a second thread calls a Document API that takes docMutex, and the
// listener waits a bounded 2 s for it. Under a fire-under-lock mutant that wait
// times out and the assertion fails; the probe thread is then joined AFTER
// markPageDirty() has returned and released the lock, so it always completes.
TEST_F(DocumentTest, PageDirtyListenerRunsWithTheDocumentLockReleased) {
    auto doc = Document::open(context, pageDirtyFixture());
    ASSERT_NE(doc, nullptr);

    std::shared_ptr<std::future<void>> probe;
    std::atomic<bool> probeFinishedInTime{false};

    // pageInfo(), NOT pageCount(). pageCount() returns a cached int and never
    // touches docMutex, so a probe built on it passes whether the lock is held
    // or not — measured: the fire-under-lock mutant survived it. The probe has
    // to call something that actually contends for the lock.
    const auto token = doc->addPageDirtyListener([&](int) {
        probe = std::make_shared<std::future<void>>(std::async(
            std::launch::async, [&] { (void)doc->pageInfo(0); }));
        probeFinishedInTime =
            (probe->wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    });

    doc->markPageDirty(0);
    if (probe) probe->wait();  // lock is released by now; joins unconditionally
    doc->removePageDirtyListener(token);

    EXPECT_TRUE(probeFinishedInTime.load())
        << "a Document call from another thread could not complete while a "
           "page-dirty listener was running — the listener is being invoked "
           "with docMutex still held, which deadlocks any listener that touches "
           "the document it was just told about";
}

// A throwing listener must not turn a successful edit into a failed one.
TEST_F(DocumentTest, ThrowingPageDirtyListenerDoesNotBreakTheEdit) {
    auto doc = Document::open(context, pageDirtyFixture());
    ASSERT_NE(doc, nullptr);

    int laterCalls = 0;
    const auto bad  = doc->addPageDirtyListener(
        [](int) { throw std::runtime_error("listener blew up"); });
    const auto good = doc->addPageDirtyListener([&](int) { ++laterCalls; });

    EXPECT_NO_THROW(doc->markPageDirty(1));
    EXPECT_EQ(laterCalls, 1)
        << "a throwing listener suppressed a later one";

    doc->removePageDirtyListener(bad);
    doc->removePageDirtyListener(good);
}
