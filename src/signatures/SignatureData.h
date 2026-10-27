#pragma once

#include "DocMdpTypes.h"

#include <QRectF>
#include <string>
#include <vector>
#include <cstdint>

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
