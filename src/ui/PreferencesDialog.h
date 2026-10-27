#pragma once
#include <QDialog>

class QTabWidget;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLineEdit;

namespace pdfe::app { class AppSettings; }

namespace pdfe::ui {

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(pdfe::app::AppSettings* settings, QWidget* parent = nullptr);
    ~PreferencesDialog() override = default;

private slots:
    void accept() override;

protected:
    // #50: shown on an OFF->ON transition of the JPEG2000 preference — warns
    // that enabling re-exposes the OpenJPEG heap-overflow/RCE risk on untrusted
    // files. Returns true iff the user confirms. virtual so headless tests can
    // override it (a real modal QMessageBox would wedge a headless run).
    virtual bool confirmEnableJpxRisk();

    // SEC-56 (#56): the same warn-once for the JBIG2 preference — enabling
    // re-exposes jbig2dec's memory-safety history on untrusted files. Returns
    // true iff the user confirms. virtual for the same headless-test reason.
    virtual bool confirmEnableJbig2Risk();

private:
    void loadSettings();

    // Pipeline phase-14-loop (Server-2019 UIA hang fix): test-only pipe
    // seam. On Windows-Server-2019 pywinauto's UIA descendants()/FindAll
    // blocks indefinitely when enumerating the widgets of an OPEN modal
    // QDialog, which wedges the whole pytest file (the thread-method
    // timeout cannot interrupt a native COM call). To drive the dialog
    // without UIA enumeration, these dialog-CHILD QLineEdit pipes let the
    // Python harness list widgets, read a widget's current value, set a
    // widget value, and accept/reject the dialog — all while the modal is
    // open. Dialog-child (not MainWindow-child) pipes are required because
    // Qt modality blocks UIA ValuePattern events to other top-levels.
    // Everything is gated by PDFE_TEST_PIPES (set only by the harness) and
    // is nullptr / never created in production.
    void setupTestPipes();
    QString getWidgetValue(const QString& objectName) const;
    bool setWidgetValue(const QString& objectName, const QString& value);
    QString widgetInventory() const;

    pdfe::app::AppSettings* settings_ = nullptr;
    QTabWidget* tabWidget_ = nullptr;

    // General tab
    QSpinBox* maxRecentFilesSpinBox_ = nullptr;

    // View tab
    QDoubleSpinBox* defaultZoomSpinBox_ = nullptr;
    QSpinBox* tileCacheSpinBox_ = nullptr;

    // Advanced tab
    QComboBox* logLevelCombo_ = nullptr;
    QCheckBox* logToFileCheck_ = nullptr;
    // GitHub #127 -- opt-in version check, off by default.
    QCheckBox* updateCheckCheck_ = nullptr;
    QLineEdit* logFilePathEdit_ = nullptr;

    // OCR tab
    QComboBox* ocrLanguageCombo_ = nullptr;

    // Security tab
    QCheckBox* javaScriptEnabledCheck_ = nullptr;
    QCheckBox* externalActionsAllowedCheck_ = nullptr;

    // GitHub #240 — network locations. Two controls, because they are two
    // different questions and one switch would conflate them.
    QCheckBox* allowUserNetworkPathsCheck_ = nullptr;
    QCheckBox* allowDocumentSameShareCheck_ = nullptr;
    QCheckBox* jpxRenderingEnabledCheck_ = nullptr;  // #50: JPEG2000 opt-in (default OFF)
    QCheckBox* jbig2RenderingEnabledCheck_ = nullptr;  // SEC-56: JBIG2 opt-in (default OFF)

    // ── Test-only pipe seams (gated by PDFE_TEST_PIPES; nullptr in prod) ──
    // Widget-inventory enumeration (objectName:accessibleName:kind triples).
    QLineEdit* testPrefWidgetNamesQueryPipe_  = nullptr; // "testPrefWidgetNamesQueryPipe"
    QLineEdit* testPrefWidgetNamesResultPipe_ = nullptr; // "testPrefWidgetNamesResultPipe"
    // Read a single widget's current value (query = objectName).
    QLineEdit* testPrefGetValueQueryPipe_     = nullptr; // "testPrefGetValueQueryPipe"
    QLineEdit* testPrefGetValueResultPipe_    = nullptr; // "testPrefGetValueResultPipe"
    // Set a single widget's value (write "objectName=value").
    QLineEdit* testPrefSetValuePipe_          = nullptr; // "testPrefSetValuePipe"
    // Accept (OK) / reject (Cancel) the dialog (write any non-empty token).
    QLineEdit* testPrefAcceptPipe_            = nullptr; // "testPrefAcceptPipe"
    QLineEdit* testPrefRejectPipe_            = nullptr; // "testPrefRejectPipe"
};

} // namespace pdfe::ui
