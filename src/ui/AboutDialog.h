#pragma once
#include <QDialog>

class QPushButton;
class QTextBrowser;
class QTextEdit;

namespace pdfe::ui {

/// PDFE About dialog.
///
/// Layout (top -> bottom):
///   aboutContent          QTextBrowser rendering :/branding/about.md
///                         (logo, app name, version, libraries, license
///                         blurb, attribution, itwerx clickable logo,
///                         donation line). Marketing-editable: change
///                         src/ui/about.md and rebuild — no code
///                         changes needed.
///   licenseTextEdit       read-only QTextEdit containing full AGPL-3.0
///                         followed by the Commercial Licensing Addendum
///   okButton              closes the dialog
///
/// objectNames preserved across the 2026-05-06 markdown migration:
///   aboutDialog (this), licenseTextEdit, okButton.
/// objectNames REMOVED (collapsed into aboutContent):
///   colorLogoLabel, appNameLabel, versionLabel, librariesLabel,
///   licenseLabel, broughtToYouByLabel, itwerxLogoLabel, donationLabel.
class AboutDialog : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);
    ~AboutDialog() override;

    QString versionString() const;
    QStringList libraryVersions() const;

private:
    void setupUi();

    QTextBrowser* aboutContent_ = nullptr;
    QTextEdit* licenseTextEdit_ = nullptr;
    QPushButton* okButton_ = nullptr;
};

} // namespace pdfe::ui
