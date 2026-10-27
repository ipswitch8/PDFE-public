#include "TextEditToolBar.h"
#include <QComboBox>
#include <QLabel>
#include <QFontComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QToolButton>
#include <QColorDialog>

namespace pdfe::ui {

namespace {

// Strip the PDF subset prefix from a font name for display.
// PDF 32000-1 §9.6.4 specifies subset font names as six uppercase
// ASCII letters + '+' + the original font name (e.g.
// "AAAAAA+PalatinoLTPro-Bold"). The prefix is a PDF-internal
// artifact — same family across multiple subsets gets different
// prefixes — and is meaningless to the user. The internal model
// keeps the prefix; the toolbar displays only the family.
QString stripSubsetPrefix(const QString& name)
{
    if (name.size() >= 8 && name[6] == QLatin1Char('+')) {
        bool allUpperAlpha = true;
        for (int i = 0; i < 6; ++i) {
            const QChar c = name[i];
            if (!(c >= QLatin1Char('A') && c <= QLatin1Char('Z'))) {
                allUpperAlpha = false;
                break;
            }
        }
        if (allUpperAlpha)
            return name.mid(7);
    }
    return name;
}

} // namespace

TextEditToolBar::TextEditToolBar(QWidget* parent)
    : QToolBar(tr("Text Edit"), parent)
{
    setObjectName("textEditToolBar");

    // Font family selector
    auto* fontLabel = new QLabel(tr("Font: "), this);
    addWidget(fontLabel);

    fontComboBox_ = new QFontComboBox(this);
    fontComboBox_->setObjectName("fontComboBox");
    fontComboBox_->setAccessibleName(tr("Font Family"));
    fontComboBox_->setMinimumWidth(180);
    addWidget(fontComboBox_);

    addSeparator();

    // Font size selector
    auto* sizeLabel = new QLabel(tr("Size: "), this);
    addWidget(sizeLabel);

    fontSizeSpinBox_ = new QDoubleSpinBox(this);
    fontSizeSpinBox_->setObjectName("fontSizeSpinBox");
    fontSizeSpinBox_->setAccessibleName(tr("Font Size"));
    fontSizeSpinBox_->setRange(0.0, 999.0);
    fontSizeSpinBox_->setSingleStep(0.5);
    fontSizeSpinBox_->setSuffix(tr(" pt"));
    fontSizeSpinBox_->setSpecialValueText(tr("-"));
    fontSizeSpinBox_->setMinimumWidth(80);
    addWidget(fontSizeSpinBox_);

    addSeparator();

    // Bold button
    boldButton_ = new QToolButton(this);
    boldButton_->setObjectName("boldButton");
    boldButton_->setAccessibleName(tr("Bold"));
    boldButton_->setText(tr("B"));
    boldButton_->setCheckable(true);
    boldButton_->setShortcut(QKeySequence::Bold);
    boldButton_->setToolTip(tr("Bold (Ctrl+B)"));
    QFont boldFont = boldButton_->font();
    boldFont.setBold(true);
    boldButton_->setFont(boldFont);
    addWidget(boldButton_);

    // Italic button
    italicButton_ = new QToolButton(this);
    italicButton_->setObjectName("italicButton");
    italicButton_->setAccessibleName(tr("Italic"));
    italicButton_->setText(tr("I"));
    italicButton_->setCheckable(true);
    italicButton_->setShortcut(QKeySequence::Italic);
    italicButton_->setToolTip(tr("Italic (Ctrl+I)"));
    QFont italicFont = italicButton_->font();
    italicFont.setItalic(true);
    italicButton_->setFont(italicFont);
    addWidget(italicButton_);

    // Underline button
    underlineButton_ = new QToolButton(this);
    underlineButton_->setObjectName("underlineButton");
    underlineButton_->setAccessibleName(tr("Underline"));
    underlineButton_->setText(tr("U"));
    underlineButton_->setCheckable(true);
    underlineButton_->setShortcut(QKeySequence::Underline);
    underlineButton_->setToolTip(tr("Underline (Ctrl+U)"));
    QFont underlineFont = underlineButton_->font();
    underlineFont.setUnderline(true);
    underlineButton_->setFont(underlineFont);
    addWidget(underlineButton_);

    // Strikethrough button
    strikethroughButton_ = new QToolButton(this);
    strikethroughButton_->setObjectName("strikethroughButton");
    strikethroughButton_->setAccessibleName(tr("Strikethrough"));
    strikethroughButton_->setText(tr("S"));
    strikethroughButton_->setCheckable(true);
    strikethroughButton_->setToolTip(tr("Strikethrough"));
    QFont strikeFont = strikethroughButton_->font();
    strikeFont.setStrikeOut(true);
    strikethroughButton_->setFont(strikeFont);
    addWidget(strikethroughButton_);

    addSeparator();

    // Text color button
    textColorButton_ = new QToolButton(this);
    textColorButton_->setObjectName("textColorButton");
    textColorButton_->setAccessibleName(tr("Text Color"));
    textColorButton_->setText(tr("A"));
    textColorButton_->setToolTip(tr("Text Color"));
    textColorButton_->setFixedSize(28, 28);
    textColorButton_->setStyleSheet(
        QStringLiteral("QToolButton { color: %1; font-weight: bold; }")
            .arg(currentTextColor_.name()));
    addWidget(textColorButton_);

    connect(textColorButton_, &QToolButton::clicked, this, [this]() {
        QColor chosen = QColorDialog::getColor(
            currentTextColor_, this, tr("Select Text Color"));
        if (chosen.isValid() && chosen != currentTextColor_) {
            currentTextColor_ = chosen;
            textColorButton_->setStyleSheet(
                QStringLiteral("QToolButton { color: %1; font-weight: bold; }")
                    .arg(currentTextColor_.name()));
            if (!updatingFromSelection_) {
                emit textColorChanged(currentTextColor_);
            }
        }
    });

    // Connect signals - only emit when not updating from selection
    connect(fontComboBox_, &QFontComboBox::currentFontChanged, this, [this](const QFont& font) {
        if (!updatingFromSelection_) {
            emit fontFamilyChanged(font.family());
        }
    });

    connect(fontSizeSpinBox_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!updatingFromSelection_) {
            emit fontSizeChanged(static_cast<float>(value));
        }
    });

    connect(boldButton_, &QToolButton::toggled, this, [this](bool checked) {
        if (!updatingFromSelection_) {
            emit boldToggled(checked);
        }
    });

    connect(italicButton_, &QToolButton::toggled, this, [this](bool checked) {
        if (!updatingFromSelection_) {
            emit italicToggled(checked);
        }
    });

    connect(underlineButton_, &QToolButton::toggled, this, [this](bool checked) {
        if (!updatingFromSelection_) {
            emit underlineToggled(checked);
        }
    });

    connect(strikethroughButton_, &QToolButton::toggled, this, [this](bool checked) {
        if (!updatingFromSelection_) {
            emit strikethroughToggled(checked);
        }
    });

    addSeparator();

    // Alignment buttons
    // Alignment glyphs: bar-stack symbols (UI-VF-01..04). The pre-Phase-9
    // glyphs (\u2190 \u2194 \u2192 \u2195) were generic arrows that semantically meant
    // "drag handle" / "vertical position adjustment", not text alignment.
    // The bar-stack glyphs read universally as alignment in toolbar UIs.
    alignLeftButton_ = new QToolButton(this);
    alignLeftButton_->setObjectName("alignLeftButton");
    alignLeftButton_->setAccessibleName(tr("Align Left"));
    alignLeftButton_->setText(QStringLiteral("\u2b05"));  // \u2b05 left bar-arrow
    alignLeftButton_->setCheckable(true);
    alignLeftButton_->setChecked(true); // default to left-aligned
    alignLeftButton_->setToolTip(tr("Align Left"));
    addWidget(alignLeftButton_);

    alignCenterButton_ = new QToolButton(this);
    alignCenterButton_->setObjectName("alignCenterButton");
    alignCenterButton_->setAccessibleName(tr("Align Center"));
    alignCenterButton_->setText(QStringLiteral("\u2261"));  // \u2261 three bars
    alignCenterButton_->setCheckable(true);
    alignCenterButton_->setToolTip(tr("Align Center"));
    addWidget(alignCenterButton_);

    alignRightButton_ = new QToolButton(this);
    alignRightButton_->setObjectName("alignRightButton");
    alignRightButton_->setAccessibleName(tr("Align Right"));
    alignRightButton_->setText(QStringLiteral("\u27a1"));  // \u27a1 right bar-arrow
    alignRightButton_->setCheckable(true);
    alignRightButton_->setToolTip(tr("Align Right"));
    addWidget(alignRightButton_);

    alignJustifyButton_ = new QToolButton(this);
    alignJustifyButton_->setObjectName("alignJustifyButton");
    alignJustifyButton_->setAccessibleName(tr("Justify"));
    alignJustifyButton_->setText(QStringLiteral("\u2630"));  // \u2630 trigram (justify)
    alignJustifyButton_->setCheckable(true);
    alignJustifyButton_->setToolTip(tr("Justify"));
    addWidget(alignJustifyButton_);

    auto uncheckOtherAlignButtons = [this](QToolButton* active, Qt::Alignment align) {
        if (!updatingFromSelection_) {
            for (auto* btn : {alignLeftButton_, alignCenterButton_, alignRightButton_, alignJustifyButton_}) {
                if (btn != active) btn->setChecked(false);
            }
            if (active->isChecked()) {
                emit alignmentChanged(align);
            } else {
                active->setChecked(true); // at least one must be checked
            }
        }
    };

    connect(alignLeftButton_, &QToolButton::toggled, this, [=](bool checked) {
        if (checked) uncheckOtherAlignButtons(alignLeftButton_, Qt::AlignLeft);
    });
    connect(alignCenterButton_, &QToolButton::toggled, this, [=](bool checked) {
        if (checked) uncheckOtherAlignButtons(alignCenterButton_, Qt::AlignHCenter);
    });
    connect(alignRightButton_, &QToolButton::toggled, this, [=](bool checked) {
        if (checked) uncheckOtherAlignButtons(alignRightButton_, Qt::AlignRight);
    });
    connect(alignJustifyButton_, &QToolButton::toggled, this, [=](bool checked) {
        if (checked) uncheckOtherAlignButtons(alignJustifyButton_, Qt::AlignJustify);
    });

    // Atlas mode controls (hidden by default)
    atlasSeparatorAction_ = addSeparator();

    atlasModeCombo_ = new QComboBox(this);
    atlasModeCombo_->setObjectName("atlasModeCombo");
    atlasModeCombo_->setAccessibleName(QStringLiteral("atlas-mode-combo"));
    atlasModeCombo_->addItem(tr("Scan Glyphs"));
    atlasModeCombo_->addItem(tr("Font Matched"));
    atlasModeCombo_->setToolTip(tr("Atlas rendering mode"));
    atlasModeComboAction_ = addWidget(atlasModeCombo_);

    compareButton_ = new QToolButton(this);
    compareButton_->setObjectName("compareAtlasButton");
    compareButton_->setAccessibleName(QStringLiteral("atlas-compare-button"));
    compareButton_->setText(tr("Compare..."));
    compareButton_->setToolTip(tr("Compare atlas modes side-by-side"));
    compareButtonAction_ = addWidget(compareButton_);

    // Hide atlas controls by default
    showAtlasControls(false);

    connect(atlasModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (!updatingFromSelection_) {
            emit atlasModeChanged(index);
        }
    });

    connect(compareButton_, &QToolButton::clicked, this, [this]() {
        emit compareAtlasRequested();
    });
}

