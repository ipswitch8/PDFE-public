#include "LtvDssWriter.h"

#include "Pkcs7OpenSslBridge.h"
#include "OcspResponseParser.h"
#include "CrlParser.h"
#include "OcspCrlData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/Error.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <openssl/evp.h>

#include <QLoggingCategory>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace pdfe::signatures {

namespace {
Q_LOGGING_CATEGORY(lcDss, "pdfe.signatures.dss")

// Uppercase hex of one byte into a 2-char buffer (no null terminator written).
inline void byteToHexUpper(uint8_t b, char* out)
{
    static const char* k = "0123456789ABCDEF";
    out[0] = k[(b >> 4) & 0xF];
    out[1] = k[b & 0xF];
}

// Wrap a DER blob as a PDF stream object and add it to the document. The
// returned pdf_obj* is an indirect reference (already added). Ownership of
// the returned ref is the caller's; the new dict that backs the stream is
// internal to MuPDF.
//
// MUST be called from inside MuPdfContext::execute (i.e. fz_try scope).
pdf_obj* addCertStream(fz_context* ctx, pdf_document* pdoc,
                       const uint8_t* der, size_t derLen)
{
    fz_buffer* buf = fz_new_buffer_from_copied_data(ctx, der, derLen);
    pdf_obj* streamRef = nullptr;
    fz_try(ctx)
    {
        streamRef = pdf_add_stream(ctx, pdoc, buf, nullptr, 0);
    }
    fz_always(ctx)
    {
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx)
    {
        fz_rethrow(ctx);
    }
    return streamRef;
}

// Build a /Certs (or /OCSPs / /CRLs) array of indirect stream refs from a
// vector of DER blobs. Returns the new array object (not yet inserted into
// any parent dict).
pdf_obj* buildStreamRefArray(fz_context* ctx, pdf_document* pdoc,
                              const std::vector<std::vector<uint8_t>>& blobs)
{
    pdf_obj* arr = pdf_new_array(ctx, pdoc, static_cast<int>(blobs.size()));
    fz_try(ctx)
    {
        for (const auto& blob : blobs) {
            if (blob.empty()) continue;
            pdf_obj* streamRef = addCertStream(ctx, pdoc,
                                                blob.data(), blob.size());
            pdf_array_push(ctx, arr, streamRef);
            pdf_drop_obj(ctx, streamRef);
        }
    }
    fz_catch(ctx)
    {
        pdf_drop_obj(ctx, arr);
        fz_rethrow(ctx);
    }
    return arr;
}

// Core /DSS write logic. Mutates the in-memory pdf_document; caller must
// follow up with an incremental save. Runs inside MuPdfContext::execute.
void writeDssDict(fz_context* ctx, pdf_document* pdoc,
                  const std::string& vriKey,
                  const std::vector<std::vector<uint8_t>>& certs,
                  const std::vector<std::vector<uint8_t>>& ocsps,
                  const std::vector<std::vector<uint8_t>>& crls)
{
    pdf_obj* trailer = pdf_trailer(ctx, pdoc);
    pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
    if (!root) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "Document has no /Root");
    }

    // Look up or create /Catalog/DSS.
    pdf_obj* dss = pdf_dict_gets(ctx, root, "DSS");
    if (!dss) {
        dss = pdf_dict_puts_dict(ctx, root, "DSS", 4);
    }

    // Build /Certs (top-level union of all certs we know about). If a /Certs
    // already exists from a prior LTV update, append to it rather than
    // replacing — multiple signatures share one /DSS.
    if (!certs.empty()) {
        pdf_obj* existing = pdf_dict_gets(ctx, dss, "Certs");
        pdf_obj* newCerts = buildStreamRefArray(ctx, pdoc, certs);
        if (existing && pdf_is_array(ctx, existing)) {
            int n = pdf_array_len(ctx, newCerts);
            for (int i = 0; i < n; ++i) {
                pdf_obj* item = pdf_array_get(ctx, newCerts, i);
                pdf_array_push(ctx, existing, item);
            }
            pdf_drop_obj(ctx, newCerts);
        } else {
            pdf_dict_puts_drop(ctx, dss, "Certs", newCerts);
        }
    }

    if (!ocsps.empty()) {
        pdf_obj* existing = pdf_dict_gets(ctx, dss, "OCSPs");
        pdf_obj* newOcsps = buildStreamRefArray(ctx, pdoc, ocsps);
        if (existing && pdf_is_array(ctx, existing)) {
            int n = pdf_array_len(ctx, newOcsps);
            for (int i = 0; i < n; ++i) {
                pdf_obj* item = pdf_array_get(ctx, newOcsps, i);
                pdf_array_push(ctx, existing, item);
            }
            pdf_drop_obj(ctx, newOcsps);
        } else {
            pdf_dict_puts_drop(ctx, dss, "OCSPs", newOcsps);
        }
    }

    if (!crls.empty()) {
        pdf_obj* existing = pdf_dict_gets(ctx, dss, "CRLs");
        pdf_obj* newCrls = buildStreamRefArray(ctx, pdoc, crls);
        if (existing && pdf_is_array(ctx, existing)) {
            int n = pdf_array_len(ctx, newCrls);
            for (int i = 0; i < n; ++i) {
                pdf_obj* item = pdf_array_get(ctx, newCrls, i);
                pdf_array_push(ctx, existing, item);
            }
            pdf_drop_obj(ctx, newCrls);
        } else {
            pdf_dict_puts_drop(ctx, dss, "CRLs", newCrls);
        }
    }

    // /VRI sub-dict keyed by SHA-1(Contents hex) → uppercase hex. ISO 32000-2
    // is silent on case; both Adobe and ETSI EN 319 142 reference uppercase.
    pdf_obj* vri = pdf_dict_gets(ctx, dss, "VRI");
    if (!vri) {
        vri = pdf_dict_puts_dict(ctx, dss, "VRI", 1);
    }

    // Each VRI entry is itself a dict with /Cert, /OCSP, /CRL arrays of
    // stream indirect refs. We write fresh streams per VRI entry so that
    // /Certs (top-level) and /VRI/<key>/Cert can each be parsed standalone.
    pdf_obj* vriEntry = pdf_dict_puts_dict(ctx, vri, vriKey.c_str(), 4);

    if (!certs.empty()) {
        pdf_obj* arr = buildStreamRefArray(ctx, pdoc, certs);
        pdf_dict_puts_drop(ctx, vriEntry, "Cert", arr);
    }
    if (!ocsps.empty()) {
        pdf_obj* arr = buildStreamRefArray(ctx, pdoc, ocsps);
        pdf_dict_puts_drop(ctx, vriEntry, "OCSP", arr);
    }
    if (!crls.empty()) {
        pdf_obj* arr = buildStreamRefArray(ctx, pdoc, crls);
        pdf_dict_puts_drop(ctx, vriEntry, "CRL", arr);
    }
}

