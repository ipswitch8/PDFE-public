// -*- coding: utf-8 -*-
#include <gtest/gtest.h>
#include "ui/TextEditToolBar.h"
#include "ui/SearchPanel.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "textedit/TextEditCommands.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/SearchEngine.h"
#include "core/SearchResult.h"
#include "core/UndoManager.h"
#include <QApplication>
#include <QToolButton>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSignalSpy>
#include <QColor>
#include <QRegularExpression>
#include <filesystem>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::textedit;
using namespace pdfe::core;
using namespace pdfe::commands;

// ========== Text Color Toolbar Tests ==========

class TextColorToolBarTest : public ::testing::Test {
protected:
    void SetUp() override {
        toolbar = std::make_unique<TextEditToolBar>();
    }

    void TearDown() override {
        toolbar.reset();
    }

    std::unique_ptr<TextEditToolBar> toolbar;
};

TEST_F(TextColorToolBarTest, TextColorButtonExists) {
    auto* btn = toolbar->findChild<QToolButton*>("textColorButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_EQ(btn->objectName(), QString("textColorButton"));
}

TEST_F(TextColorToolBarTest, TextColorButtonHasTooltip) {
    auto* btn = toolbar->findChild<QToolButton*>("textColorButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->toolTip().isEmpty());
}

TEST_F(TextColorToolBarTest, SetSelectionFormattingUpdatesColor) {
    QSignalSpy spy(toolbar.get(), &TextEditToolBar::textColorChanged);
    toolbar->setSelectionFormatting("Arial", 12.0f, false, false, false, false, QColor(Qt::red));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(TextColorToolBarTest, DefaultColorIsBlack) {
    auto* btn = toolbar->findChild<QToolButton*>("textColorButton");
    ASSERT_NE(btn, nullptr);
    // Phase-16 fix (qt_test_gap_audit finding #28): anchor on
    // `color: #000000` (the TextEditToolBar swatch sets the QToolButton
    // text color to encode the picked color, not the background).
    // The previous contains("#000000") could pass on a border-color
    // declaration; this regex requires the property to actually be
    // `color` (NOT `border-color`, NOT `background-color`).
    const QString css = btn->styleSheet();
    EXPECT_TRUE(css.contains(QRegularExpression(
        QStringLiteral("(?:^|[\\s{;])color\\s*:\\s*#000000"),
        QRegularExpression::CaseInsensitiveOption)))
        << "Default swatch color must be #000000; got: "
        << css.toStdString();
}

// ========== Text Color in FormattingRun Tests ==========

TEST(FormattingRunColorTest, DefaultColorIsBlack) {
    FormattingRun run;
    EXPECT_EQ(run.textColor, 0x000000u);
}

TEST(FormattingRunColorTest, ColorPreservedInSameFormatAs) {
    FormattingRun a;
    a.textColor = 0xFF0000;
    FormattingRun b;
    b.textColor = 0xFF0000;
    EXPECT_TRUE(a.sameFormatAs(b));
}

TEST(FormattingRunColorTest, DifferentColorNotSameFormat) {
    FormattingRun a;
    a.textColor = 0xFF0000;
    FormattingRun b;
    b.textColor = 0x0000FF;
    EXPECT_FALSE(a.sameFormatAs(b));
}

// ========== ContentStreamEditor Color Tests ==========

class ContentStreamEditorColorTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        pdfPath = fixturesDir / "test_3page.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        document = Document::open(context, pdfPath);
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path pdfPath;
    std::unique_ptr<Document> document;
};

TEST_F(ContentStreamEditorColorTest, ApplyEditWithColorDoesNotCrash) {
    ContentStreamEditor editor(context.get());
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto blocks = page->extractText();
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    std::vector<TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i)
        spans.push_back(model.span(i));

    int targetIdx = -1;
    for (int i = 0; i < static_cast<int>(spans.size()); ++i) {
        if (spans[i].text.size() >= 3) { targetIdx = i; break; }
    }
    ASSERT_GE(targetIdx, 0);

    FormattingRun run;
    run.charStart = 0;
    run.charEnd = static_cast<int>(spans[targetIdx].text.size());
    run.textColor = 0xFF0000;
    spans[targetIdx].formattingRuns.push_back(run);

    std::vector<std::pair<int, std::u32string>> mods;
    mods.emplace_back(targetIdx, spans[targetIdx].text);

    EXPECT_NO_THROW(
        editor.applyEdits(page.get(), document.get(), spans, mods);
    );
}

TEST_F(ContentStreamEditorColorTest, ApplyEditWithBlackColorDoesNotCrash) {
    ContentStreamEditor editor(context.get());
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto blocks = page->extractText();
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    std::vector<TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i)
        spans.push_back(model.span(i));

    int targetIdx = -1;
    for (int i = 0; i < static_cast<int>(spans.size()); ++i) {
        if (spans[i].text.size() >= 3) { targetIdx = i; break; }
    }
    ASSERT_GE(targetIdx, 0);

    std::vector<std::pair<int, std::u32string>> mods;
    mods.emplace_back(targetIdx, spans[targetIdx].text);

    EXPECT_NO_THROW(
        editor.applyEdits(page.get(), document.get(), spans, mods);
    );
}

