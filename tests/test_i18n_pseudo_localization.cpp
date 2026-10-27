// -*- coding: utf-8 -*-
//
// Phase F11 — gaps WIN-50 / WIN-51: pseudo-localization and RTL layout.
//
// WHAT WAS WRONG BEFORE
// ---------------------
// `pseudo.loc` / `pseudolocal` had zero hits tree-wide. The i18n story is
// infrastructure-only: there are no compiled .qm catalogues, no LinguistTools
// in CMake, and translations/app_fr.ts is a ~6-message stub. So nothing had
//
// >>> SUPERSEDED IN PART, 2026-09-06 (GitHub #128). Two of the three claims in
// >>> the paragraph below are no longer true: LinguistTools IS in the build
// >>> (via the `qttools` vcpkg dependency added for #128) and compiled .qm
// >>> catalogues ARE produced and shipped -- qt_add_lrelease in
// >>> src/app/CMakeLists.txt, staged next to pdfe.exe and installed to
// >>> translations/ for CPack. app_fr.ts is still a ~6-message stub, so the
// >>> catalogue is real but small.
// >>>
// >>> What this file argues remains correct and it is left alone: a
// >>> QTranslator subclass is still the right way to test tr() routing and
// >>> layout growth headlessly. The ".qm loading path itself" that the note
// >>> below records as uncovered is now the part that became testable, and
// >>> that is tracked separately rather than bolted on here.
//
// ever proved that (a) the UI strings are actually routed through tr() at all,
// or (b) a layout survives the 30-40% string growth that real translation
// causes, or (c) the widget tree mirrors correctly right-to-left.
//
// WHY A TRANSLATOR SUBCLASS RATHER THAN A GENERATED .qm
// ------------------------------------------------------
// WIN-51's suggested route is `lrelease` over a script-transformed .ts. That
// needs the Qt LinguistTools component in the build, a build-time generation
// step, and a live UI launch to screenshot — an XL of new infrastructure. A
// QTranslator subclass installed into the running QApplication reaches exactly
// the same code path (QCoreApplication::translate consults installed
// translators in reverse install order), needs no toolchain change, and runs
// headlessly in the normal suite. What it CANNOT cover is the .qm loading path
// itself; that stays open and is recorded in phase-11-dispositions.md.
//
// WHAT THIS FILE ASSERTS
// ----------------------
//   * NON-ASCII SUBSTITUTION: with the pseudo-locale installed, the visible
//     strings of real product dialogs actually come back accented. A dialog
//     that hard-codes its labels instead of calling tr() shows plain ASCII and
//     REDs this — that is the defect pseudo-localization exists to find.
//   * EXPANSION SURVIVAL: with every string ~40% longer, no push-button's text
//     is clipped by its own width, and no child widget escapes the dialog's
//     rectangle. A dialog with a hard-coded width REDs this.
//   * RTL (WIN-50): under Qt::RightToLeft the dialogs' layouts mirror (a
//     control's x position genuinely moves) and nothing spills outside the
//     parent rect.

#include <gtest/gtest.h>

#include "ui/CustomizeToolbarsDialog.h"
#include "ui/ValidationRequestDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QChar>
#include <QDialog>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <QWidget>

#include <functional>
#include <memory>
#include <vector>

using namespace pdfe::ui;

namespace {

// ── The pseudo-locale ──────────────────────────────────────────────────────
//
// Latin letters are replaced by visually-similar accented forms (so a human
// can still read the UI) and the string is padded to ~140% of its original
// length, bracketed so truncation is obvious. This is the standard
// pseudo-localization recipe: substitution proves the string is translatable,
// expansion proves the layout is elastic.
class PseudoLocaleTranslator : public QTranslator {
public:
    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation = nullptr,
                      int n = -1) const override
    {
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (!sourceText || !*sourceText) return QString();
        ++consulted_;
        return pseudo(QString::fromUtf8(sourceText));
    }

