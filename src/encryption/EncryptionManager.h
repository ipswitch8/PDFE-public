#pragma once

#include "EncryptionSettings.h"
#include <memory>
#include <filesystem>

// Forward declarations for MuPDF C types (must be at global scope, with C linkage)
extern "C" {
struct pdf_document;
}

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::encryption {

/// Manager for PDF encryption operations.
///
/// Provides comprehensive encryption support:
/// - Query encryption status and permissions
/// - Password authentication
/// - Apply encryption with AES-256
/// - Remove encryption
class EncryptionManager {
public:
    explicit EncryptionManager(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~EncryptionManager();

    // Non-copyable
    EncryptionManager(const EncryptionManager&) = delete;
    EncryptionManager& operator=(const EncryptionManager&) = delete;

    // Non-movable (like RedactionManager)
    EncryptionManager(EncryptionManager&&) = delete;
    EncryptionManager& operator=(EncryptionManager&&) = delete;

    // --- Query encryption status ---

    /// Get detailed encryption information for a document.
    /// @param doc Document to query
    /// @return Encryption details including algorithm, version, permissions
    EncryptionInfo getEncryptionInfo(const pdfe::core::Document* doc) const;

    /// Get permission flags for a document.
    /// @param doc Document to query
    /// @return Current document permissions
    PermissionFlags getPermissions(const pdfe::core::Document* doc) const;

    /// Check if a document is encrypted.
    /// @param doc Document to check
    /// @return true if document has encryption
    bool isEncrypted(const pdfe::core::Document* doc) const;

    /// Check if a document needs a password to open.
    /// @param doc Document to check
    /// @return true if password is required
    bool needsPassword(const pdfe::core::Document* doc) const;

    // --- Password authentication ---

    /// Authenticate with a password.
    /// @param doc Document to authenticate
    /// @param password Password to try
    /// @return 0=fail, 1=user password, 2=owner password, 3=both
    int authenticatePassword(const pdfe::core::Document* doc, const std::string& password);

    // --- Encryption operations ---

    /// Save document with encryption applied.
    /// Uses a two-stage pipeline:
    /// 1. MuPDF saves to temp (unencrypted)
    /// 2. QPDF applies encryption and saves to output
    /// @param doc Document to encrypt
    /// @param outputPath Where to save encrypted PDF
    /// @param settings Encryption parameters (algorithm, passwords, permissions)
    /// @param report Optional out-parameter. When non-null it is filled with
    ///        the handler actually written and any permission restriction the
    ///        handler could not express (PDF-27). Never null-checked by the
    ///        caller path — passing nullptr keeps the pre-existing behaviour.
    void saveEncrypted(
        pdfe::core::Document* doc,
        const std::filesystem::path& outputPath,
        const EncryptionSettings& settings,
        EncryptionSaveReport* report = nullptr
    );

    /// PDF-27 — compute, WITHOUT writing anything, what the given settings will
    /// lose when they are applied. `saveEncrypted` fills its `report` with
    /// exactly this value, so a caller can preview the loss before committing
    /// (e.g. to warn in a dialog) and assert it afterwards.
    static EncryptionSaveReport previewPermissionMapping(
        const EncryptionSettings& settings
    );

    /// Remove encryption from a document.
    /// Simply saves the document without encryption.
    ///
    /// F26 — this is a FULL REWRITE (do_garbage + do_compress). If the source
    /// carries signatures or a certification, the written copy's `/ByteRange`
    /// offsets no longer describe the file and every signature on it becomes
    /// unverifiable. That is WARNED, never refused: see RemoveEncryptionReport.
    ///
    /// @param doc Document to decrypt
    /// @param outputPath Where to save decrypted PDF
    /// @param report Optional out-parameter. When non-null it is filled with
    ///        the rewrite-integrity finding for the SOURCE document. Passing
    ///        nullptr keeps the pre-existing behaviour; the warning is still
    ///        recorded in lastRemoveEncryptionWarning() so a caller with no
    ///        return channel can read it.
    void removeEncryption(
        const pdfe::core::Document* doc,
        const std::filesystem::path& outputPath,
        RemoveEncryptionReport* report = nullptr
    );

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// F26 — the most recent rewrite-integrity warning recorded on THIS thread by
/// removeEncryption(), or empty.
///
/// Thread-local, and cleared on every removeEncryption() entry, so a stale
/// warning can never be read as a fresh one. Exists for the same reason
/// pdfe::signatures::lastDirectWriteWarning() does: a caller that passes no
/// report (the two-argument overload, and every pre-F26 call site) must still
/// be ABLE to find out, rather than the finding evaporating.
const std::string& lastRemoveEncryptionWarning();

/// Clear the thread's recorded rewrite-integrity warning.
void clearLastRemoveEncryptionWarning();

} // namespace pdfe::encryption
