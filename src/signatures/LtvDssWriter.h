#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pdfe::core {
class Document;
}

namespace pdfe::signatures {

/// Inputs LtvDssWriter::write() needs about an existing signature in the
/// document: the /Contents bytes (binary + hex), the cert chain extracted
/// from the PKCS#7 SignedData, and the first signed byte range. Populated
/// by LtvDssWriter::extractFirstSignatureInputs().
struct SignatureInputs {
    std::string contentsHex;
    std::vector<uint8_t> contentsBin;
    std::vector<std::vector<uint8_t>> certChain;
    int64_t firstByteRangeOffset = 0;
    int64_t firstByteRangeLength = 0;
};

/// Snapshot of /Catalog/DSS contents, returned by
/// LtvDssWriter::inspectDss(). Used by both validation code and tests.
struct DssCounts {
    bool hasDss = false;
    /// True when /Root/DSS is an INDIRECT reference rather than a direct
    /// dictionary. The two shapes take structurally different rollback restore
    /// routes (pdf_update_object vs pdf_dict_puts), so tests need to be able to
    /// assert which one a fixture actually has instead of assuming it
    /// (meta-audit follow-up F57, phase-52).
    bool dssIsIndirect = false;
    int topCerts = 0;
    int topOcsps = 0;
    int topCrls = 0;
    bool hasVriKey = false;
    int vriCerts = 0;
    int vriOcsps = 0;
    int vriCrls = 0;
    bool vriHasOcspKey = false;
    bool vriHasCrlKey = false;
};

/**
 * @brief Writes a /DSS (Document Security Store) dictionary into a signed PDF,
 *        carrying long-term-validation (LTV) material.
 *
 * NOTE (meta-audit F20, gap PDF-34): this used to call itself the foundation
 * for an ETSI baseline profile. It is not — PDFE's signature is
 * `adbe.pkcs7.detached` and its
 * PKCS#7 container fails CAdES-BES, so no ETSI baseline level is reached.
 * /DSS is a real ISO 32000-2 §12.8.4.3 feature in its own right. Profile
 * support is stated in exactly one place: docs/signature-profile-support.md
 * (guarded by tests/test_pades_profiles.cpp).
 *
 * The /DSS dict is appended to /Catalog and contains:
 *   - /Certs : array of indirect refs to streams holding DER-encoded X.509 certs
 *   - /OCSPs: array of indirect refs to OCSP-response streams (optional)
 *   - /CRLs : array of indirect refs to CRL streams (optional)
 *   - /VRI  : sub-dict keyed by uppercase-hex SHA-1 of the signature's /Contents
 *             hex string, with per-signature /Cert, /OCSP, /CRL sub-arrays.
 *
 * The write is committed via an incremental save so the original signature
 * /ByteRange is preserved byte-for-byte. This is the invariant on which
 * post-signing LTV augmentation depends.
 *
 * Reference: ISO 32000-2 §12.8.4.3 (DSS dictionary), ETSI EN 319 142-1.
 */
class LtvDssWriter {
public:
    /// Write /DSS into @p doc (in-memory) and incrementally save back to its
    /// source file path.
    ///
    /// @param doc                  Open Document. Must have been loaded from a
    ///                             file path (saveIncremental requires it).
    /// @param signatureContentsHex Hex string from the signature's /Contents
    ///                             field — drives the /VRI key (uppercase SHA-1).
    /// @param certs                DER-encoded certs to embed in /Certs and the
    ///                             VRI /Cert sub-array.
    /// @param ocsps                OCSP response DERs (optional). Empty vector
    ///                             means "do not emit /OCSPs and /OCSP".
    /// @param crls                 CRL DERs (optional). Empty vector means
    ///                             "do not emit /CRLs and /CRL".
    /// @return true on success. False on any failure — and when it is false,
    ///         lastError() carries the reason. Before F57 the bool was the
    ///         ONLY channel, so a refusal to write and a catastrophic
    ///         un-rollbackable mutation were indistinguishable to the caller.
    bool write(pdfe::core::Document* doc,
               const std::string& signatureContentsHex,
               const std::vector<std::vector<uint8_t>>& certs,
               const std::vector<std::vector<uint8_t>>& ocsps = {},
               const std::vector<std::vector<uint8_t>>& crls = {});

    /// Why the last write()/writeWithFiles() returned false. Empty after a
    /// successful call. Cleared at the top of every call.
    ///
    /// Added by meta-audit follow-up F57 (phase-52). The failure this exists
    /// for is not an ordinary one: if the pre-mutation rollback snapshot
    /// cannot be taken, the /DSS write is refused OUTRIGHT, and the caller
    /// needs to be able to say so rather than reporting the same generic
    /// "failed to write /DSS" it reports for a network hiccup.
    const std::string& lastError() const { return lastError_; }

    /// The message write() reports when it refuses to proceed because the
    /// rollback snapshot of a pre-existing /DSS could not be taken. Named so
    /// tests can assert the SPECIFIC branch fired rather than merely that
    /// something failed.
    static const char* kRollbackCaptureFailedMessage;

