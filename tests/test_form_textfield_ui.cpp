// -*- coding: utf-8 -*-
//
// tests/test_form_textfield_ui.cpp
//
// Pipeline form-textfield, phase 5: UI-layer tests for GitHub #460 (a form
// text field's font, size and colour could not be changed). They complement
// the phase-1 probes in tests/test_form_textfield_state.cpp, which stay
// untouched, and the phase-4 model tests in tests/test_form_textfield_model.cpp.
//
//   FormFieldFontPanel       -- FormFieldPropertiesPanel's font group on its
//       own: populated from the field's /DA, shown only for text, combo and
//       list fields, one signal per committed change and none while
//       populating, spin-box bounds, symbol fonts, names. Pattern:
//       tests/test_annotation_font_controls.cpp.
//   FormFieldFontMainWindow  -- the same controls inside a real MainWindow:
//       each control issues exactly one SetFieldAppearanceCommand and undo
//       reverts it; a stale panel cannot write a stale value back; a stale
//       widget index is refused; a change made while the user is typing into
//       the field keeps the typing and commit writes it under the new /DA;
//       accessible names are unique in the window.
//   FormFieldFontToolbar     -- the text-edit toolbar is hidden (and its font
//       info cleared) under the Form tool and every other non-text-edit tool
//       path, and comes back under Edit Text.
//
// Every assertion runs unconditionally. No DISABLED_, GTEST_SKIP or
// expected-fail marker.
//
// No MuPDF call is made from this file: everything is read back through
// FormManager's public API, so there is no MuPdfContext::execute lambda here
// (tools/gate/fz_try_raii_scan.py).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"
#include "ui/FormFieldPropertiesPanel.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "ui/TextEditToolBar.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDockWidget>
#include <QFontComboBox>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTest>
#include <QToolBar>
#include <QWidget>

#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using pdfe::commands::UndoManager;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;
using pdfe::forms::FormManager;
using pdfe::forms::FormTool;
using pdfe::ui::FormFieldFontRequest;
using pdfe::ui::FormFieldPropertiesPanel;
using pdfe::ui::MainWindow;
using pdfe::ui::PdfCanvas;

namespace {

constexpr const char* kB3 = "GitHub #460 (font settings ignored for form text fields): ";

constexpr const char* kComboName = "formFieldFontCombo";
constexpr const char* kSpinName = "formFieldFontSizeSpin";
constexpr const char* kColorName = "formFieldFontColorButton";
constexpr const char* kGroupName = "formFieldFontGroup";

void settle() {
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

FormFieldData textField(const std::string& font, float size, int n,
                        std::array<float, 4> color) {
    FormFieldData d;
    d.type = FormFieldType::Text;
    d.fieldName = "ui_text";
    d.rect = QRectF(100, 100, 200, 24);
    d.pageIndex = 0;
    d.widgetIndex = 0;
    d.daFontName = font;
    d.daFontSize = size;
    d.daColorComponents = n;
    d.daColor = color;
    return d;
}

struct PanelControls {
    QComboBox* combo = nullptr;
    QSpinBox* spin = nullptr;
    QPushButton* color = nullptr;
    QGroupBox* group = nullptr;
};

PanelControls controlsOf(QWidget& panel) {
    PanelControls c;
    c.combo = panel.findChild<QComboBox*>(QString::fromLatin1(kComboName));
    c.spin = panel.findChild<QSpinBox*>(QString::fromLatin1(kSpinName));
    c.color = panel.findChild<QPushButton*>(QString::fromLatin1(kColorName));
    c.group = panel.findChild<QGroupBox*>(QString::fromLatin1(kGroupName));
    return c;
}

// The way a user commits a spin-box value: Enter in the box.
void commitSpin(QSpinBox* spin) {
    QTest::keyClick(spin, Qt::Key_Return);
    settle();
}

// ═══════════════════════════════════════════════════════════════════════════
// FormFieldFontPanel -- the panel on its own
// ═══════════════════════════════════════════════════════════════════════════

class FormFieldFontPanel : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(QApplication::instance(), nullptr) << "harness: no QApplication";
        panel_ = std::make_unique<FormFieldPropertiesPanel>();
        panel_->show();
        QObject::connect(panel_.get(), &FormFieldPropertiesPanel::fieldFontChangeRequested,
                         [this](FormFieldData f, FormFieldFontRequest r) {
                             fields_.push_back(f);
                             requests_.push_back(r);
                         });
        c_ = controlsOf(*panel_);
        ASSERT_NE(c_.combo, nullptr) << kB3 << "no " << kComboName;
        ASSERT_NE(c_.spin, nullptr) << kB3 << "no " << kSpinName;
        ASSERT_NE(c_.color, nullptr) << kB3 << "no " << kColorName;
        ASSERT_NE(c_.group, nullptr) << kB3 << "no " << kGroupName;
    }
    void TearDown() override { panel_.reset(); }

