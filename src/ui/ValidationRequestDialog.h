// -*- coding: utf-8 -*-
#pragma once

#include <QDialog>
#include <QStringList>

class QListWidget;
class QPushButton;
class QLabel;

namespace pdfe::ui {

/**
 * @brief Modal "are you sure?" dialog before any validation network fetch.
 *
 * Pipeline F phase-9. The user MUST explicitly accept this dialog before
 * any HTTP/HTTPS request leaves the process. The acceptance criteria
 * specifically forbid silent / automatic external network calls.
 *
 * Layout:
 *   - Header: "Request Validation Data Fetch" (warning icon).
 *   - Big bold security warning text describing what will be revealed
 *     (IP address, document open time) and to whom (the listed servers).
 *   - QListWidget of URLs, each prefixed "[OCSP] " or "[CRL] ".
 *   - Fetch and Cancel buttons. Cancel is the default (Esc, Enter both
 *     dismiss without contacting the network).
 *
 * If the URL list is empty the dialog still opens (so the user sees the
 * "no validation URLs available" state) but the Fetch button is disabled.
 *
 * objectName: "validationRequestDialog" (pywinauto target).
 */
class ValidationRequestDialog : public QDialog {
    Q_OBJECT
public:
    /// @param ocspUrls Pre-filtered http/https OCSP responder URLs.
    /// @param crlUrls  Pre-filtered http/https CRL DP URLs.
    ValidationRequestDialog(const QStringList& ocspUrls,
                            const QStringList& crlUrls,
                            QWidget* parent = nullptr);

    /// Combined list, in display order (OCSP entries first, then CRL).
    QStringList allUrls() const;

    /// Just OCSP responder URLs.
    QStringList ocspUrls() const;

    /// Just CRL URLs.
    QStringList crlUrls() const;

    /// True if the constructor argument lists were both empty.
    bool isUrlListEmpty() const;

    /// Pipeline f3c91a2e phase-7 (GAP-A14): the constructor records a
    /// pipe-delimited inventory of the dialog's real child widgets +
    /// URL count here when PDFE_TEST_PIPES is set, so a pywinauto test
    /// can assert the consent dialog opened with the privacy warning +
    /// URL list + Cancel/Fetch buttons via MainWindow's result pipe —
    /// even though UIA cannot enumerate the modal dialog on Windows
    /// Server 2019. Format:
    ///   "validationRequestDialog|warning=1|urlList=1|urlCount=<n>|"
    ///   "fetch=1|cancel=1|fetchEnabled=<0|1>".
    /// Empty until the dialog has been constructed at least once.
    static QString lastTestIntrospection();

private:
    QStringList ocspUrls_;
    QStringList crlUrls_;
    QListWidget* listWidget_ = nullptr;
    QPushButton* fetchButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QLabel* warningLabel_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
};

} // namespace pdfe::ui
