#pragma once

#include "DocMdpTypes.h"

#include <QRectF>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// GitHub #374 — forward declaration only: SigningOptions carries a
// shared_ptr<QTemporaryFile> keepalive, and a shared_ptr member is complete
// without the pointee's definition (the deleter is captured where the file is
// actually constructed, in SignDialog.cpp). Keeps <QTemporaryFile> out of the
// pure signing engines that include this header.
class QTemporaryFile;

namespace pdfe::signatures {

/// Identity information extracted from a signing certificate.
struct SignerIdentity {
    std::string commonName;
    std::string organization;
    std::string orgUnit;
    std::string email;
    std::string country;
    std::string formattedName;

    bool isEmpty() const {
        return commonName.empty() && organization.empty() &&
               orgUnit.empty() && email.empty() && country.empty();
    }

    bool operator==(const SignerIdentity&) const = default;
};

/// Signature verification error codes (mirrors MuPDF pdf_signature_error).
enum class SignatureError {
    Okay,
    NoSignatures,
    NoCertificate,
    DigestFailure,
    SelfSigned,
    SelfSignedInChain,
    NotTrusted,
    NotSigned,
    Unknown
};

/// Convert SignatureError to human-readable string.
inline std::string signatureErrorToString(SignatureError err) {
    switch (err) {
    case SignatureError::Okay:               return "OK";
    case SignatureError::NoSignatures:       return "No signatures";
    case SignatureError::NoCertificate:      return "No certificate";
    case SignatureError::DigestFailure:      return "Digest failure";
    case SignatureError::SelfSigned:         return "Self-signed certificate";
    case SignatureError::SelfSignedInChain:  return "Self-signed certificate in chain";
    case SignatureError::NotTrusted:         return "Certificate not trusted";
    case SignatureError::NotSigned:          return "Not signed";
    case SignatureError::Unknown:            return "Unknown error";
    }
    return "Unknown error";
}

/// Map from MuPDF pdf_signature_error integer to our enum.
inline SignatureError signatureErrorFromMuPdf(int muPdfError) {
    switch (muPdfError) {
    case 0: return SignatureError::Okay;
    case 1: return SignatureError::NoSignatures;
    case 2: return SignatureError::NoCertificate;
    case 3: return SignatureError::DigestFailure;
    case 4: return SignatureError::SelfSigned;
    case 5: return SignatureError::SelfSignedInChain;
    case 6: return SignatureError::NotTrusted;
    case 7: return SignatureError::NotSigned;
    default: return SignatureError::Unknown;
    }
}

/// Result of verifying a single signature.
struct SignatureVerifyResult {
    SignatureError digestStatus = SignatureError::Unknown;
    SignatureError certificateStatus = SignatureError::Unknown;
    bool digestValid = false;
    bool certificateValid = false;
    bool modifiedAfterSigning = false;
    std::string digestMessage;
    std::string certificateMessage;

    bool isFullyValid() const {
        return digestValid && certificateValid && !modifiedAfterSigning;
    }

    bool operator==(const SignatureVerifyResult&) const = default;
};

/// Data for a single signature field/widget.
struct SignatureData {
    int pageIndex = -1;
    int widgetIndex = -1;
    std::string fieldName;
    QRectF rect;
    bool isSigned = false;
    SignerIdentity signer;
    std::string reason;
    std::string location;
    std::string signingTimeStr;
    int64_t signingTime = 0;
    SignatureVerifyResult verification;
};

// CertificationLevel now lives in the Qt-free DocMdpTypes.h (F24 remediation)
// so the pure-MuPDF engines can name it without pulling <QRectF> — and with it
// Qt's `emit` macro — into their translation units. Included below; every
// existing user of SignatureData.h still sees the name unchanged.

/// FieldMDP (`/Lock`, ISO 32000-1 §12.7.4.5 Table 233) — which form fields a
/// signature locks against further change. `None` writes no `/Lock` at all,
/// which is what PDFE did before F21 and is still the default.
enum class FieldLockAction {
    None = 0,   ///< No /Lock dictionary is written.
    All,        ///< /Action /All — every field in the document is locked.
    Include,    ///< /Action /Include — only the named fields are locked.
    Exclude,    ///< /Action /Exclude — every field EXCEPT the named ones.
};

/// Options for signing a signature field.
struct SigningOptions {
    std::string pfxPath;
    std::string pfxPassword;
    std::string reason;
    std::string location;