// ---------------------------------------------------------------------------
// Rollback for an ABANDONED /DSS write  (meta-audit follow-up F47 item F5,
// phase-49).
//
// THE DEFECT THIS EXISTS TO CLOSE. write() mutates the in-memory document
// FIRST and saves SECOND. When the save failed it returned false — and left
// the /DSS objects sitting in the open document. Any later save of that same
// document, for any unrelated reason, then published revocation material the
// caller had been explicitly told was NOT written. The error was reported and
// the effect happened anyway, which is the worst of both: the UI says the LTV
// upgrade failed while the file on disk says it succeeded.
//
// It matters more here than it would on an ordinary edit because this save is
// an INCREMENTAL APPEND.
//
// F58 (phase-52) UPDATE: that append no longer lands on the live file. It is
// staged to core::StagingFile's "<target>.pdfe_part.<128-bit CSPRNG hex>" — the
// phase-52 text said "<target>.pdfe_tmp", which the F58-a hardening replaced
// (see core/AtomicFileReplace.h) — and published by an atomic rename, so a failed
// save cannot damage the user's copy. The rollback is still required and still
// does the same job — staging protects the BYTES ON DISK, and the defect this
// rollback closes is about the OBJECTS LEFT IN MEMORY, which a rename cannot
// touch. The two are orthogonal: without the rollback a failed staged save
// still leaves /DSS objects loaded for the next unrelated save to publish.
//
// Measured by DssSaveFaultInjectionTest.FailedDssWriteIsNotPublishedByALaterSave
// (tests/test_dss_save_fault_injection.cpp), which was RED before this.
//
// HOW THE ROLLBACK IS COMPLETE. Everything writeDssDict() does is one of:
//   (a) creating new indirect objects (the cert/OCSP/CRL streams), which are
//       necessarily numbered at or above the xref length observed before the
//       operation started — so deleting that numeric range removes exactly the
//       objects this operation created and nothing else; and
//   (b) mutating /Root/DSS, either by installing a fresh dict or by appending
//       to the arrays of one that was already there — undone by restoring a
//       deep copy taken before the mutation, or by removing the key outright
//       when there was no /DSS to begin with.
// The capture must therefore happen BEFORE writeDssDict runs, and the rollback
// must run when writeDssDict ITSELF throws, not only when the save does: a
// half-applied /DSS is exactly as publishable as a whole one.
//
// F57 (phase-52) — THE ORIGINAL DEFECT SURVIVED INSIDE ITS OWN FIX.
//
// captureDssRollback() used to return void, and rollbackDss() used to read:
//
//     if (!rb.hadDss)        { ...delete the /DSS we added... }
//     else if (rb.backup)    { ...restore the saved copy... }
//     // <- no else
//
// There is a third state. A document that DID have a /DSS whose
// pdf_deep_copy_obj() came back null takes NEITHER branch: the mutated /DSS is
// left in place and the caller is told the rollback succeeded. That is F5
// exactly — a failed save leaving a silently mutated document — surviving
// inside the very code written to close it. The `void` return is what made it
// possible to write: with no failure channel out of the capture, a null backup
// was indistinguishable from "there was no /DSS to copy", which is the one
// case where a null backup is CORRECT.
//
// That null is reachable without inventing anything: pdf_resolve_indirect()
// returns NULL (not PDF_NULL) when the referenced object cannot be loaded —
// see pdf_cache_object's error path in pdf-xref.c — and pdf_deep_copy_obj(NULL)
// returns NULL because NULL < PDF_LIMIT. An Acrobat-shaped document with an
// indirect /Root/DSS whose target got damaged lands there.
//
// WHY THE FIX IS AN ORDERING CHANGE AND NOT A THIRD BRANCH.
// The invariant is: IF YOU CANNOT ROLL BACK, YOU MUST NOT HAVE MUTATED. There
// is no honest third branch for rollbackDss() to take — by the time it runs,
// the /DSS has already been mutated and the only thing that could put it back
// is the snapshot that does not exist. Repairing the symptom there would leave
// the caller a choice between two bad outcomes. So the check moves EARLIER:
// captureDssRollback() now reports failure, and write() refuses to call
// writeDssDict() at all when it does. Nothing is mutated, so nothing needs
// undoing, and the failure is reported before it can become damage.
//
// rollbackDss() still gains the missing branch, but only as a fail-closed
// backstop: it now returns false rather than silently claiming success, and
// write() surfaces that as kRollbackIncompleteMessage. With the pre-mutation
// guard in place that path is unreachable; it exists so that if a future edit
// reintroduces a mutation-before-capture ordering, the result is a loud error
// rather than the silent republication F5 and F57 are both about.
struct DssRollback {
    int preXrefLen = 0;
    bool hadDss = false;
    bool dssWasIndirect = false;
    int dssObjNum = 0;
    pdf_obj* backup = nullptr;  // owned deep copy of the previous /DSS
};

