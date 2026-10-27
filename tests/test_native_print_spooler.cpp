// -*- coding: utf-8 -*-
//
// Meta-audit follow-up F12 — WIN-26 (Critical): the QPrinter::NativeFormat
// spooler path.
//
// THE GAP
// -------
// Production printing (src/ui/MainWindow.cpp, MainWindow::onPrint) builds its
// QPrinter with
//
//     QPrinter printer(QPrinter::HighResolution);
//     printer.setOutputFormat(QPrinter::NativeFormat);
//
// and hands it to pdfe::core::PrintEngine. Every pre-existing print test
// (tests/test_printing.cpp, tests/test_permission_enforcement.cpp) uses
// QPrinter::PdfFormat instead. Those are two DIFFERENT Qt back-ends:
// PdfFormat drives QPdfEngine (pure Qt, no OS involvement) while NativeFormat
// on Windows drives QWin32PrintEngine — CreateDC to a real device context,
// StartDoc/StartPage against the Windows print spooler, driver DEVMODE, driver
// DPI, driver page geometry. The back-end the user actually invokes had zero
// coverage.
//
// HOW THIS TEST REACHES THE REAL SPOOLER WITHOUT A PAPER PRINTER
// --------------------------------------------------------------
// Windows ships two virtual print devices ("Microsoft XPS Document Writer" and
// "Microsoft Print to PDF"). Both are installed on the standard port
// PORTPROMPT:, which normally raises a Save-As dialog — unusable in an
// automated test.
//
// The escape is a documented Qt/GDI behaviour that keeps us on the NATIVE
// back-end while redirecting the spooled bytes to a file:
//
//   * QPrinter::setOutputFileName() only forces PdfFormat when the file's
//     suffix is ".pdf" (qtbase src/printsupport/kernel/qprinter.cpp:717-729).
//     With any other suffix the output format is left ALONE — so a NativeFormat
//     printer stays NativeFormat.
//   * QWin32PrintEngine turns that property into DOCINFO::lpszOutput
//     (qtbase src/printsupport/platform/windows/qprintengine_win.cpp:105-107),
//     which makes the spooler write the job to that path INSTEAD of routing it
//     through the port monitor. No port prompt, real spooler, real driver.
//
// So the artifact this test asserts on is produced by the Windows spooler after
// a real StartDoc/StartPage/EndDoc sequence issued by QWin32PrintEngine. The
// assertion `printer.outputFormat() == QPrinter::NativeFormat` immediately
// before the print is what makes that claim checkable rather than asserted in a
// comment: if a future change flips the file suffix to .pdf (or drops
// setOutputFormat) the test fails instead of silently degrading into one more
// PdfFormat test — which is exactly the defect WIN-26 records.
//
// PRECONDITION HANDLING
// ---------------------
// If the machine genuinely has no virtual printer the test SKIPS with a message
// that names the precondition explicitly ("PRECONDITION-ABSENT"), so a missing
// facility can never be mistaken for a passing product. It is a real check —
// QPrinterInfo::availablePrinters() is enumerated — not a blanket guard.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PrintEngine.h"

#include <QCoreApplication>
#include <QPrinter>
#include <QPrinterInfo>
#include <QString>
#include <QStringList>
#include <QThread>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace pdfe::core;

