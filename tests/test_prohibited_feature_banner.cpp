// -*- coding: utf-8 -*-
//
// GitHub #60 (SEC-62) — pipeline p84-remaining-open, phase-2.
//
// WHAT WAS ACTUALLY WRONG. `ProhibitedFeatureScanner` existed and was well
// covered by tests/test_prohibited_feature_scanner.cpp, but every one of its
// callers was on the PDF/A compliance path — `PdfAValidator::validate`,
// `PdfAConverter`, `PdfUAConverter`. Verified by enumerating references before
// anything was written here: outside its own translation unit the symbol
// appeared only in those three files, a friend declaration in core/Document.h,
// and the scanner's own unit test. Nothing ran it when a document was OPENED.
//
// That silence is the defect. PDFE default-DENIES JavaScript and /URI actions
// (AppSettings::javaScriptEnabled / externalActionsAllowed, both false since
// the phase-3 corpus runs measured live network resolution from MuPDF's /AA
// and /OpenAction handlers), and AppSettings.h has named "a per-document
// banner" as the intended opt-in UX ever since. The banner did not exist, so
// the user was told neither that the document carried active content nor that
// PDFE had held it back.
//
// WHY THIS IS A GOOGLETEST AND NOT pywinauto. The criterion says to assert the
// banner "without attempting to detect it as a modal dialog", and the reason
// is measured, not theoretical: pywinauto's UIA backend cannot enumerate a Qt
// app-modal QDialog::exec() at all — a dialog sat open for four minutes while
// a 240 s poll reported nothing. The product answer to that is to make the
// advisory a QFrame in the widget tree rather than a dialog. Once it IS a
// widget in the tree, everything the criterion asks about (objectName,
// accessibleName, visibility, dismissal) is readable in-process, so a live-UI
// test would be strictly weaker evidence at several times the cost. The
// objectName/accessibleName assertions below are what keep the widget
// discoverable by the pywinauto layer for whoever needs it later: pywinauto
// reads objectName as `auto_id` and accessibleName as the UIA `Name`.
//
// THE NEGATIVE CONTROL IS NOT OPTIONAL. A banner that fires for every document
// is as bad as one that never fires — the user learns to dismiss it without
// reading. `OpeningACleanDocumentShowsNoBanner` is what makes the positive
// tests mean something; without it a mutant that unconditionally calls show()
// passes the whole file.
//
// FIXTURES ARE BUILT IN-TEST, in the same shape
// tests/test_prohibited_feature_scanner.cpp uses (startxref 0 forces MuPDF's
// repair, which rebuilds the xref). They go through MainWindow::openFile, so
// they also clear the out-of-process preflight gate the way a real open does.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QFrame>
#include <QLabel>
#include <QWidget>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using pdfe::ui::MainWindow;

