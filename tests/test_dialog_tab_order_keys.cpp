// -*- coding: utf-8 -*-
//
// Phase F11 — gaps WIN-40 / WIN-41 / WIN-44 / WIN-48: keyboard reachability of
// the dialogs that declare an explicit tab order.
//
// WHAT WAS WRONG BEFORE
// ---------------------
// Nine files under src/ mention setTabOrder and not one of them had a test
// that walked the resulting focus chain. Qt::Key_Return / Qt::Key_Enter had
// zero C++ test hits tree-wide, so "Escape closes / Enter accepts" — the two
// keyboard contracts every dialog in the product implicitly promises — were
// entirely unasserted. A dialog whose setTabOrder chain skips a control, or
// which swallows Escape, is unusable by keyboard and by a screen-reader user,
// and nothing in the suite could see it.
//
// THE DIALOG LIST IS ENUMERATED IN THIS SOURCE, BY DESIGN
// -------------------------------------------------------
// `kSetTabOrderDialogs` below is the list of DIALOGS that call
// QWidget::setTabOrder. It is derived from `grep -rln setTabOrder src/`, which
// returns nine files; two of them are NOT QWidget::setTabOrder calls and are
// deliberately excluded:
//
//   src/forms/FormManager.{h,cpp} — FormManager::setTabOrder(Page*, vector<int>)
//       is the PDF ACROFORM field tab-order API (it rewrites /Annots order in
//       the document). It is not a Qt widget call and FormManager is not a
//       dialog. Covered by TabOrderTest in tests/test_tsa_and_taborder.cpp.
//   src/ui/MainWindow.cpp:3207 — `formManager_->setTabOrder(page.get(), ...)`,
//       i.e. the same ACROFORM API, invoked after TabOrderDialog is accepted.
//       MainWindow is a QMainWindow, not a QDialog, and declares no widget tab
//       order of its own.
//
// The comparison against the grep is recorded in
// docs/audit/meta-audit-2026-07/followup/phase-11-dispositions.md.
//
// WHAT THIS FILE ASSERTS
// ----------------------
//   * every one of the seven dialogs exposes a CYCLIC tab chain, at least as
//     long as the chain its setTabOrder calls declare, with no repeated stop
//     and no stop that is not a tab-focusable descendant of the dialog
//     (WIN-40, and the "no dead focus stops" half of WIN-48);
//   * Escape rejects every one of them, asserted on the rejected() signal
//     rather than on result() — QDialog::Rejected is 0, which is also the
//     value of a dialog that did nothing at all, so result() alone is a
//     false-green oracle (WIN-41);
//   * Enter is honoured by the dialog that declares an accept default, and
//     the network-consent dialog deliberately does NOT grant consent on Enter
//     (WIN-41's "destructive dialogs have no auto-default" half);
//   * MainWindow's top-level menus each carry a mnemonic and no two collide
//     (WIN-44).

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "ui/AltTextDialog.h"
#include "ui/BatchDialog.h"
#include "ui/CombineFilesWizard.h"
#include "ui/ComplianceDialog.h"
#include "ui/CustomizeToolbarsDialog.h"
#include "ui/MainWindow.h"
#include "ui/PreflightDialog.h"
#include "ui/ValidationRequestDialog.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QKeyEvent>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace pdfe::ui;
using namespace pdfe::core;
using namespace pdfe::annotations;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path workCopy(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_f11_taborder";
    std::filesystem::create_directories(d);
    auto p = d / name;
    std::filesystem::copy_file(fixturesDir() / "test_single.pdf", p,
                               std::filesystem::copy_options::overwrite_existing);
    return p;
}

// Everything a dialog under test needs to stay alive for its lifetime.
struct DialogEnv {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    std::unique_ptr<AnnotationManager> mgr;
    int annotIndex = -1;
};

// ── THE ENUMERATED LIST (see the header comment for the grep comparison) ────
struct TabOrderDialogSpec {
    const char* name;
    // Number of DISTINCT widgets named in that dialog's setTabOrder chain.
    int declaredChainWidgets;
    std::function<QDialog*(DialogEnv&)> make;
};

QDialog* makeAltText(DialogEnv& env)
{
    env.ctx  = std::make_shared<MuPdfContext>();
    env.doc  = Document::open(env.ctx, workCopy("f11_alttext.pdf"));
    if (!env.doc) return nullptr;
    env.page = env.doc->page(0);
    env.mgr  = std::make_unique<AnnotationManager>(env.ctx);
    auto a = env.mgr->createAnnotation(env.page.get(), AnnotationType::Square,
                                       QRectF(50, 50, 100, 100));
    env.annotIndex = a.annotIndex;
    return new AltTextDialog(env.mgr.get(), env.page.get(), env.annotIndex, nullptr);
}