// TEST-ONLY FAULT GATE (F57 control 1).
//
// pdf_deep_copy_obj() returning null for an OTHERWISE HEALTHY /DSS is the state
// that makes the defect observable, and it is the one state no hostile document
// can produce: every document that drives the copy to null (a /Root/DSS whose
// indirect target will not load) also makes writeDssDict() throw before it
// mutates anything, so the mutation never happens and there is nothing left
// behind to see. A fixture alone therefore CANNOT red the two-branch form.
//
// This gate exists to separate those two things. It is read from the
// environment — the same idiom the UI test pipes use (PDFE_TEST_PIPES) — so it
// is inert in production, needs no production API surface, and cannot be left
// armed by a stray test that forgot to reset a global.
//
// It deliberately fires AFTER the real copy and drops the result, so the copy
// itself is genuinely exercised and the only thing simulated is its return
// value.
bool dssRollbackCaptureFaultArmed()
{
#ifdef _MSC_VER
    size_t len = 0;
    char buf[8]{};
    if (getenv_s(&len, buf, sizeof(buf),
                 "PDFE_TEST_DSS_ROLLBACK_CAPTURE_FAULT") != 0)
        return false;
    return len > 0 && buf[0] != '\0' && buf[0] != '0';
#else
    const char* v = std::getenv("PDFE_TEST_DSS_ROLLBACK_CAPTURE_FAULT");
    return v && v[0] != '\0' && v[0] != '0';
#endif
}

