// -*- coding: utf-8 -*-
#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QNetworkAccessManager;
class QWidget;

namespace pdfe::core {
class Document;
}

namespace pdfe::signatures {

class ValidationFetcher;

/**
 * @brief Orchestrates the user-confirmed validation-data fetch flow.
 *
 * Pipeline F phase-9. Sequence:
 *
 *   1. Take the first verified signature in the document.
 *   2. Run OcspUrlExtractor + CrlUrlExtractor on the signer cert.
 *   3. Show ValidationRequestDialog with the URL list. If user cancels:
 *      emit cancelled() and stop. NO network calls.
 *   4. On accept: build OCSP request DER (cert + issuer), fire fetches
 *      via ValidationFetcher.
 *   5. On TLS / HTTP / parse failure: QMessageBox::critical, emit failed.
 *   6. On success: call LtvDssWriter::write() with the fetched bytes,
 *      QMessageBox::information("Validation data embedded"), emit completed.
 *
 * The class is single-shot: call request() exactly once. Re-using is
 * undefined.
 */
class ValidationCoordinator : public QObject {
    Q_OBJECT
public:
    explicit ValidationCoordinator(QObject* parent = nullptr);

    /// Test/DI variant: caller-provided NAM for ValidationFetcher.
    /// Caller retains ownership.
    ValidationCoordinator(QNetworkAccessManager* nam, QObject* parent);

    ~ValidationCoordinator() override;

    ValidationCoordinator(const ValidationCoordinator&) = delete;
    ValidationCoordinator& operator=(const ValidationCoordinator&) = delete;

    /// Drive the full flow, anchored to @p parent for modal dialogs.
    /// @return true if the flow began (a dialog was shown). The actual
    ///         result arrives later via cancelled / completed / failed.
    ///         Returns false (synchronously) on hard pre-conditions: no
    ///         document, no verified signature.
    bool request(QWidget* parent, pdfe::core::Document* doc);

    // -- Test seams --

    /// For tests: skip the dialog (caller already verified the URLs).
    /// Production code never calls this.
    void setSkipDialogForTests(bool skip);

    /// For tests: feed an explicit choice ("Fetch" or "Cancel") to the
    /// dialog without showing it. Used in headless GoogleTest.
    void setTestDialogResult(bool acceptIt);

    /// For tests: inject pre-fetched OCSP/CRL DER bytes instead of
    /// hitting the wire. The mock NAM still runs, so URL filtering and
    /// request construction can be exercised, but the bytes used for
    /// LtvDssWriter come from these setters.
    void setOverrideOcspDer(const QByteArray& der);
    void setOverrideCrlDer(const QByteArray& der);

    /// For tests: override the URLs that would have been extracted from
    /// the signer cert. Lets a test exercise the full mock-network path
    /// without needing a signed PDF whose signer cert happens to carry
    /// AIA / CRL DP extensions. Must be called BEFORE request().
    void setTestUrls(const QStringList& ocsp, const QStringList& crl);

    // Last URLs presented (for tests).
    QStringList lastOcspUrls() const;
    QStringList lastCrlUrls() const;

    /// F58 (phase-52) — let the owner run the /DSS write inside its own
    /// page-draining scope.
    ///
    /// WHY THIS EXISTS. Since F58 the /DSS write commits by staging the append
    /// and renaming the staged file over the original, and Windows cannot
    /// rename over a file MuPDF still has open — so LtvDssWriter::write() now
    /// releases the fz_document before the rename, exactly as Document::save()
    /// has always done. A live pdfe::core::Page keeps that fz_document alive
    /// (fz_keep_document), so a caller holding editing-tool or undo-command
    /// pages would make the rename fail. That is a REPORTED failure with the
    /// original left byte-intact — never data loss — but it is still a failure,
    /// and the owner is the only party that can drop and re-resolve its pages.
    ///
    /// MainWindow installs pdfe::core::saveWithCommandPageRebind's
    /// drain -> write -> rebind sequence here. The default simply invokes the
    /// write, so every non-UI caller (and every test) is unaffected.
    ///
    /// The hook MUST call @p write exactly once; anything it throws is treated
    /// as the write failing.
    void setWriteScope(std::function<void(const std::function<void()>& write)> scope);

    /// F58-g (phase-53 adversarial review, finding 5) — the owner's answer to
    /// "which Document do you have open RIGHT NOW?", consulted immediately
    /// before the deferred /DSS write.
    ///
    /// THE DEFECT THIS CLOSES. request() records the Document* and returns; the
    /// OCSP/CRL fetches are asynchronous and deliberately do not block the UI
    /// thread (see the security note above MainWindow::onRequestValidation).
    /// tryWriteDss() therefore runs an UNBOUNDED time later — after a slow or
    /// stalled responder, and after the user has had every opportunity to use
    /// File>Close or File>Open, neither of which is disabled while a fetch is in
    /// flight. Nothing noticed: the coordinator dereferenced its stored raw
    /// pointer, and MainWindow's write scope dereferenced `*document_` and
    /// `*undoManager_` on top of it. Closing the document mid-fetch was a
    /// use-after-free; replacing it was worse than a crash, because it would
    /// have written one document's revocation material into another's /DSS and
    /// reported success.
    ///
    /// WHAT THE RESOLVER MUST RETURN: the Document the owner currently has
    /// open, or nullptr if none. It is NOT asked to compare anything — the
    /// coordinator does the identity check itself, against the file path and
    /// the first signature's /Contents recorded at request() time. That is
    /// deliberate: raw-pointer equality is neither necessary (a Document
    /// re-opened at a new address holding the same file and the same signature
    /// is the right target) nor sufficient (the allocator can hand a DIFFERENT
    /// document the address the destroyed one had), so a pointer comparison is
    /// exactly the check that would have looked right and been wrong.
    ///
    /// With no resolver installed the identity gate is skipped and the write
    /// runs against the recorded pointer, i.e. today's behaviour. A caller that
    /// does not install one is asserting that it owns the Document
    /// synchronously across the whole flow — true of every non-UI caller and
    /// every test, and NOT true of MainWindow, which installs one.
    void setLiveDocumentResolver(std::function<pdfe::core::Document*()> resolver);

    /// Reported via failed() when the resolver says no document is open any
    /// more. Named so a test can assert THIS branch fired rather than merely
    /// that something failed.
    static const char* kDocumentNoLongerOpenMessage;

    /// Reported via failed() when a document IS open but is not the one the
    /// request was made against (different file path, or a different first
    /// signature). The material fetched for one document must never be written
    /// into another.
    static const char* kDocumentReplacedMessage;

signals:
    void cancelled();
    void completed();
    void failed(QString reason);

private slots:
    void onOcspReceived(QByteArray der);
    void onCrlReceived(QByteArray der);
    void onFetchFailed(QString reason);

private:
    bool extractFirstSignerCertChain(pdfe::core::Document* doc,
                                     std::vector<std::vector<uint8_t>>& chain,
                                     std::string& contentsHex);

    QByteArray buildOcspRequestDer(const std::vector<uint8_t>& signerDer,
                                    const std::vector<uint8_t>& issuerDer,
                                    QString& errorOut);

    void tryWriteDss();
    void emitFailed(QString reason);
    void cleanup();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::signatures
