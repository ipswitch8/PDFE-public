// -*- coding: utf-8 -*-
// File > Export Plain Text... — covers both the MainWindow action
// enable/disable state AND the actual file-content path via the
// standalone PlainTextExporter helper.
#include <gtest/gtest.h>
#include "ui/MainWindow.h"
#include "exports/PlainTextExporter.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QAction>
#include <QApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace {
std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}
} // namespace

TEST(ExportPlainTextTest, ActionExistsAndStartsDisabled)
{
    pdfe::ui::MainWindow mw;
    auto* action = mw.findChild<QAction*>("exportPlainTextAction");
    ASSERT_NE(action, nullptr);
    EXPECT_FALSE(action->isEnabled()) << "action must start disabled (no doc)";
}

TEST(ExportPlainTextTest, ActionEnablesAfterDocumentOpen)
{
    auto src = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }
    pdfe::ui::MainWindow mw;
    mw.openFile(QString::fromStdString(src.string()));
    QApplication::processEvents();
    auto* action = mw.findChild<QAction*>("exportPlainTextAction");
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isEnabled()) << "action must enable after document loads";
}

TEST(ExportPlainTextTest, ActionDisablesAgainAfterDocumentClose)
{
    auto src = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(src)) GTEST_SKIP();
    pdfe::ui::MainWindow mw;
    mw.openFile(QString::fromStdString(src.string()));
    QApplication::processEvents();
    auto* action = mw.findChild<QAction*>("exportPlainTextAction");
    ASSERT_NE(action, nullptr);
    ASSERT_TRUE(action->isEnabled());

    auto* closeAction = mw.findChild<QAction*>("closeAction");
    ASSERT_NE(closeAction, nullptr);
    closeAction->trigger();
    QApplication::processEvents();
    EXPECT_FALSE(action->isEnabled());
}

// -- PlainTextExporter content tests --------------------------------
// These exercise the actual write path bypass the UI, so a
// regression in page->extractPlainText() or the file-write is
// caught by the suite. Karen review flagged the original action-
// only tests as structural-not-functional coverage.

namespace {
std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST(PlainTextExporterTest, NullDocReturnsFalse)
{
    QTemporaryDir d;
    auto out = std::filesystem::path(d.path().toStdString()) / "null.txt";
    EXPECT_FALSE(pdfe::exports::PlainTextExporter::write(nullptr, out));
    EXPECT_FALSE(std::filesystem::exists(out));
}

TEST(PlainTextExporterTest, WritesUtf8BomFirst)
{
    auto src = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(src)) GTEST_SKIP();
    QTemporaryDir d;
    auto out = std::filesystem::path(d.path().toStdString()) / "bom.txt";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, src);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(pdfe::exports::PlainTextExporter::write(doc.get(), out));

    std::string content = slurp(out);
    ASSERT_GE(content.size(), 3u);
    // UTF-8 BOM = 0xEF 0xBB 0xBF
    EXPECT_EQ(static_cast<unsigned char>(content[0]), 0xEFu);
    EXPECT_EQ(static_cast<unsigned char>(content[1]), 0xBBu);
    EXPECT_EQ(static_cast<unsigned char>(content[2]), 0xBFu);
}

TEST(PlainTextExporterTest, ThreePageDocEmitsTwoPageSeparators)
{
    auto src = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(src)) GTEST_SKIP();
    QTemporaryDir d;
    auto out = std::filesystem::path(d.path().toStdString()) / "sep.txt";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, src);
    ASSERT_TRUE(pdfe::exports::PlainTextExporter::write(doc.get(), out));

    // Form-feed (0x0C) between pages — exactly one per page boundary.
    std::string content = slurp(out);
    int ff = 0;
    for (char c : content) if (c == '\x0C') ++ff;
    EXPECT_EQ(ff, doc->pageCount() - 1);
}

TEST(PlainTextExporterTest, ContentMatchesPageExtractPlainText)
{
    auto src = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(src)) GTEST_SKIP();
    QTemporaryDir d;
    auto out = std::filesystem::path(d.path().toStdString()) / "match.txt";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, src);
    ASSERT_TRUE(pdfe::exports::PlainTextExporter::write(doc.get(), out));

    std::string content = slurp(out);
    // The file must contain the text of every page. Build the
    // expected concatenation and check as-substring rather than
    // exact match because separator positioning is what other tests
    // verify.
    for (int i = 0; i < doc->pageCount(); ++i) {
        auto page = doc->page(i);
        ASSERT_NE(page, nullptr);
        const std::string pt = page->extractPlainText();
        if (!pt.empty()) {
            EXPECT_NE(content.find(pt), std::string::npos)
                << "page " << i << " text missing from export";
        }
    }
}

TEST(PlainTextExporterTest, SinglePageDocHasNoPageSeparator)
{
    auto src = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(src)) GTEST_SKIP();
    QTemporaryDir d;
    auto out = std::filesystem::path(d.path().toStdString()) / "single.txt";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, src);
    ASSERT_TRUE(pdfe::exports::PlainTextExporter::write(doc.get(), out));

    std::string content = slurp(out);
    int ff = 0;
    for (char c : content) if (c == '\x0C') ++ff;
    EXPECT_EQ(ff, 0) << "1-page doc must not have page separators";
}

TEST(PlainTextExporterTest, InvalidOutputPathReturnsFalse)
{
    auto src = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(src)) GTEST_SKIP();

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, src);
    // A path with nonexistent parent directory and no permission to
    // create it forces ofstream open failure.
    std::filesystem::path bad = "Z:/definitely/does/not/exist/out.txt";
    EXPECT_FALSE(pdfe::exports::PlainTextExporter::write(doc.get(), bad));
}