/// @return false when the document HAS a /DSS whose pre-mutation snapshot
///         could not be taken. The caller must not mutate in that case.
///         True when the snapshot was taken, AND when there is nothing to
///         snapshot (no /Root, or no /DSS) — those are undoable by removing
///         the key, which needs no backup.
[[nodiscard]] bool captureDssRollback(fz_context* ctx, pdf_document* pdoc,
                                      DssRollback& rb)
{
    rb.preXrefLen = pdf_xref_len(ctx, pdoc);
    pdf_obj* trailer = pdf_trailer(ctx, pdoc);
    pdf_obj* root = trailer ? pdf_dict_get(ctx, trailer, PDF_NAME(Root)) : nullptr;
    if (!root) return true;  // writeDssDict throws on this; nothing to undo
    pdf_obj* slot = pdf_dict_gets(ctx, root, "DSS");
    if (!slot) return true;
    rb.hadDss = true;
    if (pdf_is_indirect(ctx, slot)) {
        rb.dssWasIndirect = true;
        rb.dssObjNum = pdf_to_num(ctx, slot);
    }
    // Deep copy keeps nested indirect references as references, which is what
    // we want: the pre-existing cert/CRL streams are never mutated, only the
    // arrays that point at them are.
    rb.backup = pdf_deep_copy_obj(ctx, pdf_resolve_indirect(ctx, slot));
    if (dssRollbackCaptureFaultArmed()) {
        pdf_drop_obj(ctx, rb.backup);
        rb.backup = nullptr;
    }
    return rb.backup != nullptr;
}

/// @return false when the pre-mutation state could NOT be fully restored.
///         See the ordering argument above: with captureDssRollback()'s
///         verdict honoured this cannot happen, and a false here means the
///         document in memory is mutated in a way nothing can undo.
[[nodiscard]] bool rollbackDss(fz_context* ctx, pdf_document* pdoc,
                               const DssRollback& rb)
{
    bool restored = true;
    pdf_obj* trailer = pdf_trailer(ctx, pdoc);
    pdf_obj* root = trailer ? pdf_dict_get(ctx, trailer, PDF_NAME(Root)) : nullptr;
    if (rb.hadDss) {
        if (!root || !rb.backup) {
            // The third state the two-branch form fell through silently.
            restored = false;
        } else if (rb.dssWasIndirect) {
            pdf_update_object(ctx, pdoc, rb.dssObjNum, rb.backup);
        } else {
            pdf_dict_puts(ctx, root, "DSS", rb.backup);
        }
    } else if (root) {
        pdf_dict_dels(ctx, root, "DSS");
    }
    const int now = pdf_xref_len(ctx, pdoc);
    for (int num = rb.preXrefLen; num < now; ++num) {
        pdf_delete_object(ctx, pdoc, num);
    }
    return restored;
}

} // namespace

const char* LtvDssWriter::kRollbackCaptureFailedMessage =
    "cannot snapshot the existing /DSS for rollback; refusing to write "
    "long-term-validation data that could not be undone";

const char* LtvDssWriter::kRollbackIncompleteMessage =
    "the /DSS mutation could NOT be rolled back; this document must not be "
    "saved again";

const char* LtvDssWriter::kPublishedButReopenFailedMessage =
    "the long-term-validation data WAS written and the file on disk is "
    "complete; only re-opening it afterwards failed, so close and re-open the "
    "document — do NOT repeat the operation";

std::string LtvDssWriter::sha1HexUpper(const std::string& input)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) return {};

    std::string out;
    if (EVP_DigestInit_ex(mdctx, EVP_sha1(), nullptr) == 1 &&
        EVP_DigestUpdate(mdctx, input.data(), input.size()) == 1 &&
        EVP_DigestFinal_ex(mdctx, digest, &digestLen) == 1)
    {
        out.resize(digestLen * 2);
        for (unsigned int i = 0; i < digestLen; ++i) {
            byteToHexUpper(digest[i], &out[i * 2]);
        }
    }
    EVP_MD_CTX_free(mdctx);
    return out;
}

