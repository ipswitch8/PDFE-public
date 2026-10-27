#include <gtest/gtest.h>
#include "ui/TextEditToolBar.h"
#include <QApplication>
#include <QFontComboBox>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <QSignalSpy>
#include <QLineEdit>
#include <memory>

using namespace pdfe::ui;

class TextEditToolBarTest : public ::testing::Test {
protected:
    void SetUp() override {
        toolbar = std::make_unique<TextEditToolBar>();
    }

    void TearDown() override {
        toolbar.reset();
    }

    std::unique_ptr<TextEditToolBar> toolbar;
};

TEST_F(TextEditToolBarTest, CreateAndDestroy) {
    EXPECT_NO_THROW({
        auto tmpToolbar = std::make_unique<TextEditToolBar>();
    });

    EXPECT_NO_THROW({
        auto tmpToolbar = std::make_unique<TextEditToolBar>();
        tmpToolbar.reset();
    });
}

TEST_F(TextEditToolBarTest, ObjectNameSet) {
    EXPECT_EQ(toolbar->objectName(), QString("textEditToolBar"));
}

TEST_F(TextEditToolBarTest, FontComboBoxExists) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);
    EXPECT_GE(combo->minimumWidth(), 180);
}

TEST_F(TextEditToolBarTest, FontSizeSpinBoxExists) {
    auto* spinBox = toolbar->findChild<QDoubleSpinBox*>("fontSizeSpinBox");
    ASSERT_NE(spinBox, nullptr);
    EXPECT_DOUBLE_EQ(spinBox->minimum(), 0.0);
    EXPECT_DOUBLE_EQ(spinBox->maximum(), 999.0);
    EXPECT_DOUBLE_EQ(spinBox->singleStep(), 0.5);
    EXPECT_TRUE(spinBox->suffix().contains("pt"));
}

TEST_F(TextEditToolBarTest, StyleButtonsExist) {
    auto* bold = toolbar->findChild<QToolButton*>("boldButton");
    auto* italic = toolbar->findChild<QToolButton*>("italicButton");
    auto* underline = toolbar->findChild<QToolButton*>("underlineButton");
    auto* strikethrough = toolbar->findChild<QToolButton*>("strikethroughButton");

    ASSERT_NE(bold, nullptr);
    ASSERT_NE(italic, nullptr);
    ASSERT_NE(underline, nullptr);
    ASSERT_NE(strikethrough, nullptr);

    EXPECT_TRUE(bold->isCheckable());
    EXPECT_TRUE(italic->isCheckable());
    EXPECT_TRUE(underline->isCheckable());
    EXPECT_TRUE(strikethrough->isCheckable());
}

TEST_F(TextEditToolBarTest, SetFontInfoUpdatesFontCombo) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    auto* spinBox = toolbar->findChild<QDoubleSpinBox*>("fontSizeSpinBox");
    ASSERT_NE(combo, nullptr);
    ASSERT_NE(spinBox, nullptr);

    toolbar->setFontInfo("Arial", 12.5f);

    EXPECT_EQ(combo->currentFont().family(), "Arial");
    EXPECT_NEAR(spinBox->value(), 12.5, 0.01);
}

TEST_F(TextEditToolBarTest, SetFontInfoResetsStyleButtons) {
    auto* bold = toolbar->findChild<QToolButton*>("boldButton");
    auto* italic = toolbar->findChild<QToolButton*>("italicButton");

    // Manually check bold and italic
    bold->setChecked(true);
    italic->setChecked(true);

    // setFontInfo should reset style buttons
    toolbar->setFontInfo("Arial", 12.0f);

    EXPECT_FALSE(bold->isChecked());
    EXPECT_FALSE(italic->isChecked());
}

TEST_F(TextEditToolBarTest, ClearFontInfoResetsAll) {
    auto* spinBox = toolbar->findChild<QDoubleSpinBox*>("fontSizeSpinBox");
    auto* bold = toolbar->findChild<QToolButton*>("boldButton");

    toolbar->setFontInfo("Times New Roman", 14.0f);
    bold->setChecked(true);

    toolbar->clearFontInfo();

    EXPECT_NEAR(spinBox->value(), 0.0, 0.01);
    EXPECT_FALSE(bold->isChecked());
}

