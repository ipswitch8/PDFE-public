// -*- coding: utf-8 -*-
// GitHub #241 — multi-selecting several PDFs in Explorer must open all of them.
//
// The report described ONE symptom with TWO independent singular layers behind
// it, and a fix to either alone would have left the bug reachable:
//
//   1. src/main.cpp read `positionalArguments().first()` into a single QString,
//      so a batched Explorer invocation (one process, N paths) discarded 2..N.
//   2. SingleInstanceGuard forwarded exactly one path and its receiver emitted
//      only the FIRST terminated line, so the other Explorer behaviour (one
//      process per file, each forwarding to the primary) also collapsed.
//
// A third, unreported instance of the same shape was in dropEvent(): a drop of
// several PDFs opened the first that existed and dropped the rest.
//
// WHAT MUST NOT REGRESS
//
// The single-instance guard exists for R3-02: two editors on the SAME file can
// each save it, and whichever saves last silently clobbers the other's edits.
// "Open N documents" must not become "open N editors on one document", so the
// identity tests below are as load-bearing as the counting ones.
#include <gtest/gtest.h>

#include "app/SingleInstanceGuard.h"
#include "app/WindowManager.h"
#include "core/Document.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QTimer>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPoint>
#include <QUrl>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <string>

using pdfe::app::SingleInstanceGuard;
using pdfe::app::WindowManager;
using pdfe::ui::MainWindow;

namespace {

std::string fixture(const char* name)
{
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/",
                               "../../tests/fixtures/"}) {
        const std::string candidate = std::string(prefix) + name;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

QString qfixture(const char* name)
{
    return QString::fromStdString(fixture(name));
}

void settle()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

/// Pump until `pred` holds or the budget runs out. The guard's forward is
/// event-loop driven on both ends, so a fixed processEvents() count is a race.
///
/// The budget is deliberately generous. At 3000 ms this was ORDER-DEPENDENT: the
/// mutation harness (tests/scripts/run_mutation_check.py) reported
/// TheGuardForwardsEveryPathNotJustTheFirst dying under M3 and M6 — two
/// mutations in code the guard never calls. There is no causal path, so the
/// cause was load: both leave extra MainWindows alive earlier in the run, and
/// their deferred (deleteLater) teardown competes with the socket handling this
/// waits on. A test that fails because an unrelated test was heavier is a defect
/// in the test. A long budget costs nothing when the forward succeeds in
/// milliseconds, which it normally does.
template <typename Pred>
bool pumpUntil(Pred pred, int budgetMs = 20000)
{
    QElapsedTimer timer;
    timer.start();
    while (!pred() && timer.elapsed() < budgetMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return pred();
}

class MultiDocumentOpenTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        a_ = qfixture("test_single.pdf");
        b_ = qfixture("test_form.pdf");
        c_ = qfixture("test_multipage.pdf");
        if (a_.isEmpty() || b_.isEmpty()) {
            GTEST_SKIP() << "fixtures not available";
        }
    }

    void TearDown() override
    {
        // Windows are WA_DeleteOnClose and reaped via deleteLater, so without
        // this the previous case's teardown runs inside the NEXT case's event
        // pumping. That is how the guard test became order-dependent.
        for (int i = 0; i < 5; ++i) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        }
    }

    QString a_, b_, c_;
};

}  // namespace

// ─── Layer 1: the window manager ────────────────────────────────────────────

TEST_F(MultiDocumentOpenTest, EachDistinctFileGetsItsOwnWindow)
{
    WindowManager manager;
    MainWindow* wa = manager.openPath(a_);
    MainWindow* wb = manager.openPath(b_);

    ASSERT_NE(wa, nullptr);
    ASSERT_NE(wb, nullptr);
    EXPECT_NE(wa, wb) << "two different documents must not share one window";
    EXPECT_EQ(manager.windowCount(), 2);
    EXPECT_NE(wa->documentForTesting(), nullptr);
    EXPECT_NE(wb->documentForTesting(), nullptr);
}

