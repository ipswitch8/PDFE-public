// Unit tests for PDFE CLI support and MainWindow file operations
// Tests command-line parsing and openFile() API without crashing

#include <gtest/gtest.h>
#include "ui/MainWindow.h"
#include "support/SettingsIsolation.h"
#include <optional>
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QTimer>
#include <QString>

using namespace pdfe::ui;

/// Schedule auto-close of any modal dialog that appears within 200ms.
/// Must be called BEFORE the action that triggers the dialog, since
/// QMessageBox::critical() enters a nested event loop where the timer fires.
static void autoCloseModalDialog() {
    QTimer::singleShot(200, []() {
        if (auto* widget = QApplication::activeModalWidget()) {
            if (auto* msgBox = qobject_cast<QMessageBox*>(widget)) {
                msgBox->accept();
            } else {
                widget->close();
            }
        }
    });
}

// Test fixture for CLI tests
class CliTest : public ::testing::Test {
protected:
    void SetUp() override {
        // QApplication already created by test_main.cpp.
        //
        // ISOLATION (meta-audit follow-up F28). CommandLineParser_VersionOption
        // below calls QCoreApplication::setApplicationName("PDFE") and
        // setApplicationVersion("1.0.0") with no restore. The NAME happens to
        // be a no-op (test_main.cpp already sets "PDFE"), but the VERSION is
        // not: test_main.cpp sets "0.3.0", matching src/main.cpp, and after
        // that test every later test in the process saw "1.0.0". Anything
        // asserting on, persisting, or embedding the application version —
        // including PDF /Producer metadata — would silently observe a version
        // this binary is not. A capture-and-restore SettingsScope makes the
        // mutation local to the test regardless of which CliTest test sets it.
        settingsScope_.emplace();
    }

    void TearDown() override {
        settingsScope_.reset();
    }

    std::optional<pdfe_test::SettingsScope> settingsScope_;
};

// Test 1: Verify openFile() with empty path doesn't crash
TEST_F(CliTest, OpenFile_EmptyPath_DoesNotCrash) {
    // Arrange - Create MainWindow instance (not shown)
    MainWindow window;

    // Act & Assert - Opening empty path should not crash or throw
    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile(""))
        << "openFile with empty string should not crash";

    // Additional verification - calling with empty QString
    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile(QString()))
        << "openFile with default QString() should not crash";
}

// Test 2: Verify openFile() with nonexistent path doesn't crash
// Note: openFile shows error dialog internally — we auto-close it
TEST_F(CliTest, OpenFile_NonexistentPath_DoesNotCrash) {
    // Arrange - Create MainWindow instance
    MainWindow window;

    // Act & Assert - Opening nonexistent file should not crash
    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile("/nonexistent/path.pdf"))
        << "openFile with nonexistent path should not crash";

    // Additional test cases for various invalid paths
    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile("C:\\nonexistent\\windows\\path.pdf"))
        << "openFile with nonexistent Windows path should not crash";

    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile("invalid_file_without_path.pdf"))
        << "openFile with invalid relative path should not crash";

    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile("not_a_pdf.txt"))
        << "openFile with non-PDF file should not crash";

    // Edge case: Very long path
    QString longPath = "C:\\";
    for (int i = 0; i < 50; ++i) {
        longPath += "very_long_directory_name\\";
    }
    longPath += "file.pdf";

    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile(longPath))
        << "openFile with very long path should not crash";
}

// Test 3: Verify QCommandLineParser setup with PDFE options
TEST_F(CliTest, CommandLineParser_Setup) {
    // Arrange - Create command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription("PDFE - Professional PDF Editor");

    // Add standard Qt options (help, version)
    QCommandLineOption helpOption = parser.addHelpOption();
    QCommandLineOption versionOption = parser.addVersionOption();

    // Add positional argument for file path (as used in main.cpp)
    parser.addPositionalArgument("file", "PDF file to open", "[file]");

    // Act - Parse empty arguments (like when no file is provided)
    QStringList emptyArgs;
    emptyArgs << "pdfe"; // Program name

    bool parseResult = parser.parse(emptyArgs);

    // Assert - Parsing should succeed with no arguments
    EXPECT_TRUE(parseResult)
        << "Parser should successfully parse empty arguments";

    // Verify no positional arguments
    QStringList positionalArgs = parser.positionalArguments();
    EXPECT_EQ(0, positionalArgs.size())
        << "Should have no positional arguments when none provided";

    // Verify help option exists and is not set
    EXPECT_FALSE(parser.isSet(helpOption))
        << "Help option should not be set when not provided";

    EXPECT_FALSE(parser.isSet(versionOption))
        << "Version option should not be set when not provided";
}