    std::unique_ptr<FormFieldPropertiesPanel> panel_;
    PanelControls c_;
    std::vector<FormFieldData> fields_;
    std::vector<FormFieldFontRequest> requests_;
};

TEST_F(FormFieldFontPanel, PopulatesFamilySizeAndColourFromTheFieldsDa) {
    panel_->setFormField(textField("TiRo", 18.0f, 3, {1.0f, 0.0f, 0.0f, 0.0f}));
    settle();
    EXPECT_EQ(c_.combo->currentData().toString(), QStringLiteral("TiRo"));
    EXPECT_EQ(c_.combo->currentText(), QStringLiteral("Times"));
    EXPECT_EQ(c_.spin->value(), 18);
    EXPECT_EQ(panel_->currentFontColor(), QColor(255, 0, 0));

    // Gray /DA colour (g) and Courier.
    panel_->setFormField(textField("Cour", 9.0f, 1, {0.5f, 0.0f, 0.0f, 0.0f}));
    settle();
    EXPECT_EQ(c_.combo->currentData().toString(), QStringLiteral("Cour"));
    EXPECT_EQ(c_.combo->currentText(), QStringLiteral("Courier"));
    EXPECT_EQ(c_.spin->value(), 9);
    const QColor gray = panel_->currentFontColor();
    EXPECT_EQ(gray.red(), gray.green());
    EXPECT_EQ(gray.green(), gray.blue());
    EXPECT_NEAR(gray.red(), 128, 1);

    // CMYK /DA colour (k): pure cyan.
    panel_->setFormField(textField("Helv", 12.0f, 4, {1.0f, 0.0f, 0.0f, 0.0f}));
    settle();
    EXPECT_EQ(c_.combo->currentData().toString(), QStringLiteral("Helv"));
    EXPECT_EQ(panel_->currentFontColor(), QColor(0, 255, 255));

    // No colour: black. Size 0 is auto-size and shows as "Auto".
    panel_->setFormField(textField("Helv", 0.0f, 0, {}));
    settle();
    EXPECT_EQ(c_.spin->value(), 0);
    EXPECT_EQ(c_.spin->text(), QStringLiteral("Auto"));
    EXPECT_EQ(panel_->currentFontColor(), QColor(0, 0, 0));

    // A fractional size shows rounded.
    panel_->setFormField(textField("Helv", 10.6f, 0, {}));
    settle();
    EXPECT_EQ(c_.spin->value(), 11);

    // A hand-built out-of-range colour is clamped, not passed to QColor raw.
    panel_->setFormField(textField("Helv", 12.0f, 3, {2.0f, -1.0f, NAN, 0.0f}));
    settle();
    EXPECT_EQ(panel_->currentFontColor(), QColor(255, 0, 0));

    EXPECT_TRUE(requests_.empty())
        << kB3 << "populating the panel emitted " << requests_.size()
        << " font change request(s); it must emit none.";
}

TEST_F(FormFieldFontPanel, GroupIsShownForTextComboAndListOnly) {
    const struct {
        FormFieldType type;
        bool shown;
    } cases[] = {
        {FormFieldType::Text, true},          {FormFieldType::ComboBox, true},
        {FormFieldType::ListBox, true},       {FormFieldType::Checkbox, false},
        {FormFieldType::RadioButton, false},  {FormFieldType::PushButton, false},
        {FormFieldType::Signature, false},    {FormFieldType::Unknown, false},
    };
    for (const auto& tc : cases) {
        FormFieldData d = textField("Helv", 12.0f, 0, {});
        d.type = tc.type;
        panel_->setFormField(d);
        settle();
        const int t = static_cast<int>(tc.type);
        EXPECT_EQ(c_.group->isVisibleTo(panel_.get()), tc.shown) << "type " << t;
        EXPECT_EQ(c_.combo->isVisibleTo(panel_.get()), tc.shown) << "type " << t;
        EXPECT_EQ(c_.spin->isVisibleTo(panel_.get()), tc.shown) << "type " << t;
        EXPECT_EQ(c_.color->isVisibleTo(panel_.get()), tc.shown) << "type " << t;
        EXPECT_EQ(FormFieldPropertiesPanel::fieldTypeHasFontControls(tc.type), tc.shown)
            << "type " << t;
    }
    // Hidden again once the panel is cleared.
    panel_->setFormField(textField("Helv", 12.0f, 0, {}));
    settle();
    ASSERT_TRUE(c_.group->isVisibleTo(panel_.get()));
    panel_->clear();
    settle();
    EXPECT_FALSE(c_.group->isVisibleTo(panel_.get()));
}

