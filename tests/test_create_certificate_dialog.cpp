// -*- coding: utf-8 -*-
//
// The "Create certificate..." flow in the Sign dialog, driven through the REAL
// entry form (user report, 2026-11-26: "option to create a certificate (PFX)
// file does not actually create it (also no dialog for certificate data to be
// entered)" and "errors regarding 'CN missing' but no way to add that").
// DCA 0129.
//
// WHAT WAS WRONG
// --------------
// SignDialog::onCreateCert() showed a subject/passphrase form whose OK button
// accepted ANY input, then a Save-As dialog, and only THEN checked the Common
// Name, the passphrase and the path. An empty CN therefore produced: an
// accepted form, a Save-As prompt, no file, and "Common Name (CN) is
// required." written to a label at the very bottom of the Sign dialog's
// scroll area -- out of sight -- with the form already gone, so there was no
// way to supply the CN short of starting over. The existing
// CreateSelfSignedCert.SignDialogSeam* tests injected the values and skipped
// both modals, so the form, the order of validation and Save-As, and the
// visibility of the error were never exercised.
//
// HOW THESE TESTS DRIVE THE FORM
// ------------------------------
// Never through UIA, and never by injecting values past the form. With
// PDFE_TEST_PIPES set, the dialog's registerDialogIntrospection() call
// replays a queued action script against its real widgets on its own event
// loop (testseam::enqueueDialogActions), and a Browse button takes its path
// from testseam::enqueueFileDialogPath. A Qt-level probe (a timer reading the
// open modal's widgets, not UIA) records what the user would have seen while
// the form was open, and closes the form if it is still open after a deadline
// so a regression fails instead of hanging.

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "signatures/Pkcs7OpenSslBridge.h"
#include "ui/CreateCertificateDialog.h"
#include "ui/SignDialog.h"
#include "ui/TestDialogSeam.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QKeyEvent>
#include <QTimer>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <memory>
#include <string>

using pdfe::signatures::Pkcs7OpenSslBridge;
namespace testseam = pdfe::ui::testseam;

namespace {

constexpr const char* kPass = "test-pass-123";
const QString kForm = QStringLiteral("createCertDialog");

// What the user could see while the create-certificate form was open.
struct FormObservation {
    bool opened = false;          // the form was shown at all
    QString errorShownInForm;     // last non-empty, VISIBLE error text inside the form
    QString focusWhenError;       // objectName of the focused widget while that error showed
    bool forcedClosed = false;    // still open at the deadline; the probe closed it
};

QDialog* openForm()
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->objectName() == kForm && w->isVisible()) return qobject_cast<QDialog*>(w);
    }
    return nullptr;
}

// Runs the Sign dialog's Create-certificate button with @p script queued for
// the form, and observes the form while it is open.
FormObservation runCreateCert(pdfe::ui::SignDialog& sign, const QStringList& script)
{
    FormObservation obs;
    testseam::enqueueDialogActions(kForm, script);

    QElapsedTimer sinceOpen;
    QTimer probe;
    probe.setInterval(15);
    QObject::connect(&probe, &QTimer::timeout, [&]() {
        QDialog* form = openForm();
        if (!form) return;
        if (!obs.opened) {
            obs.opened = true;
            sinceOpen.start();
        }
        if (auto* err = form->findChild<QLabel*>(QStringLiteral("certErrorLabel"))) {
            if (err->isVisible() && !err->text().trimmed().isEmpty()) {
                obs.errorShownInForm = err->text();
                if (QWidget* f = form->focusWidget()) obs.focusWhenError = f->objectName();
            }
        }
        if (sinceOpen.isValid() && sinceOpen.elapsed() > 5000) {
            obs.forcedClosed = true;
            form->reject();
        }
    });
    probe.start();

    auto* create = sign.findChild<QPushButton*>(QStringLiteral("createCertButton"));
    EXPECT_NE(create, nullptr) << "the Create-certificate button is missing";
    if (create) create->click();  // runs the modal form; returns when it closes
    probe.stop();
    return obs;
}

QString fieldText(pdfe::ui::SignDialog& sign, const char* name)
{
    auto* e = sign.findChild<QLineEdit*>(QLatin1String(name));
    return e ? e->text() : QStringLiteral("<missing %1>").arg(QLatin1String(name));
}

