// -*- coding: utf-8 -*-
//
// WIN-28 / GitHub #78 — the stopped-spooler / no-printers failure path of
// pdfe::core::PrintEngine::print, and the user-facing error it must produce.
//
// THE GAP (audit evidence, docs/audit/meta-audit-2026-07/windows-qa-gaps.md:842)
//   "no test in tests/ references printer enumeration or an empty-printer-list
//    condition."
//
// The register's proposed work says "[manual] on a VM with the print spooler
// stopped". Stopping the Windows Print Spooler is a machine-wide, elevated act,
// so no test run may do it to its host — and it is not necessary. A stopped
// Spooler makes winspool's EnumPrinters return nothing, so
// QPrinterInfo::availablePrinters() comes back EMPTY; that empty list is the
// entire observable PDFE has, and it is identical to the observable on a
// machine with no printers installed at all. src/core/PrintDeviceSeam.h injects
// exactly that observable at the engine's single enumeration call site, so the
// empty-printer-list case is asserted DIRECTLY on every host — including this
// one, which does have printers — rather than being left as a
// precondition-absent skip.
//
// There is deliberately NO GTEST_SKIP in this file. A skip here would report a
// pass while proving nothing, which is precisely the shape of false green
// WIN-28 exists to remove. The seam is compiled out of a packaged build
// (-DPDFE_TESTING=OFF) by the same `if constexpr` convention
// src/ui/TestDialogSeam.h uses, so nothing new ships.
//
// BEFORE THIS WORK: every failure inside PrintEngine::print was a bare `false`
// and MainWindow showed the single string "Failed to print the document." — a
// dead end for the user whose actual problem is a stopped Spooler service.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PrintDeviceSeam.h"
#include "core/PrintEngine.h"

#include <QPrinter>
#include <QPrinterInfo>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

using namespace pdfe::core;

namespace {

// The oracle for "a CLEAN, USER-FACING error".
//
// This is deliberately not "the string is non-empty". A non-empty but generic
// string ("Print failed.", "Error 0x8007007B") is exactly the failure mode
// WIN-28 is about: it is shown to a user who then has no idea what to do. So
// the message must (a) be a complete sentence, (b) carry no machine detail, and
// (c) name both the thing that is wrong and an action the user can take.
void expectCleanUserFacingMessage(const QString& message,
                                  const QStringList& mustMention,
                                  const std::string& context)
{
    ASSERT_FALSE(message.isEmpty())
        << context
        << ": the engine reported a failure but produced NO message, so the UI "
           "has nothing to show the user but a blank warning box";

    EXPECT_GE(message.size(), 40)
        << context << ": \"" << message.toStdString()
        << "\" is too short to be an explanation plus a remedy; a user-facing "
           "print error must say what went wrong AND what to do about it";

    EXPECT_TRUE(message.at(0).isUpper())
        << context << ": a user-facing sentence must start with a capital: \""
        << message.toStdString() << "\"";
    EXPECT_TRUE(message.trimmed().endsWith(QLatin1Char('.')))
        << context << ": a user-facing sentence must be terminated: \""
        << message.toStdString() << "\"";

    // No machine detail leaking into a user-facing string.
    static const char* const kForbidden[] = {
        "0x", "HRESULT", "errno", "GetLastError", "::", "nullptr",
        "PrintError", "QPrinter", "QPainter", "false",
    };
    for (const char* bad : kForbidden) {
        EXPECT_FALSE(message.contains(QLatin1String(bad), Qt::CaseInsensitive))
            << context << ": the message leaks the implementation token \""
            << bad << "\" to the user: \"" << message.toStdString() << "\"";
    }

    // An actionable remedy, not just a diagnosis.
    const bool actionable =
        message.contains(QLatin1String("try again"), Qt::CaseInsensitive)
        || message.contains(QLatin1String("check that"), Qt::CaseInsensitive)
        || message.contains(QLatin1String("choose"), Qt::CaseInsensitive)
        || message.contains(QLatin1String("open a PDF"), Qt::CaseInsensitive)
        || message.contains(QLatin1String("ask the document"),
                            Qt::CaseInsensitive);
    EXPECT_TRUE(actionable)
        << context
        << ": the message diagnoses but never tells the user what to DO: \""
        << message.toStdString() << "\"";

    // And it must name the specific condition, not a catch-all.
    for (const QString& want : mustMention) {
        EXPECT_TRUE(message.contains(want, Qt::CaseInsensitive))
            << context << ": the message must name \"" << want.toStdString()
            << "\" so the user can recognise their own situation; got \""
            << message.toStdString() << "\"";
    }

    EXPECT_NE(message, QStringLiteral("Failed to print the document."))
        << context
        << ": this is the old catch-all string WIN-28 exists to replace — it "
           "names no cause and offers no remedy";
}

// What the spooler-related messages must both mention.
QStringList spoolerTerms()
{
    return QStringList{QStringLiteral("printer"), QStringLiteral("Spooler")};
}

} // namespace

class PrintSpoolerUnavailableTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        context_ = std::make_shared<MuPdfContext>();
        pdf_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(pdf_))
            << "test_3page.pdf is a configure-time required fixture";
        document_ = Document::open(context_, pdf_);
        ASSERT_NE(document_, nullptr);
        // Start from the real world in every test, whatever a previous one did.
        testseam::clearPrinterCountOverride();
    }

    void TearDown() override
    {
        testseam::clearPrinterCountOverride();
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path pdf_;
    std::unique_ptr<Document> document_;
};

// ─────────────────────────────────────────────────────────────────────────
// The empty-printer-list case, asserted DIRECTLY (not skipped).
//
// RED / mutation: delete the no-printer guard in PrintEngine::print (or make it
// return true) and both the EXPECT_FALSE and the lastError() assertion fire;
// blank or genericise the message and expectCleanUserFacingMessage fires.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(PrintSpoolerUnavailableTest, EmptyPrinterListProducesCleanUserFacingError)
{
    // Inject the stopped-spooler observable: EnumPrinters returns nothing.
    testseam::ScopedPrinterCount none(0);
    ASSERT_EQ(testseam::availablePrinterCount(), 0)
        << "the seam must actually report an empty printer list; if it does not "
           "this test is exercising the ordinary success path and proves nothing";

    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::NativeFormat);
    ASSERT_EQ(printer.outputFormat(), QPrinter::NativeFormat)
        << "WIN-28 is about the NATIVE (Win32 spooler) back-end that "
           "MainWindow::onPrint uses";

    PrintEngine engine(document_.get());
    const bool printed = engine.print(&printer, PrintOptions{});

    EXPECT_FALSE(printed)
        << "printing with zero available print devices must FAIL; returning "
           "true reports success for a job that can never reach paper";
    EXPECT_EQ(engine.lastError(), PrintError::NoPrinterAvailable)
        << "the failure must be classified as 'no printer available', not left "
           "unclassified where the UI cannot explain it";

    expectCleanUserFacingMessage(engine.lastErrorMessage(), spoolerTerms(),
                                 "empty printer list");
}

// The engine must fail cleanly for every range/copies shape a user could have
// chosen at the moment the spooler went down — not just the default.
TEST_F(PrintSpoolerUnavailableTest, EmptyPrinterListFailsCleanlyForEveryRangeChoice)
{
    testseam::ScopedPrinterCount none(0);

    std::vector<std::pair<std::string, PrintOptions>> cases;
    {
        PrintOptions all;
        all.range = PrintRange::AllPages;
        cases.emplace_back("AllPages", all);

        PrintOptions cur;
        cur.range = PrintRange::CurrentPage;
        cur.fromPage = 2;
        cases.emplace_back("CurrentPage", cur);

        PrintOptions rng;
        rng.range = PrintRange::PageRange;
        rng.fromPage = 1;
        rng.toPage = 3;
        rng.copies = 2;
        cases.emplace_back("PageRange 1-3 x2", rng);
    }

    for (const auto& entry : cases) {
        const std::string& label = entry.first;
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::NativeFormat);

        PrintEngine engine(document_.get());
        EXPECT_FALSE(engine.print(&printer, entry.second))
            << label << ": must fail when no print device exists";
        EXPECT_EQ(engine.lastError(), PrintError::NoPrinterAvailable)
            << label << ": must be classified as 'no printer available'";
        expectCleanUserFacingMessage(engine.lastErrorMessage(), spoolerTerms(),
                                     label);
    }
}