// Additional test: Verify parser with file argument
TEST_F(CliTest, CommandLineParser_WithFileArgument) {
    // Arrange
    QCommandLineParser parser;
    parser.setApplicationDescription("PDFE - Professional PDF Editor");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "PDF file to open", "[file]");

    // Act - Parse with file argument
    QStringList args;
    args << "pdfe" << "test_document.pdf";

    bool parseResult = parser.parse(args);

    // Assert
    EXPECT_TRUE(parseResult)
        << "Parser should successfully parse arguments with file";

    QStringList positionalArgs = parser.positionalArguments();
    // GitHub #273: the count was asserted with EXPECT_EQ and then re-tested in
    // an `if` purely to keep .at(0) in range, because EXPECT_ continues past a
    // failure. ASSERT_ aborts instead, so the value check below always runs.
    ASSERT_EQ(1, positionalArgs.size())
        << "Should have one positional argument";

    EXPECT_EQ("test_document.pdf", positionalArgs.at(0))
        << "Positional argument should be the file path";
}

// Additional test: Verify parser with help option
TEST_F(CliTest, CommandLineParser_HelpOption) {
    // Arrange
    QCommandLineParser parser;
    parser.setApplicationDescription("PDFE - Professional PDF Editor");
    QCommandLineOption helpOption = parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "PDF file to open", "[file]");

    // Act - Parse with help flag
    QStringList args;
    args << "pdfe" << "--help";

    bool parseResult = parser.parse(args);

    // Assert
    EXPECT_TRUE(parseResult)
        << "Parser should successfully parse help option";

    EXPECT_TRUE(parser.isSet(helpOption))
        << "Help option should be set";

    // Note: In real CLI, parser.isSet(helpOption) would trigger helpText() display
    QString helpText = parser.helpText();
    EXPECT_FALSE(helpText.isEmpty())
        << "Help text should be generated";
}

// Additional test: Verify parser with version option
TEST_F(CliTest, CommandLineParser_VersionOption) {
    // Arrange
    QCommandLineParser parser;
    parser.setApplicationDescription("PDFE - Professional PDF Editor");
    parser.addHelpOption();
    QCommandLineOption versionOption = parser.addVersionOption();
    parser.addPositionalArgument("file", "PDF file to open", "[file]");

    // Set application version for version option
    QCoreApplication::setApplicationName("PDFE");
    QCoreApplication::setApplicationVersion("1.0.0");

    // Act - Parse with version flag
    QStringList args;
    args << "pdfe" << "--version";

    bool parseResult = parser.parse(args);

    // Assert
    EXPECT_TRUE(parseResult)
        << "Parser should successfully parse version option";

    EXPECT_TRUE(parser.isSet(versionOption))
        << "Version option should be set";
}

// Additional test: Verify parser with multiple files (if supported)
TEST_F(CliTest, CommandLineParser_MultipleFiles) {
    // Arrange
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "PDF file to open", "[file]");

    // Act - Parse with multiple file arguments
    QStringList args;
    args << "pdfe" << "file1.pdf" << "file2.pdf";

    bool parseResult = parser.parse(args);

    // Assert
    EXPECT_TRUE(parseResult)
        << "Parser should handle multiple files";

    QStringList positionalArgs = parser.positionalArguments();
    EXPECT_EQ(2, positionalArgs.size())
        << "Should capture both file arguments";

    // Note: PDFE likely only opens the first file, but parser should accept multiple
}

// Additional test: Verify openFile with unusual paths doesn't crash
TEST_F(CliTest, OpenFile_EmptyAfterTrim_DoesNotCrash) {
    // Arrange
    MainWindow window;

    // Act & Assert - Whitespace-only path should be handled safely
    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile("   "))
        << "openFile with whitespace path should not crash";
}