TEST_F(TextEditToolBarTest, SetSelectionFormattingUpdatesAll) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    auto* spinBox = toolbar->findChild<QDoubleSpinBox*>("fontSizeSpinBox");
    auto* bold = toolbar->findChild<QToolButton*>("boldButton");
    auto* italic = toolbar->findChild<QToolButton*>("italicButton");
    auto* underline = toolbar->findChild<QToolButton*>("underlineButton");
    auto* strikethrough = toolbar->findChild<QToolButton*>("strikethroughButton");

    toolbar->setSelectionFormatting("Courier New", 16.0f, true, false, true, false);

    EXPECT_EQ(combo->currentFont().family(), "Courier New");
    EXPECT_NEAR(spinBox->value(), 16.0, 0.01);
    EXPECT_TRUE(bold->isChecked());
    EXPECT_FALSE(italic->isChecked());
    EXPECT_TRUE(underline->isChecked());
    EXPECT_FALSE(strikethrough->isChecked());
}

TEST_F(TextEditToolBarTest, SelectionGuardPreventsSignalFeedback) {
    // setSelectionFormatting should NOT emit signals (guard prevents feedback)
    QSignalSpy fontSpy(toolbar.get(), &TextEditToolBar::fontFamilyChanged);
    QSignalSpy sizeSpy(toolbar.get(), &TextEditToolBar::fontSizeChanged);
    QSignalSpy boldSpy(toolbar.get(), &TextEditToolBar::boldToggled);

    toolbar->setSelectionFormatting("Georgia", 14.0f, true, false, false, false);

    // Phase-16 fix (qt_test_gap_audit finding #30): flush any queued
    // signals before the count check. Without processEvents(), a bug
    // where the guard fails to suppress a queued emit would not be
    // caught (the EXPECT_EQ runs before the queued emit delivers).
    QApplication::processEvents();

    EXPECT_EQ(fontSpy.count(), 0);
    EXPECT_EQ(sizeSpy.count(), 0);
    EXPECT_EQ(boldSpy.count(), 0);
}

TEST_F(TextEditToolBarTest, UserInteractionEmitsSignals) {
    auto* bold = toolbar->findChild<QToolButton*>("boldButton");
    ASSERT_NE(bold, nullptr);

    QSignalSpy boldSpy(toolbar.get(), &TextEditToolBar::boldToggled);

    // Direct click should emit the signal (updatingFromSelection_ is false)
    bold->toggle();

    EXPECT_GE(boldSpy.count(), 1);
    EXPECT_EQ(boldSpy.at(0).at(0).toBool(), true);
}

TEST_F(TextEditToolBarTest, MultipleSetFontInfoCalls) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    auto* spinBox = toolbar->findChild<QDoubleSpinBox*>("fontSizeSpinBox");

    toolbar->setFontInfo("Helvetica", 10.0f);
    EXPECT_NEAR(spinBox->value(), 10.0, 0.01);

    toolbar->setFontInfo("Courier New", 8.5f);
    EXPECT_NEAR(spinBox->value(), 8.5, 0.01);

    toolbar->setFontInfo("Georgia", 16.0f);
    EXPECT_NEAR(spinBox->value(), 16.0, 0.01);
}

TEST_F(TextEditToolBarTest, SetClearSetCycle) {
    auto* spinBox = toolbar->findChild<QDoubleSpinBox*>("fontSizeSpinBox");
    auto* bold = toolbar->findChild<QToolButton*>("boldButton");

    toolbar->setFontInfo("Arial", 12.0f);
    EXPECT_NEAR(spinBox->value(), 12.0, 0.01);

    toolbar->clearFontInfo();
    EXPECT_NEAR(spinBox->value(), 0.0, 0.01);

    toolbar->setFontInfo("Verdana", 14.0f);
    EXPECT_NEAR(spinBox->value(), 14.0, 0.01);

    toolbar->clearFontInfo();
    EXPECT_NEAR(spinBox->value(), 0.0, 0.01);
    EXPECT_FALSE(bold->isChecked());
}

TEST_F(TextEditToolBarTest, SetFontInfoWithSpecialCharacters) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);

    toolbar->setFontInfo("DejaVu Sans Mono", 11.0f);
    // QFontComboBox may map to the closest matching font
    // Just verify it doesn't crash
    EXPECT_FALSE(combo->currentFont().family().isEmpty());
}