TEST_F(FormFieldFontPanel, ButtonFieldsHideTheFontGroupAndClickingColourDoesNothing) {
    FormFieldData d = textField("Helv", 12.0f, 0, {});
    d.type = FormFieldType::PushButton;
    panel_->setFormField(d);
    settle();
    EXPECT_FALSE(c_.group->isVisibleTo(panel_.get()));
    int pickerCalls = 0;
    panel_->setColorPickerForTesting([&pickerCalls](const QColor&, QWidget*) {
        ++pickerCalls;
        return QColor(255, 0, 0);
    });
    c_.color->click();  // a programmatic click reaches the slot even when hidden
    settle();
    EXPECT_EQ(pickerCalls, 0);
    EXPECT_TRUE(requests_.empty());
}

TEST_F(FormFieldFontPanel, FamilyListIsBase14AndOffersSymbolFontsOnlyWhenInUse) {
    panel_->setFormField(textField("Helv", 12.0f, 0, {}));
    settle();
    ASSERT_EQ(c_.combo->count(), 3);
    EXPECT_EQ(c_.combo->itemData(0).toString(), QStringLiteral("Helv"));
    EXPECT_EQ(c_.combo->itemData(1).toString(), QStringLiteral("TiRo"));
    EXPECT_EQ(c_.combo->itemData(2).toString(), QStringLiteral("Cour"));
    EXPECT_EQ(c_.combo->itemText(0), QStringLiteral("Helvetica"));
    for (int i = 0; i < c_.combo->count(); ++i) {
        EXPECT_TRUE(pdfe::forms::isBase14DefaultAppearanceFont(
            c_.combo->itemData(i).toString().toStdString()));
    }

    panel_->setFormField(textField("ZaDb", 12.0f, 0, {}));
    settle();
    ASSERT_EQ(c_.combo->count(), 4);
    EXPECT_EQ(c_.combo->currentData().toString(), QStringLiteral("ZaDb"));
    EXPECT_EQ(c_.combo->currentText(), QStringLiteral("ZapfDingbats"));

    panel_->setFormField(textField("Symb", 12.0f, 0, {}));
    settle();
    ASSERT_EQ(c_.combo->count(), 4);
    EXPECT_EQ(c_.combo->currentData().toString(), QStringLiteral("Symb"));

    panel_->setFormField(textField("TiRo", 12.0f, 0, {}));
    settle();
    EXPECT_EQ(c_.combo->count(), 3) << "a symbol-font entry outlived the field that used it";
    EXPECT_EQ(c_.combo->currentData().toString(), QStringLiteral("TiRo"));

    // An unknown key in hand-built data shows Helvetica, MuPDF's fallback.
    panel_->setFormField(textField("NotAFont", 12.0f, 0, {}));
    settle();
    EXPECT_EQ(c_.combo->count(), 3);
    EXPECT_EQ(c_.combo->currentData().toString(), QStringLiteral("Helv"));
    EXPECT_TRUE(requests_.empty());
}

TEST_F(FormFieldFontPanel, SizeSpinIsBoundedToTheSettersRange) {
    EXPECT_EQ(c_.spin->minimum(), 0);
    EXPECT_EQ(c_.spin->maximum(), 1000);
    EXPECT_EQ(c_.spin->specialValueText(), QStringLiteral("Auto"));
    panel_->setFormField(textField("Helv", 12.0f, 0, {}));
    settle();
    c_.spin->setValue(5000);
    EXPECT_EQ(c_.spin->value(), 1000);
    c_.spin->setValue(-7);
    EXPECT_EQ(c_.spin->value(), 0);
    for (int v = 0; v <= 1000; ++v) {
        EXPECT_TRUE(pdfe::forms::isAcceptedDefaultAppearanceSize(static_cast<float>(v)))
            << "the spin box can produce " << v << ", which the setter rejects";
    }
    // A hostile size in hand-built data is shown inside the range.
    panel_->setFormField(textField("Helv", 1.0e9f, 0, {}));
    settle();
    EXPECT_EQ(c_.spin->value(), 1000);
    panel_->setFormField(textField("Helv", NAN, 0, {}));
    settle();
    EXPECT_EQ(c_.spin->value(), 0);
    EXPECT_TRUE(requests_.empty());
}