// The guard must be SCOPED. Print-to-PDF goes through QPdfEngine and needs no
// spooler and no installed device, so an empty printer list must not break it —
// otherwise the WIN-28 fix would remove the one export route still working on a
// machine whose spooler is down.
//
// RED / mutation: drop the `outputFormat() == NativeFormat` conjunct from the
// guard and this fails with 'no printer available'.
TEST_F(PrintSpoolerUnavailableTest, EmptyPrinterListDoesNotBlockPdfFormatOutput)
{
    testseam::ScopedPrinterCount none(0);

    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_win28_pdfformat.pdf";
    std::error_code ec;
    std::filesystem::remove(out, ec);
    {
        QPrinter printer;
        printer.setResolution(150);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(QString::fromStdString(out.string()));

        PrintEngine engine(document_.get());
        EXPECT_TRUE(engine.print(&printer, PrintOptions{}))
            << "an empty printer list must not stop PdfFormat output, which "
               "never touches the spooler; the engine said: "
            << engine.lastErrorMessage().toStdString();
        EXPECT_EQ(engine.lastError(), PrintError::None);
        EXPECT_TRUE(engine.lastErrorMessage().isEmpty())
            << "a successful print must leave no stale message for the UI to "
               "display";
    }
    EXPECT_TRUE(std::filesystem::exists(out, ec))
        << "the PdfFormat job must still have produced its file";
    std::filesystem::remove(out, ec);
}

// The spooler-stopped-mid-flight shape: the device was enumerable but will not
// open. Injected by pointing the printer at an output path that cannot be
// created, which is what makes QPainter::begin() fail — the same branch a
// driver/StartDoc refusal takes.
//
// RED / mutation: return a bare `false` from the !painter.begin() branch and
// the message assertions fire.
TEST_F(PrintSpoolerUnavailableTest, DeviceThatWillNotOpenProducesCleanUserFacingError)
{
    // A printer list IS present — so the empty-list guard above must not be
    // what fails this, and the classification assertion below proves it.
    testseam::ScopedPrinterCount some(2);

    const std::filesystem::path bad = std::filesystem::temp_directory_path()
                                      / "pdfe_win28_no_such_dir" / "nested"
                                      / "out.pdf";
    std::error_code ec;
    std::filesystem::remove_all(bad.parent_path().parent_path(), ec);
    ASSERT_FALSE(std::filesystem::exists(bad.parent_path(), ec))
        << "the target directory must genuinely not exist for the device open "
           "to fail; otherwise this test exercises the success path";

    QPrinter printer;
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(QString::fromStdString(bad.string()));

    PrintEngine engine(document_.get());
    EXPECT_FALSE(engine.print(&printer, PrintOptions{}))
        << "a device that cannot be opened must not report a successful print";
    EXPECT_EQ(engine.lastError(), PrintError::DeviceOpenFailed)
        << "a refused job must be classified as a device-open failure, not as "
           "'no printer available' — the printer list was not empty";
    expectCleanUserFacingMessage(engine.lastErrorMessage(), spoolerTerms(),
                                 "device will not open");
}

// A successful print must leave the error surface clean, so the UI can trust
// lastError()/lastErrorMessage() as the sole failure signal.
TEST_F(PrintSpoolerUnavailableTest, SuccessfulPrintClearsAnyPriorError)
{
    PrintEngine engine(document_.get());

    {
        testseam::ScopedPrinterCount none(0);
        QPrinter native(QPrinter::HighResolution);
        native.setOutputFormat(QPrinter::NativeFormat);
        ASSERT_FALSE(engine.print(&native, PrintOptions{}));
        ASSERT_EQ(engine.lastError(), PrintError::NoPrinterAvailable);
        ASSERT_FALSE(engine.lastErrorMessage().isEmpty());
    }

    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_win28_recovery.pdf";
    std::error_code ec;
    std::filesystem::remove(out, ec);
    {
        QPrinter printer;
        printer.setResolution(150);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(QString::fromStdString(out.string()));
        EXPECT_TRUE(engine.print(&printer, PrintOptions{}));
    }
    EXPECT_EQ(engine.lastError(), PrintError::None)
        << "the previous failure must not persist across a successful print";
    EXPECT_TRUE(engine.lastErrorMessage().isEmpty())
        << "a stale message would make the UI warn about a job that succeeded";
    std::filesystem::remove(out, ec);
}

// The seam itself must be honest: with no override installed it must report the
// machine's REAL enumeration, so the guard is live in production and this file
// is not testing a permanently-faked world.
TEST_F(PrintSpoolerUnavailableTest, SeamFallsBackToTheRealPrinterEnumeration)
{
    const int real = static_cast<int>(QPrinterInfo::availablePrinters().size());

    EXPECT_EQ(testseam::availablePrinterCount(), real)
        << "with no override the engine's enumeration call site must be the "
           "real QPrinterInfo list; if it is not, the guard could never fire "
           "for a real user with a stopped spooler";

    {
        testseam::ScopedPrinterCount none(0);
        EXPECT_EQ(testseam::availablePrinterCount(), 0);
    }
    EXPECT_EQ(testseam::availablePrinterCount(), real)
        << "the scoped override must restore the real enumeration on scope "
           "exit, or it would leak into every later test in this binary";
}