TEST_F(TextEditToolBarTest, StyleButtonShortcuts) {
    auto* bold = toolbar->findChild<QToolButton*>("boldButton");
    auto* italic = toolbar->findChild<QToolButton*>("italicButton");
    auto* underline = toolbar->findChild<QToolButton*>("underlineButton");

    ASSERT_NE(bold, nullptr);
    ASSERT_NE(italic, nullptr);
    ASSERT_NE(underline, nullptr);

    EXPECT_FALSE(bold->shortcut().isEmpty());
    EXPECT_FALSE(italic->shortcut().isEmpty());
    EXPECT_FALSE(underline->shortcut().isEmpty());
}

TEST_F(TextEditToolBarTest, FontSizeSpinBoxSignal) {
    auto* spinBox = toolbar->findChild<QDoubleSpinBox*>("fontSizeSpinBox");
    ASSERT_NE(spinBox, nullptr);

    QSignalSpy sizeSpy(toolbar.get(), &TextEditToolBar::fontSizeChanged);

    // Direct value change (user interaction)
    spinBox->setValue(18.0);

    EXPECT_GE(sizeSpy.count(), 1);
}

TEST_F(TextEditToolBarTest, SetFontInfoGuardPreventsSignals) {
    QSignalSpy fontSpy(toolbar.get(), &TextEditToolBar::fontFamilyChanged);
    QSignalSpy sizeSpy(toolbar.get(), &TextEditToolBar::fontSizeChanged);

    toolbar->setFontInfo("Arial", 14.0f);

    // Phase-16 fix (qt_test_gap_audit finding #30 sibling): flush
    // queued emits before count check. Same defect as above.
    QApplication::processEvents();

    EXPECT_EQ(fontSpy.count(), 0);
    EXPECT_EQ(sizeSpy.count(), 0);
}

// ─── Pipeline G phase-7: font-name display stabilization ────────────────────
//
// QFontComboBox::setCurrentFont passes the family through QFontInfo on
// Windows, which substitutes installed-family names for fonts the
// system lacks (e.g. Hebrew "Alef" → "Tahoma"). The toolbar must
// display the literal PDF font name regardless of whether the system
// has the font installed.

TEST_F(TextEditToolBarTest, SetFontInfoDisplaysLiteralEmbeddedFontName) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);

    // "Alef" is the canonical case. On a Windows install where Alef
    // is absent, Qt's QFontInfo resolves QFont("Alef") to Tahoma. The
    // line edit must still show "Alef" — the literal PDF font name —
    // not the Qt-resolved substitute.
    toolbar->setFontInfo("Alef", 12.0f);
    EXPECT_EQ(combo->currentText(), QString("Alef"))
        << "Toolbar should display the literal PDF font name 'Alef', "
           "not Qt's QFontInfo-resolved substitute";
}

TEST_F(TextEditToolBarTest, SetSelectionFormattingDisplaysLiteralFamily) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);
    toolbar->setSelectionFormatting("Alef", 12.0f, false, false, false, false,
                                     QColor(Qt::black));
    EXPECT_EQ(combo->currentText(), QString("Alef"))
        << "setSelectionFormatting must use the same literal-name override "
           "as setFontInfo — otherwise the toolbar font name jumps on the "
           "first keystroke when emitSelectionFormatting fires";
}

TEST_F(TextEditToolBarTest, NoFontNameJumpOnSelectionUpdateAfterSpanActivation) {
    // Reproduces the exact bug: span-click sets the toolbar font to
    // 'Alef'; the first keystroke fires emitSelectionFormatting which
    // calls setSelectionFormatting('Alef', ...). After both calls, the
    // displayed font name must STILL be 'Alef', not jumped to a
    // Qt-resolved substitute.
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);

    // Step 1: span activated → setFontInfo
    toolbar->setFontInfo("Alef", 12.0f);
    const QString afterActivation = combo->currentText();
    EXPECT_EQ(afterActivation, QString("Alef"));

    // Step 2: first keystroke → setSelectionFormatting
    toolbar->setSelectionFormatting("Alef", 12.0f, false, false, false, false,
                                     QColor(Qt::black));
    const QString afterKeystroke = combo->currentText();

    EXPECT_EQ(afterActivation, afterKeystroke)
        << "Font name jumped from '" << afterActivation.toStdString()
        << "' to '" << afterKeystroke.toStdString()
        << "' on selection update — phase-7 regression";
    EXPECT_EQ(afterKeystroke, QString("Alef"));
}