TEST_F(FormFieldFontPanel, EachControlEmitsExactlyOncePerCommittedChange) {
    panel_->setFormField(textField("Helv", 12.0f, 0, {}));
    settle();
    ASSERT_TRUE(requests_.empty());

    // Family: one selection, one request.
    c_.combo->setCurrentIndex(c_.combo->findData(QStringLiteral("TiRo")));
    settle();
    ASSERT_EQ(requests_.size(), 1u);
    EXPECT_EQ(requests_[0].changed, FormFieldFontRequest::Property::Family);
    EXPECT_EQ(requests_[0].font, "TiRo");
    EXPECT_EQ(fields_[0].widgetIndex, 0);

    // Re-selecting the shown family is not a change.
    c_.combo->setCurrentIndex(c_.combo->findData(QStringLiteral("TiRo")));
    settle();
    EXPECT_EQ(requests_.size(), 1u);

    // Size: eight steps, then one commit -- one request, not eight.
    for (int v = 13; v <= 20; ++v) c_.spin->setValue(v);
    settle();
    EXPECT_EQ(requests_.size(), 1u) << kB3 << "the spin box emitted per step";
    commitSpin(c_.spin);
    ASSERT_EQ(requests_.size(), 2u);
    EXPECT_EQ(requests_[1].changed, FormFieldFontRequest::Property::Size);
    EXPECT_FLOAT_EQ(requests_[1].size, 20.0f);
    EXPECT_EQ(requests_[1].font, "TiRo");
    // editingFinished again (Enter twice, or Enter then focus-out) with no
    // change is not a second request.
    commitSpin(c_.spin);
    EXPECT_EQ(requests_.size(), 2u);

    // Colour: chosen colour -> one request; cancel, translucent and the same
    // colour -> none.
    QColor answer(255, 0, 0);
    QColor seenInitial;
    panel_->setColorPickerForTesting([&](const QColor& initial, QWidget*) {
        seenInitial = initial;
        return answer;
    });
    c_.color->click();
    settle();
    ASSERT_EQ(requests_.size(), 3u);
    EXPECT_EQ(requests_[2].changed, FormFieldFontRequest::Property::Color);
    EXPECT_EQ(requests_[2].color, QColor(255, 0, 0));
    EXPECT_EQ(seenInitial, QColor(0, 0, 0));
    EXPECT_EQ(panel_->currentFontColor(), QColor(255, 0, 0));

    answer = QColor();  // cancelled
    c_.color->click();
    answer = QColor(0, 0, 255, 128);  // translucent
    c_.color->click();
    answer = QColor(255, 0, 0);  // unchanged
    c_.color->click();
    settle();
    EXPECT_EQ(requests_.size(), 3u);
    EXPECT_EQ(panel_->currentFontColor(), QColor(255, 0, 0));

    // A new field resets the spin's committed value, so a stale edit is not
    // emitted against the new field.
    panel_->setFormField(textField("Helv", 12.0f, 0, {}));
    settle();
    commitSpin(c_.spin);
    EXPECT_EQ(requests_.size(), 3u);
}

TEST_F(FormFieldFontPanel, NewWidgetsHaveObjectAndAccessibleNames) {
    for (QWidget* w : std::initializer_list<QWidget*>{c_.combo, c_.spin, c_.color, c_.group}) {
        EXPECT_FALSE(w->objectName().isEmpty());
        EXPECT_FALSE(w->accessibleName().isEmpty()) << w->objectName().toStdString();
    }
    EXPECT_EQ(c_.combo->accessibleName(), QStringLiteral("Form Field Font"));
    EXPECT_EQ(c_.spin->accessibleName(), QStringLiteral("Form Field Font Size"));
    EXPECT_EQ(c_.color->accessibleName(), QStringLiteral("Form Field Font Color"));
    EXPECT_EQ(c_.group->accessibleName(), QStringLiteral("Form Field Font Settings"));
}

// ═══════════════════════════════════════════════════════════════════════════
// MainWindow harness
// ═══════════════════════════════════════════════════════════════════════════

std::filesystem::path formFixture() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
}

FormTool* liveFormTool(MainWindow& mw) {
    auto* canvas = mw.findChild<PdfCanvas*>();
    return canvas ? dynamic_cast<FormTool*>(canvas->currentTool()) : nullptr;
}