// The CN of the PFX at @p path when opened with @p pass, or "" if it does not load.
std::string loadedCommonName(const std::filesystem::path& path, const char* pass)
{
    std::string cn;
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    Pkcs7OpenSslBridge bridge;
    try {
        ctx->execute([&](fz_context* c) {
            pdf_pkcs7_signer* signer = bridge.readPfx(c, path.string().c_str(), pass);
            if (!signer) return;
            if (signer->get_signing_name) {
                pdf_pkcs7_distinguished_name* dn = signer->get_signing_name(c, signer);
                if (dn && dn->cn) cn = dn->cn;
                pdf_signature_drop_distinguished_name(c, dn);
            }
            Pkcs7OpenSslBridge::dropSigner(c, signer);
        });
    } catch (const std::exception&) {
        cn.clear();
    }
    return cn;
}

class CreateCertificateForm : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!qApp) GTEST_SKIP() << "QApplication not available";
        ASSERT_TRUE(tmp_.isValid());
        hadPipes_ = qEnvironmentVariableIsSet("PDFE_TEST_PIPES");
        qputenv("PDFE_TEST_PIPES", "1");
        testseam::dialogActionQueue().clear();
        testseam::fileDialogPathQueue().clear();
    }
    void TearDown() override
    {
        testseam::dialogActionQueue().clear();
        testseam::fileDialogPathQueue().clear();
        if (!hadPipes_) qunsetenv("PDFE_TEST_PIPES");
    }

    std::filesystem::path out(const char* name) const
    {
        return std::filesystem::path(tmp_.path().toStdString()) / name;
    }

    QTemporaryDir tmp_;
    bool hadPipes_ = false;
};

}  // namespace

TEST_F(CreateCertificateForm, EmptyCommonNameKeepsTheFormOpenWithTheErrorInIt)
{
    pdfe::ui::SignDialog sign(nullptr);
    const auto target = out("empty-cn.pfx");

    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=   "),  // whitespace only
        QStringLiteral("edit:certPassphraseEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certPassphraseConfirmEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certOutputPathEdit=%1").arg(QString::fromStdString(target.string())),
        QStringLiteral("click:certOkButton"),
        QStringLiteral("reject"),  // only reached if the form is still open
    });

    ASSERT_TRUE(obs.opened) << "the create-certificate form never appeared";
    EXPECT_FALSE(obs.forcedClosed) << "the form did not respond to the script";
    EXPECT_TRUE(obs.errorShownInForm.contains(QStringLiteral("Common Name")))
        << "no visible Common Name error inside the form; saw: '"
        << obs.errorShownInForm.toStdString() << "'";
    EXPECT_EQ(obs.focusWhenError, QStringLiteral("certCnEdit"))
        << "focus was not put on the field to fix";
    EXPECT_FALSE(std::filesystem::exists(target)) << "a certificate was written without a CN";
    EXPECT_TRUE(fieldText(sign, "pfxPathEdit").isEmpty())
        << "the Sign dialog was filled from a rejected form";
    EXPECT_TRUE(testseam::fileDialogPathQueue().isEmpty());
}

TEST_F(CreateCertificateForm, MismatchedPassphraseKeepsTheFormOpenWithTheErrorInIt)
{
    pdfe::ui::SignDialog sign(nullptr);
    const auto target = out("mismatch.pfx");

    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=Jane Doe"),
        QStringLiteral("edit:certPassphraseEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certPassphraseConfirmEdit=something-else"),
        QStringLiteral("edit:certOutputPathEdit=%1").arg(QString::fromStdString(target.string())),
        QStringLiteral("click:certOkButton"),
        QStringLiteral("reject"),
    });

    ASSERT_TRUE(obs.opened);
    EXPECT_FALSE(obs.forcedClosed);
    EXPECT_TRUE(obs.errorShownInForm.contains(QStringLiteral("passphrase"), Qt::CaseInsensitive))
        << "no visible passphrase error inside the form; saw: '"
        << obs.errorShownInForm.toStdString() << "'";
    EXPECT_FALSE(obs.errorShownInForm.contains(QLatin1String(kPass)))
        << "the error text contains the passphrase";
    EXPECT_EQ(obs.focusWhenError, QStringLiteral("certPassphraseConfirmEdit"))
        << "focus was not put on the field to fix";
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_TRUE(fieldText(sign, "pfxPathEdit").isEmpty());
}