namespace {

// Virtual devices that ship with Windows and spool through the real
// GDI/winspool stack. Ordered by preference.
const char* const kVirtualPrinterNames[] = {
    "Microsoft XPS Document Writer",
    "Microsoft Print to PDF",
};

// Every installed device whose name matches one of the known virtual printers,
// in preference order. Empty when the machine has none.
QStringList availableVirtualPrinters()
{
    QStringList found;
    const QList<QPrinterInfo> all = QPrinterInfo::availablePrinters();
    for (const char* want : kVirtualPrinterNames) {
        const QString wanted = QString::fromLatin1(want);
        for (const QPrinterInfo& pi : all) {
            if (pi.printerName().compare(wanted, Qt::CaseInsensitive) == 0) {
                found << pi.printerName();
                break;
            }
        }
    }
    return found;
}

QStringList allPrinterNames()
{
    QStringList names;
    for (const QPrinterInfo& pi : QPrinterInfo::availablePrinters())
        names << pi.printerName();
    return names;
}

// The spooler writes the redirected job asynchronously: the file can appear
// empty for a moment after EndDoc returns. Wait on the real observable
// condition (exists AND non-zero) rather than sleeping a fixed amount.
bool waitForNonEmptyFile(const std::filesystem::path& p, int budgetMs)
{
    for (int waited = 0; waited <= budgetMs; waited += 100) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec) {
            const auto sz = std::filesystem::file_size(p, ec);
            if (!ec && sz > 0)
                return true;
        }
        QCoreApplication::processEvents();
        QThread::msleep(100);
    }
    return false;
}

std::filesystem::path uniqueSpoolTarget(const std::string& tag)
{
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    // NOTE: the suffix is deliberately NOT ".pdf" — see the header comment.
    // A ".pdf" suffix would make QPrinter::setOutputFileName silently switch
    // the engine back to QPdfEngine and this would stop being a native test.
    return std::filesystem::temp_directory_path() /
           ("pdfe_win26_" + tag + "_" + std::to_string(stamp) + ".prn");
}

} // namespace

class NativePrintSpoolerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        context_ = std::make_shared<MuPdfContext>();
        pdf_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(pdf_))
            << "test_3page.pdf is a configure-time required fixture";
        document_ = Document::open(context_, pdf_);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override
    {
        for (const auto& p : spooled_) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
        spooled_.clear();
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path pdf_;
    std::unique_ptr<Document> document_;
    std::vector<std::filesystem::path> spooled_;
};

// ─────────────────────────────────────────────────────────────────────────
// WIN-26 — the shipping back-end, exercised end to end.
//
// RED / mutation: change `printer.setOutputFormat(QPrinter::NativeFormat)`
// below to PdfFormat and the ASSERT_EQ on outputFormat() fires; break
// PrintEngine::print's painter.begin()/newPage() handling and the spooled
// artifact never appears, so the exists + file_size assertions fire.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(NativePrintSpoolerTest, NativeFormatPrintToVirtualPrinterProducesSpooledArtifact)
{
    const QStringList candidates = availableVirtualPrinters();
    if (candidates.isEmpty()) {
        GTEST_SKIP()
            << "PRECONDITION-ABSENT (facility missing, NOT a product failure): "
               "no Windows virtual printer is installed. Looked for "
               "\"Microsoft XPS Document Writer\" and \"Microsoft Print to PDF\"; "
               "QPrinterInfo::availablePrinters() reported ["
            << allPrinterNames().join(", ").toStdString()
            << "]. WIN-26 cannot be closed on this machine — install a virtual "
               "printer and re-run.";
    }

    PrintOptions options;
    options.range = PrintRange::AllPages;
    options.copies = 1;
    options.fitToPage = true;
    options.centerOnPage = true;

    QString usedPrinter;
    std::filesystem::path artifact;
    std::string attempts;

    for (const QString& name : candidates) {
        const std::filesystem::path out = uniqueSpoolTarget(std::to_string(spooled_.size()));
        spooled_.push_back(out);

        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::NativeFormat);
        printer.setPrinterName(name);
        printer.setDocName("PDFE WIN-26 native spooler test");
        // Redirect the spooled bytes to a file (DOCINFO::lpszOutput). Non-.pdf
        // suffix, so the engine stays native — asserted immediately below.
        printer.setOutputFileName(QString::fromStdString(out.string()));

        ASSERT_EQ(printer.outputFormat(), QPrinter::NativeFormat)
            << "the printer under test must still be on the NATIVE (Win32/GDI "
               "spooler) back-end after setPrinterName + setOutputFileName; if "
               "this fires the test has silently become another PdfFormat test, "
               "which is precisely the WIN-26 defect";
        ASSERT_EQ(printer.printerName(), name)
            << "setPrinterName must have bound the real device (Qt silently "
               "ignores unknown names)";

        PrintEngine engine(document_.get());
        const bool printed = engine.print(&printer, options);
        if (!printed) {
            attempts += "[" + name.toStdString() + ": PrintEngine::print returned false] ";
            continue;
        }

        if (waitForNonEmptyFile(out, 90000)) {
            usedPrinter = name;
            artifact = out;
            break;
        }
        attempts += "[" + name.toStdString() +
                    ": print succeeded but the spooler produced no output at " +
                    out.string() + "] ";
    }

    ASSERT_FALSE(usedPrinter.isEmpty())
        << "no available virtual printer produced a spooled artifact via the "
           "NativeFormat path. Attempts: "
        << attempts;

    // The two assertions WIN-26's acceptance criterion names, on the artifact
    // the Windows spooler actually wrote.
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(artifact, ec))
        << "the NativeFormat print must leave a spooled output artifact at "
        << artifact.string() << " (printer: " << usedPrinter.toStdString() << ")";
    const auto size = std::filesystem::file_size(artifact, ec);
    EXPECT_FALSE(static_cast<bool>(ec)) << "could not stat the spooled artifact";
    EXPECT_GT(size, 0u)
        << "the spooled output artifact must be non-zero in size (printer: "
        << usedPrinter.toStdString() << ")";

    // A three-page document rendered at HighResolution is not a handful of
    // bytes; this discriminates a real spooled job from a stub/empty file the
    // driver may create up front.
    EXPECT_GT(size, 1024u)
        << "a 3-page NativeFormat job should spool substantially more than 1 KiB; "
        << size << " bytes suggests the job body was never written";

    RecordProperty("win26_printer", usedPrinter.toStdString());
    RecordProperty("win26_artifact_bytes", static_cast<int>(size));
}