class FormFieldFontMainWindow : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::exists(formFixture())) << "harness: test_form.pdf missing";
        mw_ = std::make_unique<MainWindow>();
        mw_->openFile(QString::fromStdString(formFixture().string()));
        settle();
        doc_ = mw_->documentForTesting();
        ASSERT_NE(doc_, nullptr) << "harness: fixture did not open";
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);
        reader_ = std::make_unique<FormManager>(doc_->context());

        undo_ = mw_->findChild<UndoManager*>();
        ASSERT_NE(undo_, nullptr) << "harness: no UndoManager";
        panel_ = mw_->findChild<FormFieldPropertiesPanel*>(QStringLiteral("formFieldPropertiesPanel"));
        ASSERT_NE(panel_, nullptr) << "harness: no form properties panel";
        c_ = controlsOf(*panel_);
        ASSERT_NE(c_.combo, nullptr);
        ASSERT_NE(c_.spin, nullptr);
        ASSERT_NE(c_.color, nullptr);
        ASSERT_NE(c_.group, nullptr);

        auto* formAct = mw_->findChild<QAction*>(QStringLiteral("formToolAction"));
        ASSERT_NE(formAct, nullptr);
        formAct->trigger();
        settle();
        tool_ = liveFormTool(*mw_);
        ASSERT_NE(tool_, nullptr) << "harness: no live FormTool";
        // The Form tool starts armed to create a text field (a press starts a
        // rubber band). Drop to interaction mode, the state the tool is in
        // after a field is placed (GitHub #369), so a press selects and edits.
        tool_->setCreationFieldType(FormFieldType::Unknown);
        ASSERT_FALSE(tool_->isCreationMode());

        first_ = reader_->getFormField(page_.get(), 0);
        ASSERT_EQ(first_.type, FormFieldType::Text) << "harness: widget 0 is not a text field";
        ASSERT_EQ(first_.fieldName, "FirstName") << "harness: unexpected widget 0";
    }
    void TearDown() override {
        tool_ = nullptr;
        reader_.reset();
        page_.reset();
        mw_.reset();
    }

    FormFieldData field(int idx) { return reader_->getFormField(page_.get(), idx); }

    // Press on a field the way the canvas would deliver it.
    void pressAt(QPointF pdfPoint) {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        ASSERT_NE(tool_, nullptr);
        tool_->onMousePress(&press, 0, pdfPoint);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(0, 0), QPointF(0, 0),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        tool_->onMouseRelease(&release, 0, pdfPoint);
        settle();
    }

    void selectFirstField() {
        pressAt(first_.rect.center());
        ASSERT_EQ(panel_->currentWidgetIndex(), 0) << "harness: the panel did not show widget 0";
        ASSERT_TRUE(c_.group->isVisibleTo(panel_)) << "harness: font group hidden for a text field";
    }

    int findWidget(FormFieldType type) {
        const auto all = reader_->getFormFields(page_.get());
        for (const auto& f : all) {
            if (f.type == type) return f.widgetIndex;
        }
        return -1;
    }

    std::unique_ptr<MainWindow> mw_;
    pdfe::core::Document* doc_ = nullptr;
    std::unique_ptr<pdfe::core::Page> page_;
    std::unique_ptr<FormManager> reader_;
    UndoManager* undo_ = nullptr;
    FormFieldPropertiesPanel* panel_ = nullptr;
    PanelControls c_;
    FormTool* tool_ = nullptr;
    FormFieldData first_;
};

QColor daColor(const FormFieldData& d) {
    if (d.daColorComponents == 3) {
        return QColor::fromRgbF(d.daColor[0], d.daColor[1], d.daColor[2]);
    }
    if (d.daColorComponents == 1) {
        return QColor::fromRgbF(d.daColor[0], d.daColor[0], d.daColor[0]);
    }
    if (d.daColorComponents == 0) return QColor(0, 0, 0);
    return QColor();  // CMYK is not expected for the fixture
}

TEST_F(FormFieldFontMainWindow, SelectingAFieldPopulatesTheFontGroupFromItsDa) {
    ASSERT_NO_FATAL_FAILURE(selectFirstField());
    // The fixture's FirstName /DA is "/Helv 10 Tf 0 0 0 rg".
    EXPECT_EQ(c_.combo->currentData().toString(), QStringLiteral("Helv"));
    EXPECT_EQ(c_.spin->value(), 10);
    EXPECT_EQ(panel_->currentFontColor(), QColor(0, 0, 0));
}