TEST_F(ContentStreamEditorColorTest, ColorPersistsThroughSaveAndReopen) {
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto blocks = page->extractText();
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    std::vector<TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i)
        spans.push_back(model.span(i));

    int targetIdx = -1;
    for (int i = 0; i < static_cast<int>(spans.size()); ++i) {
        if (spans[i].text.size() >= 3) { targetIdx = i; break; }
    }
    ASSERT_GE(targetIdx, 0);

    FormattingRun run;
    run.charStart = 0;
    run.charEnd = static_cast<int>(spans[targetIdx].text.size());
    run.textColor = 0xFF0000;
    spans[targetIdx].formattingRuns.push_back(run);

    ContentStreamEditor editor(context.get());
    std::u32string replacement = U"RED";
    std::vector<std::pair<int, std::u32string>> mods;
    mods.emplace_back(targetIdx, replacement);
    editor.applyEdits(page.get(), document.get(), spans, mods);

    auto tmpPath = std::filesystem::temp_directory_path() / "pdfe_color_test_persist.pdf";
    std::filesystem::remove(tmpPath);
    page.reset();
    ASSERT_NO_THROW(document->save(tmpPath));
    document.reset();

    // Verify color survives save/reload by rendering and checking for red pixels
    auto doc2 = Document::open(context, tmpPath);
    ASSERT_NE(doc2, nullptr);
    auto page2 = doc2->page(0);
    ASSERT_NE(page2, nullptr);

    auto plainText = page2->extractPlainText();
    EXPECT_NE(plainText.find("RED"), std::string::npos);

    // Render the page and check for red-dominant pixels
    QImage rendered = page2->render(page2->boundingBox(), 1.0f);
    ASSERT_FALSE(rendered.isNull());
    int redPixelCount = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            QColor c = rendered.pixelColor(x, y);
            if (c.red() > 200 && c.green() < 80 && c.blue() < 80)
                ++redPixelCount;
        }
    }
    // Phase-16 fix (qt_test_gap_audit finding #29): tighter floor —
    // a single stray red pixel would pass the > 0 threshold even if
    // all the "RED" text became black. Calibrated to expected glyph
    // size (3 chars * ~10px height = ~300 ideal red pixels with
    // anti-aliasing; conservative floor at 50 for thin strokes).
    EXPECT_GT(redPixelCount, 50)
        << "Expected >50 red pixels from the 'RED' text run; got "
        << redPixelCount << " — rg color operator did not persist "
        << "through save/reload.";

    page2.reset();
    doc2.reset();
    std::filesystem::remove(tmpPath);
}

// ========== SearchPanel Replace UI Tests ==========

class SearchPanelReplaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        panel = std::make_unique<SearchPanel>();
    }

    void TearDown() override {
        panel.reset();
    }

    std::unique_ptr<SearchPanel> panel;
};

TEST_F(SearchPanelReplaceTest, ReplaceFieldExists) {
    auto* field = panel->findChild<QLineEdit*>("replaceField");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->accessibleName(), QString("Replace with"));
}