// ─────────────────────────────────────────────────────────────────────────
// WIN-26 support — the production configuration is reachable and valid on this
// machine. This is the assertion that says "the back-end MainWindow::onPrint
// selects is a real, bound, valid Win32 device", separately from whether a job
// spools.
//
// RED / mutation: if Qt ever stopped resolving the default device for
// NativeFormat (or PDFE's QPrinter construction changed), isValid() goes false.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(NativePrintSpoolerTest, ProductionQPrinterConfigurationSelectsAValidNativeDevice)
{
    if (QPrinterInfo::availablePrinters().isEmpty()) {
        GTEST_SKIP()
            << "PRECONDITION-ABSENT (facility missing, NOT a product failure): "
               "the machine has no print devices at all, so there is no native "
               "back-end to bind.";
    }

    // Exactly the two lines MainWindow::onPrint executes.
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::NativeFormat);

    EXPECT_EQ(printer.outputFormat(), QPrinter::NativeFormat)
        << "the production print path must be on the native back-end";
    EXPECT_TRUE(printer.isValid())
        << "the native back-end must bind a valid device; an invalid QPrinter "
           "here means File->Print would fail for every user on this machine";
    EXPECT_FALSE(printer.printerName().isEmpty())
        << "a valid native printer must report a device name";
    EXPECT_GT(printer.resolution(), 0)
        << "the native device must report a driver DPI (PdfFormat would report "
           "Qt's own fixed resolution, which is the substitution WIN-26 is about)";
}

