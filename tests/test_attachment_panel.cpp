// -*- coding: utf-8 -*-
// Phase 5c UI wiring: AttachmentPanel exercise — construction,
// objectName surface for UIA, and refresh() walking an attached doc.
#include <gtest/gtest.h>
#include "ui/AttachmentPanel.h"
#include "attachments/AttachmentManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QSignalSpy>

#include <filesystem>
#include <fstream>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::attachments;
using namespace pdfe::core;

namespace {
std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}
std::filesystem::path workCopy(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_attachment_ui";
    std::filesystem::create_directories(d);
    auto p = d / name;
    std::filesystem::copy_file(fixturesDir() / "test_single.pdf", p,
        std::filesystem::copy_options::overwrite_existing);
    return p;
}
} // namespace

class AttachmentPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        if (!std::filesystem::exists(fixturesDir() / "test_single.pdf")) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        docPath = workCopy("attach_panel.pdf");
        doc = Document::open(ctx, docPath);

        // Put one attachment into the doc before the panel reads it.
        mgr = std::make_unique<AttachmentManager>(ctx);
        auto srcTxt = std::filesystem::temp_directory_path()
                    / "pdfe_attachment_ui" / "note.txt";
        std::ofstream(srcTxt, std::ios::binary) << "hello attachment";
        mgr->add(doc.get(), srcTxt, "note.txt");
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path docPath;
    std::unique_ptr<Document> doc;
    std::unique_ptr<AttachmentManager> mgr;
};

TEST_F(AttachmentPanelTest, PanelExposesNamedChildrenForAutomation)
{
    AttachmentPanel panel;
    EXPECT_EQ(panel.objectName(), QString("attachmentPanel"));
    EXPECT_NE(panel.findChild<QTreeWidget*>("attachmentTree"), nullptr);
    EXPECT_NE(panel.findChild<QPushButton*>("attachmentAddButton"), nullptr);
    EXPECT_NE(panel.findChild<QPushButton*>("attachmentExtractButton"), nullptr);
    EXPECT_NE(panel.findChild<QPushButton*>("attachmentDeleteButton"), nullptr);
}

TEST_F(AttachmentPanelTest, SetDocumentPopulatesTree)
{
    AttachmentPanel panel;
    panel.setDocument(ctx, doc.get());
    auto* tree = panel.findChild<QTreeWidget*>("attachmentTree");
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 1);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QString("note.txt"));
}

TEST_F(AttachmentPanelTest, ExtractAndDeleteButtonsDisabledWhenNothingSelected)
{
    AttachmentPanel panel;
    panel.setDocument(ctx, doc.get());
    auto* ex = panel.findChild<QPushButton*>("attachmentExtractButton");
    auto* del = panel.findChild<QPushButton*>("attachmentDeleteButton");
    ASSERT_NE(ex, nullptr);
    ASSERT_NE(del, nullptr);
    EXPECT_FALSE(ex->isEnabled());
    EXPECT_FALSE(del->isEnabled());
}

TEST_F(AttachmentPanelTest, ExtractAndDeleteBecomeEnabledOnSelect)
{
    AttachmentPanel panel;
    panel.setDocument(ctx, doc.get());
    auto* tree = panel.findChild<QTreeWidget*>("attachmentTree");
    tree->setCurrentItem(tree->topLevelItem(0));

    auto* ex = panel.findChild<QPushButton*>("attachmentExtractButton");
    auto* del = panel.findChild<QPushButton*>("attachmentDeleteButton");
    EXPECT_TRUE(ex->isEnabled());
    EXPECT_TRUE(del->isEnabled());
}

TEST_F(AttachmentPanelTest, RefreshAfterBackingStoreChangeReflectsState)
{
    AttachmentPanel panel;
    panel.setDocument(ctx, doc.get());
    auto* tree = panel.findChild<QTreeWidget*>("attachmentTree");
    ASSERT_EQ(tree->topLevelItemCount(), 1);

    // Add another attachment via the manager + trigger refresh.
    auto srcBin = std::filesystem::temp_directory_path()
                / "pdfe_attachment_ui" / "data.bin";
    std::ofstream(srcBin, std::ios::binary) << "binary";
    mgr->add(doc.get(), srcBin, "data.bin");
    panel.refresh();
    EXPECT_EQ(tree->topLevelItemCount(), 2);
}