    bool isEmpty() const override { return false; }

    int consulted() const { return consulted_; }
    void resetCounter() const { consulted_ = 0; }

    static QString pseudo(const QString& in)
    {
        static const QString kFrom = QStringLiteral("aeiouyAEIOUYcnsCNSzZgGdDlLtTrRbBpPfFhHmMwWkKvVxXjJqQ");
        static const QString kTo   = QString::fromUtf8(
            "àéîöüÿ"          // a e i o u y
            "ÀÉÎÖÜŸ"          // A E I O U Y
            "çñšÇÑŠ"          // c n s C N S
            "žŽğĞďĎ"          // z Z g G d D
            "ĺĹťŤřŘ"          // l L t T r R
            "ḃḂṗṖḟḞ"          // b B p P f F
            "ḥḤḿḾẃẂ"          // h H m M w W
            "ḱḰṽṼẋẊ"          // k K v V x X
            "ĵĴǫǪ");                    // j J q Q
        QString out;
        out.reserve(in.size() * 2 + 6);
        out.append(QChar(0x00ab));                          // «
        for (QChar c : in) {
            const int idx = kFrom.indexOf(c);
            out.append((idx >= 0 && idx < kTo.size()) ? kTo.at(idx) : c);
        }
        // ~40% expansion, the documented worst case for de-DE / fi-FI.
        const int pad = (in.size() * 2) / 5 + 2;
        for (int i = 0; i < pad; ++i) out.append(QChar(0x00b7));   // ·
        out.append(QChar(0x00bb));                          // »
        return out;
    }

private:
    mutable int consulted_ = 0;
};

QDialog* makeValidationRequestDialog()
{
    return new ValidationRequestDialog(
        QStringList{QStringLiteral("http://ocsp.example.test/responder")},
        QStringList{QStringLiteral("http://crl.example.test/list.crl")}, nullptr);
}

QDialog* makeCustomizeToolbarsDialog()
{
    return new CustomizeToolbarsDialog(std::vector<QAction*>{},
                                       std::vector<ToolbarLayout>{}, nullptr);
}

struct DialogFactory {
    const char* name;
    std::function<QDialog*()> make;
};

const std::vector<DialogFactory>& dialogsUnderTest()
{
    static const std::vector<DialogFactory> kDialogs = {
        {"ValidationRequestDialog", makeValidationRequestDialog},
        {"CustomizeToolbarsDialog", makeCustomizeToolbarsDialog},
    };
    return kDialogs;
}

bool hasNonAscii(const QString& s)
{
    for (QChar c : s)
        if (c.unicode() > 127) return true;
    return false;
}

void layOut(QDialog* dlg)
{
    dlg->ensurePolished();
    // The dialog is actually SHOWN. A never-shown top-level widget never
    // receives a QResizeEvent, so its layout keeps whatever geometry the
    // constructor left behind and the "child escapes the dialog" oracle would
    // fire on the TEST's sizing rather than on a product defect. Showing it
    // gives the real, natural, laid-out geometry. The dialogs under test are
    // modeless and are destroyed at the end of the scope.
    dlg->show();
    QApplication::processEvents();
    if (dlg->layout()) dlg->layout()->activate();
    QApplication::processEvents();
}

class I18nPseudoLocalizationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        savedDirection_ = QApplication::layoutDirection();
        translator_ = std::make_unique<PseudoLocaleTranslator>();
    }
    void TearDown() override
    {
        if (installed_) QApplication::instance()->removeTranslator(translator_.get());
        QApplication::setLayoutDirection(savedDirection_);
    }
    void installPseudoLocale()
    {
        QApplication::instance()->installTranslator(translator_.get());
        installed_ = true;
    }

    std::unique_ptr<PseudoLocaleTranslator> translator_;
    bool installed_ = false;
    Qt::LayoutDirection savedDirection_ = Qt::LeftToRight;
};

} // namespace