bool LtvDssWriter::write(pdfe::core::Document* doc,
                         const std::string& signatureContentsHex,
                         const std::vector<std::vector<uint8_t>>& certs,
                         const std::vector<std::vector<uint8_t>>& ocsps,
                         const std::vector<std::vector<uint8_t>>& crls)
{
    lastError_.clear();
    auto fail = [&](const char* why) {
        lastError_ = why;
        qCWarning(lcDss) << "/DSS write refused:" << why;
        return false;
    };

    if (!doc) return fail("no document");
    if (signatureContentsHex.empty()) return fail("empty signature /Contents");
    if (doc->filePath().empty())
        return fail("document has no file path to save back to");

    const std::string vriKey = sha1HexUpper(signatureContentsHex);
    if (vriKey.empty()) return fail("could not compute the /VRI key (SHA-1)");

    auto context = doc->context();
    if (!context) return fail("document has no MuPDF context");

    fz_document* fzDoc = doc->fzDocument();
    if (!fzDoc) return fail("document has no underlying fz_document");

    // F47 item F5 (phase-49): the mutation below must be undone if ANYTHING
    // after it fails, or a rejected /DSS rides out on the next save. See
    // DssRollback above.
    DssRollback rb;
    bool mutationStarted = false;
    // F58: set by saveIncrementalStaged() once it has dropped the in-memory
    // document and re-opened it from disk. See the undo lambda.
    bool documentReloaded = false;
    // F58-f, consumed here by the phase-53 review (finding 6): set when the
    // revision reached disk intact and only the re-open afterwards failed. That
    // is a SUCCESSFUL /DSS upgrade with a dead Document, and reporting it as a
    // plain failure invites the user to redo work that is already done. See
    // kPublishedButReopenFailedMessage.
    bool publishedButReopenFailed = false;

    // @return false when the mutation could not be fully undone — the state
    //         F57 makes unreachable by refusing to mutate without a snapshot,
    //         and which must NEVER be reported to the caller as a plain
    //         "the write failed". See kRollbackIncompleteMessage.
    auto undo = [&]() {
        // F58. When the staged save re-opened the document, the pdf_document
        // that was mutated no longer exists: the mutation went with it and the
        // Document now holds a fresh parse of the UNCHANGED original. That is
        // a completed rollback, and running rollbackDss() against the new
        // document would be actively destructive — rb.preXrefLen was measured
        // against the old one, so the "delete everything we added" loop would
        // delete legitimate objects of the re-opened file.
        if (documentReloaded) return true;
        bool restored = false;
        try {
            context->execute([&](fz_context* ctx) {
                // Re-fetch rather than reuse the `fzDoc` captured above: it is
                // dangling if the document was re-opened, and re-fetching is
                // correct in every case.
                fz_document* live = doc->fzDocument();
                pdf_document* pdoc =
                    live ? pdf_document_from_fz_document(ctx, live) : nullptr;
                // No pdf_document means no mutation happened either.
                restored = !pdoc || rollbackDss(ctx, pdoc, rb);
            });
        } catch (const std::exception&) {
            // Nothing further can be done here, and throwing out of the
            // failure path would replace a reported failure with an
            // unreported one. The caller still gets false — but it gets it
            // WITH the un-rollbackable diagnostic, which is the difference
            // F57 exists to make.
            restored = false;
        }
        return restored;
    };
    auto dropBackup = [&]() {
        if (!rb.backup) return;
        try {
            context->execute([&](fz_context* ctx) {
                pdf_drop_obj(ctx, rb.backup);
            });
        } catch (const std::exception&) {
        }
        rb.backup = nullptr;
    };

    try {
        context->execute([&](fz_context* ctx) {
            pdf_document* pdoc = pdf_document_from_fz_document(ctx, fzDoc);
            if (!pdoc) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Not a PDF document");
            }
            // F57: capture FIRST and refuse if it failed. Only once a usable
            // undo exists may anything be mutated — see the ordering argument
            // above DssRollback. fz_throw (not a C++ throw) because we are
            // inside an fz_try region: setjmp/longjmp does not unwind.
            if (!captureDssRollback(ctx, pdoc, rb)) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "%s",
                         LtvDssWriter::kRollbackCaptureFailedMessage);
            }
            mutationStarted = true;
            writeDssDict(ctx, pdoc, vriKey, certs, ocsps, crls);
        });
        // F58 (phase-52) — THE USER'S ONLY COPY IS NO LONGER THE WRITE TARGET.
        //
        // This used to be doc->saveIncremental(), i.e.
        // pdf_save_document(do_incremental=1) in APPEND mode straight onto the
        // live signed document. A genuine mid-write failure (disk full, media
        // pulled, power loss, I/O error) therefore left the user's only copy
        // carrying a partial appended revision: a broken trailing xref and a
        // /ByteRange that no longer resolves — the signature destroyed by the
        // very operation that was supposed to strengthen it. DssRollback below
        // cannot reach that: it undoes in-memory OBJECT mutations, not bytes
        // already on disk.
        //
        // saveIncrementalStaged() copies the original into a staging file
        // beside it, appends THERE, and publishes by an atomic rename.
        //
        // THE STAGING FILE IS NOT "<target>.pdfe_tmp" ANY MORE (F58-a,
        // phase-53). It is core::StagingFile's
        // "<target>.pdfe_part.<128-bit CSPRNG hex>", created CREATE_NEW with an
        // exclusive handle held from creation to publication. The predictable
        // name with no handle across the write→rename gap was the CRITICAL
        // defect of the phase-52 shape — an unprivileged process could stamp
        // bytes into the staged file and have PDFE publish them over the user's
        // signed document. See core/AtomicFileReplace.h.
        //
        // WHAT THE COPY IS AND IS NOT FOR (corrected in the phase-53 review,
        // finding 7.1). The phase-52 text here said the copy-then-append shape
        // "preserves the one property that matters — the original bytes stay a
        // byte-identical PREFIX". That is FALSE, and dangerously so: it implies
        // that removing the copy would break signature preservation, when in
        // fact MuPDF guarantees the prefix independently and unconditionally.
        // do_pdf_save_document() opens an incremental write with
        //     ensure_initial_incremental_contents(ctx, doc->file, opts->out,
        //                                         doc->file_size)
        // (mupdf 1.26.10 source/pdf/pdf-write.c:2384), which byte-compares the
        // output's existing contents against the still-open original and, on
        // ANY mismatch, rewinds and rewrites the original contents from
        // doc->file before appending. So the prefix holds whether we hand it a
        // pre-seeded copy or an empty file.
        //
        // The copy is therefore a PERFORMANCE SHORTCUT over MuPDF's own copy,
        // not the safety mechanism. The safety mechanism is the staging plus
        // the atomic rename: it is what keeps a mid-write failure off the
        // user's only copy. Anyone optimising here must remove the right half.
        //
        // A GENUINE (previously unclaimed) IMPROVEMENT, recorded so it is not
        // optimised away either: when the file has GROWN on disk since PDFE
        // opened it — a concurrent writer appended its own revision —
        // ensure_initial_incremental_contents finds the first doc->file_size
        // bytes identical, seeks the output to doc->file_size and calls
        // fz_truncate_output. Unstaged, that truncation lands on the LIVE file,
        // destroying the other writer's revision in place immediately, even if
        // this save then fails and nothing replaces it. Staged, it lands in the
        // throwaway copy: the live file keeps that revision unless and until
        // this save succeeds and atomically replaces it. (The published result
        // still does not carry the concurrent revision — staging changes when
        // and how it is lost, not whether.)
        //
        // The two out-parameters both matter to the caller:
        //  * documentReloaded — the handle was dropped and the document
        //    re-opened (Windows will not rename over an open file). It is what
        //    stops the rollback below from being applied to a document that
        //    never carried the mutation.
        //  * publishedButReopenFailed — the revision IS on disk and correct;
        //    only the re-open failed. Reporting that as a plain failure is
        //    finding 6.
        doc->saveIncrementalStaged(&documentReloaded, &publishedButReopenFailed);
    } catch (const std::exception& e) {
        lastError_ = e.what();
        // Order matters: the rollback decision is unchanged, but the REPORT
        // must lead with "it was published" when it was, because that is the
        // one failure the user must not respond to by retrying.
        const bool rolledBack = (!mutationStarted || undo());
        if (publishedButReopenFailed) {
            // Finding 6 (phase-53). The /DSS revision reached disk and is
            // complete; only re-opening it failed. `documentReloaded` is set
            // before the rename, so undo() above correctly did nothing —
            // there is no in-memory mutation left to roll back, and rolling
            // anything back here would be wrong: the file on disk legitimately
            // carries the /DSS.
            lastError_ = std::string(kPublishedButReopenFailedMessage) + " (" +
                         lastError_ + ")";
            qCWarning(lcDss) << "/DSS published but the document could not be "
                                 "re-opened:" << lastError_.c_str();
        } else if (!rolledBack) {
            // The document in memory is mutated and cannot be put back. This
            // is strictly worse than a failed write and must not be reported
            // as one: the next save of this document — for any unrelated
            // reason — publishes /DSS material the caller was just told was
            // not written. Surface it explicitly.
            lastError_ = std::string(kRollbackIncompleteMessage) + " (" +
                         lastError_ + ")";
            qCCritical(lcDss) << "/DSS rollback INCOMPLETE:"
                              << lastError_.c_str();
        } else {
            qCWarning(lcDss) << "/DSS write failed:" << lastError_.c_str();
        }
        dropBackup();
        return false;
    }
    dropBackup();
    return true;
}