// ─── Regression: SpecimenA Green PDF font-name fidelity ─────────────────────
//
// The SpecimenA Green PDF (the user's real-world reproducer) embeds
// subsetted PalatinoLTPro fonts. PyMuPDF / MuPDF report the spans'
// fz_font_name as the literal PostScript-style name with the subset
// prefix stripped:
//
//     PalatinoLTPro-Bold, PalatinoLTPro-Roman,
//     PalatinoLTPro-Italic, PalatinoLTPro-BoldItalic
//
// (verified against the actual PDF by tests/diagnose_pdf_fonts.py).
//
// Earlier the user observed the toolbar showing 'Alef' on this same PDF
// before any keystroke. This test pins down the contract: given the
// exact fz_font_name string the pipeline supplies, the toolbar's
// displayed text must equal that input verbatim — no Qt-substituted
// or Hebrew-family-resolved name. If the user ever sees 'Alef' on this
// PDF again, the bug is upstream of the toolbar, not in setFontInfo.
TEST_F(TextEditToolBarTest, SpecimenAPalatinoFontsDisplayLiterally) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);

    // The four PostScript-style names MuPDF emits on this PDF.
    const QStringList specimen_aFonts{
        QStringLiteral("PalatinoLTPro-Bold"),
        QStringLiteral("PalatinoLTPro-Roman"),
        QStringLiteral("PalatinoLTPro-Italic"),
        QStringLiteral("PalatinoLTPro-BoldItalic"),
    };

    for (const QString& fontName : specimen_aFonts) {
        toolbar->setFontInfo(fontName, 50.0f);
        EXPECT_EQ(combo->currentText(), fontName)
            << "setFontInfo(" << fontName.toStdString()
            << ") must display literally; got '"
            << combo->currentText().toStdString() << "'";
        EXPECT_FALSE(combo->currentText().contains(
                QStringLiteral("Alef"), Qt::CaseInsensitive))
            << "Toolbar produced 'Alef' from input '"
            << fontName.toStdString()
            << "' — that family is not on this PDF; bug is in the "
               "toolbar codepath";

        // Same for the selection-formatting path.
        toolbar->setSelectionFormatting(fontName, 50.0f, false, false,
                                         false, false, QColor(Qt::black));
        EXPECT_EQ(combo->currentText(), fontName)
            << "setSelectionFormatting(" << fontName.toStdString()
            << ") must display literally; got '"
            << combo->currentText().toStdString() << "'";
        EXPECT_FALSE(combo->currentText().contains(
                QStringLiteral("Alef"), Qt::CaseInsensitive));
    }
}

