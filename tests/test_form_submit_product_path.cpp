// -*- coding: utf-8 -*-
//
// GitHub #18 (meta-audit row PDF-48) — pipeline p83-outstanding, phase-2.
//
// WHAT THIS FILE IS FOR, AND WHY test_form_field_flags.cpp IS NOT ENOUGH.
// ----------------------------------------------------------------------
// Commit 972a63d2 added required-field enforcement to FormSubmitter and seven
// GoogleTests over it. Every one of those tests calls FormSubmitter directly
// with a hand-built std::vector<FormField>. They prove the LIBRARY refuses.
//
// The library was linked into nothing. `pdfe_formsubmit` was a static library
// referenced by neither `pdfe_ui` nor the `pdfe` executable, and MainWindow's
// "Submit Form…" action opened a QDialog whose Accept button called
// QDialog::accept() and stopped there. So an incomplete required-field form
// was refused in a unit test and submitted fine in the shipped product — and
// the issue's own closing comment says exactly that. A test that re-proves the
// library therefore cannot close the row; it is the evidence that was already
// there when the row stayed open.
//
// These tests drive the PRODUCT path: a real MainWindow, a real document
// opened through MainWindow::openFile, fields read by the live FormManager,
// through MainWindow::submitCurrentFormTo — the same function the Submit Form
// dialog calls, reached here via the PDFE_TEST_PIPES-gated Q_INVOKABLE
// MainWindow::submitCurrentFormDiag.
//
// NO TEST HERE CAN OPEN A NETWORK CONNECTION. The seam only ever runs the
// UNCONFIRMED pass, which stops at "confirm-host:<host>" and returns before
// FormSubmitter::submitForm is reached. That is a property of the seam's
// signature, not of the URLs these tests happen to use.
//
// FIXTURES ARE BUILT IN-TEST, not committed — the same choice, for the same
// reason, as tests/test_pdf20_features.cpp: the three documents below differ
// from each other by ONE dictionary entry (`/V` absent, `/V ( )`, `/V (Ada)`),
// and that difference is the whole subject. Three near-identical committed
// PDFs would hide it, and would take on the three fixture obligations
// (README row, coverage_inventory.json, .expected.json sidecar) for nothing.

#include <gtest/gtest.h>

#include "app/AppSettings.h"
#include "ui/MainWindow.h"
#include "ui/TestDialogSeam.h"

#include <QApplication>
#include <QByteArray>
#include <QMetaMethod>
#include <QMetaObject>
#include <QString>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::ui;

namespace {

namespace fs = std::filesystem;

fs::path tempDir()
{
    auto d = fs::temp_directory_path() / "pdfe_form_submit_product_path";
    fs::create_directories(d);
    return d;
}

void writeBytes(const fs::path& p, const std::string& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A one-page AcroForm with TWO text fields:
//
//   applicant_name  /Ff 2  -> the AcroForm Required bit (bit position 2).
//                             Its /V is controlled by `requiredValue`:
//                             nullptr emits no /V at all (never filled in).
//   comments               -> optional, always carries a value, so a rule that
//                             refused every form — or that keyed on "any empty
//                             field" rather than on the Required bit — would
//                             still have something to be wrong about.
//
// No xref table: MuPDF reconstructs, as the neighbouring in-test generators
// (test_pdf20_features.cpp, the malicious-PDF corpus) already rely on.
// Computing offsets here would make the generator the thing under test.
std::string requiredFieldFormPdf(const char* requiredValue)
{
    const std::string content =
        "BT /Helv 12 Tf 60 740 Td (Application form) Tj ET\n";

    std::string out = "%PDF-1.7\n";
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /AcroForm "
           "<< /Fields [5 0 R 6 0 R] /DA (/Helv 0 Tf 0 g) "
           "/DR << /Font << /Helv 7 0 R >> >> >> >>\nendobj\n";
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Resources << /Font << /Helv 7 0 R >> >> /Contents 4 0 R "
           "/Annots [5 0 R 6 0 R] >>\nendobj\n";
    out += "4 0 obj\n<< /Length " + std::to_string(content.size()) +
           " >>\nstream\n" + content + "endstream\nendobj\n";

    std::string requiredField =
        "5 0 obj\n<< /Type /Annot /Subtype /Widget /FT /Tx /T (applicant_name) "
        "/Ff 2 /Rect [60 700 300 720] /F 4 /P 3 0 R /DA (/Helv 12 Tf 0 g)";
    if (requiredValue != nullptr) {
        requiredField += " /V (";
        requiredField += requiredValue;
        requiredField += ")";
    }
    requiredField += " >>\nendobj\n";
    out += requiredField;

    out += "6 0 obj\n<< /Type /Annot /Subtype /Widget /FT /Tx /T (comments) "
           "/Rect [60 650 300 670] /F 4 /P 3 0 R /DA (/Helv 12 Tf 0 g) "
           "/V (no comment) >>\nendobj\n";
    out += "7 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
           "/Name /Helv >>\nendobj\n";
    out += "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

int findInvokableIndex(const QObject* obj, const char* name)
{
    const QMetaObject* mo = obj->metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        QMetaMethod m = mo->method(i);
        if (m.methodType() == QMetaMethod::Method
            && QString(m.name()) == QString(name)) {
            return i;
        }
    }
    return -1;
}

// A host that is not on the (empty) allowlist and is NOT a blocked
// destination, so the decision that comes back tells us how far the product
// got: "confirm-host:…" means it passed the required-field rule.
constexpr const char* kExternalUrl = "https://forms.example.com/submit";

class FormSubmitProductPathTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        hadPipes_ = qEnvironmentVariableIsSet("PDFE_TEST_PIPES");
        savedPipes_ = hadPipes_ ? qgetenv("PDFE_TEST_PIPES") : QByteArray();
        qputenv("PDFE_TEST_PIPES", "1");

        hadSettings_ = qEnvironmentVariableIsSet("PDFE_SETTINGS_PATH");
        savedSettings_ =
            hadSettings_ ? qgetenv("PDFE_SETTINGS_PATH") : QByteArray();

        td_ = tempDir();
        iniPath_ = td_ / "settings.ini";
        std::error_code ec;
        fs::remove(iniPath_, ec);
        qputenv("PDFE_SETTINGS_PATH",
                QByteArray(iniPath_.string().c_str()));

        // Open the SEC-49 consent gate for the tests that need to get past it.
        // It defaults to OFF, which is itself asserted below.
        setExternalActions(true);

        mw_ = std::make_unique<MainWindow>();
    }