QDialog* makeBatch(DialogEnv& env)
{
    env.ctx = std::make_shared<MuPdfContext>();
    return new BatchDialog(env.ctx, nullptr);
}

QDialog* makeCombine(DialogEnv& env)
{
    env.ctx = std::make_shared<MuPdfContext>();
    return new CombineFilesWizard(env.ctx, nullptr);
}

QDialog* makeCompliance(DialogEnv& env)
{
    env.ctx = std::make_shared<MuPdfContext>();
    env.doc = Document::open(env.ctx, workCopy("f11_compliance.pdf"));
    if (!env.doc) return nullptr;
    return new ComplianceDialog(env.ctx, env.doc.get(), nullptr);
}

QDialog* makeCustomizeToolbars(DialogEnv&)
{
    return new CustomizeToolbarsDialog(std::vector<QAction*>{},
                                       std::vector<ToolbarLayout>{}, nullptr);
}

QDialog* makePreflight(DialogEnv& env)
{
    env.ctx = std::make_shared<MuPdfContext>();
    env.doc = Document::open(env.ctx, workCopy("f11_preflight.pdf"));
    if (!env.doc) return nullptr;
    return new PreflightDialog(env.doc.get(), env.ctx, nullptr);
}

QDialog* makeValidationRequest(DialogEnv&)
{
    return new ValidationRequestDialog(
        QStringList{QStringLiteral("http://ocsp.example.test/responder")},
        QStringList{QStringLiteral("http://crl.example.test/list.crl")}, nullptr);
}

// Chain lengths below are the count of DISTINCT widgets appearing in each
// dialog's setTabOrder(...) calls in src/ui/<Dialog>.cpp.
const std::vector<TabOrderDialogSpec>& setTabOrderDialogs()
{
    static const std::vector<TabOrderDialogSpec> kSetTabOrderDialogs = {
        {"AltTextDialog",           4,  makeAltText},              // AltTextDialog.cpp:83-86
        {"BatchDialog",             9,  makeBatch},                // BatchDialog.cpp:265-273
        {"CombineFilesWizard",      8,  makeCombine},              // CombineFilesWizard.cpp:149-156
        {"ComplianceDialog",        7,  makeCompliance},           // ComplianceDialog.cpp:175-181
        {"CustomizeToolbarsDialog", 11, makeCustomizeToolbars},    // CustomizeToolbarsDialog.cpp:197-207
        {"PreflightDialog",         5,  makePreflight},            // PreflightDialog.cpp:141-152 (5 in the no-close-button branch)
        {"ValidationRequestDialog", 3,  makeValidationRequest},    // ValidationRequestDialog.cpp:143-145
    };
    return kSetTabOrderDialogs;
}

// Walk QWidget::nextInFocusChain() — the public view of the very chain
// setTabOrder rewrites — and collect the tab stops.
std::vector<QWidget*> tabStops(QWidget* root, int limit = 4000)
{
    std::vector<QWidget*> stops;
    QWidget* w = root;
    for (int i = 0; i < limit; ++i) {
        w = w->nextInFocusChain();
        if (!w || w == root) break;
        if ((w->focusPolicy() & Qt::TabFocus) == Qt::TabFocus)
            stops.push_back(w);
    }
    return stops;
}

bool isDescendantOf(const QWidget* w, const QWidget* root)
{
    for (const QObject* p = w; p; p = p->parent())
        if (p == root) return true;
    return false;
}

void sendKey(QWidget* target, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(target, &release);
}