// ─── the transform itself, so a broken recipe is not mistaken for a UI bug ──

TEST_F(I18nPseudoLocalizationTest, PseudoTransformSubstitutesAndExpands)
{
    const QString src = QStringLiteral("Fetch revocation data");
    const QString out = PseudoLocaleTranslator::pseudo(src);

    EXPECT_TRUE(hasNonAscii(out)) << "pseudo() produced pure ASCII";
    EXPECT_GE(out.size(), src.size() * 13 / 10)
        << "pseudo() expansion is under 30%: " << out.size() << " vs " << src.size();
    EXPECT_TRUE(out.startsWith(QChar(0x00ab)) && out.endsWith(QChar(0x00bb)))
        << "pseudo() must bracket the string so truncation is visible";
}

// ─── WIN-51: non-ASCII substitution reaches the real dialogs ───────────────

TEST_F(I18nPseudoLocalizationTest, VisibleDialogStringsAreSubstitutedByThePseudoLocale)
{
    ASSERT_NE(qApp, nullptr);
    installPseudoLocale();

    for (const auto& d : dialogsUnderTest()) {
        translator_->resetCounter();
        std::unique_ptr<QDialog> dlg(d.make());
        ASSERT_NE(dlg, nullptr) << d.name;
        layOut(dlg.get());

        EXPECT_GT(translator_->consulted(), 0)
            << d.name << " never consulted the installed translator — its "
               "strings are not wrapped in tr() and can never be localized";

        int textual = 0, substituted = 0;
        for (QAbstractButton* b : dlg->findChildren<QAbstractButton*>()) {
            if (b->text().isEmpty()) continue;
            ++textual;
            if (hasNonAscii(b->text())) ++substituted;
        }
        for (QLabel* l : dlg->findChildren<QLabel*>()) {
            if (l->text().isEmpty()) continue;
            ++textual;
            if (hasNonAscii(l->text())) ++substituted;
        }
        ASSERT_GT(textual, 0) << d.name << " exposes no textual controls";
        EXPECT_GE(substituted, (textual * 2) / 3)
            << d.name << ": only " << substituted << " of " << textual
            << " visible strings were substituted — the rest are hard-coded "
               "literals that no translation can reach";
    }
}

// ─── WIN-51: expansion survival ───────────────────────────────────────────

TEST_F(I18nPseudoLocalizationTest, ExpandedStringsAreNotClippedAndDoNotEscapeTheDialog)
{
    ASSERT_NE(qApp, nullptr);
    installPseudoLocale();

    for (const auto& d : dialogsUnderTest()) {
        std::unique_ptr<QDialog> dlg(d.make());
        ASSERT_NE(dlg, nullptr) << d.name;
        layOut(dlg.get());

        const QRect frame = dlg->rect();
        ASSERT_GT(frame.width(), 0) << d.name;

        int checkedButtons = 0;
        for (QPushButton* b : dlg->findChildren<QPushButton*>()) {
            if (b->text().isEmpty() || !b->isVisibleTo(dlg.get())) continue;
            ++checkedButtons;
            const QFontMetrics fm(b->font());
            const int needed = fm.horizontalAdvance(b->text());
            // sizeHint is what the layout is asked to honour; if it is smaller
            // than the text the expanded label is clipped at render time.
            EXPECT_GE(b->sizeHint().width(), needed)
                << d.name << ": button " << b->objectName().toStdString()
                << " sizeHint " << b->sizeHint().width()
                << " cannot hold its expanded label (" << needed << "px)";
        }
        EXPECT_GT(checkedButtons, 0) << d.name << " exposed no push buttons";

        // Nothing may spill outside the dialog once the layout has absorbed the
        // expansion. A hard-coded width shows up here.
        for (QWidget* w : dlg->findChildren<QWidget*>()) {
            if (!w->isVisibleTo(dlg.get())) continue;
            if (w->width() <= 0 || w->height() <= 0) continue;
            const QRect mapped(w->mapTo(dlg.get(), QPoint(0, 0)), w->size());
            EXPECT_TRUE(frame.contains(mapped))
                << d.name << ": widget " << w->objectName().toStdString()
                << " rect " << mapped.x() << "," << mapped.y() << " "
                << mapped.width() << "x" << mapped.height()
                << " escapes the dialog " << frame.width() << "x" << frame.height()
                << " under pseudo-locale expansion";
        }
    }
}