    void TearDown() override
    {
        mw_.reset();
        QApplication::processEvents();

        if (hadPipes_) { qputenv("PDFE_TEST_PIPES", savedPipes_); }
        else           { qunsetenv("PDFE_TEST_PIPES"); }
        if (hadSettings_) { qputenv("PDFE_SETTINGS_PATH", savedSettings_); }
        else              { qunsetenv("PDFE_SETTINGS_PATH"); }
    }

    // Writes through a test-owned AppSettings on the per-test INI named by
    // PDFE_SETTINGS_PATH, then syncs, so a MainWindow constructed AFTERWARDS
    // reads the value. Never touches the real HKCU hive (F28).
    static void setExternalActions(bool enabled)
    {
        pdfe::app::AppSettings settings;
        settings.setExternalActionsAllowed(enabled);
        settings.sync();
    }

    // Write one of the three variants and open it through the product.
    void openForm(const char* requiredValue, const std::string& name)
    {
        const auto path = td_ / name;
        writeBytes(path, requiredFieldFormPdf(requiredValue));
        mw_->openFile(QString::fromStdString(path.string()));
        QApplication::processEvents();
    }

    fs::path td_;
    fs::path iniPath_;
    bool hadPipes_ = false;
    bool hadSettings_ = false;
    QByteArray savedPipes_;
    QByteArray savedSettings_;
    std::unique_ptr<MainWindow> mw_;
};

}  // namespace

// ---------------------------------------------------------------------------
// THE ROW. The shipped product must refuse.
// ---------------------------------------------------------------------------
TEST_F(FormSubmitProductPathTest, ShippedSubmitRefusesAnIncompleteRequiredForm)
{
    openForm(/*requiredValue=*/nullptr, "required_empty.pdf");

    const QString outcome = mw_->submitCurrentFormDiag(kExternalUrl);

    EXPECT_TRUE(outcome.startsWith(QLatin1String("reject-missing-required:")))
        << "MainWindow's submit path did not refuse a form whose Required "
           "field is empty. Got: " << outcome.toStdString();
    // Naming the field is part of the behaviour, not decoration: a refusal
    // that will not say WHICH field is missing is a dead end on a real form.
    EXPECT_TRUE(outcome.contains(QLatin1String("applicant_name")))
        << "the refusal did not name the offending field. Got: "
        << outcome.toStdString();
    // ... and it must not sweep in the optional field that IS filled.
    EXPECT_FALSE(outcome.contains(QLatin1String("comments")))
        << "the optional field was reported missing. Got: "
        << outcome.toStdString();
}

// CONTROL. Without this, "refuse every submit" passes the test above.
TEST_F(FormSubmitProductPathTest, AFilledRequiredFieldIsNotRefusedByTheProduct)
{
    openForm(/*requiredValue=*/"Ada", "required_filled.pdf");

    const QString outcome = mw_->submitCurrentFormDiag(kExternalUrl);

    EXPECT_FALSE(outcome.startsWith(QLatin1String("reject-missing-required")))
        << "a COMPLETE form was refused as incomplete. Got: "
        << outcome.toStdString();
    // The next thing the product asks about is the destination, and
    // forms.example.com is not on the session allowlist — so this is exactly
    // how far a complete form gets, and it is as far as it gets without a
    // human answering the prompt. No connection is opened.
    EXPECT_EQ(outcome, QString("confirm-host:forms.example.com"))
        << "unexpected decision for a complete form: " << outcome.toStdString();
}