// Additional test: Verify openFile with special characters
TEST_F(CliTest, OpenFile_SpecialCharacters_DoesNotCrash) {
    // Arrange
    MainWindow window;

    // Act & Assert - Test various special characters in paths
    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile("file with spaces.pdf"))
        << "openFile with spaces should not crash";

    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile("file-with-dashes.pdf"))
        << "openFile with dashes should not crash";

    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile("file_with_underscores.pdf"))
        << "openFile with underscores should not crash";

    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile("file.with.dots.pdf"))
        << "openFile with multiple dots should not crash";

    // Unicode characters
    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile(QString::fromUtf8("文件.pdf")))
        << "openFile with Unicode characters should not crash";

    autoCloseModalDialog();
    EXPECT_NO_THROW(window.openFile(QString::fromUtf8("Müller_Dokument.pdf")))
        << "openFile with umlauts should not crash";
}

// ===========================================================================
// Meta-audit follow-up F17 — gap A1-35 (CLI / file-open entry point recorded
// HAPPY-PATH-ONLY in docs/audit/rtm-coverage.md).
//
// The register noted this file had "15 negative-keyword hits — improved".
// Read closely, every one of those was `EXPECT_NO_THROW(... DoesNotCrash)`:
// they prove the process survives, and nothing else. An openFile() that
// silently swallowed a bad path, or one that happily opened a directory as a
// document, passes all of them. The CLI/open entry point therefore was still
// happy-path-only in the sense the RTM meant.
//
// The tests below pin the OBSERVABLE OUTCOME instead:
//   * the user is told (a modal error dialog actually appears, and we read
//     its text rather than blind-accepting it), and
//   * nothing is opened (the window title stays at the no-document "PDFE",
//     and the Print action stays disabled).
// The positive control at the end is what makes those two negatives mean
// something: it proves the same observables DO move when a real PDF opens.
// ===========================================================================

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PreflightOpen.h"
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <filesystem>
#include <functional>
#include <memory>

namespace {

/// Records the text of the first dialog SHOWN while it is installed.
///
/// Capturing on the Show event, not by polling for activeModalWidget(), is
/// what makes this reliable. autoCloseModalDialog() above arms a 200 ms
/// single-shot that closes ANY modal dialog, and a test whose action shows
/// no dialog (openFile("") returns immediately) never spins an event loop,
/// so its timers stay PENDING and already OVERDUE. They then fire inside the
/// next test's nested exec() loop and dismiss that test's dialog before a
/// poller can read it — a cross-test race that depends purely on which tests
/// share a shard, and which produced a green standalone run and a red
/// sharded one for the same code. The Show event is delivered synchronously
/// while the dialog opens, before any timer can run, so this cannot lose the
/// race no matter who closes the dialog afterwards.
class ModalDialogRecorder : public QObject {
public:
    bool seen = false;
    QString text;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (!seen && ev->type() == QEvent::Show) {
            if (auto* msgBox = qobject_cast<QMessageBox*>(obj)) {
                text = msgBox->text() + " | " + msgBox->informativeText();
                seen = true;
            } else if (auto* dlg = qobject_cast<QDialog*>(obj)) {
                text = dlg->windowTitle();
                seen = true;
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

/// Run `action`, record the text of the first dialog it shows, and make sure
/// that dialog is closed. Returns the captured text ("" if none appeared).
/// Unlike autoCloseModalDialog() this REPORTS what it saw, so a test can
/// assert the user was actually told the right thing.
QString captureAndCloseModalDialog(std::function<void()> action)
{
    // Let any overdue single-shots left by earlier tests fire now, while no
    // dialog of ours is up, so they cannot dismiss the one we are about to
    // capture. Harmless: with no modal widget they do nothing.
    QApplication::processEvents(QEventLoop::AllEvents, 10);

    ModalDialogRecorder recorder;
    qApp->installEventFilter(&recorder);

    // Still needed to CLOSE the dialog so the blocking exec() returns; the
    // text has already been recorded by then.
    auto* timer = new QTimer();
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, []() {
        if (auto* widget = QApplication::activeModalWidget()) {
            if (auto* msgBox = qobject_cast<QMessageBox*>(widget)) {
                msgBox->accept();
            } else {
                widget->close();
            }
        }
    });
    timer->start();

    action();

    // Drain anything the action queued but did not show synchronously.
    for (int i = 0; i < 20 && !recorder.seen; ++i)
        QApplication::processEvents(QEventLoop::AllEvents, 25);

    timer->stop();
    timer->deleteLater();
    qApp->removeEventFilter(&recorder);
    return recorder.text;
}

const char* kNoDocumentTitle = "PDFE";

} // namespace

TEST_F(CliTest, OpenFile_NonexistentPath_TellsTheUserAndOpensNothing) {
    MainWindow window;
    ASSERT_EQ(window.windowTitle(), QString(kNoDocumentTitle))
        << "precondition: a fresh window has no document";

    const QString bogus =
        QDir::tempPath() + "/pdfe_f17_definitely_missing_9f31c2.pdf";
    ASSERT_FALSE(QFile::exists(bogus));

    const QString dialogText =
        captureAndCloseModalDialog([&] { window.openFile(bogus); });

    EXPECT_FALSE(dialogText.isEmpty())
        << "openFile() on a nonexistent path must TELL the user; it returned "
           "silently. (A1-35: 'does not crash' is not a negative oracle.)";
    EXPECT_EQ(window.windowTitle(), QString(kNoDocumentTitle))
        << "the window title changed, so something was opened for a path that "
           "does not exist";
    EXPECT_FALSE(window.isPrintEnabled())
        << "Print became enabled after a failed open — the UI believes a "
           "document is loaded";
}

TEST_F(CliTest, OpenFile_DirectoryPath_TellsTheUserAndOpensNothing) {
    MainWindow window;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QFileInfo(dir.path()).isDir());

