// -*- coding: utf-8 -*-
// Phase 8a: comment threading — /IRT reply-chain round-trip through save
// and reload, setInReplyTo / getInReplyTo correctness, and robustness on
// missing parents.
#include <gtest/gtest.h>
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <memory>

using namespace pdfe::annotations;
using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path tempOut(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_threading_tests";
    std::filesystem::create_directories(d);
    return d / name;
}

} // namespace

class CommentThreadingTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto src = fixturesDir() / "test_single.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        work = tempOut("thread_work.pdf");
        std::filesystem::copy_file(src, work,
            std::filesystem::copy_options::overwrite_existing);
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path work;
};

TEST_F(CommentThreadingTest, SetAndGetInReplyToOnSamePage)
{
    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    AnnotationManager mgr(ctx);
    // Parent sticky note
    auto parent = mgr.createAnnotation(page.get(), AnnotationType::Text,
                                       QRectF(100, 100, 20, 20));
    mgr.setContents(page.get(), parent.annotIndex, "Parent comment");
    // Reply
    auto reply = mgr.createAnnotation(page.get(), AnnotationType::Text,
                                      QRectF(130, 100, 20, 20));
    mgr.setContents(page.get(), reply.annotIndex, "Reply body");

    // Initially, the reply has no IRT.
    EXPECT_EQ(mgr.getInReplyTo(page.get(), reply.annotIndex), -1);

    mgr.setInReplyTo(page.get(), reply.annotIndex, parent.annotIndex);
    EXPECT_EQ(mgr.getInReplyTo(page.get(), reply.annotIndex),
              parent.annotIndex);
}

TEST_F(CommentThreadingTest, SetInReplyToMinusOneClearsIrt)
{
    auto doc = Document::open(ctx, work);
    auto page = doc->page(0);
    AnnotationManager mgr(ctx);
    auto parent = mgr.createAnnotation(page.get(), AnnotationType::Text,
                                       QRectF(100, 100, 20, 20));
    auto reply = mgr.createAnnotation(page.get(), AnnotationType::Text,
                                      QRectF(130, 100, 20, 20));
    mgr.setInReplyTo(page.get(), reply.annotIndex, parent.annotIndex);
    ASSERT_EQ(mgr.getInReplyTo(page.get(), reply.annotIndex),
              parent.annotIndex);

    mgr.setInReplyTo(page.get(), reply.annotIndex, -1);
    EXPECT_EQ(mgr.getInReplyTo(page.get(), reply.annotIndex), -1);
}

TEST_F(CommentThreadingTest, IrtSurvivesSaveAndReload)
{
    // Attach a reply, save the PDF, reopen, verify the reply still
    // points at the parent.
    int parentIdx = -1;
    int replyIdx  = -1;
    {
        auto doc = Document::open(ctx, work);
        auto page = doc->page(0);
        AnnotationManager mgr(ctx);
        auto parent = mgr.createAnnotation(page.get(), AnnotationType::Text,
                                           QRectF(100, 100, 20, 20));
        mgr.setContents(page.get(), parent.annotIndex, "Original");
        auto reply = mgr.createAnnotation(page.get(), AnnotationType::Text,
                                          QRectF(130, 100, 20, 20));
        mgr.setContents(page.get(), reply.annotIndex, "Reply-text");
        mgr.setInReplyTo(page.get(), reply.annotIndex, parent.annotIndex);
        parentIdx = parent.annotIndex;
        replyIdx  = reply.annotIndex;

        auto saved = tempOut("thread_saved.pdf");
        doc->saveAs(saved);
        ASSERT_TRUE(std::filesystem::exists(saved));
        work = saved;
    }

    auto doc2 = Document::open(ctx, work);
    auto page2 = doc2->page(0);
    AnnotationManager mgr2(ctx);
    const auto annots = mgr2.getAnnotations(page2.get());
    ASSERT_EQ(annots.size(), 2u);

    // Find reply by contents; parent by contents.
    int foundReplyIdx = -1;
    int foundParentIdx = -1;
    for (const auto& a : annots) {
        if (a.contents == "Reply-text") foundReplyIdx = a.annotIndex;
        if (a.contents == "Original")   foundParentIdx = a.annotIndex;
    }
    ASSERT_GE(foundReplyIdx, 0);
    ASSERT_GE(foundParentIdx, 0);

    // getInReplyTo resolves the persisted /IRT back to the parent's index.
    EXPECT_EQ(mgr2.getInReplyTo(page2.get(), foundReplyIdx), foundParentIdx);

    // The readAnnotationData path populates AnnotationData.inReplyToIndex
    // too — check the cached field agrees.
    for (const auto& a : annots) {
        if (a.annotIndex == foundReplyIdx) {
            EXPECT_EQ(a.inReplyToIndex, foundParentIdx);
        }
        if (a.annotIndex == foundParentIdx) {
            EXPECT_EQ(a.inReplyToIndex, -1);
        }
    }
}

TEST_F(CommentThreadingTest, GetInReplyToOnNonReplyReturnsMinusOne)
{
    auto doc = Document::open(ctx, work);
    auto page = doc->page(0);
    AnnotationManager mgr(ctx);
    auto a = mgr.createAnnotation(page.get(), AnnotationType::Text,
                                  QRectF(100, 100, 20, 20));
    EXPECT_EQ(mgr.getInReplyTo(page.get(), a.annotIndex), -1);
}

TEST_F(CommentThreadingTest, SetInReplyToOutOfRangeThrows)
{
    auto doc = Document::open(ctx, work);
    auto page = doc->page(0);
    AnnotationManager mgr(ctx);
    auto a = mgr.createAnnotation(page.get(), AnnotationType::Text,
                                  QRectF(100, 100, 20, 20));
    EXPECT_THROW(mgr.setInReplyTo(page.get(), 99, a.annotIndex), std::exception);
    EXPECT_THROW(mgr.setInReplyTo(page.get(), a.annotIndex, 99), std::exception);
}