TEST_F(CreateCertificateForm, ValidDataWritesALoadablePfxAndFillsTheSignDialog)
{
    pdfe::ui::SignDialog sign(nullptr);
    // The Browse dialog answers with a name WITHOUT the suffix; the form must
    // add .pfx, or the file is hidden by the "PFX Files (*.pfx)" filter the
    // next time the user browses for it.
    const auto chosen = out("mycert");
    const auto expected = out("mycert.pfx");
    testseam::enqueueFileDialogPath(QString::fromStdString(chosen.string()));

    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=Jane Doe"),
        QStringLiteral("edit:certOrgEdit=Example Org"),
        QStringLiteral("edit:certPassphraseEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certPassphraseConfirmEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("click:certBrowseButton"),
        QStringLiteral("click:certOkButton"),
    });

    ASSERT_TRUE(obs.opened);
    EXPECT_FALSE(obs.forcedClosed) << "valid data did not close the form";
    EXPECT_TRUE(obs.errorShownInForm.isEmpty())
        << "valid data showed an error: '" << obs.errorShownInForm.toStdString() << "'";
    EXPECT_FALSE(std::filesystem::exists(chosen)) << "the file was written without the .pfx suffix";
    ASSERT_TRUE(std::filesystem::exists(expected)) << "no certificate at " << expected.string();
    EXPECT_EQ(loadedCommonName(expected, kPass), "Jane Doe")
        << "the written PFX does not load with its passphrase, or has the wrong CN";

    EXPECT_EQ(fieldText(sign, "pfxPathEdit"), QString::fromStdString(expected.string()));
    EXPECT_EQ(fieldText(sign, "pfxPasswordEdit"), QLatin1String(kPass));

    // The outcome is shown next to the button that started it, not only in
    // the validation label at the bottom of the scroll area.
    auto* status = sign.findChild<QLabel*>(QStringLiteral("createCertStatusLabel"));
    ASSERT_NE(status, nullptr) << "no status label beside the Create-certificate button";
    EXPECT_TRUE(status->text().contains(QStringLiteral("mycert.pfx")))
        << "status: '" << status->text().toStdString() << "'";
}

TEST_F(CreateCertificateForm, CancelWritesNothingAndLeavesTheSignDialogEmpty)
{
    pdfe::ui::SignDialog sign(nullptr);
    const auto target = out("cancelled.pfx");

    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=Jane Doe"),
        QStringLiteral("edit:certPassphraseEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certPassphraseConfirmEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certOutputPathEdit=%1").arg(QString::fromStdString(target.string())),
        QStringLiteral("reject"),
    });

    ASSERT_TRUE(obs.opened);
    EXPECT_FALSE(obs.forcedClosed);
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_TRUE(fieldText(sign, "pfxPathEdit").isEmpty());
}

TEST_F(CreateCertificateForm, AnExistingFileAtATypedPathIsNotOverwritten)
{
    // A typed path has had no "replace this file?" prompt, so an existing file
    // there is refused rather than silently replaced. (A path chosen through
    // Browse has: the save dialog asks before returning it.)
    pdfe::ui::SignDialog sign(nullptr);
    const auto target = out("existing.pfx");
    {
        std::FILE* f = std::fopen(target.string().c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fputs("not a certificate", f);
        std::fclose(f);
    }
    const auto before = std::filesystem::file_size(target);

    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=Jane Doe"),
        QStringLiteral("edit:certPassphraseEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certPassphraseConfirmEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certOutputPathEdit=%1").arg(QString::fromStdString(target.string())),
        QStringLiteral("click:certOkButton"),
        QStringLiteral("reject"),
    });

    ASSERT_TRUE(obs.opened);
    EXPECT_FALSE(obs.forcedClosed);
    EXPECT_TRUE(obs.errorShownInForm.contains(QStringLiteral("already exists")))
        << "saw: '" << obs.errorShownInForm.toStdString() << "'";
    EXPECT_EQ(obs.focusWhenError, QStringLiteral("certOutputPathEdit"));
    EXPECT_EQ(std::filesystem::file_size(target), before) << "the existing file was overwritten";
    EXPECT_TRUE(fieldText(sign, "pfxPathEdit").isEmpty());
}

namespace {

void writeJunk(const std::filesystem::path& p)
{
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("not a certificate", f);
    std::fclose(f);
}

}  // namespace