// R3-02. This is the property the single-instance guard was built to protect,
// and the one a naive "just loop over the arguments" fix would have broken.
TEST_F(MultiDocumentOpenTest, TheSameFileTwiceRaisesTheExistingWindow)
{
    WindowManager manager;
    MainWindow* first = manager.openPath(a_);
    MainWindow* again = manager.openPath(a_);

    EXPECT_EQ(first, again) << "the same file must never get a second editor — "
                               "both could save it";
    EXPECT_EQ(manager.windowCount(), 1);
}

// "Already open" has to mean the same FILE, not the same string, or the R3-02
// guarantee is one `..` segment away from being bypassed.
TEST_F(MultiDocumentOpenTest, TheSameFileNamedDifferentlyStillRaisesTheExistingWindow)
{
    WindowManager manager;
    MainWindow* first = manager.openPath(a_);

    // Same file reached through a redundant directory hop.
    const QFileInfo info(a_);
    const QString roundabout =
        info.absolutePath() + QStringLiteral("/./") + info.fileName();

    MainWindow* again = manager.openPath(roundabout);
    EXPECT_EQ(first, again) << "a differently-spelled path to the same file "
                               "opened a second editor on it";
    EXPECT_EQ(manager.windowCount(), 1);
}

// Windows paths are case-insensitive, so C:\A.pdf and c:\a.pdf are ONE file.
//
// `resolved.toLower()` in canonicalKey() is the entire case-fold mechanism and
// nothing exercised it — the only "differently spelled path" test used a
// redundant directory hop, which `canonicalFilePath()` collapses without the
// fold ever mattering. A gate review caught that the claim in WindowManager.h
// was wider than its coverage.
TEST_F(MultiDocumentOpenTest, TheSameFileInDifferentCaseRaisesTheExistingWindow)
{
    WindowManager manager;
    MainWindow* first = manager.openPath(a_);
    ASSERT_NE(first, nullptr);

    // Flip the case of the file name. On Windows this names the same file; the
    // manager must not treat it as a second document.
    const QFileInfo info(a_);
    const QString flipped =
        info.absolutePath() + QLatin1Char('/') + info.fileName().toUpper();
    ASSERT_NE(flipped, info.absoluteFilePath())
        << "the fixture name has no case to flip, so this test would assert "
           "nothing";

    MainWindow* again = manager.openPath(flipped);
    EXPECT_EQ(first, again)
        << "a case-differing spelling of the same file opened a second editor "
           "on it — two editors that can each save it";
    EXPECT_EQ(manager.windowCount(), 1);
}

// canonicalKey() is exposed precisely so a test can assert on the same identity
// rule the manager uses rather than reimplementing it. It had no callers, which
// made that a claim rather than a seam.
TEST_F(MultiDocumentOpenTest, CanonicalKeyCollapsesTheSpellingsTheRuleClaims)
{
    const QFileInfo info(a_);
    const QString canonical = WindowManager::canonicalKey(a_);

    EXPECT_FALSE(canonical.isEmpty());

    // Redundant directory hop.
    EXPECT_EQ(WindowManager::canonicalKey(info.absolutePath() +
                                          QStringLiteral("/./") + info.fileName()),
              canonical);

    // Case-differing spelling.
    EXPECT_EQ(WindowManager::canonicalKey(info.absolutePath() +
                                          QLatin1Char('/') +
                                          info.fileName().toUpper()),
              canonical);

    // A parent-then-back-down hop.
    EXPECT_EQ(WindowManager::canonicalKey(info.absolutePath() +
                                          QStringLiteral("/../") +
                                          info.dir().dirName() +
                                          QLatin1Char('/') + info.fileName()),
              canonical);

    // A DIFFERENT file must not collapse onto it, or the rule would be
    // vacuously true and every document would share one window.
    EXPECT_NE(WindowManager::canonicalKey(b_), canonical);

    // An empty path has no identity.
    EXPECT_TRUE(WindowManager::canonicalKey(QString()).isEmpty());
}