TEST_F(FormFieldFontMainWindow, EachControlIssuesOneCommandAndUndoRevertsIt) {
    ASSERT_NO_FATAL_FAILURE(selectFirstField());
    const int base = undo_->undoStackSize();

    // Family.
    c_.combo->setCurrentIndex(c_.combo->findData(QStringLiteral("TiRo")));
    settle();
    EXPECT_EQ(undo_->undoStackSize(), base + 1) << kB3 << "family change: not one command";
    EXPECT_EQ(field(0).daFontName, "TiRo");
    EXPECT_FLOAT_EQ(field(0).daFontSize, 10.0f) << "the family change altered the size";
    undo_->undo();
    settle();
    EXPECT_EQ(field(0).daFontName, "Helv") << "undo did not revert the family";
    EXPECT_EQ(undo_->undoStackSize(), base);

    // Size: several steps, then one commit.
    for (int v = 11; v <= 18; ++v) c_.spin->setValue(v);
    settle();
    EXPECT_EQ(undo_->undoStackSize(), base) << kB3 << "the spin box issued a command per step";
    EXPECT_FLOAT_EQ(field(0).daFontSize, 10.0f);
    commitSpin(c_.spin);
    EXPECT_EQ(undo_->undoStackSize(), base + 1) << kB3 << "size commit: not one command";
    EXPECT_FLOAT_EQ(field(0).daFontSize, 18.0f);
    // The panel still showed TiRo (it is not re-read after an undo). The size
    // change must take the family from the document, not write TiRo back.
    EXPECT_EQ(field(0).daFontName, "Helv") << "a stale panel family was written back";
    undo_->undo();
    settle();
    EXPECT_FLOAT_EQ(field(0).daFontSize, 10.0f) << "undo did not revert the size";
    EXPECT_EQ(undo_->undoStackSize(), base);

    // Colour.
    panel_->setColorPickerForTesting([](const QColor&, QWidget*) { return QColor(255, 0, 0); });
    c_.color->click();
    settle();
    EXPECT_EQ(undo_->undoStackSize(), base + 1) << kB3 << "colour change: not one command";
    EXPECT_EQ(daColor(field(0)), QColor(255, 0, 0));
    undo_->undo();
    settle();
    EXPECT_EQ(daColor(field(0)), QColor(0, 0, 0)) << "undo did not revert the colour";
    EXPECT_EQ(undo_->undoStackSize(), base);

    // Redo puts it back.
    undo_->redo();
    settle();
    EXPECT_EQ(daColor(field(0)), QColor(255, 0, 0));
}

TEST_F(FormFieldFontMainWindow, ButtonFieldHidesTheFontGroup) {
    ASSERT_NO_FATAL_FAILURE(selectFirstField());
    const int checkbox = findWidget(FormFieldType::Checkbox);
    ASSERT_GE(checkbox, 0) << "harness: fixture has no check box";
    pressAt(field(checkbox).rect.center());
    ASSERT_EQ(panel_->currentWidgetIndex(), checkbox) << "harness: check box not selected";
    EXPECT_EQ(panel_->currentFieldType(), FormFieldType::Checkbox);
    EXPECT_FALSE(c_.group->isVisibleTo(panel_));
    EXPECT_FALSE(c_.combo->isVisibleTo(panel_));
    EXPECT_FALSE(c_.spin->isVisibleTo(panel_));
    EXPECT_FALSE(c_.color->isVisibleTo(panel_));
}

TEST_F(FormFieldFontMainWindow, StaleWidgetIndexIsRefused) {
    ASSERT_NO_FATAL_FAILURE(selectFirstField());
    const int base = undo_->undoStackSize();
    const std::string daBefore = field(0).defaultAppearanceString;

    // The panel believes widget 0 is a different field (as it would if the
    // field set changed after selection without the panel being told).
    FormFieldData stale = first_;
    stale.fieldName = "SomeOtherField";
    panel_->setFormField(stale);
    settle();
    c_.combo->setCurrentIndex(c_.combo->findData(QStringLiteral("Cour")));
    settle();
    EXPECT_EQ(undo_->undoStackSize(), base) << "a command was issued for a stale index";
    EXPECT_EQ(field(0).defaultAppearanceString, daBefore);
    EXPECT_EQ(panel_->currentWidgetIndex(), -1) << "the panel kept showing the stale field";

    // An index past the end.
    FormFieldData gone = first_;
    gone.widgetIndex = 9999;
    panel_->setFormField(gone);
    settle();
    c_.combo->setCurrentIndex(c_.combo->findData(QStringLiteral("Cour")));
    settle();
    EXPECT_EQ(undo_->undoStackSize(), base);
    EXPECT_EQ(panel_->currentWidgetIndex(), -1);

    // A page index past the end.
    FormFieldData badPage = first_;
    badPage.pageIndex = 42;
    panel_->setFormField(badPage);
    settle();
    c_.combo->setCurrentIndex(c_.combo->findData(QStringLiteral("Cour")));
    settle();
    EXPECT_EQ(undo_->undoStackSize(), base);
    EXPECT_EQ(field(0).defaultAppearanceString, daBefore);
}