bool LtvDssWriter::extractFirstSignatureInputs(pdfe::core::Document* doc,
                                                SignatureInputs& out)
{
    out = {};
    if (!doc) return false;
    auto context = doc->context();
    if (!context) return false;
    fz_document* fzDoc = doc->fzDocument();
    if (!fzDoc) return false;

    // Use Document::page() so the page is loaded the same way SignatureManager
    // loads it (Document caches and tracks the pdf_page lifetime). Re-loading
    // a page directly via pdf_load_page on a freshly-signed doc trips
    // stale-xref-cache assertions inside MuPDF.
    const int pageCount = doc->pageCount();
    bool ok = false;
    for (int pi = 0; pi < pageCount && !ok; ++pi) {
        auto page = doc->page(pi);
        if (!page) continue;
        fz_page* fzPage = page->fzPage();
        if (!fzPage) continue;

        context->execute([&](fz_context* c) {
            pdf_document* pdoc = pdf_document_from_fz_document(c, fzDoc);
            if (!pdoc) return;
            pdf_page* pp = pdf_page_from_fz_page(c, fzPage);
            if (!pp) return;

            for (pdf_annot* w = pdf_first_widget(c, pp); w;
                 w = pdf_next_widget(c, w)) {
                if (pdf_widget_type(c, w) != PDF_WIDGET_TYPE_SIGNATURE)
                    continue;
                if (!pdf_widget_is_signed(c, w)) continue;

                pdf_obj* widgetObj = pdf_annot_obj(c, w);
                pdf_obj* sigDict = pdf_dict_get(c, widgetObj, PDF_NAME(V));
                if (!sigDict) continue;

                // /ByteRange — direct read instead of pdf_signature_byte_range()
                // (which trips an "object out of range" assertion immediately
                // after a same-context incremental sign on some MuPDF builds).
                pdf_obj* brObj = pdf_dict_get(c, sigDict, PDF_NAME(ByteRange));
                if (brObj && pdf_is_array(c, brObj)) {
                    int brLen = pdf_array_len(c, brObj);
                    if (brLen >= 2) {
                        out.firstByteRangeOffset =
                            pdf_to_int64(c, pdf_array_get(c, brObj, 0));
                        out.firstByteRangeLength =
                            pdf_to_int64(c, pdf_array_get(c, brObj, 1));
                    }
                }

                // /Contents — direct read of the hex string bytes.
                pdf_obj* contentsObj = pdf_dict_get(c, sigDict, PDF_NAME(Contents));
                if (contentsObj && pdf_is_string(c, contentsObj)) {
                    size_t sigLen = static_cast<size_t>(pdf_to_str_len(c, contentsObj));
                    const char* sigBytes = pdf_to_str_buf(c, contentsObj);
                    if (sigBytes && sigLen > 0) {
                        out.contentsBin.assign(sigBytes, sigBytes + sigLen);
                        static const char* k = "0123456789ABCDEF";
                        out.contentsHex.resize(sigLen * 2);
                        for (size_t i = 0; i < sigLen; ++i) {
                            unsigned char b = static_cast<unsigned char>(sigBytes[i]);
                            out.contentsHex[i * 2]     = k[(b >> 4) & 0xF];
                            out.contentsHex[i * 2 + 1] = k[b & 0xF];
                        }

                        auto vr = Pkcs7OpenSslBridge::verifyWithChain(
                            out.contentsBin.data(), out.contentsBin.size());
                        // F26 — `vr.chainTrusted` is now a CONTROLLED verdict
                        // (CertChainValidator + the user's trust anchors) rather
                        // than the ambient-system-store guess it used to be, and
                        // it is still DELIBERATELY not consulted here.
                        //
                        // /DSS is an evidence container (ISO 32000-2 §12.8.4.3),
                        // not a verdict. Its job is to embed the certificates,
                        // CRLs and OCSP responses a validator will need LATER —
                        // possibly years later, when the anchors, the
                        // revocation state, and (for some OTHER validator that
                        // reads one — PDFE does not, GitHub #212) the system
                        // store all differ from today's.
                        // Refusing to embed a chain that is untrusted right now
                        // would delete the exact material needed to establish
                        // that it was trusted at signing time, which is the whole
                        // point of long-term validation. Trust is evaluated at
                        // VALIDATION time by LtvValidator, which F23 routed
                        // through the same CertChainValidator.
                        // Deferral recorded in phase-26-dispositions.md §3.
                        out.certChain = std::move(vr.certChain);
                        ok = !out.certChain.empty();
                    }
                }
                if (ok) break;
            }
        });
    }
    return ok;
}