TEST(CreateCertificateSuffix, KeepsCertificateSuffixesAndAddsPfxOtherwise)
{
    using pdfe::ui::CreateCertificateDialog;
    EXPECT_EQ(CreateCertificateDialog::withPfxSuffix("a"), "a.pfx");
    EXPECT_EQ(CreateCertificateDialog::withPfxSuffix("dir/name.txt"), "dir/name.txt.pfx");
    EXPECT_EQ(CreateCertificateDialog::withPfxSuffix("a.pfx"), "a.pfx");
    EXPECT_EQ(CreateCertificateDialog::withPfxSuffix("a.PFX"), "a.PFX");
    EXPECT_EQ(CreateCertificateDialog::withPfxSuffix("a.p12"), "a.p12");
    EXPECT_EQ(CreateCertificateDialog::withPfxSuffix("A.P12"), "A.P12");
    EXPECT_EQ(CreateCertificateDialog::withPfxSuffix("  b  "), "b.pfx");
    EXPECT_EQ(CreateCertificateDialog::withPfxSuffix(""), "");
}

TEST_F(CreateCertificateForm, EmptyPassphraseIsShownInTheFormWithFocusOnIt)
{
    pdfe::ui::SignDialog sign(nullptr);
    const auto target = out("nopass.pfx");
    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=Jane Doe"),
        QStringLiteral("edit:certOutputPathEdit=%1").arg(QString::fromStdString(target.string())),
        QStringLiteral("click:certOkButton"),
        QStringLiteral("reject"),
    });
    ASSERT_TRUE(obs.opened);
    EXPECT_FALSE(obs.forcedClosed);
    EXPECT_TRUE(obs.errorShownInForm.contains(QStringLiteral("passphrase"), Qt::CaseInsensitive))
        << "saw: '" << obs.errorShownInForm.toStdString() << "'";
    EXPECT_EQ(obs.focusWhenError, QStringLiteral("certPassphraseEdit"));
    EXPECT_FALSE(std::filesystem::exists(target));
}

TEST_F(CreateCertificateForm, EmptyPathIsShownInTheFormWithFocusOnIt)
{
    pdfe::ui::SignDialog sign(nullptr);
    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=Jane Doe"),
        QStringLiteral("edit:certPassphraseEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certPassphraseConfirmEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("click:certOkButton"),
        QStringLiteral("reject"),
    });
    ASSERT_TRUE(obs.opened);
    EXPECT_FALSE(obs.forcedClosed);
    EXPECT_TRUE(obs.errorShownInForm.contains(QStringLiteral("save the certificate")))
        << "saw: '" << obs.errorShownInForm.toStdString() << "'";
    EXPECT_EQ(obs.focusWhenError, QStringLiteral("certOutputPathEdit"));
    EXPECT_TRUE(fieldText(sign, "pfxPathEdit").isEmpty());
}

TEST_F(CreateCertificateForm, AWriteFailureIsShownInTheFormWithFocusOnThePath)
{
    pdfe::ui::SignDialog sign(nullptr);
    // A directory that does not exist: the backend cannot create the file.
    const auto target = out("no-such-dir") / "cert.pfx";
    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=Jane Doe"),
        QStringLiteral("edit:certPassphraseEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certPassphraseConfirmEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certOutputPathEdit=%1").arg(QString::fromStdString(target.string())),
        QStringLiteral("click:certOkButton"),
        QStringLiteral("reject"),
    });
    ASSERT_TRUE(obs.opened);
    EXPECT_FALSE(obs.forcedClosed);
    EXPECT_TRUE(obs.errorShownInForm.contains(QStringLiteral("could not be created")))
        << "saw: '" << obs.errorShownInForm.toStdString() << "'";
    EXPECT_FALSE(obs.errorShownInForm.contains(QLatin1String(kPass)));
    EXPECT_EQ(obs.focusWhenError, QStringLiteral("certOutputPathEdit"));
    EXPECT_TRUE(fieldText(sign, "pfxPathEdit").isEmpty());
}

TEST_F(CreateCertificateForm, BrowsingToAnExistingCertificateReplacesItAsTheDialogConfirmed)
{
    // The save dialog returned this exact file after asking "replace it?", so
    // writing over it is what the user chose.
    pdfe::ui::SignDialog sign(nullptr);
    const auto target = out("replace-me.pfx");
    writeJunk(target);
    testseam::enqueueFileDialogPath(QString::fromStdString(target.string()));

    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=Jane Doe"),
        QStringLiteral("edit:certPassphraseEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certPassphraseConfirmEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("click:certBrowseButton"),
        QStringLiteral("click:certOkButton"),
    });
    ASSERT_TRUE(obs.opened);
    EXPECT_FALSE(obs.forcedClosed);
    EXPECT_TRUE(obs.errorShownInForm.isEmpty()) << obs.errorShownInForm.toStdString();
    EXPECT_EQ(loadedCommonName(target, kPass), "Jane Doe") << "the confirmed file was not replaced";
}