// The phase-1 notes asked for this at MainWindow level: the probe
// MidEditFontChangeKeepsTheBufferAndCommitUsesNewDa drives the setter
// directly, so it cannot see MainWindow calling markPageDirty /
// onPageContentChanged (which would run FormTool::onAnnotationsChanged and
// drop the edit).
TEST_F(FormFieldFontMainWindow, MidEditChangeFromThePanelKeepsTheTypingAndCommitUsesTheNewDa) {
    ASSERT_NO_FATAL_FAILURE(selectFirstField());
    ASSERT_TRUE(tool_->isEditingField()) << "harness: clicking the text field did not start an edit";
    const std::string valueBefore = field(0).value;
    for (const QChar ch : QStringLiteral("XYZ")) {
        QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
        tool_->onKeyPress(&ev);
    }
    ASSERT_EQ(tool_->editBufferForTesting(), "XYZ") << "harness: typing failed";
    const int base = undo_->undoStackSize();

    c_.combo->setCurrentIndex(c_.combo->findData(QStringLiteral("TiRo")));
    settle();
    EXPECT_TRUE(tool_->isEditingField()) << kB3 << "the family change ended the edit";
    EXPECT_EQ(tool_->editBufferForTesting(), "XYZ") << kB3 << "the family change lost the typing";

    c_.spin->setValue(18);
    commitSpin(c_.spin);
    EXPECT_TRUE(tool_->isEditingField()) << kB3 << "the size change ended the edit";
    EXPECT_EQ(tool_->editBufferForTesting(), "XYZ") << kB3 << "the size change lost the typing";

    panel_->setColorPickerForTesting([](const QColor&, QWidget*) { return QColor(255, 0, 0); });
    c_.color->click();
    settle();
    EXPECT_TRUE(tool_->isEditingField()) << kB3 << "the colour change ended the edit";
    EXPECT_EQ(tool_->editBufferForTesting(), "XYZ") << kB3 << "the colour change lost the typing";

    EXPECT_EQ(undo_->undoStackSize(), base + 3) << "three changes should be three commands";
    EXPECT_EQ(liveFormTool(*mw_), tool_) << "the font change replaced the FormTool";
    // Applied to the document now (the overlay re-reads it on every paint)...
    const FormFieldData mid = field(0);
    EXPECT_EQ(mid.daFontName, "TiRo");
    EXPECT_FLOAT_EQ(mid.daFontSize, 18.0f);
    EXPECT_EQ(daColor(mid), QColor(255, 0, 0));
    // ...while the typed value is still only in the buffer.
    EXPECT_EQ(mid.value, valueBefore);

    // Commit by clicking outside every field.
    pressAt(QPointF(5.0, 5.0));
    EXPECT_FALSE(tool_->isEditingField());
    const FormFieldData after = field(0);
    EXPECT_EQ(after.value, "XYZ") << kB3 << "commit did not write the typed value";
    EXPECT_EQ(after.daFontName, "TiRo") << kB3 << "commit lost the new family";
    EXPECT_FLOAT_EQ(after.daFontSize, 18.0f) << kB3 << "commit lost the new size";
    EXPECT_EQ(daColor(after), QColor(255, 0, 0)) << kB3 << "commit lost the new colour";
}