    /// GitHub #327 — path to an image file (PNG/JPEG/…) to use as the
    /// VISIBLE signature graphic. When non-empty and the signature is
    /// visible, it is loaded into an `fz_image*` and passed to
    /// `pdf_sign_signature` as the appearance graphic; the text fields
    /// (name/date/reason/DN per `appearanceFlags`) still compose on top.
    /// Empty means the text-only appearance PDFE produced before #327.
    std::string imagePath;

    /// GitHub #374 — when `imagePath` points at a temp file materialised from
    /// the SignatureImageLibrary (SignDialog::onSelectSavedImage), this keeps
    /// that file alive for as long as the options object itself lives. The
    /// dialog is destroyed BEFORE SignatureManager::signField reads
    /// `imagePath` (promptSigningOptions returns the options by value and the
    /// dialog leaves scope), and the QTemporaryFile's autoRemove used to
    /// delete the PNG with it — so signing with a library image always threw
    /// post-reopen. autoRemove semantics are preserved: when the last
    /// SigningOptions copy goes away, the temp file is deleted (no litter).
    /// Empty for a browsed on-disk image; the signing engines never read it.
    std::shared_ptr<QTemporaryFile> imageKeepalive;
    int appearanceFlags = 0x3F; // PDF_SIGNATURE_DEFAULT_APPEARANCE
    bool useTimestamp = false;
    std::string tsaUrl;

    /// Suppress the visible appearance stream so the signature is
    /// present in the /AcroForm but invisible on-page. Combine with a
    /// zero-area field rect for a truly invisible signature.
    bool invisible = false;

    /// If != NotCertification, stamp a /Perms /DocMDP dict so the
    /// signature certifies the document and restricts future changes
    /// to the specified level. A document can have at most one
    /// certification signature per PDF spec; the signer is responsible
    /// for ensuring this.
    CertificationLevel certification = CertificationLevel::NotCertification;

    /// FieldMDP: write a `/Lock` dictionary on the signature FIELD so that
    /// readers refuse post-signature changes to the named form fields.
    /// Written BEFORE `pdf_sign_signature` plans the byte-range layout, and
    /// onto the field dictionary rather than the signature value dictionary
    /// `/V` — modifying `/V` after planning corrupts the emitted container
    /// (measured, F20 mutants M10/M13).
    FieldLockAction fieldLockAction = FieldLockAction::None;

    /// Field names for `/Lock /Fields`. Ignored when `fieldLockAction` is
    /// `None` or `All`.
    std::vector<std::string> lockedFields;

    /// GitHub #370 — placement for a signature field CREATED by this signing
    /// flow. Before this, the field rect was hard-coded QRectF(50,50,200,60)
    /// at the MainWindow call site and neither SignDialog nor SigningOptions
    /// carried any geometry. `fieldRect` is in fitz space (Y-down page
    /// points), the same space the hard-coded rect used and the space MuPDF
    /// rasterises the appearance into (the widget rect IS the appearance
    /// geometry). Ignored when signing an existing field —
    /// `hasFieldGeometry` stays false there.
    bool hasFieldGeometry = false;
    QRectF fieldRect;
    int fieldPageIndex = -1;
};

/// Summary of all signatures in a document.
struct SignatureSummary {
    int totalCount = 0;
    int signedCount = 0;
    int unsignedCount = 0;
    int validCount = 0;
    int invalidCount = 0;
    std::vector<SignatureData> signatures;
};

} // namespace pdfe::signatures