    const QString dialogText =
        captureAndCloseModalDialog([&] { window.openFile(dir.path()); });

    EXPECT_FALSE(dialogText.isEmpty())
        << "openFile() on a DIRECTORY must be refused visibly";
    EXPECT_EQ(window.windowTitle(), QString(kNoDocumentTitle));
    EXPECT_FALSE(window.isPrintEnabled());
}

TEST_F(CliTest, OpenFile_NonPdfContent_TellsTheUserAndOpensNothing) {
    MainWindow window;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString junk = dir.path() + "/not_a_pdf.pdf";
    {
        QFile f(junk);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        // Deliberately not a PDF: no %PDF- header, no xref, no EOF marker.
        f.write("this file is plain text pretending to be a PDF\n");
    }
    ASSERT_TRUE(QFile::exists(junk));

    const QString dialogText =
        captureAndCloseModalDialog([&] { window.openFile(junk); });

    EXPECT_FALSE(dialogText.isEmpty())
        << "openFile() on a file whose bytes are not a PDF must be refused "
           "visibly, not opened as an empty document";
    EXPECT_EQ(window.windowTitle(), QString(kNoDocumentTitle));
    EXPECT_FALSE(window.isPrintEnabled());
}

// Positive control. Without this, all three negatives above are satisfied by
// an openFile() that never opens anything at all.
TEST_F(CliTest, OpenFile_ValidPdf_OpensItAndEnablesPrint) {
    MainWindow window;
    const QString fixture =
        QString::fromStdString((std::filesystem::path(TEST_FIXTURES_DIR) /
                                "test_3page.pdf").string());
    ASSERT_TRUE(QFile::exists(fixture)) << fixture.toStdString();

    window.openFile(fixture);
    QApplication::processEvents(QEventLoop::AllEvents, 100);

    EXPECT_NE(window.windowTitle(), QString(kNoDocumentTitle))
        << "a valid PDF did not change the window title — the negative tests "
           "above would then be vacuous";
    EXPECT_TRUE(window.windowTitle().contains("test_3page"))
        << "title should name the opened file, got: "
        << window.windowTitle().toStdString();
    EXPECT_TRUE(window.isPrintEnabled())
        << "Print must be enabled once a document is open";
}

// ===========================================================================
// USER-REPORTED DEFECT: "the message displayed when refusing to open a
// malformed PDF file references 'pdfe' instead of the PDF filename".
//
// The tests above proved the user is TOLD something. They never read WHAT,
// and what they were told was wrong. openFile() ran no existence or type
// check before the crash-isolation gate, and isDocumentOpenSafe() cannot
// distinguish a hostile document from one that is not there: the preflight
// child calls fz_open_document() on whatever path it is handed, any failure
// exits non-zero, and the parent reads non-zero as UNSAFE. So a missing file,
// a folder or a text file was accused of being "malformed or crafted to
// exhaust resources" — a security accusation for what is normally a typo.
//
// The reported instance is the sharpest form of it: launched with the bare
// argument "pdfe" (a shortcut or "Open with" entry putting the program's own
// name in the argument list), main.cpp forwards it unvalidated and
// QFileInfo("pdfe").fileName() is "pdfe", so the dialog accused the program
// itself of being a hostile document.
//
// These assertions are on the TEXT, and each carries its negative: naming the
// right cause is worthless if the malformed-PDF accusation is still in the
// same dialog. The refusal test at the end is what keeps this honest — it
// proves the gate was not defanged to make the others pass.
// ===========================================================================

namespace {

// The phrases the user must NOT see for an ordinary bad path.
bool accusesDocumentOfBeingHostile(const QString& text) {
    return text.contains("malformed", Qt::CaseInsensitive)
        || text.contains("exhaust resources", Qt::CaseInsensitive)
        || text.contains("opened safely", Qt::CaseInsensitive);
}

} // namespace

TEST_F(CliTest, OpenFile_NonexistentPath_SaysNotFoundNotMalformed) {
    MainWindow window;
    const QString bogus =
        QDir::tempPath() + "/pdfe_missing_document_5c1e97.pdf";
    ASSERT_FALSE(QFile::exists(bogus));

    const QString dialogText =
        captureAndCloseModalDialog([&] { window.openFile(bogus); });

    ASSERT_FALSE(dialogText.isEmpty()) << "the user must be told";
    EXPECT_TRUE(dialogText.contains("could not be found"))
        << "a missing file must be reported as missing, got: "
        << dialogText.toStdString();
    EXPECT_FALSE(accusesDocumentOfBeingHostile(dialogText))
        << "a file that is not there was accused of being malformed or "
           "crafted to exhaust resources: " << dialogText.toStdString();
    EXPECT_EQ(window.windowTitle(), QString(kNoDocumentTitle))
        << "nothing may be opened for a path that does not exist";
    EXPECT_FALSE(window.isPrintEnabled());
}

// The reported case, verbatim: the bare argument "pdfe".
TEST_F(CliTest, OpenFile_BareProgramNameArgument_SaysNotFoundAndNamesIt) {
    MainWindow window;
    ASSERT_FALSE(QFile::exists("pdfe"))
        << "precondition: no file named 'pdfe' in the test working directory";

    const QString dialogText =
        captureAndCloseModalDialog([&] { window.openFile("pdfe"); });

    ASSERT_FALSE(dialogText.isEmpty());
    EXPECT_TRUE(dialogText.contains("pdfe"))
        << "the message should still quote what it was asked to open";
    EXPECT_TRUE(dialogText.contains("could not be found"))
        << "the launch argument 'pdfe' is a missing file, and saying so is "
           "what reveals the launcher problem to the user; got: "
        << dialogText.toStdString();
    EXPECT_FALSE(accusesDocumentOfBeingHostile(dialogText))
        << "this is the reported defect: the program's own name was accused "
           "of being a malformed document. Got: " << dialogText.toStdString();
    EXPECT_EQ(window.windowTitle(), QString(kNoDocumentTitle));
    EXPECT_FALSE(window.isPrintEnabled());
}

TEST_F(CliTest, OpenFile_DirectoryPath_SaysFolderNotMalformed) {
    MainWindow window;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QFileInfo(dir.path()).isDir());

