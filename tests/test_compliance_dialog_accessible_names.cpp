// -*- coding: utf-8 -*-
//
// GitHub #286 — ComplianceDialog is open but announces nothing.
//
// The reported symptom:
//
//   test_ui_compliance_accessibility ::
//     test_uia_events_announce_menu_dialog_and_page_text_names
//   AssertionError: the ComplianceDialog window is open (confirmed via Win32:
//     ['PDF Standards Compliance', ...]) but announced nothing
//
// WHY THIS IS A gtest AND NOT A pywinauto SCRIPT.
// The phase criterion says "verified by a pywinauto or UIA-tree script that
// enumerates the dialog's controls". A UIA enumeration cannot do that job
// here: pywinauto's UIA backend cannot enumerate a Qt APP-MODAL dialog while
// its exec() loop runs — this repo measured a dialog sitting open for four
// minutes while a 240 s UIA poll reported nothing, and that is why
// `registerDialogIntrospection()` exists at all. A UIA script would therefore
// be asserting against an empty tree and would pass for the wrong reason.
//
// Enumerating the real QWidget tree is strictly stronger: it sees every child,
// it needs no window manager or interactive desktop, and it cannot be fooled by
// a control that UIA simply failed to surface. The one thing it does not prove
// is that UIA forwards the names — which is what
// test_ui_compliance_accessibility already covers end-to-end.
//
// WHAT IT PINS. Every control the dialog authors — interactive controls and
// labels alike — carries a stable objectName, and every INTERACTIVE control
// additionally carries an accessibleName.
//
// Labels are deliberately exempt from the name requirement. An accessible name
// OVERRIDES the text in what UIA reports, so naming a label whose text is the
// INFORMATION hides the value behind a constant. An earlier version of this
// test demanded names for labels; satisfying it broke seven tests at once. See
// requiresAccessibleName() for the measurement.

#include <gtest/gtest.h>

#include "ui/ComplianceDialog.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QHeaderView>
#include <QLineEdit>
#include <QWidget>

#include <string>
#include <vector>

namespace {

/// A control a user can reach or act on, and which therefore needs a NAME of
/// its own because it has no text that means anything on its own.
///
/// QLabel is deliberately NOT here — see requiresAccessibleName below.
bool isInteractive(const QWidget* w) {
    return qobject_cast<const QAbstractButton*>(w) != nullptr
        || qobject_cast<const QComboBox*>(w) != nullptr
        || qobject_cast<const QLineEdit*>(w) != nullptr
        || qobject_cast<const QAbstractItemView*>(w) != nullptr;
}

/// Everything this test inspects at all: interactive controls plus labels.
bool isAnnounceable(const QWidget* w) {
    return isInteractive(w) || qobject_cast<const QLabel*>(w) != nullptr;
}

/// Whether @p w must carry an explicit accessibleName.
///
/// INTERACTIVE CONTROLS: yes. A button's visible text is a verb, a combo has no
/// caption of its own; without a name a screen reader has nothing useful.
///
/// QLabel: NO, and setting one is actively harmful when the label's text is the
/// INFORMATION rather than a caption. An accessible name OVERRIDES the text in
/// what UIA reports, so a static name pins the announcement to a constant and
/// hides the live value.
///
/// That is measured, not theoretical. Setting
/// `statusLabel_->setAccessibleName(tr("Compliance Status"))` to satisfy an
/// earlier version of this test broke SEVEN tests at once — every PDF/A and
/// PDF/UA validate row plus the compliance-report row — each reporting
/// "StatusLabel did not transition (still 'Compliance Status')". The label had
/// started announcing its caption instead of its value.
///
/// So labels are checked for a stable objectName only. Their text is their
/// announcement, and the dialog-level test below covers the thing #286 actually
/// reported: that the DIALOG announces itself when it opens.
bool requiresAccessibleName(const QWidget* w) {
    return isInteractive(w);
}

/// True for a widget Qt builds as part of ANOTHER control's implementation.
///
/// A QTableWidget brings three QHeaderViews and a QTableCornerButton; a
/// QComboBox brings a QComboBoxListView for its popup. Those are parts of an
/// already-named control, not controls the dialog authored, and a screen reader
/// announces them through their owner. Demanding names for them would push the
/// test into asserting Qt's internals rather than this dialog's.
///
/// Detected by ANCESTRY, not by class name or an objectName prefix. The first
/// version of this test used `objectName().startsWith("qt_")`, which matched
/// none of them — Qt leaves these unnamed — so the test failed on five widgets
/// nobody should name. Ancestry is the property that actually distinguishes
/// "part of a control" from "a control".
bool isInternalPartOfAnotherControl(const QWidget* w) {
    if (w->objectName().startsWith(QLatin1String("qt_"))) {
        return true;
    }
    for (const QObject* p = w->parent(); p != nullptr; p = p->parent()) {
        if (qobject_cast<const QAbstractItemView*>(p) != nullptr
            || qobject_cast<const QComboBox*>(p) != nullptr) {
            return true;
        }
    }
    // A combo's popup view is parented to a transient popup container rather
    // than to the combo itself on some styles, so catch it by type too.
    return qobject_cast<const QHeaderView*>(w) != nullptr;
}

}  // namespace

