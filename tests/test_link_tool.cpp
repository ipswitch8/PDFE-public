// -*- coding: utf-8 -*-
#include <gtest/gtest.h>
#include "annotations/LinkTool.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include <filesystem>
#include <memory>

using namespace pdfe::annotations;
using namespace pdfe::core;

TEST(LinkAnnotationTypeTest, LinkIsInEnum) {
    EXPECT_NE(static_cast<int>(AnnotationType::Link), static_cast<int>(AnnotationType::Unknown));
}

TEST(LinkAnnotationTypeTest, LinkHasDisplayName) {
    EXPECT_EQ(annotationTypeName(AnnotationType::Link), "Link");
}

TEST(LinkAnnotationTypeTest, LinkRoundTrip) {
    int pdfType = toPdfAnnotType(AnnotationType::Link);
    AnnotationType back = fromPdfAnnotType(pdfType);
    EXPECT_EQ(back, AnnotationType::Link);
}

class LinkToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx;
};

TEST_F(LinkToolTest, ToolTypeIsLink) {
    LinkTool tool(nullptr);
    EXPECT_EQ(tool.toolType(), AnnotationType::Link);
}

TEST_F(LinkToolTest, SetDefaultUri) {
    LinkTool tool(nullptr);
    tool.setDefaultUri("https://example.com");
    // No getter but we can verify no crash
    SUCCEED();
}