// Live UI reproduction (via tests/diagnose_live_toolbar_font.py) on
// the SpecimenA Green PDF showed:
//   selectSpan: span.fontName = "AAAAAA+PalatinoLTPro-Bold"
//   emitSelectionFormatting: family = "AAAAAA+PalatinoLTPro-Bold"
// → toolbar displayed "Tahoma".
//
// The subset prefix "AAAAAA+" is a PDF-internal artifact of font
// subsetting (PDF 32000-1 §9.6.4) — six uppercase letters plus '+'.
// When that prefixed name reaches Qt's QFontComboBox, the line edit
// silently substitutes a system family (Tahoma on Windows). The
// fix is to strip the subset prefix at the toolbar boundary so the
// user sees the actual font family name.
TEST_F(TextEditToolBarTest, SubsetPrefixedFontNameStripsPrefixForDisplay) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);

    toolbar->setFontInfo("AAAAAA+PalatinoLTPro-Bold", 50.0f);
    EXPECT_EQ(combo->currentText(), QString("PalatinoLTPro-Bold"))
        << "Subset prefix should be stripped for display; got "
        << combo->currentText().toStdString();
    ASSERT_NE(combo->lineEdit(), nullptr);
    EXPECT_EQ(combo->lineEdit()->text(), QString("PalatinoLTPro-Bold"));

    // setSelectionFormatting (the post-keystroke path) must do the same.
    toolbar->setSelectionFormatting("AAAAAA+PalatinoLTPro-Bold", 50.0f,
                                     true, false, false, false,
                                     QColor(Qt::black));
    EXPECT_EQ(combo->currentText(), QString("PalatinoLTPro-Bold"));

    // Verify the prefix-detection only fires for the canonical PDF
    // pattern (six uppercase letters + '+'). Names that look almost
    // like a subset prefix but aren't must pass through verbatim.
    toolbar->setFontInfo("AAAAAA-PalatinoLTPro", 12.0f);
    EXPECT_EQ(combo->currentText(), QString("AAAAAA-PalatinoLTPro"))
        << "Hyphen-delimited name is not a subset prefix; must not strip";
    toolbar->setFontInfo("aaaaaa+palatino", 12.0f);
    EXPECT_EQ(combo->currentText(), QString("aaaaaa+palatino"))
        << "Lowercase prefix is not a PDF subset prefix; must not strip";
    toolbar->setFontInfo("AAAAA+Palatino", 12.0f);
    EXPECT_EQ(combo->currentText(), QString("AAAAA+Palatino"))
        << "5-letter prefix is not a PDF subset prefix; must not strip";
}

// clearFontInfo's setCurrentIndex(0) leaves the line edit showing
// whatever family QFontComboBox sorts first — which on systems
// where "Alef" is installed (translation/i18n toolchains, some
// MS Office add-ons) is "Alef". The user's confusion was exactly
// this: text-edit tool activated → toolbar shows "Alef" before
// any span is clicked. The empty-state should show an empty line
// edit, not a misleading family name.
TEST_F(TextEditToolBarTest, ClearFontInfoLeavesEmptyDisplayedText) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);

    // Set a known font first so the test has a defined "before" state.
    toolbar->setFontInfo("Arial", 12.0f);
    EXPECT_EQ(combo->currentText(), QString("Arial"));

    toolbar->clearFontInfo();
    EXPECT_TRUE(combo->currentText().isEmpty())
        << "clearFontInfo should leave the line edit empty, not show "
           "the alphabetically-first installed family. Got: "
        << combo->currentText().toStdString();
}

// Belt-and-suspenders: the literal-display contract must hold for ANY
// PostScript-style name (no spaces, hyphenated style suffix, often
// not installed on Windows under that exact spelling). Without
// setEditText, QFontComboBox would silently resolve these through
// QFontInfo and pick a near-match family for the line edit.
TEST_F(TextEditToolBarTest, PostScriptStyleNamesDisplayLiterally) {
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);

    const QStringList psNames{
        QStringLiteral("Helvetica-Bold"),
        QStringLiteral("Times-Roman"),
        QStringLiteral("PalatinoLTPro-BoldItalic"),
        QStringLiteral("MinionPro-Regular"),
        QStringLiteral("CMR10"),
    };
    for (const QString& nm : psNames) {
        toolbar->setFontInfo(nm, 12.0f);
        EXPECT_EQ(combo->currentText(), nm)
            << "PostScript-style name '" << nm.toStdString()
            << "' must display literally; got '"
            << combo->currentText().toStdString() << "'";
    }
}

TEST_F(TextEditToolBarTest, UserInitiatedFontChangeStillEmitsSignal) {
    // Phase 7's display-stabilization fix must not break the user's
    // ability to pick a different font from the combo. The user-
    // initiated path runs without the updatingFromSelection_ guard, so
    // emit count should be >= 1 after a real font change.
    auto* combo = toolbar->findChild<QFontComboBox*>("fontComboBox");
    ASSERT_NE(combo, nullptr);

    QSignalSpy spy(toolbar.get(), &TextEditToolBar::fontFamilyChanged);

    // Programmatically pick a different font, simulating the user
    // having clicked a different family in the dropdown.
    QFont arial(QStringLiteral("Arial"));
    combo->setCurrentFont(arial);

    EXPECT_GE(spy.count(), 1)
        << "User-initiated font change must still emit fontFamilyChanged "
           "(phase-7 must not break the existing font-pick UX)";
}
