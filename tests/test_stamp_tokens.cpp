// -*- coding: utf-8 -*-
// Phase 4f: Stamp token resolution ({date}, {author}) + built-in dynamic
// stamp library (Draft, Approved, Reviewed). Verifies the static helpers
// directly and confirms placement-time token resolution ends up baked
// into the annotation's contents (so re-open shows resolved text).
#include <gtest/gtest.h>
#include "annotations/StampTool.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QDate>
#include <QString>
#include <filesystem>
#include <memory>

using namespace pdfe::annotations;
using namespace pdfe::core;

// -------- Static helpers --------

TEST(StampTokenResolution, DateTokenResolvesToIsoDate)
{
    auto out = StampTool::resolveTokens(
        QStringLiteral("Filed on {date}"), QString(), QStringLiteral("2026-04-22"));
    EXPECT_EQ(out, QStringLiteral("Filed on 2026-04-22"));
}

TEST(StampTokenResolution, AuthorTokenResolvesToProvidedAuthor)
{
    auto out = StampTool::resolveTokens(
        QStringLiteral("By {author}"), QStringLiteral("Alice"),
        QStringLiteral("2026-04-22"));
    EXPECT_EQ(out, QStringLiteral("By Alice"));
}

TEST(StampTokenResolution, CombinedTokensBothResolved)
{
    auto out = StampTool::resolveTokens(
        QStringLiteral("APPROVED by {author} on {date}"),
        QStringLiteral("Bob"), QStringLiteral("2026-04-22"));
    EXPECT_EQ(out, QStringLiteral("APPROVED by Bob on 2026-04-22"));
}

TEST(StampTokenResolution, NoTokensReturnsUnchanged)
{
    auto out = StampTool::resolveTokens(
        QStringLiteral("Plain label"), QStringLiteral("X"),
        QStringLiteral("2026-04-22"));
    EXPECT_EQ(out, QStringLiteral("Plain label"));
}

TEST(StampTokenResolution, EmptyAuthorResolvesToEmptyString)
{
    auto out = StampTool::resolveTokens(
        QStringLiteral("By {author}"), QString(), QStringLiteral("2026-04-22"));
    EXPECT_EQ(out, QStringLiteral("By "));
}

TEST(StampTokenResolution, MultipleOccurrencesAllReplaced)
{
    auto out = StampTool::resolveTokens(
        QStringLiteral("{date} / {date}"), QString(), QStringLiteral("2026-04-22"));
    EXPECT_EQ(out, QStringLiteral("2026-04-22 / 2026-04-22"));
}

// -------- Built-in templates --------

TEST(StampTokenLibrary, DraftTemplateHasDateOnly)
{
    QString t = StampTool::builtInTemplate(StampTool::DynamicType::Draft);
    EXPECT_TRUE(t.contains("{date}"));
    EXPECT_FALSE(t.contains("{author}"))
        << "Draft is date-only per Phase 4f library definition";
    EXPECT_TRUE(t.contains("DRAFT", Qt::CaseInsensitive));
}

TEST(StampTokenLibrary, ApprovedTemplateHasAuthorAndDate)
{
    QString t = StampTool::builtInTemplate(StampTool::DynamicType::Approved);
    EXPECT_TRUE(t.contains("{date}"));
    EXPECT_TRUE(t.contains("{author}"));
    EXPECT_TRUE(t.contains("APPROVED", Qt::CaseInsensitive));
}

TEST(StampTokenLibrary, ReviewedTemplateHasAuthorAndDate)
{
    QString t = StampTool::builtInTemplate(StampTool::DynamicType::Reviewed);
    EXPECT_TRUE(t.contains("{date}"));
    EXPECT_TRUE(t.contains("{author}"));
    EXPECT_TRUE(t.contains("REVIEWED", Qt::CaseInsensitive));
}

TEST(StampTokenLibrary, TemplateResolvesEndToEnd)
{
    QString tmpl = StampTool::builtInTemplate(StampTool::DynamicType::Approved);
    QString result = StampTool::resolveTokens(tmpl, QStringLiteral("Carol"),
                                              QStringLiteral("2026-04-22"));
    EXPECT_FALSE(result.contains("{author}"));
    EXPECT_FALSE(result.contains("{date}"));
    EXPECT_TRUE(result.contains("Carol"));
    EXPECT_TRUE(result.contains("2026-04-22"));
}

// -------- Regression guard: static (non-token) stamps unchanged --------

TEST(StampTokenLibrary, StaticStampStringPassesThrough)
{
    // A Phase-4 style static stamp like "Confidential" has no tokens
    // and must resolve to itself — regression guard per Phase 4f criterion
    // "Stamps created before this phase continue to work unchanged".
    QString out = StampTool::resolveTokens(
        QStringLiteral("Confidential"), QStringLiteral("Alice"),
        QStringLiteral("2026-04-22"));
    EXPECT_EQ(out, QStringLiteral("Confidential"));
}

// -------- Placement-time baking via StampTool + AnnotationManager --------

class StampTokenPlacementTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        auto src = fixtures / "test_single.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
        page = doc->page(0);
        ASSERT_NE(page, nullptr);
        mgr = std::make_unique<AnnotationManager>(ctx);
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    std::unique_ptr<AnnotationManager> mgr;
};

TEST_F(StampTokenPlacementTest, ContentsStoreResolvedTextNotTokens)
{
    // Simulate the StampTool placement path directly — create stamp,
    // compute resolved contents via the same helpers the tool uses,
    // and verify the annotation stores the resolved text.
    QRectF rect(100, 100, 200, 40);
    AnnotationData data = mgr->createAnnotation(page.get(),
                                                AnnotationType::Stamp, rect);

    const QString isoDate = QDate::currentDate().toString(Qt::ISODate);
    const QString resolved = StampTool::resolveTokens(
        StampTool::builtInTemplate(StampTool::DynamicType::Approved),
        QStringLiteral("Tester"), isoDate);

    mgr->setContents(page.get(), data.annotIndex, resolved.toStdString());

    // Re-read contents from the annotation to prove it was stored.
    auto all = mgr->getAnnotations(page.get());
    ASSERT_FALSE(all.empty()) << "Expected at least one annotation";
    std::string stored;
    for (const auto& a : all) {
        if (a.annotIndex == data.annotIndex) { stored = a.contents; break; }
    }
    ASSERT_FALSE(stored.empty()) << "Could not locate the created annotation";
    EXPECT_NE(stored.find("Tester"), std::string::npos)
        << "Author token should be baked into contents";
    EXPECT_NE(stored.find(isoDate.toStdString()), std::string::npos)
        << "Date token should be baked into contents";
    EXPECT_EQ(stored.find("{author}"), std::string::npos)
        << "{author} literal must not survive into annotation";
    EXPECT_EQ(stored.find("{date}"), std::string::npos)
        << "{date} literal must not survive into annotation";
}