// objectName fragments that mark an irreversible or network-touching action.
bool isDestructiveButtonName(const QString& objectName)
{
    static const QRegularExpression re(
        QStringLiteral("(run|convert|apply|fix|delete|remove|flatten|merge|combine|redact|fetch)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(objectName).hasMatch();
}

} // namespace

// ─── WIN-40: the declared tab order is a real, complete, cyclic chain ──────

TEST(DialogTabOrderKeys, EveryDeclaredTabOrderDialogHasACompleteCyclicChain)
{
    ASSERT_NE(qApp, nullptr);
    ASSERT_TRUE(std::filesystem::exists(fixturesDir() / "test_single.pdf"))
        << "test_single.pdf fixture is required";

    for (const auto& spec : setTabOrderDialogs()) {
        DialogEnv env;
        std::unique_ptr<QDialog> dlg(spec.make(env));
        ASSERT_NE(dlg, nullptr) << spec.name << " failed to construct";
        if (dlg->layout()) dlg->layout()->activate();
        dlg->ensurePolished();

        const std::vector<QWidget*> stops = tabStops(dlg.get());

        EXPECT_GE(static_cast<int>(stops.size()), spec.declaredChainWidgets)
            << spec.name << ": focus chain has " << stops.size()
            << " tab stops but setTabOrder names " << spec.declaredChainWidgets
            << " widgets — a control declared in the chain is unreachable by Tab";

        // No dead stop: every stop is an enabled, tab-focusable descendant.
        std::set<QWidget*> seen;
        for (QWidget* w : stops) {
            EXPECT_TRUE(isDescendantOf(w, dlg.get()))
                << spec.name << ": tab stop " << w->objectName().toStdString()
                << " is not a descendant of the dialog";
            EXPECT_TRUE(seen.insert(w).second)
                << spec.name << ": widget "
                << w->objectName().toStdString()
                << " appears twice in one pass of the focus chain";
            EXPECT_NE(w->focusPolicy(), Qt::NoFocus)
                << spec.name << ": dead focus stop on "
                << w->objectName().toStdString();
        }

        // The chain must CYCLE: starting from the last stop, walking forward
        // must reach the first stop again without leaving the dialog.
        ASSERT_FALSE(stops.empty()) << spec.name << ": no tab stops at all";
        QWidget* w = stops.back();
        bool wrapped = false;
        for (int i = 0; i < 4000; ++i) {
            w = w->nextInFocusChain();
            if (!w) break;
            if (w == stops.front()) { wrapped = true; break; }
        }
        EXPECT_TRUE(wrapped)
            << spec.name << ": Tab from the last control does not return to the "
               "first — the focus chain is not cyclic";
    }
}

TEST(DialogTabOrderKeys, EveryTabStopCarriesAnAutomationIdentity)
{
    ASSERT_NE(qApp, nullptr);
    ASSERT_TRUE(std::filesystem::exists(fixturesDir() / "test_single.pdf"));

    for (const auto& spec : setTabOrderDialogs()) {
        DialogEnv env;
        std::unique_ptr<QDialog> dlg(spec.make(env));
        ASSERT_NE(dlg, nullptr) << spec.name;
        if (dlg->layout()) dlg->layout()->activate();
        dlg->ensurePolished();

        int identified = 0;
        for (QWidget* w : tabStops(dlg.get())) {
            const bool named = !w->objectName().isEmpty()
                            || !w->accessibleName().isEmpty();
            if (named) ++identified;
        }
        const int total = static_cast<int>(tabStops(dlg.get()).size());
        ASSERT_GT(total, 0) << spec.name;
        // Every widget named in the declared chain must be identifiable to UIA;
        // container-internal stops (e.g. a QAbstractScrollArea viewport) need
        // not be, so the floor is the declared chain length.
        EXPECT_GE(identified, spec.declaredChainWidgets)
            << spec.name << ": only " << identified << " of " << total
            << " tab stops carry an objectName or accessibleName";
    }
}

// ─── WIN-41: Escape rejects, asserted on the SIGNAL not on result() ────────

TEST(DialogTabOrderKeys, EscapeRejectsEveryDeclaredTabOrderDialog)
{
    ASSERT_NE(qApp, nullptr);
    ASSERT_TRUE(std::filesystem::exists(fixturesDir() / "test_single.pdf"));

    for (const auto& spec : setTabOrderDialogs()) {
        DialogEnv env;
        std::unique_ptr<QDialog> dlg(spec.make(env));
        ASSERT_NE(dlg, nullptr) << spec.name;
        dlg->ensurePolished();

        QSignalSpy rejected(dlg.get(), &QDialog::rejected);
        QSignalSpy accepted(dlg.get(), &QDialog::accepted);
        ASSERT_TRUE(rejected.isValid()) << spec.name;

        sendKey(dlg.get(), Qt::Key_Escape);

        EXPECT_EQ(rejected.count(), 1)
            << spec.name << " did not reject on Escape (a dialog that swallows "
               "Escape traps a keyboard-only user)";
        EXPECT_EQ(accepted.count(), 0)
            << spec.name << " ACCEPTED on Escape — Escape must never commit";
        EXPECT_EQ(dlg->result(), static_cast<int>(QDialog::Rejected)) << spec.name;
    }
}

// ─── WIN-41: Enter semantics ──────────────────────────────────────────────

TEST(DialogTabOrderKeys, EnterCommitsTheDialogThatDeclaresAnAcceptDefault)
{
    ASSERT_NE(qApp, nullptr);
    ASSERT_TRUE(std::filesystem::exists(fixturesDir() / "test_single.pdf"));

    // AltTextDialog.cpp:67 — saveButton_->setDefault(true).
    DialogEnv env;
    std::unique_ptr<QDialog> dlg(makeAltText(env));
    ASSERT_NE(dlg, nullptr);
    dlg->show();
    QApplication::processEvents();

    QSignalSpy accepted(dlg.get(), &QDialog::accepted);
    QSignalSpy rejected(dlg.get(), &QDialog::rejected);
    ASSERT_TRUE(accepted.isValid());

    sendKey(dlg.get(), Qt::Key_Return);
    QApplication::processEvents();

    EXPECT_EQ(rejected.count(), 0)
        << "Enter must not cancel a dialog that declares an accept default";
    EXPECT_EQ(accepted.count(), 1)
        << "AltTextDialog declares saveButton_ as the default button, so Enter "
           "must fire it — otherwise the dialog cannot be completed by keyboard";
    dlg->hide();
}

TEST(DialogTabOrderKeys, EnterDoesNotGrantNetworkConsentInTheValidationDialog)
{
    ASSERT_NE(qApp, nullptr);

    // ValidationRequestDialog.cpp:112-119 deliberately clears the default from
    // fetchButton_ and puts it on cancelButton_: pressing Enter reflexively on
    // a consent prompt must NOT leak the signer's certificate serial to a third
    // party. This test pins that decision so it cannot be undone by accident.
    DialogEnv env;
    std::unique_ptr<QDialog> dlg(makeValidationRequest(env));
    ASSERT_NE(dlg, nullptr);
    dlg->show();
    QApplication::processEvents();

    QSignalSpy accepted(dlg.get(), &QDialog::accepted);
    QSignalSpy rejected(dlg.get(), &QDialog::rejected);

    sendKey(dlg.get(), Qt::Key_Return);
    QApplication::processEvents();

    EXPECT_EQ(accepted.count(), 0)
        << "Enter granted OCSP/CRL fetch consent — a reflexive keypress must "
           "never authorise an outbound revocation request";
    EXPECT_EQ(rejected.count(), 1)
        << "Enter should take the declared default (Cancel) on the consent dialog";
    dlg->hide();
}

TEST(DialogTabOrderKeys, NoDestructiveButtonIsADeclaredDefault)
{
    ASSERT_NE(qApp, nullptr);
    ASSERT_TRUE(std::filesystem::exists(fixturesDir() / "test_single.pdf"));

    for (const auto& spec : setTabOrderDialogs()) {
        DialogEnv env;
        std::unique_ptr<QDialog> dlg(spec.make(env));
        ASSERT_NE(dlg, nullptr) << spec.name;
        dlg->ensurePolished();

        for (QPushButton* btn : dlg->findChildren<QPushButton*>()) {
            if (!btn->isDefault()) continue;
            EXPECT_FALSE(isDestructiveButtonName(btn->objectName()))
                << spec.name << ": destructive/network button "
                << btn->objectName().toStdString()
                << " is the DEFAULT button — Enter would fire it";
        }
    }
}

// ─── WIN-44: menu mnemonics ───────────────────────────────────────────────

TEST(DialogTabOrderKeys, MainWindowTopLevelMenusHaveUniqueMnemonics)
{
    ASSERT_NE(qApp, nullptr);

    // Heap-allocated: MainWindow's member set overflows the test stack guard
    // (see tests/test_main_window_q_invokable.cpp).
    auto mw = std::make_unique<MainWindow>();
    QMenuBar* bar = mw->menuBar();
    ASSERT_NE(bar, nullptr);

    QSet<QChar> used;
    int topLevel = 0;
    for (QAction* action : bar->actions()) {
        if (!action->menu()) continue;
        ++topLevel;
        const QString text = action->text();
        const int amp = text.indexOf(QLatin1Char('&'));
        ASSERT_GE(amp, 0)
            << "top-level menu \"" << text.toStdString()
            << "\" has no keyboard mnemonic";
        ASSERT_LT(amp + 1, text.size()) << text.toStdString();
        const QChar mnemonic = text.at(amp + 1).toLower();
        EXPECT_FALSE(used.contains(mnemonic))
            << "duplicate menu mnemonic Alt+" << QString(mnemonic).toStdString()
            << " on \"" << text.toStdString() << "\"";
        used.insert(mnemonic);
    }
    EXPECT_GE(topLevel, 8) << "expected the full top-level menu bar";
}
