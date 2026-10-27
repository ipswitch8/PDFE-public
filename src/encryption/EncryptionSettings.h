#pragma once

#include <string>
#include <vector>

namespace pdfe::encryption {

/// Encryption algorithm to use for PDF protection.
enum class EncryptionAlgorithm {
    None,       ///< No encryption
    RC4_40,     ///< RC4 40-bit encryption (PDF 1.1+)
    RC4_128,    ///< RC4 128-bit encryption (PDF 1.4+)
    AES_128,    ///< AES 128-bit encryption (PDF 1.6+)
    AES_256     ///< AES 256-bit encryption (PDF 2.0)
};

/// Permission flags for encrypted PDFs.
struct PermissionFlags {
    bool print = true;              ///< Allow printing
    bool printHighQuality = true;   ///< Allow high-quality printing
    bool modify = true;             ///< Allow document modification
    bool copy = true;               ///< Allow text/graphics copying
    bool annotate = true;           ///< Allow annotations and form fields
    bool formFill = true;           ///< Allow form field fill-in
    bool assemble = true;           ///< Allow document assembly (insert, rotate, delete pages)
    bool accessibility = true;      ///< Allow content extraction for accessibility

    /// MuPDF permission bit constants
    static inline constexpr int PDF_PERM_PRINT = 1 << 2;
    static inline constexpr int PDF_PERM_MODIFY = 1 << 3;
    static inline constexpr int PDF_PERM_COPY = 1 << 4;
    static inline constexpr int PDF_PERM_ANNOTATE = 1 << 5;
    static inline constexpr int PDF_PERM_FORM = 1 << 8;
    static inline constexpr int PDF_PERM_ACCESSIBILITY = 1 << 9;
    static inline constexpr int PDF_PERM_ASSEMBLE = 1 << 10;
    static inline constexpr int PDF_PERM_PRINT_HQ = 1 << 11;

    /// Convert permissions to MuPDF bitmask.
    int toBitmask() const {
        int mask = 0;
        if (print) mask |= PDF_PERM_PRINT;
        if (printHighQuality) mask |= PDF_PERM_PRINT_HQ;
        if (modify) mask |= PDF_PERM_MODIFY;
        if (copy) mask |= PDF_PERM_COPY;
        if (annotate) mask |= PDF_PERM_ANNOTATE;
        if (formFill) mask |= PDF_PERM_FORM;
        if (assemble) mask |= PDF_PERM_ASSEMBLE;
        if (accessibility) mask |= PDF_PERM_ACCESSIBILITY;
        return mask;
    }

    /// Create permissions from MuPDF bitmask.
    static PermissionFlags fromBitmask(int mask) {
        PermissionFlags flags;
        flags.print = (mask & PDF_PERM_PRINT) != 0;
        flags.printHighQuality = (mask & PDF_PERM_PRINT_HQ) != 0;
        flags.modify = (mask & PDF_PERM_MODIFY) != 0;
        flags.copy = (mask & PDF_PERM_COPY) != 0;
        flags.annotate = (mask & PDF_PERM_ANNOTATE) != 0;
        flags.formFill = (mask & PDF_PERM_FORM) != 0;
        flags.assemble = (mask & PDF_PERM_ASSEMBLE) != 0;
        flags.accessibility = (mask & PDF_PERM_ACCESSIBILITY) != 0;
        return flags;
    }

    /// All permissions enabled (default).
    static PermissionFlags all() {
        return PermissionFlags{};  // All fields default to true
    }

    /// Read-only permissions (only accessibility enabled).
    static PermissionFlags readOnly() {
        PermissionFlags flags;
        flags.print = false;
        flags.printHighQuality = false;
        flags.modify = false;
        flags.copy = false;
        flags.annotate = false;
        flags.formFill = false;
        flags.assemble = false;
        flags.accessibility = true;
        return flags;
    }