bool LtvDssWriter::inspectDss(pdfe::core::Document* doc,
                              const std::string& vriKey,
                              DssCounts& out)
{
    out = {};
    if (!doc) return false;
    auto context = doc->context();
    if (!context) return false;
    fz_document* fzDoc = doc->fzDocument();
    if (!fzDoc) return false;

    context->execute([&](fz_context* fc) {
        pdf_document* pdoc = pdf_document_from_fz_document(fc, fzDoc);
        if (!pdoc) return;
        pdf_obj* root = pdf_dict_get(fc, pdf_trailer(fc, pdoc), PDF_NAME(Root));
        if (!root) return;
        pdf_obj* dss = pdf_dict_gets(fc, root, "DSS");
        if (!dss) return;
        out.hasDss = true;
        // Raw slot value, deliberately NOT resolved: the point is which of the
        // two shapes the catalog actually holds (F57).
        out.dssIsIndirect = pdf_is_indirect(fc, dss) != 0;

        pdf_obj* certs = pdf_dict_gets(fc, dss, "Certs");
        if (certs && pdf_is_array(fc, certs))
            out.topCerts = pdf_array_len(fc, certs);
        pdf_obj* ocsps = pdf_dict_gets(fc, dss, "OCSPs");
        if (ocsps && pdf_is_array(fc, ocsps))
            out.topOcsps = pdf_array_len(fc, ocsps);
        pdf_obj* crls = pdf_dict_gets(fc, dss, "CRLs");
        if (crls && pdf_is_array(fc, crls))
            out.topCrls = pdf_array_len(fc, crls);

        pdf_obj* vri = pdf_dict_gets(fc, dss, "VRI");
        if (!vri) return;
        pdf_obj* entry = pdf_dict_gets(fc, vri, vriKey.c_str());
        if (!entry) return;
        out.hasVriKey = true;
        pdf_obj* vc = pdf_dict_gets(fc, entry, "Cert");
        if (vc && pdf_is_array(fc, vc)) out.vriCerts = pdf_array_len(fc, vc);
        pdf_obj* vo = pdf_dict_gets(fc, entry, "OCSP");
        if (vo) {
            out.vriHasOcspKey = true;
            if (pdf_is_array(fc, vo)) out.vriOcsps = pdf_array_len(fc, vo);
        }
        pdf_obj* vr = pdf_dict_gets(fc, entry, "CRL");
        if (vr) {
            out.vriHasCrlKey = true;
            if (pdf_is_array(fc, vr)) out.vriCrls = pdf_array_len(fc, vr);
        }
    });
    return out.hasDss;
}