// Startup shows an empty window; the first file belongs IN it, not beside it.
TEST_F(MultiDocumentOpenTest, TheFirstFileReusesTheEmptyStartupWindow)
{
    WindowManager manager;
    MainWindow* empty = manager.openEmptyWindow();
    ASSERT_EQ(manager.windowCount(), 1);
    ASSERT_EQ(empty->documentForTesting(), nullptr);

    MainWindow* opened = manager.openPath(a_);
    EXPECT_EQ(opened, empty) << "the empty startup window should absorb the "
                                "first document rather than leaving a blank "
                                "window behind";
    EXPECT_EQ(manager.windowCount(), 1);

    manager.openPath(b_);
    EXPECT_EQ(manager.windowCount(), 2)
        << "the SECOND document needs its own window";
}

// A file that cannot be opened must not claim the path, or a later retry of the
// same path would silently raise an empty window and look like a success.
TEST_F(MultiDocumentOpenTest, AFailedOpenDoesNotClaimThePath)
{
    // openFile() reports a missing file with a modal QMessageBox, and
    // QMessageBox::exec() blocks the event loop — without the auto-dismisser
    // this test does not fail, it HANGS (measured: it sat until the 600 s
    // harness timeout). PDFE_TEST_DIALOG_AUTODISMISS is the project's existing
    // seam for exactly this; it is compiled in only under PDFE_TESTING and
    // additionally gated on PDFE_TEST_PIPES, so it cannot reach a shipped build.
    qputenv("PDFE_TEST_PIPES", "1");
    qputenv("PDFE_TEST_DIALOG_AUTODISMISS", "1");
    struct EnvGuard {
        ~EnvGuard()
        {
            qunsetenv("PDFE_TEST_DIALOG_AUTODISMISS");
            qunsetenv("PDFE_TEST_PIPES");
        }
    } envGuard;

    WindowManager manager;
    const QString missing = QStringLiteral("no_such_file_p94.pdf");
    manager.openPath(missing);
    settle();

    EXPECT_EQ(manager.windowForPath(missing), nullptr)
        << "a path that failed to open must not be registered as open — a later "
           "retry would raise an empty window and look like a success";
}

// THE show()-before-openFile() ORDER, asserted from INSIDE the modal loop.
//
// This is the assertion that was previously recorded as impossible. It is not:
// openFile()'s failure dialog already registers the project's dialog seam
// (registerDialogIntrospection on `openFileErrorDialog`), and that seam answers
// queries synchronously while exec() runs. A gate review pointed out that the
// mechanism existed and had simply never been asked, which was a fair hit — the
// mutation log had recorded the gap as unguardable.
//
// Why it must be asked from inside: with openFile() before show(), the error
// dialog appears in front of a window that has never been shown, and the user
// sees a bare modal with no application behind it. Afterwards show() runs either
// way, so every after-the-fact assertion passes regardless. The defect exists
// only for the duration of the dialog.
//
// The timer fires inside the nested exec() loop — that is the whole trick.
TEST_F(MultiDocumentOpenTest, TheWindowIsOnScreenWhileTheOpenFailureDialogIsUp)
{
    qputenv("PDFE_TEST_PIPES", "1");
    struct EnvGuard {
        ~EnvGuard() { qunsetenv("PDFE_TEST_PIPES"); }
    } envGuard;

    QString answer;
    bool dialogSeen = false;

    // Scheduled BEFORE the blocking call, so it runs while the modal is up.
    QTimer::singleShot(0, [&answer, &dialogSeen]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal) return;
        dialogSeen = true;
        auto* query = modal->findChild<QLineEdit*>(
            QStringLiteral("testDialogKeyboardQueryPipe"));
        auto* result = modal->findChild<QLineEdit*>(
            QStringLiteral("testDialogKeyboardResultPipe"));
        if (query && result) {
            query->setText(QStringLiteral("parentVisible"));
            answer = result->text();
        }
        if (auto* dlg = qobject_cast<QDialog*>(modal)) dlg->accept();
    });

    WindowManager manager;
    manager.openPath(QStringLiteral("no_such_file_p94_modal_order.pdf"));
    settle();

    ASSERT_TRUE(dialogSeen)
        << "no modal dialog was up — this test asserted nothing. openFile() must "
           "still report a missing file with a modal, or this guard is vacuous";
    ASSERT_FALSE(answer.isEmpty())
        << "the dialog seam did not answer; it is registered on "
           "openFileErrorDialog and gated on PDFE_TEST_PIPES";
    EXPECT_EQ(answer, QStringLiteral("1"))
        << "the window was NOT on screen while its own error dialog was up "
           "(seam answered '" << answer.toStdString()
        << "'). WindowManager::openPath must call show() BEFORE openFile(), or "
           "a user whose file cannot be opened sees a bare modal with no "
           "application window behind it.";
}