namespace {

namespace fs = std::filesystem;

// The contract the criterion names. Spelled once, here, so a rename cannot
// silently pass by being applied to product and test at the same time without
// anyone noticing the automation surface moved.
constexpr const char* kBannerObjectName = "prohibitedFeatureBanner";
constexpr const char* kBannerTextObjectName = "prohibitedFeatureBannerText";
constexpr const char* kBannerDismissObjectName =
    "prohibitedFeatureBannerDismissButton";
constexpr const char* kBannerAccessibleName = "Active Content Warning";

fs::path bannerTempDir()
{
    auto d = fs::temp_directory_path() / "pdfe_prohibited_banner";
    fs::create_directories(d);
    return d;
}

void writeBytes(const fs::path& p, const std::string& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A perfectly ordinary one-page document: no /Names, no /OpenAction, no
// /Annots. This is the negative control's subject.
std::string cleanPdf()
{
    const std::string content =
        "BT /F1 14 Tf 60 700 Td (Nothing to see here.) Tj ET\n";
    std::string out = "%PDF-1.7\n";
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 "
           "/BaseFont /Helvetica >> >> >> /Contents 4 0 R >>\nendobj\n";
    out += "4 0 obj\n<< /Length " + std::to_string(content.size()) +
           " >>\nstream\n" + content + "endstream\nendobj\n";
    out += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

// Catalog /Names /JavaScript — the auto-fire shape.
std::string javaScriptPdf()
{
    return
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Names 4 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        ">>\nendobj\n"
        "4 0 obj\n<< /JavaScript << /Names [(hostile) 5 0 R] >> >>\nendobj\n"
        "5 0 obj\n<< /S /JavaScript /JS (app.alert('pwn')) >>\nendobj\n"
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
}

// A /GoToR annotation pointing at a UNC path — an external reference that
// resolves off-host if followed.
std::string externalReferencePdf()
{
    return
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Annots [4 0 R] >>\nendobj\n"
        "4 0 obj\n<< /Type /Annot /Subtype /Link /Rect [100 100 200 120] "
        "/A << /S /GoToR /F (\\\\\\\\attacker.example\\\\share\\\\x) "
        "/D [0 /Fit] >> >>\nendobj\n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
}

class ProhibitedFeatureBannerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        td_ = bannerTempDir();
        mw_ = std::make_unique<MainWindow>();
    }

    void TearDown() override
    {
        mw_.reset();
        QApplication::processEvents();
    }

    // Resolved by objectName every time, never by cached pointer: the point of
    // the criterion is that the automation surface is findable by NAME.
    QFrame* banner() const
    {
        return mw_->findChild<QFrame*>(QLatin1String(kBannerObjectName));
    }
    QLabel* bannerText() const
    {
        return mw_->findChild<QLabel*>(QLatin1String(kBannerTextObjectName));
    }
    QAbstractButton* dismissButton() const
    {
        return mw_->findChild<QAbstractButton*>(
            QLatin1String(kBannerDismissObjectName));
    }

    // The window is never shown in this headless suite, so isVisible() is
    // false for every widget in it regardless of state. isVisibleTo(window) is
    // the question actually being asked: would this be on screen if the window
    // were? (Explicit hide() makes it false; a shown child of a hidden parent
    // makes it true.)
    bool bannerShowing() const
    {
        auto* b = banner();
        return b != nullptr && b->isVisibleTo(mw_.get());
    }

    void open(const fs::path& p)
    {
        mw_->openFile(QString::fromStdString(p.string()));
        QApplication::processEvents();
    }

    fs::path td_;
    std::unique_ptr<MainWindow> mw_;
};

}  // namespace

// ---------------------------------------------------------------------------
// The automation contract: the widget exists, is a QFrame, and carries the
// stable names the criterion asks for. Asserted before any document is open,
// so this is about the widget's identity and not about a particular scan.
// ---------------------------------------------------------------------------
TEST_F(ProhibitedFeatureBannerTest, BannerIsAFrameWithStableAutomationNames)
{
    QFrame* b = banner();
    ASSERT_NE(b, nullptr)
        << "no QFrame named '" << kBannerObjectName << "' exists in the "
        << "MainWindow tree. pywinauto resolves widgets by objectName "
           "(auto_id), so without it the banner is unaddressable by the UI "
           "automation layer.";

    EXPECT_EQ(b->accessibleName().toStdString(), kBannerAccessibleName)
        << "the banner's accessibleName is not the stable string the "
           "criterion requires; UIA exposes it as the Name property and it is "
           "what a screen reader announces. Got: "
        << b->accessibleName().toStdString();
    EXPECT_FALSE(b->accessibleDescription().isEmpty())
        << "the banner has no accessibleDescription, so a screen-reader user "
           "hears its title and nothing about what it means";

    ASSERT_NE(bannerText(), nullptr)
        << "the banner has no label named '" << kBannerTextObjectName
        << "', so nothing can read WHICH feature was found";
    // The label sits on the open path of an attacker-controlled file format.
    // QLabel defaults to Qt::AutoText, which renders rich text.
    EXPECT_EQ(bannerText()->textFormat(), Qt::PlainText)
        << "the banner label is not pinned to PlainText; QLabel's AutoText "
           "default makes any document-derived string a markup-injection path";

    ASSERT_NE(dismissButton(), nullptr)
        << "the banner has no button named '" << kBannerDismissObjectName
        << "', so the criterion's 'dismissible' half is not addressable";
    EXPECT_FALSE(dismissButton()->accessibleName().isEmpty())
        << "the dismiss button has no accessibleName";
}

// With no document open at all there is nothing to warn about.
TEST_F(ProhibitedFeatureBannerTest, BannerIsHiddenBeforeAnyDocumentIsOpened)
{
    ASSERT_NE(banner(), nullptr);
    EXPECT_FALSE(bannerShowing())
        << "the banner is up before a document has even been opened";
}