// A space is not an answer. The library test asserts this against a
// hand-built field; this asserts it against a value that came out of a real
// document, through the real parse.
TEST_F(FormSubmitProductPathTest, WhitespaceOnlyDoesNotSatisfyRequiredInProduct)
{
    openForm(/*requiredValue=*/"   ", "required_spaces.pdf");

    const QString outcome = mw_->submitCurrentFormDiag(kExternalUrl);

    EXPECT_TRUE(outcome.startsWith(QLatin1String("reject-missing-required:")))
        << "a Required field containing only spaces was accepted by the "
           "product. Got: " << outcome.toStdString();
    EXPECT_TRUE(outcome.contains(QLatin1String("applicant_name")));
}

// ORDERING, in the product this time. A form aimed at the cloud-metadata
// address must be refused AS A BLOCKED DESTINATION whether or not it is
// complete — otherwise filling the form in changes the reason a hostile URL
// is rejected, and a security refusal is reported to the user as a
// validation nag.
TEST_F(FormSubmitProductPathTest, BlockedDestinationOutranksIncompleteInProduct)
{
    openForm(/*requiredValue=*/nullptr, "required_empty_ssrf.pdf");

    EXPECT_EQ(mw_->submitCurrentFormDiag("http://169.254.169.254/latest/meta"),
              QString("reject-blocked-destination"));

    openForm(/*requiredValue=*/"Ada", "required_filled_ssrf.pdf");
    EXPECT_EQ(mw_->submitCurrentFormDiag("http://169.254.169.254/latest/meta"),
              QString("reject-blocked-destination"))
        << "completing the form changed the reason the SSRF target was "
           "refused";
}

TEST_F(FormSubmitProductPathTest, AnInvalidUrlIsRefusedDistinctlyFromIncomplete)
{
    openForm(/*requiredValue=*/"Ada", "required_filled_badurl.pdf");
    EXPECT_EQ(mw_->submitCurrentFormDiag("file:///etc/hosts"),
              QString("reject-invalid-url"));
}

// ---------------------------------------------------------------------------
// The controls around the wiring itself.
// ---------------------------------------------------------------------------

// SEC-49. The consent gate is checked BEFORE any field is read, so with it
// closed the submit path neither observes nor sends anything. This is the
// precondition tests/test_action_egress_inertness.cpp named as the price of
// ever wiring this module up.
TEST_F(FormSubmitProductPathTest, ExternalActionsGateRefusesBeforeAnythingElse)
{
    setExternalActions(false);
    // A MainWindow constructed AFTER the write reads the closed gate.
    auto gated = std::make_unique<MainWindow>();
    const auto path = td_ / "gated_form.pdf";
    writeBytes(path, requiredFieldFormPdf("Ada"));
    gated->openFile(QString::fromStdString(path.string()));
    QApplication::processEvents();

    EXPECT_EQ(gated->submitCurrentFormDiag(kExternalUrl),
              QString("reject-external-actions-disabled"))
        << "a COMPLETE form was submittable with Security/"
           "externalActionsAllowed off (the shipped default)";

    gated.reset();
    QApplication::processEvents();
    setExternalActions(true);
}

// The seam must not be a control channel. PDFE_TEST_PIPES off => no answer,
// and in a release-configured build testPipesEnabled() is a compile-time
// false so the body is statically dead. An ungated Q_INVOKABLE here would
// hand any local process a way to drive network egress.
TEST_F(FormSubmitProductPathTest, TheSubmitSeamIsGatedOnTestPipes)
{
    openForm(/*requiredValue=*/"Ada", "required_filled_gate.pdf");

    qunsetenv("PDFE_TEST_PIPES");
    EXPECT_EQ(mw_->submitCurrentFormDiag(kExternalUrl),
              QString("seam-disabled"));

    qputenv("PDFE_TEST_PIPES", "1");
    EXPECT_NE(mw_->submitCurrentFormDiag(kExternalUrl),
              QString("seam-disabled"))
        << "the seam stayed off with PDFE_TEST_PIPES set, so the check above "
           "proves nothing";
}

TEST_F(FormSubmitProductPathTest, TheSubmitSeamIsOnTheMetaObject)
{
    EXPECT_GE(findInvokableIndex(mw_.get(), "submitCurrentFormDiag"), 0)
        << "submitCurrentFormDiag is not registered as a Q_INVOKABLE";
}

// The document's fields really did reach the submit path. Without this, a
// submitCurrentFormTo that collected NOTHING would return
// "confirm-host:forms.example.com" for every document and pass the control
// test above for the wrong reason.
TEST_F(FormSubmitProductPathTest, ADocumentWithNoFormAtAllIsNotRefusedAsIncomplete)
{
    const auto path = td_ / "no_form.pdf";
    // Same generator minus the AcroForm: one page, no widgets.
    std::string plain = "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n"
        "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    writeBytes(path, plain);
    mw_->openFile(QString::fromStdString(path.string()));
    QApplication::processEvents();

    EXPECT_EQ(mw_->submitCurrentFormDiag(kExternalUrl),
              QString("confirm-host:forms.example.com"));
}