// ═══════════════════════════════════════════════════════════════════════
// WIN-27 — the print dialog → PrintEngine option plumbing
// ═══════════════════════════════════════════════════════════════════════
//
// REGISTER CORRECTION (F12). WIN-27 is recorded as "QPrintDialog /
// QPageSetupDialog (native Windows common dialogs) have no automated
// coverage". PDFE does not use them: `grep -rn 'QPrintDialog|QPageSetupDialog|
// QPrintPreviewDialog' src` returns **0 hits**. MainWindow::onPrint constructs
// pdfe::ui::PrintDialog — an ordinary Qt QDialog — and hands `dialog.options()`
// straight to PrintEngine. There is no #32770 common dialog to drive, so the
// FlaUI approach the register proposes has no target.
//
// The surface that does exist, and that genuinely had no test, is the one the
// register's own remedy sentence names: "assert PrintEngine receives the chosen
// options". These tests drive the real dialog's real widgets (by objectName,
// the same handles a UI automation harness would use) and assert both that
// options() reflects them and that PrintEngine acts on them.
//
// RED / mutation: swap fromPageSpin_ and toPageSpin_ in PrintDialog::options(),
// or drop the `opts.copies = copiesSpin_->value()` line, and these fail. The
// end-to-end variant additionally fails if PrintEngine ignores PageRange.

#include "ui/PrintDialog.h"

#include <QCheckBox>
#include <QDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTemporaryFile>

class PrintDialogOptionsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        context_ = std::make_shared<MuPdfContext>();
        pdf_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(pdf_));
        document_ = Document::open(context_, pdf_);
        ASSERT_NE(document_, nullptr);
        ASSERT_EQ(document_->pageCount(), 3);
    }

    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path pdf_;
    std::unique_ptr<Document> document_;
};

TEST_F(PrintDialogOptionsTest, ChosenPageRangeAndCopiesReachPrintOptions)
{
    pdfe::ui::PrintDialog dlg(document_.get(), /*currentPage=*/1);

    auto* rangeRadio = dlg.findChild<QRadioButton*>("pageRangeRadio");
    auto* fromSpin = dlg.findChild<QSpinBox*>("fromPageSpinBox");
    auto* toSpin = dlg.findChild<QSpinBox*>("toPageSpinBox");
    auto* copiesSpin = dlg.findChild<QSpinBox*>("copiesSpinBox");
    auto* fitCheck = dlg.findChild<QCheckBox*>("fitToPageCheckBox");
    auto* centerCheck = dlg.findChild<QCheckBox*>("centerOnPageCheckBox");
    auto* rotateCheck = dlg.findChild<QCheckBox*>("autoRotateCheckBox");
    ASSERT_NE(rangeRadio, nullptr) << "pageRangeRadio must be addressable";
    ASSERT_NE(fromSpin, nullptr);
    ASSERT_NE(toSpin, nullptr);
    ASSERT_NE(copiesSpin, nullptr);
    ASSERT_NE(fitCheck, nullptr);
    ASSERT_NE(centerCheck, nullptr);
    ASSERT_NE(rotateCheck, nullptr);

    rangeRadio->setChecked(true);
    fromSpin->setValue(2);
    toSpin->setValue(3);
    copiesSpin->setValue(3);
    fitCheck->setChecked(false);
    centerCheck->setChecked(false);
    rotateCheck->setChecked(true);

    const PrintOptions opts = dlg.options();
    EXPECT_EQ(opts.range, PrintRange::PageRange)
        << "selecting the page-range radio must yield PrintRange::PageRange";
    EXPECT_EQ(opts.fromPage, 2) << "the From spin box must reach fromPage";
    EXPECT_EQ(opts.toPage, 3) << "the To spin box must reach toPage";
    EXPECT_EQ(opts.copies, 3) << "the Copies spin box must reach copies";
    EXPECT_FALSE(opts.fitToPage);
    EXPECT_FALSE(opts.centerOnPage);
    EXPECT_TRUE(opts.autoRotate);
}