// A failed open still leaves a visible, reusable window with no document.
//
// This does NOT guard the show()/openFile() ORDER, despite looking like it
// might: by the time it runs the dialog is gone and show() has happened either
// way, so swapping the two lines leaves it green. That ordering is guarded by
// TheWindowIsOnScreenWhileTheOpenFailureDialogIsUp above, which asks the
// question from inside the modal loop where it is still answerable.
//
// Kept because what it DOES pin is worth having and is not covered elsewhere: a
// window exists and is visible after a failed open, and the failure leaves no
// document behind.
TEST_F(MultiDocumentOpenTest, AFailedOpenStillLeavesAVisibleEmptyWindow)
{
    qputenv("PDFE_TEST_PIPES", "1");
    qputenv("PDFE_TEST_DIALOG_AUTODISMISS", "1");
    struct EnvGuard {
        ~EnvGuard()
        {
            qunsetenv("PDFE_TEST_DIALOG_AUTODISMISS");
            qunsetenv("PDFE_TEST_PIPES");
        }
    } envGuard;

    WindowManager manager;
    MainWindow* window = manager.openPath(QStringLiteral("no_such_file_p94_order.pdf"));
    settle();

    ASSERT_NE(window, nullptr) << "a failed open must still leave a window";
    EXPECT_TRUE(window->isVisible())
        << "the window was never shown — a user whose file cannot be opened sees "
           "an error dialog with no application window behind it";
    EXPECT_EQ(window->documentForTesting(), nullptr)
        << "precondition: this path must genuinely fail to open, or the test is "
           "asserting nothing about the failure ordering";
}

TEST_F(MultiDocumentOpenTest, AnEmptyPathRaisesRatherThanOpeningABlankWindow)
{
    WindowManager manager;
    manager.openPath(a_);
    ASSERT_EQ(manager.windowCount(), 1);

    manager.openPath(QString());
    EXPECT_EQ(manager.windowCount(), 1)
        << "a bare activate request must not manufacture an extra window";
}

// ─── Layer 2: the single-instance wire protocol ─────────────────────────────

// The second half of #241. Even with main.cpp fixed, a secondary forwarding a
// multi-selection would have lost paths 2..N here.
TEST_F(MultiDocumentOpenTest, TheGuardForwardsEveryPathNotJustTheFirst)
{
    const QString name = QStringLiteral("pdfe-p94-multi-%1")
                             .arg(QCoreApplication::applicationPid());

    SingleInstanceGuard primary(name);
    ASSERT_TRUE(primary.isPrimary());

    QSignalSpy spy(&primary, &SingleInstanceGuard::fileOpenRequested);

    SingleInstanceGuard secondary(name);
    ASSERT_FALSE(secondary.isPrimary());

    const QStringList paths{a_, b_, a_ + QStringLiteral(".third")};
    ASSERT_TRUE(secondary.sendPathsToPrimary(paths));

    const auto arrived = [&] {
        QStringList got;
        for (int i = 0; i < spy.size(); ++i) got << spy.at(i).at(0).toString();
        return got.join(QStringLiteral(" | "));
    };
    ASSERT_TRUE(pumpUntil([&] { return spy.size() >= paths.size(); }))
        << "only " << spy.size() << " of " << paths.size()
        << " forwarded paths arrived. Got: [" << arrived().toStdString()
        << "]. A missing MIDDLE means the receiver stopped at the first "
           "terminated line (see drain() in SingleInstanceGuard.cpp). A missing "
           "TAIL means the request was torn down before the primary read it — "
           "check that drain() still runs on the peer's `disconnected`, which is "
           "what delivers a tail that arrived in a later chunk.";

    EXPECT_EQ(spy.size(), paths.size());
    for (int i = 0; i < paths.size() && i < spy.size(); ++i) {
        EXPECT_EQ(spy.at(i).at(0).toString(), paths.at(i))
            << "path " << i << " arrived out of order or altered";
    }
}

