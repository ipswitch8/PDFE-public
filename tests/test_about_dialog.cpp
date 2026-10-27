// test_about_dialog.cpp - Unit tests for AboutDialog
//
// 2026-05-06: marketing-content widgets (colorLogoLabel, appNameLabel,
// versionLabel, librariesLabel, licenseLabel, broughtToYouByLabel,
// itwerxLogoLabel, donationLabel) were collapsed into a single
// QTextBrowser ``aboutContent`` rendering :/branding/about.md so
// marketing can edit text/links/font sizes/layout without touching
// source. Tests now verify the same exact strings + URLs + resource
// paths via the QTextBrowser content (toPlainText() / toHtml() /
// document source).
#include <gtest/gtest.h>
#include "ui/AboutDialog.h"
#include <QApplication>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextEdit>
#include <QCoreApplication>
#include <QIcon>
#include <QPixmap>
#include <QSize>
#include <memory>

using namespace pdfe::ui;

class AboutDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create fresh dialog for each test
        dialog_ = std::make_unique<AboutDialog>();
    }

    void TearDown() override {
        dialog_.reset();
    }

    std::unique_ptr<AboutDialog> dialog_;
};

TEST_F(AboutDialogTest, Construct) {
    ASSERT_NE(dialog_, nullptr) << "Dialog should be constructed successfully";
    EXPECT_EQ(dialog_->objectName(), QString("aboutDialog"))
        << "Dialog objectName should be 'aboutDialog'";
}

TEST_F(AboutDialogTest, VersionString_MatchesApp) {
    QString appVersion = QCoreApplication::applicationVersion();
    QString dialogVersion = dialog_->versionString();
    EXPECT_EQ(dialogVersion, appVersion)
        << "Dialog versionString() should match QCoreApplication::applicationVersion()";
    EXPECT_FALSE(dialogVersion.isEmpty())
        << "Version string should not be empty";
}

TEST_F(AboutDialogTest, LibraryVersions_ContainsQt) {
    QStringList libraries = dialog_->libraryVersions();
    ASSERT_FALSE(libraries.isEmpty());
    bool containsQt = false;
    for (const QString& lib : libraries) {
        if (lib.contains("Qt", Qt::CaseInsensitive)) {
            containsQt = true;
            break;
        }
    }
    EXPECT_TRUE(containsQt)
        << "Library versions should contain an entry with 'Qt'";
}

TEST_F(AboutDialogTest, LibraryVersions_ContainsMuPDF) {
    QStringList libraries = dialog_->libraryVersions();
    ASSERT_FALSE(libraries.isEmpty());
    bool containsMuPDF = false;
    for (const QString& lib : libraries) {
        if (lib.contains("MuPDF", Qt::CaseInsensitive)) {
            containsMuPDF = true;
            break;
        }
    }
    EXPECT_TRUE(containsMuPDF)
        << "Library versions should contain an entry with 'MuPDF'";
}

TEST_F(AboutDialogTest, ChildWidgets_ObjectNames) {
    QTextBrowser* aboutContent =
        dialog_->findChild<QTextBrowser*>("aboutContent");
    QTextEdit* licenseTextEdit =
        dialog_->findChild<QTextEdit*>("licenseTextEdit");
    QPushButton* okButton = dialog_->findChild<QPushButton*>("okButton");

    EXPECT_NE(aboutContent, nullptr)
        << "Child widget 'aboutContent' (QTextBrowser rendering "
           "about.md) should exist";
    EXPECT_NE(licenseTextEdit, nullptr)
        << "Child widget 'licenseTextEdit' should exist";
    EXPECT_NE(okButton, nullptr)
        << "Child widget 'okButton' should exist";
}

// ---- Marketing-content tests --------------------------------------
// All previously per-label tests now verify content of the single
// QTextBrowser. The semantic intent (URL is reachable, exact wording
// is preserved, resource is referenced) carries through; the
// implementation moved from N QLabels to one Markdown document.

TEST_F(AboutDialogTest, AboutContent_ReferencesColorLogoResource) {
    auto* aboutContent = dialog_->findChild<QTextBrowser*>("aboutContent");
    ASSERT_NE(aboutContent, nullptr);
    // The Markdown source contains an <img> referencing the resource.
    // toHtml() returns Qt's rendered HTML which preserves the src URL
    // for embedded images. If the .qrc isn't linked, the resource
    // path is still in the rendered HTML — the image just won't paint.
    const QString html = aboutContent->toHtml();
    EXPECT_TRUE(html.contains(":/branding/color_logo.png"))
        << "aboutContent must reference :/branding/color_logo.png. "
           "Actual HTML head: " << html.left(500).toStdString();
}

TEST_F(AboutDialogTest, AboutContent_ItwerxLinkAndLogo) {
    auto* aboutContent = dialog_->findChild<QTextBrowser*>("aboutContent");
    ASSERT_NE(aboutContent, nullptr);

    EXPECT_TRUE(aboutContent->openExternalLinks())
        << "aboutContent must have setOpenExternalLinks(true) so "
           "QDesktopServices opens the URL in the system browser.";

    const QString html = aboutContent->toHtml();
    EXPECT_TRUE(html.contains("https://www.itwerx.net"))
        << "aboutContent must contain the exact link target "
           "'https://www.itwerx.net'. Actual HTML head: "
        << html.left(500).toStdString();
    EXPECT_TRUE(html.contains(":/branding/itwerx_logo_small.png"))
        << "aboutContent must reference the embedded itwerx logo "
           "resource ':/branding/itwerx_logo_small.png'.";
}