// writeTo() was REMOVED in the phase-49 review remediation (finding M2). It
// carried the identical defect F5 had just fixed 160 lines above in write():
// mutate /DSS, save, `catch (...) { return false; }`, no DssRollback — so a
// failed save left the mutation live in the in-memory document to be published
// by the next successful save. Its header comment was wrong too, claiming it
// worked "on a temp copy of the source file" when it appended in place to
// outputPath.
//
// Deleted rather than repaired because it had ZERO callers in src/ or tests/
// (`grep -rn writeTo src tests` found only its own declaration and definition)
// and no caller ever exercised it. Repairing it would have meant carrying a
// second rollback path, a second header contract and a test suite for an API
// nothing uses, and the more entrenched a dead public method becomes the more
// convincing it looks to the next caller. write() is the supported entry point
// and it is the one that is covered.
//
// Nothing else in this file changed by the removal: writeDssDict() and
// DssRollback are both still used by write().
//
// F58 (phase-52) CORRECTION: this comment used to also name doIncrementalSave()
// as "still used by write()". It was not — write() had already been switched to
// doc->saveIncremental(), and the file-local helper was dead code that merely
// looked like the save write() performed. It has been deleted; the real save is
// now Document::saveIncrementalStaged().

bool LtvDssWriter::writeWithFiles(
    pdfe::core::Document* doc,
    const std::string& signatureContentsHex,
    const std::vector<std::vector<uint8_t>>& certs,
    const std::vector<std::filesystem::path>& ocspFiles,
    const std::vector<std::filesystem::path>& crlFiles)
{
    // Load OCSP responses. A single failed parse fails the whole call —
    // partial /DSS embedding would create a misleading LTV state where the
    // user thinks revocation data is present but only some of it is.
    lastError_.clear();

    std::vector<std::vector<uint8_t>> ocspBlobs;
    ocspBlobs.reserve(ocspFiles.size());
    for (const auto& p : ocspFiles) {
        OcspResponse parsed;
        if (!OcspResponseParser::parse(p, parsed) || parsed.derBytes.empty()) {
            lastError_ = "could not parse OCSP response file: " + p.string();
            return false;
        }
        ocspBlobs.push_back(std::move(parsed.derBytes));
    }

    // Load CRLs.
    std::vector<std::vector<uint8_t>> crlBlobs;
    crlBlobs.reserve(crlFiles.size());
    for (const auto& p : crlFiles) {
        CrlInfo parsed;
        if (!CrlParser::parse(p, parsed) || parsed.derBytes.empty()) {
            lastError_ = "could not parse CRL file: " + p.string();
            return false;
        }
        crlBlobs.push_back(std::move(parsed.derBytes));
    }

    return write(doc, signatureContentsHex, certs, ocspBlobs, crlBlobs);
}

} // namespace pdfe::signatures