    /// The message write() reports if a rollback is ever actually attempted
    /// and cannot be completed.
    ///
    /// WHAT IT DOES AND DOES NOT TELL YOU (corrected in the phase-53 review,
    /// finding 7.2). The phase-52 text here said "if it is ever seen in the
    /// field, the pre-mutation guard has been bypassed" — i.e. that this
    /// message is a decisive diagnostic for a specific ordering regression. It
    /// is not, and claiming so would send the next reader hunting for a
    /// non-existent bypass. F57's pre-mutation guard removes ONE route to this
    /// state (mutating without a usable snapshot); it does not remove the
    /// other, which needs no bypass at all: undo() runs rollbackDss() inside
    /// MuPdfContext::execute, and its `catch (const std::exception&) { restored
    /// = false; }` fires whenever that throws. Both pdf_update_object (the
    /// indirect-/DSS restore route) and pdf_dict_puts (the direct one) can
    /// fz_throw — on a damaged xref, on allocation failure — and a mid-rollback
    /// throw leaves exactly the half-restored state this message reports.
    ///
    /// So it means what it says and no more: the in-memory document is mutated
    /// and could not be put back, and the next save of it for ANY unrelated
    /// reason would publish /DSS material the caller was told was not written.
    /// That is worth reporting loudly and worth investigating; it is not by
    /// itself evidence of a code-ordering defect.
    static const char* kRollbackIncompleteMessage;

    /// The message write() reports when the incremental revision WAS published
    /// — the file on disk is correct and complete — but the Document could not
    /// be re-opened afterwards, so the in-memory object is no longer usable.
    ///
    /// Added by the phase-53 review (finding 6). Document::saveIncrementalStaged
    /// grew a `publishedButReopenFailed` out-parameter for exactly this case
    /// (F58-f) and write() ignored it, so a /DSS upgrade that had SUCCEEDED was
    /// reported to the user as a plain failure. On Windows the usual cause is
    /// benign and transient: an A/V scanner or the search indexer momentarily
    /// holding the just-renamed file. Telling the user "the LTV upgrade failed"
    /// there invites them to retry it — appending a second, redundant /DSS
    /// revision to a document that already has a correct one.
    ///
    /// write() still returns false, because the caller's Document is dead and
    /// must be treated as such. The distinction lives in lastError().
    ///
    /// PRECISION ABOUT WHAT WAS AND WAS NOT BROKEN, measured rather than
    /// assumed — the revert-and-observe run for this constant reported it. The
    /// finding was stated as "reports a plain failure for a revision that was
    /// actually published". The BRANCH was genuinely absent and the log line
    /// genuinely said "/DSS write failed" — but lastError() was never empty of
    /// the fact: it carried e.what(), and Document::saveIncrementalStaged's own
    /// exception text already begins "The incremental save WAS published
    /// successfully". So the information was present, as free prose owned by
    /// another layer. What did NOT exist, and is what this constant adds, is a
    /// distinct machine-checkable marker a caller or a test can key on without
    /// string-matching a core-layer sentence, a distinct log line, and an
    /// explicit statement that the correct response is to close and re-open
    /// rather than retry. Read it as the difference between an
    /// incidentally-informative string and a contract — not as evidence that
    /// the user previously saw nothing.
    static const char* kPublishedButReopenFailedMessage;

    // writeTo() removed in the phase-49 review remediation (finding M2): it
    // repeated, unrepaired, the exact rollback defect F5 fixed in write(); its
    // doc comment described behaviour it did not have ("a temp copy of the
    // source file" — it appended in place to outputPath); and it had no callers
    // anywhere in src/ or tests/. See the note at its former site in
    // LtvDssWriter.cpp for why it was deleted rather than repaired.

    /// Convenience overload: load OCSP and CRL DER blobs from local files
    /// (via OcspResponseParser and CrlParser), validate their structure, and
    /// then call write() with the parsed bytes.
    ///
    /// All file paths must point to DER-encoded artifacts on disk. If any
    /// file fails to parse, this returns false and no /DSS write is
    /// attempted — partial embedding would create a misleading LTV state.
    ///
    /// No network calls anywhere in the loading or parsing path; both
    /// parsers are strictly offline.
    bool writeWithFiles(
        pdfe::core::Document* doc,
        const std::string& signatureContentsHex,
        const std::vector<std::vector<uint8_t>>& certs,
        const std::vector<std::filesystem::path>& ocspFiles,
        const std::vector<std::filesystem::path>& crlFiles);

    /// Helper: compute uppercase-hex SHA-1 of a string. Public for tests.
    static std::string sha1HexUpper(const std::string& input);

    /// Walk the document, find the first signed signature widget, and
    /// extract the inputs that write() needs (Contents hex, cert chain,
    /// byte range). Returns false if the document has no signed signature
    /// or the chain extraction fails.
    ///
    /// Internally uses the Document's MuPdfContext and friend access to
    /// fz_document; production callers and tests both go through this
    /// helper rather than reaching into Document's private API.
    static bool extractFirstSignatureInputs(pdfe::core::Document* doc,
                                             SignatureInputs& out);

    /// Inspect /Catalog/DSS in @p doc, populating @p out with counts of
    /// /Certs, /OCSPs, /CRLs and the per-VRI sub-arrays for @p vriKey.
    /// Returns true if /Catalog/DSS exists; false otherwise (out.hasDss
    /// will also be false in that case).
    static bool inspectDss(pdfe::core::Document* doc,
                           const std::string& vriKey,
                           DssCounts& out);

private:
    std::string lastError_;
};

} // namespace pdfe::signatures