void TextEditToolBar::setFontSizeForTesting(float sizePt)
{
    // Drive the spinbox exactly as a user edit would: ensure the
    // updatingFromSelection_ guard is clear so the valueChanged lambda emits
    // fontSizeChanged (the production apply path), then restore the prior
    // guard state. setValue is a no-op (no signal) if the value is unchanged,
    // so callers should pick a target that differs from the current size.
    const bool wasUpdating = updatingFromSelection_;
    updatingFromSelection_ = false;
    fontSizeSpinBox_->setValue(static_cast<double>(sizePt));
    updatingFromSelection_ = wasUpdating;
}

float TextEditToolBar::currentFontSizeForTesting() const
{
    return static_cast<float>(fontSizeSpinBox_->value());
}

void TextEditToolBar::setFontInfo(const QString& fontName, float fontSize)
{
    updatingFromSelection_ = true;

    // Set font family. Qt's QFontComboBox::setCurrentFont passes the
    // input through QFontInfo, which on Windows substitutes a
    // visually-related installed family for fonts the system doesn't
    // ship (e.g. Hebrew "Alef" → "Tahoma"). The substituted name then
    // shows in the combo's line edit, hiding the actual PDF font name
    // from the user. We keep the standard setCurrentFont call (so the
    // dropdown's preview behaviour and font-pick path stay intact) and
    // then override the displayed text with the literal fontName via
    // setEditText. The user-initiated font-change path is unaffected:
    // the underlying QComboBox emits currentFontChanged when the user
    // edits the line edit, which our connect handler already gates on
    // updatingFromSelection_.
    if (!fontName.isEmpty()) {
        const QString displayName = stripSubsetPrefix(fontName);
        QFont font;
        font.setFamily(displayName);
        fontComboBox_->setCurrentFont(font);
        fontComboBox_->setEditText(displayName);
        if (auto* le = fontComboBox_->lineEdit()) {
            le->setText(displayName);
        }
    }

    // Set font size
    if (fontSize > 0.0f) {
        fontSizeSpinBox_->setValue(static_cast<double>(fontSize));
    }

    // Reset all style buttons to unchecked
    boldButton_->setChecked(false);
    italicButton_->setChecked(false);
    underlineButton_->setChecked(false);
    strikethroughButton_->setChecked(false);

    updatingFromSelection_ = false;
}