// N=1 must stay byte-identical on the wire, so an old secondary talking to a
// new primary (and the reverse) behaves exactly as it did.
TEST_F(MultiDocumentOpenTest, TheSinglePathForwardStillWorksUnchanged)
{
    const QString name = QStringLiteral("pdfe-p94-single-%1")
                             .arg(QCoreApplication::applicationPid());

    SingleInstanceGuard primary(name);
    ASSERT_TRUE(primary.isPrimary());
    QSignalSpy spy(&primary, &SingleInstanceGuard::fileOpenRequested);

    SingleInstanceGuard secondary(name);
    ASSERT_FALSE(secondary.isPrimary());
    ASSERT_TRUE(secondary.sendPathToPrimary(a_));

    ASSERT_TRUE(pumpUntil([&] { return spy.size() >= 1; }));
    EXPECT_EQ(spy.size(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), a_);
}

// The bare "raise the window" request is an empty path, and it must survive the
// widening — an empty LIST still means one empty line.
TEST_F(MultiDocumentOpenTest, TheBareActivateRequestStillArrives)
{
    const QString name = QStringLiteral("pdfe-p94-activate-%1")
                             .arg(QCoreApplication::applicationPid());

    SingleInstanceGuard primary(name);
    ASSERT_TRUE(primary.isPrimary());
    QSignalSpy spy(&primary, &SingleInstanceGuard::fileOpenRequested);

    SingleInstanceGuard secondary(name);
    ASSERT_FALSE(secondary.isPrimary());
    ASSERT_TRUE(secondary.sendPathsToPrimary(QStringList{}));

    ASSERT_TRUE(pumpUntil([&] { return spy.size() >= 1; }));
    EXPECT_EQ(spy.size(), 1);
    EXPECT_TRUE(spy.at(0).at(0).toString().isEmpty());
}

// ─── Layer 3: the routing the two layers meet at ────────────────────────────

// What Explorer's batched invocation actually produces, end to end: a list of
// paths in, one window each out, and the duplicate collapsing onto its original.
TEST_F(MultiDocumentOpenTest, ASelectionWithADuplicateOpensOneWindowPerFile)
{
    if (c_.isEmpty()) GTEST_SKIP() << "test_multipage.pdf not available";

    WindowManager manager;
    const QStringList selection{a_, b_, c_, a_};
    for (const QString& path : selection) {
        manager.openPath(path);
    }

    EXPECT_EQ(manager.windowCount(), 3)
        << "four arguments naming three distinct files must open three windows";
    EXPECT_EQ(manager.windowForPath(a_), manager.windowForPath(a_));
    EXPECT_NE(manager.windowForPath(a_), manager.windowForPath(b_));
    EXPECT_NE(manager.windowForPath(b_), manager.windowForPath(c_));
}

// ─── Layer 4: the drop path ─────────────────────────────────────────────────