TEST_F(CreateCertificateForm, BrowsingToANameWithoutSuffixDoesNotSilentlyReplaceTheSuffixedFile)
{
    // The save dialog confirmed "keep" (no suffix); the form would write
    // "keep.pfx", which the dialog never asked about. An existing keep.pfx
    // must be refused, not replaced.
    pdfe::ui::SignDialog sign(nullptr);
    const auto chosen = out("keep");
    const auto appended = out("keep.pfx");
    writeJunk(appended);
    const auto before = std::filesystem::file_size(appended);
    testseam::enqueueFileDialogPath(QString::fromStdString(chosen.string()));

    const FormObservation obs = runCreateCert(sign, {
        QStringLiteral("edit:certCnEdit=Jane Doe"),
        QStringLiteral("edit:certPassphraseEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("edit:certPassphraseConfirmEdit=%1").arg(QLatin1String(kPass)),
        QStringLiteral("click:certBrowseButton"),
        QStringLiteral("click:certOkButton"),
        QStringLiteral("reject"),
    });
    ASSERT_TRUE(obs.opened);
    EXPECT_FALSE(obs.forcedClosed);
    EXPECT_TRUE(obs.errorShownInForm.contains(QStringLiteral("already exists")))
        << "saw: '" << obs.errorShownInForm.toStdString() << "'";
    EXPECT_EQ(obs.focusWhenError, QStringLiteral("certOutputPathEdit"));
    EXPECT_EQ(std::filesystem::file_size(appended), before) << "keep.pfx was replaced without asking";
}

TEST(CreateCertificateFormDirect, TypingIntoAFieldClearsTheStaleError)
{
    // The form clears its error when the user starts fixing the field. That is
    // wired to QLineEdit::textEdited, which only real input emits (the test
    // seam's edit: verb uses setText and cannot reach it), so this drives a
    // non-modal instance with key events.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    pdfe::ui::CreateCertificateDialog form;
    form.show();
    form.accept();  // every field empty: rejected, form stays open
    auto* err = form.findChild<QLabel*>(QStringLiteral("certErrorLabel"));
    auto* cn = form.findChild<QLineEdit*>(QStringLiteral("certCnEdit"));
    ASSERT_NE(err, nullptr);
    ASSERT_NE(cn, nullptr);
    ASSERT_FALSE(err->isHidden()) << "precondition: the rejection shows an error";
    ASSERT_FALSE(err->text().isEmpty());
    EXPECT_TRUE(form.isVisible()) << "a rejected form closed";

    QKeyEvent press(QEvent::KeyPress, Qt::Key_J, Qt::ShiftModifier, QStringLiteral("J"));
    QApplication::sendEvent(cn, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_J, Qt::ShiftModifier, QStringLiteral("J"));
    QApplication::sendEvent(cn, &release);

    EXPECT_EQ(cn->text(), QStringLiteral("J")) << "the key did not reach the field";
    EXPECT_TRUE(err->isHidden()) << "the stale error stayed visible while the user fixed the field";
    EXPECT_TRUE(err->text().isEmpty());
    form.close();
}

TEST(CreateCertificateFormSource, TheFormSourceHasNoLoggingCalls)
{
    // The passphrase passes through this form. Its header promises it is never
    // logged; this keeps every logging channel out of the file, so a later
    // "debug the create flow" print cannot quietly carry it.
    const auto src = std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path() /
                     "src" / "ui" / "CreateCertificateDialog.cpp";
    std::ifstream in(src);
    ASSERT_TRUE(in.good()) << src.string();
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::regex logging(
        R"(\bq(Debug|Info|Warning|Critical|Fatal)\b|\bqC(Debug|Info|Warning|Critical)\b|std::(cout|cerr|clog)\b|\bf?printf\s*\(|\bspdlog\b|\bLOG_|\bPDFE_LOG)");
    std::smatch m;
    EXPECT_FALSE(std::regex_search(text, m, logging))
        << "CreateCertificateDialog.cpp contains a logging call: '" << m.str() << "'";
}