TEST_F(SearchPanelReplaceTest, ReplaceButtonExists) {
    auto* btn = panel->findChild<QPushButton*>("replaceButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->isEnabled());
}

TEST_F(SearchPanelReplaceTest, ReplaceAllButtonExists) {
    auto* btn = panel->findChild<QPushButton*>("replaceAllButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->isEnabled());
}

TEST_F(SearchPanelReplaceTest, ReplaceStatusLabelExists) {
    auto* label = panel->findChild<QLabel*>("replaceStatusLabel");
    ASSERT_NE(label, nullptr);
}

TEST_F(SearchPanelReplaceTest, ReplaceButtonHasAccessibleName) {
    auto* btn = panel->findChild<QPushButton*>("replaceButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_EQ(btn->accessibleName(), QString("Replace"));
}

TEST_F(SearchPanelReplaceTest, ReplaceAllButtonHasAccessibleName) {
    auto* btn = panel->findChild<QPushButton*>("replaceAllButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_EQ(btn->accessibleName(), QString("Replace All"));
}

TEST_F(SearchPanelReplaceTest, ReplaceStatusLabelHasAccessibleName) {
    auto* label = panel->findChild<QLabel*>("replaceStatusLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->accessibleName(), QString("Replace Status"));
}

TEST_F(SearchPanelReplaceTest, SetReplaceStatusUpdatesLabel) {
    panel->setReplaceStatus("Replaced 3 matches");
    auto* label = panel->findChild<QLabel*>("replaceStatusLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->text(), QString("Replaced 3 matches"));
}

TEST_F(SearchPanelReplaceTest, ReplaceSignalEmitted) {
    QSignalSpy spy(panel.get(), &SearchPanel::replaceRequested);
    auto* searchEdit = panel->findChild<QLineEdit*>("searchEdit");
    ASSERT_NE(searchEdit, nullptr);
    searchEdit->setText("hello");

    auto* replaceField = panel->findChild<QLineEdit*>("replaceField");
    ASSERT_NE(replaceField, nullptr);
    replaceField->setText("world");

    auto* btn = panel->findChild<QPushButton*>("replaceButton");
    ASSERT_NE(btn, nullptr);
    btn->setEnabled(true);
    btn->click();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), QString("hello"));
    EXPECT_EQ(spy.at(0).at(1).toString(), QString("world"));
}

TEST_F(SearchPanelReplaceTest, ReplaceAllSignalEmitted) {
    QSignalSpy spy(panel.get(), &SearchPanel::replaceAllRequested);
    auto* searchEdit = panel->findChild<QLineEdit*>("searchEdit");
    ASSERT_NE(searchEdit, nullptr);
    searchEdit->setText("foo");

    auto* replaceField = panel->findChild<QLineEdit*>("replaceField");
    ASSERT_NE(replaceField, nullptr);
    replaceField->setText("bar");

    auto* btn = panel->findChild<QPushButton*>("replaceAllButton");
    ASSERT_NE(btn, nullptr);
    btn->setEnabled(true);
    btn->click();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), QString("foo"));
    EXPECT_EQ(spy.at(0).at(1).toString(), QString("bar"));
}

TEST_F(SearchPanelReplaceTest, EmptyFindDoesNotEmitReplace) {
    QSignalSpy spy(panel.get(), &SearchPanel::replaceRequested);
    auto* btn = panel->findChild<QPushButton*>("replaceButton");
    ASSERT_NE(btn, nullptr);
    btn->setEnabled(true);
    btn->click();
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(SearchPanelReplaceTest, EmptyFindDoesNotEmitReplaceAll) {
    QSignalSpy spy(panel.get(), &SearchPanel::replaceAllRequested);
    auto* btn = panel->findChild<QPushButton*>("replaceAllButton");
    ASSERT_NE(btn, nullptr);
    btn->setEnabled(true);
    btn->click();
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(SearchPanelReplaceTest, ClearResetsReplaceFields) {
    auto* replaceField = panel->findChild<QLineEdit*>("replaceField");
    ASSERT_NE(replaceField, nullptr);
    replaceField->setText("something");

    auto* clearBtn = panel->findChild<QPushButton*>("clearButton");
    ASSERT_NE(clearBtn, nullptr);
    clearBtn->setEnabled(true);
    clearBtn->click();

    EXPECT_TRUE(replaceField->text().isEmpty());
}

TEST_F(SearchPanelReplaceTest, EmptyReplaceStringAllowed) {
    QSignalSpy spy(panel.get(), &SearchPanel::replaceAllRequested);
    auto* searchEdit = panel->findChild<QLineEdit*>("searchEdit");
    searchEdit->setText("delete_me");

    auto* btn = panel->findChild<QPushButton*>("replaceAllButton");
    btn->setEnabled(true);
    btn->click();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(1).toString(), QString(""));
}

TEST_F(SearchPanelReplaceTest, CurrentResultIndexWithNoResults) {
    EXPECT_EQ(panel->currentResultIndex(), -1);
}

TEST_F(SearchPanelReplaceTest, CurrentSearchResultWithNoSearch) {
    EXPECT_EQ(panel->currentSearchResult(), nullptr);
}

// ========== Replace with Undo Tests ==========

class ReplaceUndoTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        pdfPath = fixturesDir / "test_3page.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        document = Document::open(context, pdfPath);
        undoManager = std::make_unique<UndoManager>();
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path pdfPath;
    std::unique_ptr<Document> document;
    std::unique_ptr<UndoManager> undoManager;
};

TEST_F(ReplaceUndoTest, ReplaceIsUndoable) {
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);
    auto blocks = page->extractText();
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    const auto& span = model.span(0);
    ASSERT_FALSE(span.text.empty());

    std::u32string newText = U"REPLACED";

    auto cmd = std::make_unique<EditTextCommand>(
        document.get(), context, 0, 0, span, span.text, newText);
    undoManager->execute(std::move(cmd));

    EXPECT_TRUE(undoManager->canUndo());

    undoManager->undo();
    EXPECT_TRUE(undoManager->canRedo());
}

TEST_F(ReplaceUndoTest, MultipleReplacesAreIndividuallyUndoable) {
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);
    auto blocks = page->extractText();
    TextEditModel model(blocks);

    int count = std::min(3, model.spanCount());
    ASSERT_GT(count, 0);

    for (int i = 0; i < count; ++i) {
        const auto& span = model.span(i);
        if (span.text.empty()) continue;
        auto cmd = std::make_unique<EditTextCommand>(
            document.get(), context, 0, i, span,
            span.text, U"EDIT_" + std::u32string(1, U'0' + i));
        undoManager->execute(std::move(cmd));
    }

    for (int i = 0; i < count; ++i) {
        EXPECT_TRUE(undoManager->canUndo());
        undoManager->undo();
    }
}