    const QString dialogText =
        captureAndCloseModalDialog([&] { window.openFile(dir.path()); });

    ASSERT_FALSE(dialogText.isEmpty());
    EXPECT_TRUE(dialogText.contains("folder", Qt::CaseInsensitive))
        << "a directory must be reported as a directory, got: "
        << dialogText.toStdString();
    EXPECT_FALSE(accusesDocumentOfBeingHostile(dialogText))
        << "a folder was accused of being a hostile document: "
        << dialogText.toStdString();
    EXPECT_EQ(window.windowTitle(), QString(kNoDocumentTitle));
    EXPECT_FALSE(window.isPrintEnabled());
}

// A readable file whose bytes are not a PDF at all is an ordinary mistake,
// not an attack. It gets its own message.
TEST_F(CliTest, OpenFile_NonPdfContent_SaysNotAPdfNotMalformed) {
    MainWindow window;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString junk = dir.path() + "/notes.pdf";
    {
        QFile f(junk);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("just some plain text, no PDF header anywhere in here\n");
    }
    ASSERT_TRUE(QFile::exists(junk));

    const QString dialogText =
        captureAndCloseModalDialog([&] { window.openFile(junk); });

    ASSERT_FALSE(dialogText.isEmpty());
    EXPECT_TRUE(dialogText.contains("is not a PDF document"))
        << "a file with no PDF header must be reported as not-a-PDF, got: "
        << dialogText.toStdString();
    EXPECT_FALSE(accusesDocumentOfBeingHostile(dialogText))
        << "a plain text file was accused of being crafted to exhaust "
           "resources: " << dialogText.toStdString();
    EXPECT_EQ(window.windowTitle(), QString(kNoDocumentTitle));
    EXPECT_FALSE(window.isPrintEnabled());
}