void TextEditToolBar::clearFontInfo()
{
    updatingFromSelection_ = true;

    // Empty the combo's line edit. Earlier this called
    // setCurrentIndex(0), which left the combo showing the
    // alphabetically-first installed family — "Alef" on systems
    // where the Hebrew family is installed (e.g. via translation
    // toolchains). Users perceived the empty/no-selection state
    // as the toolbar mis-identifying the PDF's font.
    if (auto* le = fontComboBox_->lineEdit()) {
        le->clear();
    } else {
        fontComboBox_->setEditText(QString());
    }

    // Reset spin box
    fontSizeSpinBox_->setValue(0.0);

    // Uncheck all buttons
    boldButton_->setChecked(false);
    italicButton_->setChecked(false);
    underlineButton_->setChecked(false);
    strikethroughButton_->setChecked(false);

    updatingFromSelection_ = false;
}

void TextEditToolBar::setSelectionFormatting(const QString& fontFamily, float fontSize,
                                             bool bold, bool italic,
                                             bool underline, bool strikethrough,
                                             QColor textColor)
{
    updatingFromSelection_ = true;

    // Update font family if provided. Same QFontInfo resolution
    // override as setFontInfo — without setEditText, an embedded font
    // like "Alef" would display as a Qt-resolved substitute (e.g.
    // "Tahoma") in the combo's line edit on every selection-change
    // emission, causing a visible jump on the user's first keystroke.
    if (!fontFamily.isEmpty()) {
        const QString displayName = stripSubsetPrefix(fontFamily);
        QFont font;
        font.setFamily(displayName);
        fontComboBox_->setCurrentFont(font);
        fontComboBox_->setEditText(displayName);
        if (auto* le = fontComboBox_->lineEdit()) {
            le->setText(displayName);
        }
    }

    // Update font size if valid
    if (fontSize > 0.0f) {
        fontSizeSpinBox_->setValue(static_cast<double>(fontSize));
    }

    // Update style buttons
    boldButton_->setChecked(bold);
    italicButton_->setChecked(italic);
    underlineButton_->setChecked(underline);
    strikethroughButton_->setChecked(strikethrough);

    // Update color button swatch
    if (textColor.isValid()) {
        currentTextColor_ = textColor;
        textColorButton_->setStyleSheet(
            QStringLiteral("QToolButton { color: %1; font-weight: bold; }")
                .arg(currentTextColor_.name()));
    }

    updatingFromSelection_ = false;
}

void TextEditToolBar::showAtlasControls(bool visible)
{
    atlasSeparatorAction_->setVisible(visible);
    atlasModeComboAction_->setVisible(visible);
    compareButtonAction_->setVisible(visible);
}

void TextEditToolBar::setAtlasMode(int mode)
{
    updatingFromSelection_ = true;
    atlasModeCombo_->setCurrentIndex(mode);
    updatingFromSelection_ = false;
}

} // namespace pdfe::ui