TEST_F(FormFieldFontMainWindow, FontControlAccessibleNamesAreUniqueInTheWindow) {
    std::map<QString, int> counts;
    for (QWidget* w : mw_->findChildren<QWidget*>()) {
        const QString n = w->accessibleName();
        if (!n.isEmpty()) ++counts[n];
    }
    for (const char* n : {"Form Field Font", "Form Field Font Size", "Form Field Font Color",
                          "Form Field Font Settings"}) {
        EXPECT_EQ(counts[QString::fromLatin1(n)], 1)
            << "accessible name '" << n << "' is not unique in the main window";
    }
    for (const char* o : {kComboName, kSpinName, kColorName, kGroupName}) {
        EXPECT_EQ(mw_->findChildren<QWidget*>(QString::fromLatin1(o)).size(), 1)
            << "objectName '" << o << "' is not unique in the main window";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// FormFieldFontToolbar -- the disconnected text-edit toolbar
// ═══════════════════════════════════════════════════════════════════════════

class FormFieldFontToolbar : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::exists(formFixture())) << "harness: test_form.pdf missing";
        mw_ = std::make_unique<MainWindow>();
        mw_->openFile(QString::fromStdString(formFixture().string()));
        settle();
        ASSERT_NE(mw_->documentForTesting(), nullptr) << "harness: fixture did not open";
        toolbar_ = mw_->findChild<pdfe::ui::TextEditToolBar*>(QStringLiteral("textEditToolBar"));
        ASSERT_NE(toolbar_, nullptr) << "harness: no textEditToolBar";
        textAct_ = mw_->findChild<QAction*>(QStringLiteral("textEditToolAction"));
        ASSERT_NE(textAct_, nullptr);
    }
    void TearDown() override { mw_.reset(); }

    // Enter Edit Text and put font info on the toolbar, as a clicked span would.
    void enterTextEdit(const char* step) {
        textAct_->trigger();
        settle();
        ASSERT_TRUE(toolbar_->isVisibleTo(mw_.get()))
            << "Edit Text did not show the toolbar (" << step << ")";
        ASSERT_NE(mw_->canvasForTesting()->textEditTool(), nullptr)
            << "harness: no TextEditTool (" << step << ")";
        toolbar_->setFontInfo(QStringLiteral("Times New Roman"), 24.0f);
        ASSERT_FLOAT_EQ(toolbar_->currentFontSizeForTesting(), 24.0f);
    }

    void expectHiddenAndCleared(const char* step) {
        EXPECT_FALSE(toolbar_->isVisibleTo(mw_.get()))
            << kB3 << "text-edit toolbar still visible after " << step;
        EXPECT_FLOAT_EQ(toolbar_->currentFontSizeForTesting(), 0.0f)
            << kB3 << "font info not cleared after " << step;
        auto* combo = toolbar_->findChild<QFontComboBox*>();
        ASSERT_NE(combo, nullptr);
        EXPECT_TRUE(combo->currentText().isEmpty())
            << kB3 << "font family not cleared after " << step << ": "
            << combo->currentText().toStdString();
        EXPECT_EQ(mw_->canvasForTesting()->textEditTool(), nullptr)
            << "harness: the TextEditTool survived " << step;
    }

    std::unique_ptr<MainWindow> mw_;
    pdfe::ui::TextEditToolBar* toolbar_ = nullptr;
    QAction* textAct_ = nullptr;
};

TEST_F(FormFieldFontToolbar, HiddenAndClearedUnderTheFormToolAndRestoredUnderTextEdit) {
    auto* formAct = mw_->findChild<QAction*>(QStringLiteral("formToolAction"));
    ASSERT_NE(formAct, nullptr);

    enterTextEdit("start");
    formAct->trigger();
    settle();
    ASSERT_NE(liveFormTool(*mw_), nullptr) << "harness: Form tool did not activate";
    expectHiddenAndCleared("Edit Text -> Form tool");

    enterTextEdit("Form tool -> Edit Text");  // restored
}

TEST_F(FormFieldFontToolbar, HiddenOnEveryOtherToolSwitchPath) {
    auto* createField = mw_->findChild<QAction*>(QStringLiteral("createFormFieldAction"));
    auto* ink = mw_->findChild<QAction*>(QStringLiteral("inkToolAction"));
    auto* highlight = mw_->findChild<QAction*>(QStringLiteral("highlightToolAction"));
    auto* select = mw_->findChild<QAction*>(QStringLiteral("selectToolAction"));
    auto* image = mw_->findChild<QAction*>(QStringLiteral("imageToolAction"));
    ASSERT_NE(createField, nullptr);
    ASSERT_NE(ink, nullptr);
    ASSERT_NE(highlight, nullptr);
    ASSERT_NE(select, nullptr);
    ASSERT_NE(image, nullptr);
    ASSERT_TRUE(createField->isEnabled()) << "harness: Create Field disabled with a form open";

    enterTextEdit("start");
    createField->trigger();  // Forms > Create Field: the menu route into the Form tool
    settle();
    ASSERT_NE(liveFormTool(*mw_), nullptr) << "harness: Create Field did not activate the Form tool";
    expectHiddenAndCleared("Edit Text -> Forms > Create Field");

    enterTextEdit("Create Field -> Edit Text");
    ink->trigger();
    settle();
    expectHiddenAndCleared("Edit Text -> Ink");

    enterTextEdit("Ink -> Edit Text");
    highlight->trigger();
    settle();
    expectHiddenAndCleared("Edit Text -> Highlight");

    enterTextEdit("Highlight -> Edit Text");
    select->trigger();
    settle();
    expectHiddenAndCleared("Edit Text -> Select");

    enterTextEdit("Select -> Edit Text");
    image->trigger();
    settle();
    expectHiddenAndCleared("Edit Text -> Image");

    enterTextEdit("Image -> Edit Text");
}

}  // namespace
