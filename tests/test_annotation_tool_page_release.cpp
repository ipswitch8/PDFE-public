// tests/test_annotation_tool_page_release.cpp
//
// GitHub #236 — twelve AnnotationTool subclasses each declared their own
// `pdfe::core::Page* page_`, identically, and TWO of them released it when the
// document closed. The other ten kept a pointer into freed memory. That gap
// produced #234 and #235, both fatal.
//
// The fix is structural rather than repetitive: `page_` now lives in
// AnnotationTool, and `onDocumentClosing()` is NON-VIRTUAL. It releases the
// pointer and then calls the virtual `onDocumentClosingImpl()` hook. A derived
// class cannot skip the release, because there is nothing at that level for it
// to override.
//
// These tests pin that property against the base class directly, using local
// test tools. That is deliberate: a test that enumerated the twelve real tools
// would pass for the twelve that exist and say nothing about the thirteenth,
// and the thirteenth is exactly what went wrong last time. Asserting on the
// BASE covers every subclass that will ever exist.

#include <gtest/gtest.h>

#include "annotations/AnnotationTool.h"
#include "annotations/AnnotationType.h"
#include "core/Page.h"

namespace {

using pdfe::annotations::AnnotationTool;
using pdfe::annotations::AnnotationType;

// A tool that does nothing but expose what the base did to it.
class SpyTool : public AnnotationTool {
public:
    AnnotationType toolType() const override { return AnnotationType::Unknown; }
    void onMousePress(QMouseEvent*, int, QPointF) override {}
    void onMouseMove(QMouseEvent*, int, QPointF) override {}
    void onMouseRelease(QMouseEvent*, int, QPointF) override {}

    void adoptPage(pdfe::core::Page* p) { page_ = p; }
    const pdfe::core::Page* cachedPage() const { return page_; }

    int implCalls = 0;
    const pdfe::core::Page* pageSeenByImpl = reinterpret_cast<pdfe::core::Page*>(-1);

protected:
    void onDocumentClosingImpl() override {
        ++implCalls;
        pageSeenByImpl = page_;
    }
};

// The shape of every tool that got this wrong: cleanup that wants the page.
class ForgetfulTool : public AnnotationTool {
public:
    AnnotationType toolType() const override { return AnnotationType::Unknown; }
    void onMousePress(QMouseEvent*, int, QPointF) override {}
    void onMouseMove(QMouseEvent*, int, QPointF) override {}
    void onMouseRelease(QMouseEvent*, int, QPointF) override {}
    void adoptPage(pdfe::core::Page* p) { page_ = p; }
    const pdfe::core::Page* cachedPage() const { return page_; }
    // Deliberately no onDocumentClosingImpl override at all -- the ten tools
    // that crashed looked exactly like this.
};

// A page-shaped address that must never be dereferenced. Nothing here reads
// through it; the tests only compare the pointer value. This stands in for the
// real hazard, which is a pointer to memory the document has already freed --
// not something a test can safely hold for real.
pdfe::core::Page* poisonPage() {
    return reinterpret_cast<pdfe::core::Page*>(0xDEADBEEF);
}

// THE PROPERTY. Closing releases the page, for a tool that does nothing at all.
TEST(AnnotationToolPageRelease, ClosingReleasesThePageWithoutTheToolsHelp) {
    ForgetfulTool tool;
    tool.adoptPage(poisonPage());
    ASSERT_NE(tool.cachedPage(), nullptr);

    tool.onDocumentClosing();

    EXPECT_EQ(tool.cachedPage(), nullptr)
        << "GitHub #236: a tool that overrides nothing must still have its page "
           "released -- ten tools looked exactly like this one and kept a "
           "pointer into freed memory";
}

// Released BEFORE the hook, not after. This is the ordering that matters:
// FormTool's first draft of its cleanup fired a JS blur event through page_,
// which is the bug and not the cleanup. If the hook can still see the page, the
// next tool will use it.
TEST(AnnotationToolPageRelease, TheHookCannotSeeThePage) {
    SpyTool tool;
    tool.adoptPage(poisonPage());

    tool.onDocumentClosing();

    EXPECT_EQ(tool.implCalls, 1) << "the tool-specific hook must still run";
    EXPECT_EQ(tool.pageSeenByImpl, nullptr)
        << "the hook observed a live page: releasing after the hook leaves "
           "exactly the window this defect lived in";
    EXPECT_EQ(tool.cachedPage(), nullptr);
}

// Idempotent. PdfCanvas calls onDocumentClosing() from three separate sites
// (setDocument, clearDocument and the reload path), and more than one can fire
// for a single close.
TEST(AnnotationToolPageRelease, ClosingTwiceIsHarmless) {
    SpyTool tool;
    tool.adoptPage(poisonPage());

    tool.onDocumentClosing();
    tool.onDocumentClosing();

    EXPECT_EQ(tool.cachedPage(), nullptr);
    EXPECT_EQ(tool.implCalls, 2)
        << "each call must still reach the hook; suppressing the second would "
           "silently skip tool cleanup on the path that actually closes";
}

// The base must not be doing this THROUGH the virtual dispatch it replaced --
// i.e. onDocumentClosing() is not itself overridable. If someone makes it
// virtual again and a subclass overrides it, the release is skippable once
// more. This asserts the non-virtual call reaches the base implementation even
// through a base-class pointer, which is how PdfCanvas invokes it.
TEST(AnnotationToolPageRelease, ReleaseHappensThroughABaseClassPointer) {
    SpyTool tool;
    tool.adoptPage(poisonPage());

    AnnotationTool* asBase = &tool;
    asBase->onDocumentClosing();

    EXPECT_EQ(tool.cachedPage(), nullptr);
    EXPECT_EQ(tool.implCalls, 1);
}

}  // namespace