// THE CONTROL THAT KEEPS THE FOUR ABOVE HONEST. The refusal message exists
// for a real case — a genuine file the crash-isolation sandbox will not pass
// — and none of the new triage may weaken or bypass it.
//
// The refusal is forced deterministically through the documented test seam:
// an unresolvable executable directory means the sandbox cannot be located,
// which isDocumentOpenSafe() treats as fail-CLOSED. The document itself is a
// real, header-bearing, perfectly good PDF, so this exercises exactly the
// path the new existence/type/header triage must NOT short-circuit: the file
// exists, is a regular file, and has a %PDF- header, and it is refused
// anyway — with the security wording intact.
TEST_F(CliTest, OpenFile_RefusedByPreflight_StillSaysRefusedForSafety) {
    const QString fixture =
        QString::fromStdString((std::filesystem::path(TEST_FIXTURES_DIR) /
                                "test_3page.pdf").string());
    ASSERT_TRUE(QFile::exists(fixture)) << fixture.toStdString();

    struct SeamGuard {
        explicit SeamGuard(std::filesystem::path p) {
            pdfe::core::detail::setExecutableDirForTesting(std::move(p));
        }
        ~SeamGuard() {
            pdfe::core::detail::setExecutableDirForTesting(std::nullopt);
        }
    } guard{std::filesystem::path()};

    ASSERT_FALSE(pdfe::core::isDocumentOpenSafe(
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf"))
        << "the seam did not force a refusal; the assertion below would be "
           "testing the happy path instead";

    MainWindow window;
    const QString dialogText =
        captureAndCloseModalDialog([&] { window.openFile(fixture); });

    ASSERT_FALSE(dialogText.isEmpty())
        << "a refused document must still be reported to the user";
    EXPECT_TRUE(dialogText.contains("could not be opened safely"))
        << "the crash-isolation refusal lost its message — the triage added "
           "for missing/typo paths must not have swallowed the real refusal. "
           "Got: " << dialogText.toStdString();
    EXPECT_TRUE(dialogText.contains("test_3page.pdf"))
        << "the refusal must name the document by basename, got: "
        << dialogText.toStdString();
    EXPECT_EQ(window.windowTitle(), QString(kNoDocumentTitle))
        << "a refused document must not be opened";
    EXPECT_FALSE(window.isPrintEnabled());
}

// Fix 2, sibling consistency: dialog bodies name the file by BASENAME. A
// full path in one dialog and a basename in the next is the same confusion
// the reported defect is made of.
TEST_F(CliTest, OpenFile_FailureDialogNamesFileByBasenameNotFullPath) {
    MainWindow window;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString missing = dir.path() + "/a_document_name.pdf";
    ASSERT_FALSE(QFile::exists(missing));

    const QString dialogText =
        captureAndCloseModalDialog([&] { window.openFile(missing); });

    ASSERT_FALSE(dialogText.isEmpty());
    EXPECT_TRUE(dialogText.contains("“a_document_name.pdf”"))
        << "the dialog BODY must quote the basename (house style, see "
           "onSaveAsFile), got: " << dialogText.toStdString();
}

// File > New routes through openFile() on a temp file named
// pdfe_untitled_<epoch_ms>.pdf. That generated name is an implementation
// detail and must never reach the window title — same defect shape as the
// reported one: a name the user never chose presented as their document.
TEST_F(CliTest, NewFile_TitleShowsUntitledNotTheGeneratedTempName) {
    MainWindow window;
    // onNewFile() is a private slot; reach it the way Qt does.
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onNewFile",
                                          Qt::DirectConnection))
        << "onNewFile() slot not invokable — has it been renamed?";
    QApplication::processEvents(QEventLoop::AllEvents, 200);

    const QString title = window.windowTitle();
    ASSERT_NE(title, QString(kNoDocumentTitle))
        << "File > New did not open a document; the assertions below would "
           "be vacuous";
    EXPECT_FALSE(title.contains("pdfe_untitled"))
        << "the generated temp filename leaked into the window title: "
        << title.toStdString();
    EXPECT_TRUE(title.contains("Untitled"))
        << "a new unsaved document should be shown as Untitled, got: "
        << title.toStdString();
}

