// -*- coding: utf-8 -*-
#include "PreferencesDialog.h"
#include "../app/AppSettings.h"
#include "../core/JpxSecurityGate.h"
#include "../core/Jbig2SecurityGate.h"
#include "TestDialogSeam.h"

#include <QMessageBox>

#include <QTabWidget>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QStringList>
#include <QtGlobal>

namespace pdfe::ui {

PreferencesDialog::PreferencesDialog(pdfe::app::AppSettings* settings, QWidget* parent)
    : QDialog(parent)
    , settings_(settings)
{
    setObjectName("preferencesDialog");
    setWindowTitle(tr("Preferences"));
    setMinimumSize(450, 400);

    auto* mainLayout = new QVBoxLayout(this);

    tabWidget_ = new QTabWidget(this);
    // Pipeline 3e9b1d7c phase-2 (A1 / P-GAP-102): test_phase7_new_features.py
    // looks for objectName "preferencesTabs"; prior name was
    // "preferencesTabWidget". Renamed to match the test expectation.
    tabWidget_->setObjectName("preferencesTabs");

    // --- General tab ---
    // Pipeline a7c4f891 phase-1 (A4): each tab page gets a stable
    // objectName so tests can address pages by name instead of index.
    auto* generalTab = new QWidget(this);
    generalTab->setObjectName("preferencesGeneralTab");
    generalTab->setAccessibleName(tr("General preferences tab"));
    auto* generalLayout = new QFormLayout(generalTab);

    maxRecentFilesSpinBox_ = new QSpinBox(this);
    maxRecentFilesSpinBox_->setObjectName("maxRecentFilesSpinBox");
    maxRecentFilesSpinBox_->setAccessibleName(tr("Maximum recent files"));
    maxRecentFilesSpinBox_->setRange(1, 50);
    generalLayout->addRow(tr("Maximum recent files:"), maxRecentFilesSpinBox_);

    tabWidget_->addTab(generalTab, tr("General"));

    // --- View tab ---
    auto* viewTab = new QWidget(this);
    viewTab->setObjectName("preferencesViewTab");
    viewTab->setAccessibleName(tr("View preferences tab"));
    auto* viewLayout = new QFormLayout(viewTab);

    defaultZoomSpinBox_ = new QDoubleSpinBox(this);
    defaultZoomSpinBox_->setObjectName("defaultZoomSpinBox");
    defaultZoomSpinBox_->setAccessibleName(tr("Default zoom"));
    defaultZoomSpinBox_->setRange(0.25, 5.0);
    defaultZoomSpinBox_->setSingleStep(0.25);
    defaultZoomSpinBox_->setDecimals(2);
    defaultZoomSpinBox_->setSuffix("x");
    viewLayout->addRow(tr("Default zoom:"), defaultZoomSpinBox_);

    tileCacheSpinBox_ = new QSpinBox(this);
    tileCacheSpinBox_->setObjectName("tileCacheSpinBox");
    tileCacheSpinBox_->setAccessibleName(tr("Tile cache size (MB)"));
    tileCacheSpinBox_->setRange(64, 4096);
    tileCacheSpinBox_->setSuffix(" MB");
    viewLayout->addRow(tr("Tile cache size:"), tileCacheSpinBox_);

    tabWidget_->addTab(viewTab, tr("View"));

    // --- Security tab ---
    // Both gates default OFF. PDFs are attacker-controlled file
    // formats; without these gates a malicious PDF can run JS,
    // submit forms to attacker URLs, or launch processes silently
    // at document open. Phase-3 corpus runs measured concrete
    // network-resolution leakage when JS was unconditionally
    // enabled. Users opt back in here for documents they trust.
    auto* securityTab = new QWidget(this);
    securityTab->setObjectName("preferencesSecurityTab");
    securityTab->setAccessibleName(tr("Security preferences tab"));
    auto* securityLayout = new QVBoxLayout(securityTab);

    auto* securityIntro = new QLabel(
        tr("PDFs can contain active content (JavaScript, form-submit "
           "actions, launch actions) that runs at open. PDFE "
           "default-denies these for safety. Enable below if you "
           "open trusted documents that need them."),
        this);
    securityIntro->setWordWrap(true);
    securityLayout->addWidget(securityIntro);

    javaScriptEnabledCheck_ = new QCheckBox(
        tr("Allow JavaScript in PDF forms"), this);
    javaScriptEnabledCheck_->setObjectName("javaScriptEnabledCheck");
    javaScriptEnabledCheck_->setAccessibleName(
        tr("Allow JavaScript in PDF forms"));
    javaScriptEnabledCheck_->setToolTip(
        tr("Lets PDF forms run JavaScript for field validation "
           "and calculation. Some malicious PDFs use JS to attempt "
           "network exfiltration; default off."));
    securityLayout->addWidget(javaScriptEnabledCheck_);

    externalActionsAllowedCheck_ = new QCheckBox(
        tr("Allow external actions (Launch / URI / SubmitForm / "
           "ImportData)"),
        this);
    externalActionsAllowedCheck_->setObjectName(
        "externalActionsAllowedCheck");
    externalActionsAllowedCheck_->setAccessibleName(
        tr("Allow external actions"));
    // Meta-audit F13 (SEC-49): this checkbox now gates a real code path —
    // MainWindow::followLinkAt() refuses /URI link actions while it is off.
    // The wording states exactly what the control does, because the previous
    // wording described behaviour the setting did not actually have.
    externalActionsAllowedCheck_->setToolTip(
        tr("Lets a PDF open URLs from link annotations (/URI actions). "
           "While this is off, PDFE blocks /URI links and shows a status-bar "
           "notice instead of opening them. /Launch actions always prompt, "
           "whether this is on or off. Default off."));
    securityLayout->addWidget(externalActionsAllowedCheck_);

    // ── GitHub #240 — network locations ──────────────────────────────────
    allowUserNetworkPathsCheck_ = new QCheckBox(
        tr("Allow opening and saving files on network locations"), this);
    allowUserNetworkPathsCheck_->setObjectName("allowUserNetworkPathsCheck");
    allowUserNetworkPathsCheck_->setAccessibleName(
        tr("Allow network locations for files you open"));
    allowUserNetworkPathsCheck_->setToolTip(
        tr("Lets YOU open and save documents on mapped drives and UNC paths "
           "(\\server\share). PDFE checks the location responds before "
           "opening, so an unavailable share reports that rather than freezing. "
           "This does not let a PDF reach network resources by itself — that is "
           "the separate setting below. Default on."));
    securityLayout->addWidget(allowUserNetworkPathsCheck_);

    allowDocumentSameShareCheck_ = new QCheckBox(
        tr("Let a document opened from a network share reach other files on "
           "that same share"),
        this);
    allowDocumentSameShareCheck_->setObjectName("allowDocumentSameShareCheck");
    allowDocumentSameShareCheck_->setAccessibleName(
        tr("Allow same-share resources requested by a document"));
    // Say what the risk is, not just what the switch does. A path chosen by
    // DOCUMENT CONTENT is the egress vector GitHub #91 closed: reaching it tells
    // that host this machine opened this file, before any content is fetched.
    allowDocumentSameShareCheck_->setToolTip(
        tr("Off by default. When off, a PDF cannot make PDFE contact any network "
           "location, even one it was opened from. When on, links in the "
           "document may reach files on the SAME \\server\share only — a "
           "link to any other share or host is still refused. Leave this off "
           "unless you work with documents that legitimately cross-reference "
           "files on their own share."));
    securityLayout->addWidget(allowDocumentSameShareCheck_);

    jpxRenderingEnabledCheck_ = new QCheckBox(
        tr("Enable JPEG2000 (JPX) image rendering"), this);
    jpxRenderingEnabledCheck_->setObjectName("jpxRenderingEnabledCheck");
    jpxRenderingEnabledCheck_->setAccessibleName(
        tr("Enable JPEG2000 image rendering"));
    jpxRenderingEnabledCheck_->setToolTip(
        tr("JPEG2000 images are decoded by OpenJPEG, where a malformed "
           "image can corrupt memory (a potential remote-code-execution "
           "vector) in a way the PDF engine cannot safely trap. PDFE "
           "leaves JPEG2000 images unrendered by default and shows a "
           "placeholder. Enable only for trusted documents. Default off."));
    securityLayout->addWidget(jpxRenderingEnabledCheck_);

    jbig2RenderingEnabledCheck_ = new QCheckBox(
        tr("Enable JBIG2 image rendering"), this);
    jbig2RenderingEnabledCheck_->setObjectName("jbig2RenderingEnabledCheck");
    jbig2RenderingEnabledCheck_->setAccessibleName(
        tr("Enable JBIG2 image rendering"));
    jbig2RenderingEnabledCheck_->setToolTip(
        tr("JBIG2 images (common in scanned black-and-white documents) are "
           "decoded by jbig2dec, which has a long history of memory-safety "
           "flaws that a malformed image can trigger in a way the PDF engine "
           "cannot safely trap. PDFE leaves JBIG2 images unrendered by default "
           "and shows a placeholder. Enable only for trusted documents. "
           "Default off."));
    securityLayout->addWidget(jbig2RenderingEnabledCheck_);

    securityLayout->addStretch();

    auto* securityNote = new QLabel(
        tr("Settings are stored under "
           "Security/javaScriptEnabled, "
           "Security/externalActionsAllowed and "
           "Security/jpxRenderingEnabled and "
           "Security/jbig2RenderingEnabled in the "
           "OS-native QSettings store. JavaScript / external-action changes "
           "apply to the next document opened; JPEG2000 and JBIG2 rendering "
           "apply immediately."),
        this);
    securityNote->setWordWrap(true);
    securityNote->setStyleSheet("color: gray; font-size: 9pt;");
    securityLayout->addWidget(securityNote);

    tabWidget_->addTab(securityTab, tr("Security"));

    // --- OCR tab ---
    auto* ocrTab = new QWidget(this);
    ocrTab->setObjectName("preferencesOcrTab");
    ocrTab->setAccessibleName(tr("OCR preferences tab"));
    auto* ocrLayout = new QFormLayout(ocrTab);
    ocrLanguageCombo_ = new QComboBox(this);
    ocrLanguageCombo_->setObjectName("ocrLanguageCombo");
    ocrLanguageCombo_->setAccessibleName(tr("OCR language"));
    ocrLanguageCombo_->setEditable(true);
    // Curated short list of Tesseract languages with traineddata usually
    // shipped or easy to install; users can type any Tesseract code
    // (including multi-lang like "eng+fra") for languages not listed.
    struct LangEntry { const char* code; const char* label; };
    const LangEntry kLanguages[] = {
        {"eng", "English (eng)"},
        {"fra", "Fran\xC3\xA7""ais (fra)"},
        {"deu", "Deutsch (deu)"},
        {"spa", "Espa\xC3\xB1ol (spa)"},
        {"ita", "Italiano (ita)"},
        {"por", "Portugu\xC3\xAAs (por)"},
        {"nld", "Nederlands (nld)"},
        {"rus", "Russian (rus)"},
        {"jpn", "Japanese (jpn)"},
        {"chi_sim", "Chinese Simplified (chi_sim)"},
        {"chi_tra", "Chinese Traditional (chi_tra)"},
        {"kor", "Korean (kor)"},
        {"ara", "Arabic (ara)"},
        {"heb", "Hebrew (heb)"},
        {"hin", "Hindi (hin)"},
        {"tha", "Thai (tha)"},
    };
    for (const auto& l : kLanguages) {
        ocrLanguageCombo_->addItem(QString::fromUtf8(l.label),
                                   QString::fromUtf8(l.code));
    }
    ocrLayout->addRow(tr("OCR language:"), ocrLanguageCombo_);
    auto* ocrHelp = new QLabel(
        tr("Tesseract language code (e.g. \"eng\", \"fra\", \"eng+fra\"). "
           "Requires matching .traineddata in the tessdata directory."),
        this);
    ocrHelp->setWordWrap(true);
    ocrLayout->addRow(ocrHelp);
    tabWidget_->addTab(ocrTab, tr("OCR"));

    // --- Advanced tab ---
    // (objectName set below where the widget is created)
    auto* advancedTab = new QWidget(this);
    advancedTab->setObjectName("preferencesAdvancedTab");
    advancedTab->setAccessibleName(tr("Advanced preferences tab"));
    auto* advancedLayout = new QFormLayout(advancedTab);

    logLevelCombo_ = new QComboBox(this);
    logLevelCombo_->setObjectName("logLevelCombo");
    logLevelCombo_->setAccessibleName(tr("Log level"));
    logLevelCombo_->addItem(tr("Debug"));
    logLevelCombo_->addItem(tr("Info"));
    logLevelCombo_->addItem(tr("Warning"));
    logLevelCombo_->addItem(tr("Critical"));
    advancedLayout->addRow(tr("Log level:"), logLevelCombo_);

    logToFileCheck_ = new QCheckBox(tr("Enable file logging"), this);
    logToFileCheck_->setObjectName("logToFileCheck");
    logToFileCheck_->setAccessibleName(tr("Enable file logging"));
    advancedLayout->addRow(logToFileCheck_);

    // GitHub #127 -- opt-in version check. OFF by default: a stock install
    // makes no releases-feed request. Notify-only; it never downloads or
    // executes anything (UC-9, docs/code-signing.md).
    updateCheckCheck_ = new QCheckBox(
        tr("Check for new versions at startup (opt-in)"), this);
    updateCheckCheck_->setObjectName("updateCheckCheck");
    updateCheckCheck_->setAccessibleName(tr("Check for new versions at startup"));
    updateCheckCheck_->setToolTip(
        tr("When enabled, PDFE contacts the project's releases page once at "
           "startup to see whether a newer version exists, and tells you. It "
           "never downloads or installs anything."));
    advancedLayout->addRow(updateCheckCheck_);

    logFilePathEdit_ = new QLineEdit(this);
    logFilePathEdit_->setObjectName("logFilePathEdit");
    logFilePathEdit_->setAccessibleName(tr("Log file path"));
    logFilePathEdit_->setPlaceholderText(tr("Path to log file..."));
    advancedLayout->addRow(tr("Log file path:"), logFilePathEdit_);

    tabWidget_->addTab(advancedTab, tr("Advanced"));

    mainLayout->addWidget(tabWidget_);

    // Button box
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox->setObjectName("preferencesButtonBox");
    // Name the standard buttons so they are reachable / identifiable by
    // automation (pywinauto auto_id, UIA Name) and by the keyboard test seam
    // (which reports the default button's objectName).
    if (auto* okBtn = buttonBox->button(QDialogButtonBox::Ok)) {
        okBtn->setObjectName("preferencesOkButton");
        okBtn->setAccessibleName(tr("OK"));
    }
    if (auto* cancelBtn = buttonBox->button(QDialogButtonBox::Cancel)) {
        cancelBtn->setObjectName("preferencesCancelButton");
        cancelBtn->setAccessibleName(tr("Cancel"));
    }
    connect(buttonBox, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    loadSettings();

    setupTestPipes();

    // GAP-A14 keyboard-state seam: install the dialog-child keyboard
    // query/result pipes (gated by PDFE_TEST_PIPES inside the helper).
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

void PreferencesDialog::loadSettings()
{
    if (!settings_) return;

    maxRecentFilesSpinBox_->setValue(settings_->maxRecentFiles());
    defaultZoomSpinBox_->setValue(static_cast<double>(settings_->defaultZoom()));
    tileCacheSpinBox_->setValue(settings_->tileCacheSizeMB());
    logLevelCombo_->setCurrentIndex(settings_->logLevel());
    logToFileCheck_->setChecked(settings_->logToFile());
    updateCheckCheck_->setChecked(settings_->updateCheckEnabled());
    logFilePathEdit_->setText(settings_->logFilePath());

    const QString ocrLang = settings_->ocrLanguage();
    // Match the combo's userData (Tesseract code) to the stored value;
    // fall back to typing the code into the editable combo for unknown
    // languages.
    int idx = ocrLanguageCombo_->findData(ocrLang);
    if (idx >= 0) {
        ocrLanguageCombo_->setCurrentIndex(idx);
    } else {
        ocrLanguageCombo_->setEditText(ocrLang);
    }

    if (javaScriptEnabledCheck_) {
        javaScriptEnabledCheck_->setChecked(settings_->javaScriptEnabled());
    }
    if (externalActionsAllowedCheck_) {
        externalActionsAllowedCheck_->setChecked(
            settings_->externalActionsAllowed());
    }
    if (allowUserNetworkPathsCheck_) {
        allowUserNetworkPathsCheck_->setChecked(
            settings_->allowUserNetworkPaths());
    }
    if (allowDocumentSameShareCheck_) {
        allowDocumentSameShareCheck_->setChecked(
            settings_->allowDocumentSameShareResources());
    }
    if (jpxRenderingEnabledCheck_) {
        jpxRenderingEnabledCheck_->setChecked(settings_->jpxRenderingEnabled());
    }
    if (jbig2RenderingEnabledCheck_) {
        jbig2RenderingEnabledCheck_->setChecked(settings_->jbig2RenderingEnabled());
    }
}

void PreferencesDialog::accept()
{
    if (settings_) {
        settings_->setMaxRecentFiles(maxRecentFilesSpinBox_->value());
        settings_->setDefaultZoom(static_cast<float>(defaultZoomSpinBox_->value()));
        settings_->setTileCacheSizeMB(tileCacheSpinBox_->value());
        settings_->setLogLevel(logLevelCombo_->currentIndex());
        settings_->setLogToFile(logToFileCheck_->isChecked());
        settings_->setUpdateCheckEnabled(updateCheckCheck_->isChecked());
        settings_->setLogFilePath(logFilePathEdit_->text());

        // OCR language: prefer the item's userData (Tesseract code) when
        // the user picked an entry from the list; otherwise take the
        // free-text they typed.
        const int idx = ocrLanguageCombo_->currentIndex();
        QString ocrLang;
        if (idx >= 0 &&
            ocrLanguageCombo_->itemText(idx) == ocrLanguageCombo_->currentText()) {
            ocrLang = ocrLanguageCombo_->itemData(idx).toString();
        } else {
            ocrLang = ocrLanguageCombo_->currentText().trimmed();
            if (ocrLang.isEmpty()) ocrLang = QStringLiteral("eng");
        }
        settings_->setOcrLanguage(ocrLang);

        if (javaScriptEnabledCheck_) {
            settings_->setJavaScriptEnabled(
                javaScriptEnabledCheck_->isChecked());
        }
        if (externalActionsAllowedCheck_) {
            settings_->setExternalActionsAllowed(
                externalActionsAllowedCheck_->isChecked());
        }
        if (allowUserNetworkPathsCheck_) {
            settings_->setAllowUserNetworkPaths(
                allowUserNetworkPathsCheck_->isChecked());
        }
        if (allowDocumentSameShareCheck_) {
            settings_->setAllowDocumentSameShareResources(
                allowDocumentSameShareCheck_->isChecked());
        }

        // #50: JPEG2000 opt-in. On an OFF->ON transition, warn once about the
        // OpenJPEG heap-overflow/RCE risk; if declined, keep it OFF. Apply the
        // decision to the MuPDF decode gate immediately.
        if (jpxRenderingEnabledCheck_) {
            bool nowEnabled = jpxRenderingEnabledCheck_->isChecked();
            const bool wasEnabled = settings_->jpxRenderingEnabled();
            if (nowEnabled && !wasEnabled && !confirmEnableJpxRisk()) {
                nowEnabled = false;
                jpxRenderingEnabledCheck_->setChecked(false);
            }
            settings_->setJpxRenderingEnabled(nowEnabled);
            pdfe::core::setJpxDecodingEnabled(nowEnabled);
        }

        // SEC-56 (#56): JBIG2 opt-in, mirroring the JPX transition above. On an
        // OFF->ON transition, warn once about jbig2dec's memory-safety history;
        // if declined, keep it OFF. Apply to the MuPDF decode gate immediately.
        if (jbig2RenderingEnabledCheck_) {
            bool nowEnabled = jbig2RenderingEnabledCheck_->isChecked();
            const bool wasEnabled = settings_->jbig2RenderingEnabled();
            if (nowEnabled && !wasEnabled && !confirmEnableJbig2Risk()) {
                nowEnabled = false;
                jbig2RenderingEnabledCheck_->setChecked(false);
            }
            settings_->setJbig2RenderingEnabled(nowEnabled);
            pdfe::core::setJbig2DecodingEnabled(nowEnabled);
        }

        settings_->sync();
    }

    QDialog::accept();
}

bool PreferencesDialog::confirmEnableJpxRisk()
{
    const auto btn = QMessageBox::warning(
        this, tr("Enable JPEG2000 rendering?"),
        tr("JPEG2000 (JPX) images are decoded by OpenJPEG. A malformed "
           "JPEG2000 image in an untrusted PDF can corrupt memory — a potential "
           "remote-code-execution vector that the PDF engine cannot safely trap.\n\n"
           "Only enable this for documents you trust. Enable JPEG2000 rendering now?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return btn == QMessageBox::Yes;
}

bool PreferencesDialog::confirmEnableJbig2Risk()
{
    const auto btn = QMessageBox::warning(
        this, tr("Enable JBIG2 rendering?"),
        tr("JBIG2 images are decoded by jbig2dec, which has a long history of "
           "memory-safety flaws. A malformed JBIG2 image in an untrusted PDF can "
           "corrupt memory — a potential remote-code-execution vector that the "
           "PDF engine cannot safely trap.\n\n"
           "Only enable this for documents you trust. Enable JBIG2 rendering now?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return btn == QMessageBox::Yes;
}

// ───────────────────────────────────────────────────────────────────────
// Test-only pipe seam (gated by PDFE_TEST_PIPES). See header for rationale:
// on Server-2019 pywinauto's UIA descendants()/FindAll blocks indefinitely
// when enumerating an OPEN modal QDialog's widgets, wedging the whole test
// file. These dialog-child QLineEdit pipes let the harness inventory the
// widgets, read/set values, and accept/reject WITHOUT any UIA enumeration.
// ───────────────────────────────────────────────────────────────────────

namespace {

// Find a named interactive child widget on the dialog.
template <typename T>
T* findNamed(const QDialog* dlg, const QString& objectName)
{
    const QList<T*> kids = dlg->findChildren<T*>();
    for (T* w : kids) {
        if (w->objectName() == objectName) return w;
    }
    return nullptr;
}

} // namespace

QString PreferencesDialog::widgetInventory() const
{
    // Returns "objectName:accessibleName:kind" triples, '|'-joined.
    // QLineEdit strips '\n', so '|' is the record separator and ':' is
    // the field separator (objectNames/kinds never contain ':').
    struct Row { QWidget* w; const char* kind; };
    const Row rows[] = {
        { maxRecentFilesSpinBox_,        "spin" },
        { defaultZoomSpinBox_,           "dspin" },
        { tileCacheSpinBox_,             "spin" },
        { javaScriptEnabledCheck_,       "checkbox" },
        { externalActionsAllowedCheck_,  "checkbox" },
        { allowUserNetworkPathsCheck_,   "checkbox" },
        { allowDocumentSameShareCheck_,  "checkbox" },
        { ocrLanguageCombo_,             "combo" },
        { logLevelCombo_,                "combo" },
        { logToFileCheck_,               "checkbox" },
        { logFilePathEdit_,              "edit" },
    };
    QStringList parts;
    for (const Row& r : rows) {
        if (!r.w) continue;
        parts << r.w->objectName() + QLatin1Char(':')
                 + r.w->accessibleName() + QLatin1Char(':')
                 + QLatin1String(r.kind);
    }
    QString record = parts.join(QLatin1Char('|'));
    // Append the tab labels after a "||tabs=" marker (mirrors the
    // "||combos=" convention in TestDialogSeam.h) so tests can assert the
    // five-tab structure without enumerating TabItem controls via UIA.
    if (tabWidget_) {
        QStringList tabs;
        for (int i = 0; i < tabWidget_->count(); ++i) {
            tabs << tabWidget_->tabText(i);
        }
        if (!tabs.isEmpty()) {
            record += QStringLiteral("||tabs=") + tabs.join(QLatin1Char(';'));
        }
    }
    return record;
}

QString PreferencesDialog::getWidgetValue(const QString& objectName) const
{
    if (auto* sb = findNamed<QSpinBox>(this, objectName)) {
        return QString::number(sb->value());
    }
    if (auto* dsb = findNamed<QDoubleSpinBox>(this, objectName)) {
        return QString::number(dsb->value());
    }
    if (auto* cb = findNamed<QCheckBox>(this, objectName)) {
        return cb->isChecked() ? QStringLiteral("true")
                               : QStringLiteral("false");
    }
    if (auto* combo = findNamed<QComboBox>(this, objectName)) {
        // For an editable combo (ocrLanguageCombo) the meaningful value
        // is the current text; for a list combo (logLevelCombo) tests
        // verify the current INDEX. Return "index@@text" so the caller
        // can pick whichever it needs ('@@' avoids QLineEdit control-char
        // stripping; index is numeric so it never contains '@@').
        return QString::number(combo->currentIndex())
               + QStringLiteral("@@") + combo->currentText();
    }
    if (auto* le = findNamed<QLineEdit>(this, objectName)) {
        return le->text();
    }
    return QStringLiteral("__notfound__");
}

bool PreferencesDialog::setWidgetValue(const QString& objectName,
                                       const QString& value)
{
    if (auto* sb = findNamed<QSpinBox>(this, objectName)) {
        bool ok = false;
        const int v = value.toInt(&ok);
        if (!ok) return false;
        sb->setValue(v);
        return true;
    }
    if (auto* dsb = findNamed<QDoubleSpinBox>(this, objectName)) {
        bool ok = false;
        const double v = value.toDouble(&ok);
        if (!ok) return false;
        dsb->setValue(v);
        return true;
    }
    if (auto* cb = findNamed<QCheckBox>(this, objectName)) {
        const QString v = value.trimmed().toLower();
        cb->setChecked(v == QLatin1String("true") || v == QLatin1String("1")
                       || v == QLatin1String("checked")
                       || v == QLatin1String("yes"));
        return true;
    }
    if (auto* combo = findNamed<QComboBox>(this, objectName)) {
        // Match by display text first (drives the same accept() path a
        // user would: itemText == currentText → itemData is persisted).
        const int byText = combo->findText(value);
        if (byText >= 0) {
            combo->setCurrentIndex(byText);
            return true;
        }
        // Then by userData (Tesseract code, e.g. "deu").
        const int byData = combo->findData(value);
        if (byData >= 0) {
            combo->setCurrentIndex(byData);
            return true;
        }
        // Editable free-text fallback (unknown Tesseract code).
        if (combo->isEditable()) {
            combo->setCurrentIndex(-1);
            combo->setEditText(value);
            return true;
        }
        return false;
    }
    if (auto* le = findNamed<QLineEdit>(this, objectName)) {
        le->setText(value);
        return true;
    }
    return false;
}

void PreferencesDialog::setupTestPipes()
{
    if (!pdfe::ui::testseam::testPipesEnabled()) {
        return;
    }

    auto makePipe = [this](const char* name, const char* accName,
                           bool readOnly) -> QLineEdit* {
        auto* pipe = new QLineEdit(this);
        pipe->setObjectName(QString::fromLatin1(name));
        pipe->setAccessibleName(QString::fromLatin1(accName));
        pipe->setFixedSize(0, 0);
        pipe->setMaximumSize(0, 0);
        pipe->setFocusPolicy(Qt::NoFocus);
        if (readOnly) pipe->setReadOnly(true);
        return pipe;
    };

    // Widget inventory: any non-empty query → result pipe gets triples.
    testPrefWidgetNamesResultPipe_ =
        makePipe("testPrefWidgetNamesResultPipe",
                 "Test Preferences Widget Names Result", true);
    testPrefWidgetNamesQueryPipe_ =
        makePipe("testPrefWidgetNamesQueryPipe",
                 "Test Preferences Widget Names Query", false);
    connect(testPrefWidgetNamesQueryPipe_, &QLineEdit::textChanged,
            this, [this](const QString& text) {
        if (text.isEmpty()) return;
        testPrefWidgetNamesResultPipe_->blockSignals(true);
        testPrefWidgetNamesResultPipe_->setText(widgetInventory());
        testPrefWidgetNamesResultPipe_->blockSignals(false);
        testPrefWidgetNamesQueryPipe_->blockSignals(true);
        testPrefWidgetNamesQueryPipe_->clear();
        testPrefWidgetNamesQueryPipe_->blockSignals(false);
    });

    // Get-value: query = objectName → result = current value.
    testPrefGetValueResultPipe_ =
        makePipe("testPrefGetValueResultPipe",
                 "Test Preferences Get Value Result", true);
    testPrefGetValueQueryPipe_ =
        makePipe("testPrefGetValueQueryPipe",
                 "Test Preferences Get Value Query", false);
    connect(testPrefGetValueQueryPipe_, &QLineEdit::textChanged,
            this, [this](const QString& objectName) {
        if (objectName.isEmpty()) return;
        testPrefGetValueResultPipe_->blockSignals(true);
        testPrefGetValueResultPipe_->setText(getWidgetValue(objectName));
        testPrefGetValueResultPipe_->blockSignals(false);
        testPrefGetValueQueryPipe_->blockSignals(true);
        testPrefGetValueQueryPipe_->clear();
        testPrefGetValueQueryPipe_->blockSignals(false);
    });

    // Set-value: write "objectName=value". The reply (ok/fail) is written
    // back into the get-value result pipe so the caller can confirm.
    testPrefSetValuePipe_ =
        makePipe("testPrefSetValuePipe",
                 "Test Preferences Set Value", false);
    connect(testPrefSetValuePipe_, &QLineEdit::textChanged,
            this, [this](const QString& payload) {
        if (payload.isEmpty()) return;
        const int eq = payload.indexOf(QLatin1Char('='));
        bool ok = false;
        if (eq > 0) {
            const QString name = payload.left(eq);
            const QString val = payload.mid(eq + 1);
            ok = setWidgetValue(name, val);
        }
        if (testPrefGetValueResultPipe_) {
            testPrefGetValueResultPipe_->blockSignals(true);
            testPrefGetValueResultPipe_->setText(
                ok ? QStringLiteral("ok") : QStringLiteral("fail"));
            testPrefGetValueResultPipe_->blockSignals(false);
        }
        testPrefSetValuePipe_->blockSignals(true);
        testPrefSetValuePipe_->clear();
        testPrefSetValuePipe_->blockSignals(false);
    });

    // Accept (OK) — routes through the SAME accept() that the OK button
    // fires, so the production persistence path is exercised.
    testPrefAcceptPipe_ =
        makePipe("testPrefAcceptPipe", "Test Preferences Accept", false);
    connect(testPrefAcceptPipe_, &QLineEdit::textChanged,
            this, [this](const QString& text) {
        if (text.isEmpty()) return;
        testPrefAcceptPipe_->blockSignals(true);
        testPrefAcceptPipe_->clear();
        testPrefAcceptPipe_->blockSignals(false);
        accept();
    });

    // Reject (Cancel).
    testPrefRejectPipe_ =
        makePipe("testPrefRejectPipe", "Test Preferences Reject", false);
    connect(testPrefRejectPipe_, &QLineEdit::textChanged,
            this, [this](const QString& text) {
        if (text.isEmpty()) return;
        testPrefRejectPipe_->blockSignals(true);
        testPrefRejectPipe_->clear();
        testPrefRejectPipe_->blockSignals(false);
        reject();
    });
}

} // namespace pdfe::ui