    /// Equality comparison.
    bool operator==(const PermissionFlags&) const = default;
};

/// Settings for PDF encryption.
struct EncryptionSettings {
    EncryptionAlgorithm algorithm = EncryptionAlgorithm::None;  ///< Encryption algorithm
    std::string userPassword;                                   ///< User password (empty = no user password)
    std::string ownerPassword;                                  ///< Owner password (required if algorithm != None)
    PermissionFlags permissions;                                ///< Document permissions
    bool encryptMetadata = true;                                ///< Whether to encrypt metadata

    /// Check if encryption is enabled.
    bool isEncrypted() const {
        return algorithm != EncryptionAlgorithm::None;
    }

    /// Check if user password is set.
    bool hasUserPassword() const {
        return !userPassword.empty();
    }

    /// Check if owner password is set.
    bool hasOwnerPassword() const {
        return !ownerPassword.empty();
    }

    /// F24 — "owner-only protection": encryption is on, an owner password
    /// restricts permissions, and the USER password is empty so the document
    /// opens with full read access and no prompt.
    ///
    /// This configuration is legal and often intentional (F3's disposition:
    /// report, do not refuse), which is precisely why it needs a warning rather
    /// than a block. What was inadequate was WHERE the warning surfaced — only
    /// inside the Document Permissions dialog, which nothing prompts the user
    /// to open, i.e. after the fact and out of sight. This predicate is the
    /// single condition, evaluated at the moment the settings are composed.
    bool isOwnerOnlyProtection() const {
        return isEncrypted() && !hasUserPassword() && hasOwnerPassword();
    }
};

/// F24 — the ONE wording of the owner-only-protection warning. Defined once so
/// the encryption dialog (point of setting), the save report (API callers) and
/// the permissions dialog (after the fact) cannot drift apart.
inline const char* emptyUserPasswordWarningText() {
    return "WARNING: the user password is EMPTY. Anyone can open this document "
           "and read it with no password at all — the owner password only "
           "restricts permissions, and most PDF viewers treat those "
           "permission flags as advisory. Set a user password if the content "
           "itself must be protected.";
}

/// Information about a PDF's current encryption.
struct EncryptionInfo {
    bool isEncrypted = false;               ///< Whether document is encrypted
    int version = 0;                        ///< V value from encryption dict (5 for AES-256)
    int revision = 0;                       ///< R value from encryption dict (6 for AES-256)
    int keyLength = 0;                      ///< Key length in bits (256 for AES-256)
    std::string method;                     ///< Encryption method ("AES-256", "AES-128", "RC4", "None")
    PermissionFlags permissions;            ///< Current document permissions
    bool metadataEncrypted = true;          ///< Whether metadata is encrypted
    int authLevel = 0;                      ///< Authentication level (0=none, 1=user, 2=owner)

    // --- Encryption / permission-handler security area (docs/security-policy.md) ---

    /// Crypt-filter method actually applied to STREAMS (`/StmF`), as reported by
    /// MuPDF: "None" (i.e. `/Identity` — streams are NOT encrypted), "RC4",
    /// "AES", or "Unknown". Empty when the document is not encrypted or the
    /// crypt dictionary could not be parsed. NOTE: MuPDF collapses AESV2
    /// (AES-128) and AESV3 (AES-256) to the single label "AES" — use
    /// `keyLength` to tell them apart. Only the "None" value is load-bearing
    /// for the unprotected-streams check below.
    std::string streamCryptMethod;

    /// Crypt-filter method actually applied to STRINGS (`/StrF`). Same value
    /// domain as streamCryptMethod.
    std::string stringCryptMethod;

    /// PDF-25 — true when the document carries an `/Encrypt` dictionary but its
    /// stream crypt filter is `/Identity`, so every stream (page content,
    /// images, embedded files) is stored in PLAINTEXT inside an apparently
    /// encrypted wrapper. `isEncrypted` is still true for such a file; this flag
    /// is what distinguishes "encrypted" from "actually protects its content".
    bool streamsUnprotected = false;

    /// True when the document's strings are stored with `/Identity` (plaintext).
    bool stringsUnprotected = false;