TEST_F(I18nPseudoLocalizationTest, TheDialogLayoutGrowsToAbsorbTheExpansion)
{
    ASSERT_NE(qApp, nullptr);

    std::vector<QSize> baseline;
    for (const auto& d : dialogsUnderTest()) {
        std::unique_ptr<QDialog> dlg(d.make());
        layOut(dlg.get());
        baseline.push_back(dlg->sizeHint());
    }

    installPseudoLocale();

    for (size_t i = 0; i < dialogsUnderTest().size(); ++i) {
        std::unique_ptr<QDialog> dlg(dialogsUnderTest()[i].make());
        layOut(dlg.get());
        const QSize expanded = dlg->sizeHint();
        EXPECT_GE(expanded.width(), baseline[i].width())
            << dialogsUnderTest()[i].name
            << ": sizeHint did not grow for 40%-longer strings ("
            << baseline[i].width() << " -> " << expanded.width()
            << ") — the layout is pinned and will clip real translations";
    }
}

// ─── WIN-50: right-to-left mirroring ──────────────────────────────────────

TEST_F(I18nPseudoLocalizationTest, RightToLeftMirrorsTheLayoutWithoutSpilling)
{
    ASSERT_NE(qApp, nullptr);

    for (const auto& d : dialogsUnderTest()) {
        QApplication::setLayoutDirection(Qt::LeftToRight);
        std::unique_ptr<QDialog> ltr(d.make());
        ASSERT_NE(ltr, nullptr) << d.name;
        layOut(ltr.get());

        std::vector<int> ltrX;
        for (QPushButton* b : ltr->findChildren<QPushButton*>())
            ltrX.push_back(b->mapTo(ltr.get(), QPoint(0, 0)).x());

        QApplication::setLayoutDirection(Qt::RightToLeft);
        std::unique_ptr<QDialog> rtl(d.make());
        ASSERT_NE(rtl, nullptr) << d.name;
        layOut(rtl.get());

        EXPECT_EQ(rtl->layoutDirection(), Qt::RightToLeft) << d.name;

        std::vector<int> rtlX;
        for (QPushButton* b : rtl->findChildren<QPushButton*>())
            rtlX.push_back(b->mapTo(rtl.get(), QPoint(0, 0)).x());

        ASSERT_EQ(ltrX.size(), rtlX.size()) << d.name;
        ASSERT_GT(ltrX.size(), 1u) << d.name << " has too few buttons to mirror";
        bool anyMoved = false;
        for (size_t i = 0; i < ltrX.size(); ++i)
            if (ltrX[i] != rtlX[i]) { anyMoved = true; break; }
        EXPECT_TRUE(anyMoved)
            << d.name << ": no control changed x under Qt::RightToLeft — the "
               "layout is not mirroring (absolute positioning or a hard-coded "
               "geometry)";

        const QRect frame = rtl->rect();
        for (QWidget* w : rtl->findChildren<QWidget*>()) {
            if (!w->isVisibleTo(rtl.get())) continue;
            if (w->width() <= 0 || w->height() <= 0) continue;
            const QRect mapped(w->mapTo(rtl.get(), QPoint(0, 0)), w->size());
            EXPECT_TRUE(frame.contains(mapped))
                << d.name << ": widget " << w->objectName().toStdString()
                << " falls outside the dialog under RTL";
        }
    }
}