// ---------------------------------------------------------------------------
// GitHub #83 (WIN-53) -- a REAL >260-character path through the CLI entry point.
//
// tests/test_cli.cpp:78-85 (the evidence this issue cites) passes long-looking
// path STRINGS to openFile and asserts only "does not crash". No test ever
// created a directory that is actually past MAX_PATH, so nothing exercised the
// Win32 boundary the issue is about -- a string can be 300 characters without
// any of the filesystem behaviour that a genuinely deep path triggers.
//
// This creates the directory for real, via the \\?\ prefix (which disables
// MAX_PATH normalisation in the Win32 layer), copies a fixture into it, and
// drives it through the SAME entry point the command line uses.
//
// PathRoundTrips.ExtendedLengthPrefixRoundTrip covers the CORE layer. This is
// deliberately the UI/CLI layer, because that is where argv decoding and the
// QString<->path conversion live, and they are a separate failure surface.
// ---------------------------------------------------------------------------
TEST_F(CliTest, OpenFile_RealLongPathBeyondMaxPath_OpensAndSaves) {
#ifndef _WIN32
    GTEST_SKIP() << "\\\\?\\ extended-length paths are a Windows-only form";
#else
    namespace fs = std::filesystem;

    const fs::path fixture =
        fs::path(TEST_FIXTURES_DIR) / "test_single.pdf";
    ASSERT_TRUE(fs::exists(fixture)) << "missing fixture test_single.pdf";

    std::error_code ec;
    std::wstring w = (fs::temp_directory_path() / "pdfe_cli_longpath").wstring();
    while (w.size() < 300) {
        w += L"\\seg_2345678901234567890123456789";   // 34 chars incl. separator
    }
    const fs::path deepDir(L"\\\\?\\" + w);

    fs::remove_all(deepDir, ec);
    fs::create_directories(deepDir, ec);
    ASSERT_TRUE(fs::exists(deepDir))
        << "could not create a >260-char directory: " << ec.message();

    // The assertion that makes this test about MAX_PATH rather than about long
    // strings: the path really is past the boundary.
    const fs::path target = deepDir / "deep.pdf";
    ASSERT_GT(target.wstring().size(), 260u)
        << "the constructed path is not actually past MAX_PATH, so this test "
           "would prove nothing about the boundary it exists to cover";

    fs::copy_file(fixture, target, fs::copy_options::overwrite_existing, ec);
    ASSERT_TRUE(fs::exists(target))
        << "could not place a fixture at the deep path: " << ec.message();

    // Drive the CLI entry point.
    MainWindow window;
    autoCloseModalDialog();
    EXPECT_NO_THROW(
        window.openFile(QString::fromStdWString(target.wstring())))
        << "openFile threw on a genuine >260-char path";

    // ...and prove the bytes are reachable through that path afterwards, so a
    // silent no-op open cannot pass. The core layer is used for the save half
    // because MainWindow's save is dialog-driven and this is a headless run.
    {
        auto c = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(c, target);
        ASSERT_NE(doc, nullptr) << "the deep path could not be opened at all";
        EXPECT_GT(doc->pageCount(), 0);

        const fs::path out = deepDir / "deep_saved.pdf";
        fs::remove(out, ec);
        ASSERT_NO_THROW(doc->saveAs(out));
        EXPECT_TRUE(fs::exists(out))
            << "saveAs to a >260-char path wrote nothing";

        auto reopened = pdfe::core::Document::open(c, out);
        ASSERT_NE(reopened, nullptr) << "the saved deep-path file will not reopen";
        EXPECT_EQ(reopened->pageCount(), doc->pageCount());
    }

    fs::remove_all(deepDir, ec);
#endif
}