// ========== CompositeCommand (Replace All single-undo) Tests ==========

TEST(CompositeCommandTest, SingleUndoRevertsAll) {
    UndoManager manager;
    auto composite = std::make_unique<CompositeCommand>("Replace All");

    int counter = 0;
    struct CountCommand : public Command {
        int& val_;
        int delta_;
        CountCommand(int& v, int d) : val_(v), delta_(d) {}
        void execute() override { val_ += delta_; }
        void undo() override { val_ -= delta_; }
        std::string description() const override { return "count"; }
    };

    composite->addCommand(std::make_unique<CountCommand>(counter, 10));
    composite->addCommand(std::make_unique<CountCommand>(counter, 20));
    composite->addCommand(std::make_unique<CountCommand>(counter, 30));

    manager.execute(std::move(composite));
    EXPECT_EQ(counter, 60);
    EXPECT_EQ(manager.undoStackSize(), 1);

    manager.undo();
    EXPECT_EQ(counter, 0);
    EXPECT_FALSE(manager.canUndo());
    EXPECT_TRUE(manager.canRedo());
}

TEST(CompositeCommandTest, RedoRestoresAll) {
    UndoManager manager;
    auto composite = std::make_unique<CompositeCommand>("Replace All");

    int counter = 0;
    struct CountCommand : public Command {
        int& val_;
        int delta_;
        CountCommand(int& v, int d) : val_(v), delta_(d) {}
        void execute() override { val_ += delta_; }
        void undo() override { val_ -= delta_; }
        void redo() override { execute(); }
        std::string description() const override { return "count"; }
    };

    composite->addCommand(std::make_unique<CountCommand>(counter, 5));
    composite->addCommand(std::make_unique<CountCommand>(counter, 15));

    manager.execute(std::move(composite));
    EXPECT_EQ(counter, 20);

    manager.undo();
    EXPECT_EQ(counter, 0);

    manager.redo();
    EXPECT_EQ(counter, 20);
}

TEST(CompositeCommandTest, EmptyCompositeIsEmpty) {
    CompositeCommand composite("empty");
    EXPECT_TRUE(composite.empty());
    EXPECT_NO_THROW(composite.execute());
    EXPECT_NO_THROW(composite.undo());
}

TEST(CompositeCommandTest, DescriptionPreserved) {
    CompositeCommand composite("Replace All");
    EXPECT_EQ(composite.description(), "Replace All");
}

// ========== SearchPanel selectResult Tests ==========

TEST_F(SearchPanelReplaceTest, SelectResultAdvancesIndex) {
    // selectResult with no results doesn't crash
    panel->selectResult(0);
    EXPECT_EQ(panel->currentResultIndex(), -1);
}

// ========== Empty Replace String with Undo ==========

TEST_F(ReplaceUndoTest, EmptyReplacementIsUndoable) {
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);
    auto blocks = page->extractText();
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    const auto& span = model.span(0);
    ASSERT_FALSE(span.text.empty());

    auto cmd = std::make_unique<EditTextCommand>(
        document.get(), context, 0, 0, span, span.text, U"");
    undoManager->execute(std::move(cmd));

    EXPECT_TRUE(undoManager->canUndo());
    undoManager->undo();
    EXPECT_TRUE(undoManager->canRedo());
}