// ---------------------------------------------------------------------------
// NEGATIVE CONTROL. Without this, a mutant that shows the banner
// unconditionally satisfies every positive test in this file.
// ---------------------------------------------------------------------------
TEST_F(ProhibitedFeatureBannerTest, OpeningACleanDocumentShowsNoBanner)
{
    const auto path = td_ / "clean.pdf";
    writeBytes(path, cleanPdf());
    open(path);

    EXPECT_FALSE(bannerShowing())
        << "the active-content banner was raised for a document with no "
           "JavaScript, no /OpenAction and no annotations at all. A banner "
           "that fires for every file trains the user to dismiss it unread, "
           "which is worse than no banner.";
    ASSERT_NE(bannerText(), nullptr);
    EXPECT_TRUE(bannerText()->text().isEmpty())
        << "the banner is hidden but still carries text from somewhere: "
        << bannerText()->text().toStdString();
}

// ---------------------------------------------------------------------------
// Positive: JavaScript.
// ---------------------------------------------------------------------------
TEST_F(ProhibitedFeatureBannerTest, OpeningAJavaScriptDocumentShowsTheBanner)
{
    const auto path = td_ / "javascript.pdf";
    writeBytes(path, javaScriptPdf());
    open(path);

    EXPECT_TRUE(bannerShowing())
        << "MainWindow::openFile did not raise the active-content banner for "
           "a document carrying catalog /Names /JavaScript. "
           "ProhibitedFeatureScanner detects it; the open path is what has to "
           "tell the user.";
    ASSERT_NE(bannerText(), nullptr);
    EXPECT_TRUE(bannerText()->text().contains(QLatin1String("JavaScript")))
        << "the banner is up but does not name JavaScript, so it does not say "
           "what was found. It said: " << bannerText()->text().toStdString();
}

// ---------------------------------------------------------------------------
// Positive: external references. A second category, because a wiring that
// only ever asks hasJavaScript() would pass the test above.
// ---------------------------------------------------------------------------
TEST_F(ProhibitedFeatureBannerTest,
       OpeningAnExternalReferenceDocumentShowsTheBanner)
{
    const auto path = td_ / "external_reference.pdf";
    writeBytes(path, externalReferencePdf());
    open(path);

    EXPECT_TRUE(bannerShowing())
        << "MainWindow::openFile did not raise the active-content banner for "
           "a document whose annotation carries a /GoToR action to a UNC path";
    ASSERT_NE(bannerText(), nullptr);
    EXPECT_TRUE(bannerText()->text().contains(QLatin1String("GoToR")))
        << "the banner does not name the external-action category it found. "
           "It said: " << bannerText()->text().toStdString();
    EXPECT_FALSE(bannerText()->text().contains(QLatin1String("JavaScript")))
        << "the banner claims this document contains JavaScript; it does not. "
           "The message is not derived from the scan.";
}

// ---------------------------------------------------------------------------
// Dismissal — the other half of the criterion.
// ---------------------------------------------------------------------------
TEST_F(ProhibitedFeatureBannerTest, TheBannerCanBeDismissed)
{
    const auto path = td_ / "javascript_dismiss.pdf";
    writeBytes(path, javaScriptPdf());
    open(path);

    // Guard the precondition: dismissing an already-hidden banner would pass
    // trivially and prove nothing.
    ASSERT_TRUE(bannerShowing())
        << "the banner was not up, so this test cannot show it was dismissed";

    ASSERT_NE(dismissButton(), nullptr);
    dismissButton()->click();
    QApplication::processEvents();

    EXPECT_FALSE(bannerShowing())
        << "clicking '" << kBannerDismissObjectName
        << "' did not hide the banner, so it is not dismissible";
}

// Opening a clean document AFTER a flagged one must clear the banner. A
// dismissible banner that persists across documents would report the previous
// file's content against the current one.
TEST_F(ProhibitedFeatureBannerTest, BannerDoesNotSurviveIntoTheNextDocument)
{
    const auto flagged = td_ / "javascript_then_clean_js.pdf";
    const auto clean = td_ / "javascript_then_clean_clean.pdf";
    writeBytes(flagged, javaScriptPdf());
    writeBytes(clean, cleanPdf());

    open(flagged);
    ASSERT_TRUE(bannerShowing())
        << "the banner never came up, so the sequence below proves nothing";

    open(clean);
    EXPECT_FALSE(bannerShowing())
        << "the banner from the previous document is still up over a clean "
           "one, so it now describes a file that is no longer open";
}