TEST_F(AboutDialogTest, AboutContent_BroughtToYouByExactText) {
    auto* aboutContent = dialog_->findChild<QTextBrowser*>("aboutContent");
    ASSERT_NE(aboutContent, nullptr);
    // Branding text — must NOT be translated; criterion mandates exact
    // wording.
    const QString plain = aboutContent->toPlainText();
    EXPECT_TRUE(plain.contains("Brought to you by..."))
        << "aboutContent must contain exactly 'Brought to you by...'. "
           "Actual plain text: " << plain.toStdString();
}

TEST_F(AboutDialogTest, AboutContent_DonationExactText) {
    auto* aboutContent = dialog_->findChild<QTextBrowser*>("aboutContent");
    ASSERT_NE(aboutContent, nullptr);
    // Donation copy — must NOT be translated; criterion mandates exact
    // wording. ASCII hyphen, not em-dash.
    const QString expected =
        "Suggested donation - whatever one month of your current "
        "commercial PDF app is costing you!";
    const QString plain = aboutContent->toPlainText();
    EXPECT_TRUE(plain.contains(expected))
        << "aboutContent must contain exact donation text. "
           "Actual plain text: " << plain.toStdString();
}

TEST_F(AboutDialogTest, AboutContent_VersionPlaceholderSubstituted) {
    auto* aboutContent = dialog_->findChild<QTextBrowser*>("aboutContent");
    ASSERT_NE(aboutContent, nullptr);
    const QString plain = aboutContent->toPlainText();
    // The .md uses {{VERSION}} which the dialog substitutes at load
    // time. After substitution there must be no literal placeholder
    // remaining, and the actual app version must appear.
    EXPECT_FALSE(plain.contains("{{VERSION}}"))
        << "{{VERSION}} placeholder must be substituted at load time.";
    EXPECT_FALSE(plain.contains("{{QT_VERSION}}"))
        << "{{QT_VERSION}} placeholder must be substituted at load time.";
    EXPECT_TRUE(plain.contains(QCoreApplication::applicationVersion()))
        << "aboutContent must show the live app version. Expected '"
        << QCoreApplication::applicationVersion().toStdString()
        << "' in plain text.";
}

TEST_F(AboutDialogTest, WindowIcon_IsNonNull) {
    // Phase-4 acceptance: the AboutDialog title-bar must carry the
    // PDFE icon, not the generic IDI_APPLICATION glyph. This is
    // satisfied by AboutDialog's constructor calling
    // setWindowIcon(pdfeAppIcon()). The C++ check here verifies the
    // QWindow-level state independently of the live-process
    // pywinauto check in tests/test_ui_titlebar_icon.py (which can
    // be skipped if Help->About isn't reachable via menu keys).
    QIcon ic = dialog_->windowIcon();
    EXPECT_FALSE(ic.isNull())
        << "AboutDialog has a null windowIcon — title bar will "
           "fall back to IDI_APPLICATION. Constructor must call "
           "setWindowIcon(pdfeAppIcon()).";
    QPixmap pm = ic.pixmap(QSize(16, 16));
    EXPECT_FALSE(pm.isNull())
        << "AboutDialog windowIcon cannot render at 16x16 — DWM "
           "uses the small icon for the title bar.";
}

TEST_F(AboutDialogTest, LicenseTextEdit_ContainsAgplAndAddendum) {
    QTextEdit* licenseTextEdit =
        dialog_->findChild<QTextEdit*>("licenseTextEdit");
    ASSERT_NE(licenseTextEdit, nullptr)
        << "licenseTextEdit should exist";
    EXPECT_EQ(licenseTextEdit->objectName(), QString("licenseTextEdit"));
    EXPECT_TRUE(licenseTextEdit->isReadOnly())
        << "licenseTextEdit must be read-only";

    const QString content = licenseTextEdit->toPlainText();
    ASSERT_FALSE(content.isEmpty())
        << "licenseTextEdit content should be non-empty (LICENSE + addendum "
           "loaded from :/legal/...).";

    EXPECT_TRUE(content.contains("GNU AFFERO GENERAL PUBLIC LICENSE"))
        << "licenseTextEdit must contain the AGPL header text.";
    EXPECT_TRUE(content.contains("Commercial Licensing Addendum"))
        << "licenseTextEdit must contain the Commercial Licensing "
           "Addendum heading.";
    // User-confirmed contact email overrides the pipeline.json
    // placeholder ('itwerx@itwerx.net'); inquiries@itwerx.net is the
    // canonical contact in commercial_addendum.txt.
    EXPECT_TRUE(content.contains("inquiries@itwerx.net"))
        << "licenseTextEdit must contain the commercial-licensing "
           "contact email 'inquiries@itwerx.net'.";
}