TEST_F(PrintDialogOptionsTest, CurrentPageChoiceUsesTheCurrentPageNumber)
{
    // currentPage is 0-based on the way in and 1-based in PrintOptions.
    pdfe::ui::PrintDialog dlg(document_.get(), /*currentPage=*/2);
    auto* currentRadio = dlg.findChild<QRadioButton*>("currentPageRadio");
    ASSERT_NE(currentRadio, nullptr);
    currentRadio->setChecked(true);

    const PrintOptions opts = dlg.options();
    EXPECT_EQ(opts.range, PrintRange::CurrentPage);
    EXPECT_EQ(opts.fromPage, 3)
        << "the current-page choice must print the page the user is on (0-based "
           "2 -> 1-based 3), not page 1";
    EXPECT_EQ(opts.toPage, 3);
}

TEST_F(PrintDialogOptionsTest, AllPagesChoiceSpansTheWholeDocument)
{
    pdfe::ui::PrintDialog dlg(document_.get(), /*currentPage=*/1);
    auto* allRadio = dlg.findChild<QRadioButton*>("allPagesRadio");
    ASSERT_NE(allRadio, nullptr);
    allRadio->setChecked(true);

    const PrintOptions opts = dlg.options();
    EXPECT_EQ(opts.range, PrintRange::AllPages);
    EXPECT_EQ(opts.fromPage, 1);
    EXPECT_EQ(opts.toPage, document_->pageCount())
        << "All Pages must span the whole document";
}

// Accept and cancel are the two exits MainWindow::onPrint branches on: it only
// prints when exec() == QDialog::Accepted.
TEST_F(PrintDialogOptionsTest, PrintAcceptsAndCancelRejects)
{
    {
        pdfe::ui::PrintDialog dlg(document_.get(), 0);
        auto* cancel = dlg.findChild<QPushButton*>("cancelButton");
        ASSERT_NE(cancel, nullptr);
        cancel->click();
        EXPECT_EQ(dlg.result(), static_cast<int>(QDialog::Rejected))
            << "Cancel must reject, so MainWindow::onPrint never reaches QPrinter";
    }
    {
        pdfe::ui::PrintDialog dlg(document_.get(), 0);
        auto* print = dlg.findChild<QPushButton*>("printButton");
        ASSERT_NE(print, nullptr);
        print->click();
        EXPECT_EQ(dlg.result(), static_cast<int>(QDialog::Accepted))
            << "Print must accept, so MainWindow::onPrint proceeds";
    }
}

// End-to-end: the range chosen in the dialog is what PrintEngine actually
// emits. This is the assertion the register asks for ("assert PrintEngine
// receives the chosen options") taken all the way to the output.
TEST_F(PrintDialogOptionsTest, ChosenRangeIsHonouredByPrintEngineEndToEnd)
{
    pdfe::ui::PrintDialog dlg(document_.get(), 0);
    auto* rangeRadio = dlg.findChild<QRadioButton*>("pageRangeRadio");
    auto* fromSpin = dlg.findChild<QSpinBox*>("fromPageSpinBox");
    auto* toSpin = dlg.findChild<QSpinBox*>("toPageSpinBox");
    ASSERT_NE(rangeRadio, nullptr);
    ASSERT_NE(fromSpin, nullptr);
    ASSERT_NE(toSpin, nullptr);
    rangeRadio->setChecked(true);
    fromSpin->setValue(2);
    toSpin->setValue(3);

    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_win27_range_output.pdf";
    {
        QPrinter printer;
        printer.setResolution(150);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(QString::fromStdString(out.string()));

        PrintEngine engine(document_.get());
        ASSERT_TRUE(engine.print(&printer, dlg.options()))
            << "printing the dialog's chosen range must succeed";
    }

    ASSERT_TRUE(std::filesystem::exists(out));
    {
        auto verifyCtx = std::make_shared<MuPdfContext>();
        auto printed = Document::open(verifyCtx, out);
        ASSERT_NE(printed, nullptr) << "the printed output must be a readable PDF";
        EXPECT_EQ(printed->pageCount(), 2)
            << "the dialog asked for pages 2-3 of a 3-page document, so exactly "
               "2 pages must be emitted; 3 means the range was ignored";
    }
    std::error_code ec;
    std::filesystem::remove(out, ec);
}
