#pragma once
#include <QWidget>
#include <vector>

class QTreeWidget;
class QPushButton;
class QLabel;
class QTreeWidgetItem;

namespace pdfe::signatures {
struct SignatureData;
}

namespace pdfe::ui {

class SignaturePanel : public QWidget {
    Q_OBJECT
public:
    explicit SignaturePanel(QWidget* parent = nullptr);
    ~SignaturePanel() override;

    void setSignatures(const std::vector<pdfe::signatures::SignatureData>& signatures);
    void clear();

    /// Pipeline F phase-10. Display the LTV verification status string
    /// (e.g. "Validated (LTV)" / "Validated (online)" / "REVOKED") in
    /// the panel summary. Called by MainWindow after running
    /// LtvValidator::verify() on the open document. Empty string clears
    /// the row.
    ///
    /// PHASE-50 (F52/F53), @p detail. LtvVerifyResult::message used to be
    /// computed and then thrown away — MainWindow passed only the status enum,
    /// so the one string that said WHICH rule fired never reached a user. The
    /// status alone cannot carry that: "Revocation evidence REJECTED" is the
    /// same words whether a rogue responder was caught or a CRL had expired.
    /// Pass LtvVerifyResult::message here. Empty @p detail clears the detail
    /// row and leaves the status row alone.
    void setLtvStatusText(const QString& statusText,
                          const QString& detail = QString());

    /// Pipeline f3c91a2e phase-7 (GAP-A14): read back the current LTV
    /// status label text for the result-pipe test seam. Returns the raw
    /// label text (e.g. "Validation: Validated (LTV)"); empty when no
    /// status has been computed. Lets the pywinauto LTV test assert the
    /// label content even though UIA cannot see the label on Server 2019.
    QString ltvStatusText() const;

    /// Phase-50 (F52/F53): read back the LTV detail row — the rendered
    /// LtvVerifyResult::message. Empty when no detail has been set. Exists so a
    /// test can prove the message reaches the UI rather than being discarded.
    QString ltvStatusDetailText() const;

signals:
    void signRequested(int pageIndex, int widgetIndex);
    void verifyAllRequested();
    void signatureSelected(int pageIndex, int widgetIndex);
    void viewDetailsRequested(int pageIndex, int widgetIndex);

private slots:
    void onTreeItemClicked(QTreeWidgetItem* item, int column);
    void onSignClicked();
    void onVerifyAllClicked();
    void onViewDetailsClicked();

private:
    QTreeWidget* signatureTreeWidget_ = nullptr;
    QPushButton* verifyAllButton_ = nullptr;
    QPushButton* signSelectedButton_ = nullptr;
    QPushButton* viewDetailsButton_ = nullptr;
    QLabel* signatureSummaryLabel_ = nullptr;
    /// Pipeline F phase-10: separate label for the LTV verification
    /// status. Distinct from the summary so the panel can show both
    /// "2 of 2 signed" and "Validation: LTV" without crowding either.
    QLabel* ltvStatusLabel_ = nullptr;
    /// Phase-50 (F52/F53): the detail row beneath the status row, carrying
    /// LtvVerifyResult::message — which rule refused what, or why the chain is
    /// not trusted. Separate label so the status row keeps its short,
    /// scannable form and the pywinauto seam that reads it is unaffected.
    QLabel* ltvStatusDetailLabel_ = nullptr;
};

} // namespace pdfe::ui