    /// Empty-user-password bypass — true when the EMPTY string authenticates as
    /// the user password, i.e. the document opens with full content access and
    /// no password prompt even though an owner password restricts permissions.
    /// Most viewers honour the permission bits only advisorily, so such a
    /// document is readable (and usually copyable/printable) by anyone.
    bool emptyUserPassword = false;
};

/// PDF-27 — report of information that an encryption handler could not express
/// when `EncryptionManager::saveEncrypted` applied the requested settings.
///
/// The RC4-40 (R2) handler defines only four permission bits — print, modify,
/// copy and annotate. The other four bits the API accepts (accessibility,
/// formFill, assemble, printHighQuality) have no representation at R2, so a
/// caller who *restricts* one of them gets a file that does not restrict it.
/// Without this report that loss is silent.
struct EncryptionSaveReport {
    /// Handler actually written, e.g. "RC4-40 (V1/R2)", "AES-256 (V5/R6)".
    std::string handlerWritten;

    /// Permission names the written handler cannot represent at all.
    std::vector<std::string> unrepresentablePermissions;

    /// Subset of `unrepresentablePermissions` the caller asked to RESTRICT
    /// (requested false) and which the written file therefore does NOT
    /// restrict. This is the security-relevant loss.
    std::vector<std::string> droppedRestrictions;

    /// True when `droppedRestrictions` is non-empty — a requested restriction
    /// was silently discarded by the chosen handler.
    bool lossyPermissionMapping = false;

    /// Human-readable one-line summary; empty when nothing was lost.
    std::string summary;

    /// F24 — true when the settings being applied are owner-only protection
    /// (see EncryptionSettings::isOwnerOnlyProtection). Reported here so a
    /// caller driving the encryption API directly, without the dialog, still
    /// gets the signal at the moment it writes the file.
    bool emptyUserPassword = false;

    /// The warning text to show when `emptyUserPassword` is true; empty
    /// otherwise. Always emptyUserPasswordWarningText().
    std::string emptyUserPasswordWarning;
};

/// F26 — what `EncryptionManager::removeEncryption`'s FULL REWRITE did to the
/// integrity of the source document it was pointed at.
///
/// removeEncryption writes with `do_garbage` + `do_compress`, which rebuilds
/// the file from the object graph. That destroys the incremental revision chain
/// and every byte offset in it — including the `/ByteRange` arrays that every
/// existing signature's PKCS#7 digest is computed over. Run against a signed or
/// certified source it silently produces a file no validating reader will
/// accept: the signature is still THERE, it just no longer verifies.
///
/// THE DISPOSITION IS WARN, NEVER REFUSE. Stripping a password is a security-
/// property change the user explicitly asked for, producing a NEW artifact at a
/// named output path. "Strip the password so I can archive this" is entirely
/// lawful and must keep working; refusing it would be a worse defect than the
/// silence it replaces. This report is how the caller finds out.
///
/// Mirrors the F24 direct-write guard's `allow`-with-`message` shape
/// (src/signatures/DocMdpDirectWriteGuard.h) rather than inventing a second
/// vocabulary for the same product decision.
struct RemoveEncryptionReport {
    /// True iff the SOURCE carried at least one signature dictionary with a
    /// non-empty `/Contents` (i.e. a signature that has actually been applied,
    /// not an empty signature field waiting to be filled).
    bool sourceSigned = false;

    /// True iff the SOURCE carried `/Root /Perms /DocMDP`, i.e. it claims to be
    /// certified.
    bool sourceCertified = false;

    /// Number of applied signatures found on the source. 0 when
    /// `sourceSigned` is false.
    int signatureCount = 0;

    /// True iff the rewrite invalidated something: the single predicate a
    /// caller tests. Exactly `sourceSigned || sourceCertified`.
    bool rewriteBreaksSignatures = false;

    /// The text to show when `rewriteBreaksSignatures` is true; empty
    /// otherwise. Names what was broken and why, so an unsigned, unprotected
    /// source produces NO message at all — an indiscriminate warning would be
    /// no better than silence.
    std::string rewriteIntegrityWarning;
};

} // namespace pdfe::encryption