TEST_F(AttachmentPanelTest, ClearDocumentEmptiesTree)
{
    AttachmentPanel panel;
    panel.setDocument(ctx, doc.get());
    ASSERT_EQ(panel.findChild<QTreeWidget*>("attachmentTree")->topLevelItemCount(), 1);
    panel.clearDocument();
    EXPECT_EQ(panel.findChild<QTreeWidget*>("attachmentTree")->topLevelItemCount(), 0);
    EXPECT_FALSE(panel.findChild<QPushButton*>("attachmentAddButton")->isEnabled());
}

// ---------------------------------------------------------------------------
// Phase-18 (follow-up 71): the panel must not present a truncated attachment
// list as a complete one.
//
// AttachmentManager stops walking /EmbeddedFiles when /Kids exceeds its depth
// cap or references a node already on the path. The list it returns is then a
// PREFIX of what the document claims. A document has to be built deliberately
// to produce that shape, and presenting the prefix silently is precisely what
// such a document is built to make the application do.
//
// These read the LABEL, not AttachmentManager. Asserting the manager's flag
// here would pass while the user saw nothing, which is the failure mode the
// accessor exists to prevent in the first place.
// ---------------------------------------------------------------------------

TEST_F(AttachmentPanelTest, CyclicNameTreeShowsAnIncompleteListWarning)
{
    const auto seeds = std::filesystem::path(TEST_FIXTURES_DIR).parent_path() /
                       "security" / "fuzz" / "seeds" / "fuzz_embedded_files";
    const auto cyclic = seeds / "nametree_cyclic_kids.pdf";
    ASSERT_TRUE(std::filesystem::exists(cyclic)) << cyclic.string();

    auto hostile = Document::open(ctx, cyclic);
    ASSERT_NE(hostile, nullptr);

    AttachmentPanel panel;
    panel.setDocument(ctx, hostile.get());

    const QString warning = panel.truncationWarningForTesting();
    EXPECT_FALSE(warning.isEmpty())
        << "the walk stopped early and the panel said nothing; the user is "
           "looking at a partial list presented as the whole list";
    EXPECT_TRUE(warning.contains("incomplete", Qt::CaseInsensitive))
        << "warning shown but it does not say the list is incomplete: "
        << warning.toStdString();

    auto* label = panel.findChild<QLabel*>("attachmentTruncationWarning");
    ASSERT_NE(label, nullptr) << "no stable objectName for UI automation";
    // isHidden(), not isVisible(): every child of an unshown window reports
    // isVisible()==false, which would make the negative control below pass
    // without exercising anything.
    EXPECT_FALSE(label->isHidden());
    EXPECT_EQ(label->textFormat(), Qt::PlainText)
        << "a QLabel left on Qt::AutoText renders document-shaped strings as "
           "rich text (phase-69)";
}

TEST_F(AttachmentPanelTest, AnOrdinaryDocumentShowsNoWarning)
{
    // The control. A warning shown unconditionally would satisfy the case
    // above and would train the user to ignore it — which is worse than no
    // warning, because the one real occurrence then looks like the noise.
    AttachmentPanel panel;
    panel.setDocument(ctx, doc.get());

    EXPECT_TRUE(panel.truncationWarningForTesting().isEmpty())
        << "an ordinary one-attachment document was reported as incomplete";
    auto* label = panel.findChild<QLabel*>("attachmentTruncationWarning");
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->isHidden());
}

TEST_F(AttachmentPanelTest, TheWarningClearsWhenAGoodDocumentIsLoadedAfterAHostileOne)
{
    // A warning that latches is a warning about the wrong document.
    const auto seeds = std::filesystem::path(TEST_FIXTURES_DIR).parent_path() /
                       "security" / "fuzz" / "seeds" / "fuzz_embedded_files";
    auto hostile = Document::open(ctx, seeds / "nametree_cyclic_kids.pdf");
    ASSERT_NE(hostile, nullptr);

    AttachmentPanel panel;
    panel.setDocument(ctx, hostile.get());
    ASSERT_FALSE(panel.truncationWarningForTesting().isEmpty());

    panel.setDocument(ctx, doc.get());
    EXPECT_TRUE(panel.truncationWarningForTesting().isEmpty())
        << "the warning stuck to the panel rather than to the document";
}