// The third instance of #241's shape, and the one that was not reported: a drop
// of several PDFs opened the first that existed and discarded the rest.
//
// This sends a REAL QDropEvent, because that is the only way to exercise
// MainWindow::dropEvent. The eleven WindowManager/SingleInstanceGuard tests
// above never construct one, and the existing widget test
// (UiClipboardDragDropTest.MultiUrlDropOpensExactlyTheFirstPdf) cannot see the
// change: it builds a bare MainWindow with no WindowManager listening, so the
// openPathRequested signals for files 2..N go nowhere and it passes exactly as
// it did before. A manager has to be wired up for the behaviour to be
// observable at all.
TEST_F(MultiDocumentOpenTest, AMultiFileDropOpensEveryDroppedPdf)
{
    if (c_.isEmpty()) GTEST_SKIP() << "test_multipage.pdf not available";

    WindowManager manager;
    MainWindow* window = manager.openEmptyWindow();
    ASSERT_NE(window, nullptr);
    ASSERT_EQ(manager.windowCount(), 1);

    ASSERT_TRUE(window->dropRoutingDelegated())
        << "precondition: a managed window must have drop routing delegated, or "
           "this test silently exercises the single-window fallback and proves "
           "nothing about the multi-file path";

    // ABSOLUTE paths. QUrl::fromLocalFile() on a RELATIVE path yields a URL with
    // no scheme, which isLocalFile() rejects, so the drop carries no candidate
    // at all and the test passes or fails for reasons unrelated to its subject.
    // A real drop from Explorer always carries absolute paths anyway.
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QFileInfo(a_).absoluteFilePath()),
                  QUrl::fromLocalFile(QFileInfo(b_).absoluteFilePath()),
                  QUrl::fromLocalFile(QFileInfo(c_).absoluteFilePath())});
    ASSERT_EQ(mime.urls().size(), 3) << "this test is about a MULTI-file drop";
    for (const QUrl& u : mime.urls()) {
        ASSERT_TRUE(u.isLocalFile())
            << "the drop payload must name local files: " << u.toString().toStdString();
    }

    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(window, &enter);
    ASSERT_TRUE(enter.isAccepted()) << "dragEnter must accept a .pdf payload";

    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime,
                    Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
    QApplication::sendEvent(window, &drop);
    EXPECT_TRUE(drop.isAccepted());
    settle();

    EXPECT_EQ(manager.windowCount(), 3)
        << "three PDFs were dropped and " << manager.windowCount()
        << " window(s) resulted — files 2..N are still being discarded";

    // The first still lands in the window that received the drop, which is the
    // long-standing behaviour the existing widget test pins.
    EXPECT_EQ(manager.windowForPath(a_), window)
        << "the first dropped file should open in the window it was dropped on";
    EXPECT_NE(manager.windowForPath(b_), nullptr);
    EXPECT_NE(manager.windowForPath(c_), nullptr);
}

// A drop containing the same file twice must not defeat R3-02 either.
TEST_F(MultiDocumentOpenTest, ADropContainingADuplicateOpensItOnce)
{
    WindowManager manager;
    MainWindow* window = manager.openEmptyWindow();
    ASSERT_NE(window, nullptr);

    QMimeData mime;
    const QString abs = QFileInfo(a_).absoluteFilePath();
    mime.setUrls({QUrl::fromLocalFile(abs), QUrl::fromLocalFile(abs)});

    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(window, &enter);
    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime,
                    Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
    QApplication::sendEvent(window, &drop);
    settle();

    EXPECT_EQ(manager.windowCount(), 1)
        << "the same file dropped twice opened " << manager.windowCount()
        << " windows — two editors on one document is the R3-02 data-loss window";
}

// A window that closes must leave the books, or its path stays "open" forever
// and re-opening the file would raise a destroyed window.
TEST_F(MultiDocumentOpenTest, ClosingAWindowReleasesItsPath)
{
    WindowManager manager;
    MainWindow* window = manager.openPath(a_);
    ASSERT_NE(window, nullptr);
    ASSERT_EQ(manager.windowForPath(a_), window);

    window->close();
    settle();

    EXPECT_EQ(manager.windowCount(), 0)
        << "a closed window must be forgotten";
    EXPECT_EQ(manager.windowForPath(a_), nullptr)
        << "the closed window's path must be released, or reopening the file "
           "would raise a destroyed window";
}
