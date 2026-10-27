#pragma once

#include "DocMdpEnforcer.h"
#include "SignatureCoverageAnalyzer.h"
#include "SignatureData.h"
#include <QImage>
#include <filesystem>
#include <memory>

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
}

namespace pdfe::signatures {

class Pkcs7OpenSslBridge;

/**
 * @brief Manages digital signature operations on PDF documents.
 *
 * Handles signature field creation/removal, signing with PFX certificates,
 * verification, and incremental save to preserve signature validity.
 *
 * Uses the pimpl pattern. Must be created with a valid MuPdfContext.
 * Requires friend access to Document and Page for internal MuPDF operations.
 */
class SignatureManager {
public:
    explicit SignatureManager(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~SignatureManager();

    // Non-copyable, non-movable
    SignatureManager(const SignatureManager&) = delete;
    SignatureManager& operator=(const SignatureManager&) = delete;

    // -- Field creation (undoable via commands) --

    /// Create a new signature field on the specified page.
    SignatureData createSignatureField(const pdfe::core::Page* page,
                                       const std::string& name, QRectF rect);

    /// Remove a signature field by widget index. Returns removed data for undo.
    SignatureData removeSignatureField(const pdfe::core::Page* page, int widgetIndex);

    /// Restore a previously removed signature field. Returns new widget index.
    int restoreSignatureField(const pdfe::core::Page* page, const SignatureData& data);

    // -- Signing (NOT undoable) --

    /// Sign a signature field with a PFX certificate.
    void signField(const pdfe::core::Document* doc, const pdfe::core::Page* page,
                   int widgetIndex, const SigningOptions& opts);

    /// Clear an existing signature from a field.
    void clearSignature(const pdfe::core::Page* page, int widgetIndex);

    // -- Verification --

    /// Verify a single signature by widget index.
    SignatureVerifyResult verifySignature(const pdfe::core::Document* doc,
                                          const pdfe::core::Page* page, int widgetIndex);

    /// Verify all signatures in the document.
    SignatureSummary verifyAllSignatures(pdfe::core::Document* doc);

    /// PDF-31 / PDF-32 — DocMDP enforcement ON READ.
    ///
    /// verifySignature() answers "is the PKCS#7 digest intact"; an incremental
    /// update leaves that answer YES by design, so it cannot tell a caller
    /// whether a certification was honoured. This does: it reads
    /// /Root /Perms /DocMDP, classifies every object revised after the
    /// certifying signature, and judges each class against the /P level.
    /// `DocMdpStatus::violated` is the answer to "is this certification broken
    /// by what I just did".
    ///
    /// Evaluate a document as read from disk; a document with unsaved in-memory
    /// edits reports them via DocMdpStatus::unsavedVersions.
    DocMdpStatus checkDocMdp(const pdfe::core::Document* doc) const;

    /// PDF-36 / PDF-37 — does the signature COVER the file?
    ///
    /// verifySignature() answers "does the PKCS#7 digest over the byte ranges
    /// the file states check out". The attacker chooses those ranges, so the
    /// answer is yes for an incremental-update shadow attack and yes for a
    /// re-signed wrapping attack. This reports what the ranges leave OUT:
    /// bytes appended after the signed range, and unsigned gaps in the middle
    /// of the file that are not the /Contents hole.
    ///
    /// Takes a path rather than a Document because the question is about bytes
    /// on disk; a Document with unsaved edits cannot answer it.
    DocumentCoverage analyzeSignatureCoverage(
        const std::filesystem::path& path) const;

    // -- Query --

    /// Count total signature fields in the document.
    int countSignatures(const pdfe::core::Document* doc) const;

    /// List all signature fields with their data.
    std::vector<SignatureData> listSignatures(pdfe::core::Document* doc);

    /// Get detailed data for a specific signature widget.
    SignatureData getSignatureDetails(const pdfe::core::Document* doc,
                                      const pdfe::core::Page* page, int widgetIndex);

    // -- Save --
    //
    // saveIncremental() removed in the phase-53 adversarial-review remediation
    // (finding 3), on the same grounds phase-49 removed LtvDssWriter::writeTo()
    // (finding M2): dead public API that looks maintained is what the next
    // caller reaches for.
    //
    // WHAT IT WAS. A second, independent
    // pdf_save_document(do_incremental=1, do_compress=1) implementation writing
    // straight to a caller-supplied path, with NO staging and NO certification
    // check — neither Document's runSaveGuard nor
    // DocMdpDirectWriteGuard::directWriteRefused, the two mechanisms the
    // product uses for exactly this. Pointed at a certified document's own
    // path it appended in place and destroyed nothing but the /DocMDP
    // guarantee, silently; pointed anywhere at all, a mid-write failure landed
    // on the target with no rollback, which is the whole class of defect F58
    // was raised to close for the /DSS append.
    //
    // WHY IT WENT RATHER THAN BEING HARDENED. It had zero callers in src/. The
    // only three call sites in the whole tree were tests, i.e. the tests were
    // the sole thing keeping it alive — and hardening it would have meant
    // maintaining a SECOND staged-incremental-save implementation beside
    // Document::saveIncrementalStaged(), which is audited, guarded, and the one
    // the product actually uses. Two implementations of a durable write is the
    // condition that produced this finding in the first place.
    //
    // THE REPLACEMENT is pdfe::core::Document::saveIncrementalStaged(): same
    // byte-range-preserving incremental append, plus the DocMDP save guard,
    // plus staging behind an exclusive handle and publication by atomic rename.
    // It targets the document's own filePath() by design — an incremental
    // revision only makes sense on top of the bytes it was computed against.
    // The three tests were re-pointed at it (tests/test_signature_manager.cpp
    // Test 20, tests/test_security_phase8.cpp UI-SIG-12); Test 24 tested only
    // this function's own null check and went with it.

    // -- Preview --

    /// Generate a preview of the signature appearance.
    QImage previewSignatureAppearance(int w, int h, const SigningOptions& opts);

    // -- Bridge status --

    /// Check if the PKCS7 bridge is available.
    bool isPkcs7BridgeAvailable() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::signatures