TEST(ComplianceDialogAccessibility, EveryControlHasAnObjectNameAndAccessibleName) {
    ASSERT_NE(qApp, nullptr) << "needs a QApplication (provided by the suite main)";

    pdfe::ui::ComplianceDialog dlg(nullptr, nullptr, nullptr);

    std::vector<std::string> missingObjectName;
    std::vector<std::string> missingAccessibleName;
    int inspected = 0;

    const QList<QWidget*> kids = dlg.findChildren<QWidget*>();
    for (QWidget* w : kids) {
        if (!isAnnounceable(w) || isInternalPartOfAnotherControl(w)) {
            continue;
        }
        ++inspected;
        const QString on = w->objectName();
        const QString an = w->accessibleName();
        // Identify the offender by whatever it DOES have, so the failure names
        // something a maintainer can find in the source.
        const std::string ident =
            (on.isEmpty() ? (w->metaObject()->className()
                             + QStringLiteral(" text=\"") + w->property("text").toString()
                             + QStringLiteral("\""))
                          : on).toStdString();
        if (on.isEmpty()) missingObjectName.push_back(ident);
        if (an.isEmpty() && requiresAccessibleName(w))
            missingAccessibleName.push_back(ident);
    }

    // VACUITY GUARD. If the dialog ever stops building its children in the
    // constructor, every loop above would find nothing and this test would pass
    // while asserting precisely nothing. The dialog has a combo, four buttons,
    // a table, a tree and four labels.
    EXPECT_GE(inspected, 10)
        << "only " << inspected << " announceable controls found; the dialog "
           "should have at least ten (combo + 4 buttons + table + tree + "
           "labels). A near-empty inventory means this test is vacuous, not "
           "that the dialog is clean.";

    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& e : v) { s += "\n    "; s += e; }
        return s;
    };

    EXPECT_TRUE(missingObjectName.empty())
        << "these ComplianceDialog controls have no objectName, so no test can "
           "address them by a stable id:" << join(missingObjectName);

    EXPECT_TRUE(missingAccessibleName.empty())
        << "these ComplianceDialog controls have no accessibleName, so a screen "
           "reader announces nothing when focus lands on them (GitHub #286). An "
           "accessibleDescription does NOT substitute: UIA exposes it as a "
           "secondary property that is often not spoken."
        << join(missingAccessibleName);
}

TEST(ComplianceDialogAccessibility, TheDialogItselfAnnouncesItsTitle) {
    ASSERT_NE(qApp, nullptr);
    pdfe::ui::ComplianceDialog dlg(nullptr, nullptr, nullptr);

    EXPECT_EQ(dlg.objectName(), QStringLiteral("ComplianceDialog"));
    // The accessible name OVERRIDES the window title in what a screen reader
    // announces, so it must BE the title rather than the class name.
    EXPECT_EQ(dlg.accessibleName(), dlg.windowTitle())
        << "the dialog's accessible name must match its visible title; "
           "otherwise the announcement and the caption disagree";
    EXPECT_FALSE(dlg.windowTitle().isEmpty());
}
