#include "EncryptionManager.h"
#include "EncryptionSettings.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PdfSignatureProbe.h"
#include "core/Error.h"
#include "core/SecureWipe.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/Constants.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace pdfe::encryption {

// Optimizer-resistant memory wipe. MOVED to core/SecureWipe.h in phase-11 so
// there is one definition rather than a copy per caller — the reverification
// found the password path had no wipe at all while this one sat private to this
// translation unit. The behaviour is unchanged; only its home is.
using pdfe::core::secureWipe;

// Helper: Map EncryptionSettings to encryption method string
//
// PDF-26: /V 5 is AES-256 for BOTH revisions that use it — R6 (ISO 32000-2)
// and R5 (the deprecated Adobe Extension Level 3 handler, which MuPDF still
// READS). The old `version == 5 && revision == 6` test let every R5 document
// fall through to the `version >= 2` branch and be reported as "RC4" — a
// crypto MIS-LABEL in the direction that understates the protection, which is
// exactly the direction a user acts on. There is no /V 5 handler that is not
// AES-256, so the revision is not part of the decision.
//
// F24 — /V 4 /R 4 is NOT synonymous with AES-128. /V 4 introduced crypt
// FILTERS: the handler revision says how the key is derived, and the /CF entry
// named by /StmF says which cipher that key drives. `/CFM /AESV2` is AES-128;
// `/CFM /V2` is RC4-128, and it is legal (ISO 32000-1 Table 25) at /V 4 /R 4.
// Reporting the second as "AES-128" OVERSTATES the protection — the same
// mis-label class F3 closed for /V 1, in the opposite direction, and the
// direction a user acts on ("this file is AES, it is fine to email").
//
// @p streamMethod is MuPDF's pdf_crypt_stream_method() for the document:
// "None" (/Identity), "RC4", "AES" (MuPDF collapses AESV2 and AESV3) or
// "Unknown". Only the "RC4" value changes the answer — an /Identity /StmF is
// deliberately left reporting the handler it declares, because that condition
// has its own first-class field (EncryptionInfo::streamsUnprotected, PDF-25)
// and folding it into the method string would hide it behind a second name.
static std::string getMethodString(int version, int revision,
                                   const std::string& streamMethod) {
    if (version == 5) {
        return "AES-256";   // R5 (Adobe ext. level 3) and R6 (ISO 32000-2)
    } else if (version == 4 && revision == 4) {
        if (streamMethod == "RC4") {
            return "RC4-128";   // /CFM /V2 — legal at /V 4, and NOT AES.
        }
        return "AES-128";
    } else if (version >= 1) {
        // /V 1 is RC4 with a 40-bit key — the WEAKEST standard handler there
        // is. The old `version >= 2` bound sent it to the "None" fallback, so
        // the product reported the weakest real encryption as NO encryption.
        // Found by this area's own RC4 control test (tests/
        // test_encryption_handler_security.cpp, Aes256R5IsReportedAsAes256NotRc4).
        return "RC4";
    }
    return "None";
}

// PDF-27 — the permission bits each handler revision can actually express.
// R2 (RC4-40) defines only bits 3-6: print / modify / copy / annotate. R3 and
// above add accessibility, form-fill, assemble and high-quality print.
static bool handlerExpressesAllEightPermissionBits(EncryptionAlgorithm alg) {
    return alg != EncryptionAlgorithm::RC4_40;
}

static std::string handlerName(EncryptionAlgorithm alg) {
    switch (alg) {
        case EncryptionAlgorithm::RC4_40:  return "RC4-40 (V1/R2)";
        case EncryptionAlgorithm::RC4_128: return "RC4-128 (V2/R3)";
        case EncryptionAlgorithm::AES_128: return "AES-128 (V4/R4)";
        case EncryptionAlgorithm::AES_256: return "AES-256 (V5/R6)";
        case EncryptionAlgorithm::None:    break;
    }
    return "None";
}

// Pimpl implementation
struct EncryptionManager::Impl {
    std::shared_ptr<pdfe::core::MuPdfContext> context_;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context_(std::move(ctx)) {}

    // Get fz_document* via friend access to Document::fzDocument()
    fz_document* getFzDoc(const pdfe::core::Document* doc) {
        if (!doc) {
            throw core::PdfeException("Document is null");
        }
        return doc->fzDocument();
    }

    // Get pdf_document* from fz_document* using MuPDF API
    pdf_document* getPdfDoc(const pdfe::core::Document* doc) {
        fz_document* fzDoc = getFzDoc(doc);
        if (!fzDoc) {
            return nullptr;
        }
        return context_->executeReturn<pdf_document*>([fzDoc](fz_context* ctx) {
            return pdf_document_from_fz_document(ctx, fzDoc);
        });
    }
};

// --- EncryptionManager public methods ---

EncryptionManager::EncryptionManager(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context))) {}

EncryptionManager::~EncryptionManager() = default;

EncryptionInfo EncryptionManager::getEncryptionInfo(const pdfe::core::Document* doc) const {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    pdf_document* pdfDoc = impl_->getPdfDoc(doc);
    if (!pdfDoc) {
        // Not a PDF document - return unencrypted info
        EncryptionInfo info;
        info.isEncrypted = false;
        return info;
    }

    EncryptionInfo info;

    impl_->context_->execute([&](fz_context* ctx) {
        // Check if document is encrypted by looking for Encrypt dict
        pdf_obj* trailer = pdf_trailer(ctx, pdfDoc);
        pdf_obj* encryptObj = pdf_dict_get(ctx, trailer, PDF_NAME(Encrypt));

        if (!encryptObj) {
            // Not encrypted
            info.isEncrypted = false;
            return;
        }

        info.isEncrypted = true;

        // Get permissions directly from document
        int permBitmask = pdf_document_permissions(ctx, pdfDoc);
        info.permissions = PermissionFlags::fromBitmask(permBitmask);

        // Get detailed encryption information
        pdf_obj* idObj = pdf_dict_get(ctx, trailer, PDF_NAME(ID));
        pdf_crypt* crypt = nullptr;

        fz_try(ctx) {
            crypt = pdf_new_crypt(ctx, encryptObj, idObj);

            // Query encryption details
            info.version = pdf_crypt_version(ctx, crypt);
            info.revision = pdf_crypt_revision(ctx, crypt);
            info.keyLength = pdf_crypt_length(ctx, crypt);
            info.metadataEncrypted = pdf_crypt_encrypt_metadata(ctx, crypt) != 0;

            // PDF-25 — read the crypt filter that is actually applied to
            // streams and to strings. MuPDF maps an /Identity filter (or a
            // missing /StmF at /V >= 4) to PDF_CRYPT_NONE, which it reports as
            // the string "None". A document with /StmF /Identity carries an
            // /Encrypt dictionary but stores every stream in PLAINTEXT: page
            // content, images and embedded files can be read straight out of
            // the file with no password at all. Reporting only isEncrypted
            // would tell the user the opposite of the truth.
            const char* sm = pdf_crypt_stream_method(ctx, crypt);
            const char* tm = pdf_crypt_string_method(ctx, crypt);
            info.streamCryptMethod = sm ? sm : "";
            info.stringCryptMethod = tm ? tm : "";
            info.streamsUnprotected = (info.streamCryptMethod == "None");
            info.stringsUnprotected = (info.stringCryptMethod == "None");

            // F24 — the summary method string is derived AFTER the crypt
            // filter is known, because at /V 4 the filter is what decides the
            // cipher. Deriving it from (version, revision) alone reported every
            // /V 4 /R 4 document as AES-128, including RC4-128 (/CFM /V2) ones.
            info.method = getMethodString(info.version, info.revision,
                                          info.streamCryptMethod);

            // R2-12: authentication level (0=none,1=user,2=owner,3=both) comes
            // from the ACTUAL fz_authenticate_password result captured by the
            // Document at open — NOT inferred from the permission bitmask. The
            // old bitmask heuristic mislabeled callers in both directions (a
            // fully-permissioned USER read as owner; a QPDF all-perms owner whose
            // P value is 0xFFFFFFFC read as user). authLevel is set from
            // doc->authLevel() below, after this fz scope closes.
        }
        fz_always(ctx) {
            if (crypt) {
                pdf_drop_crypt(ctx, crypt);
            }
        }
        fz_catch(ctx) {
            // If we can't create crypt, return basic info
            info.method = "Unknown";
        }
    });

    // R2-12: report the real authentication level established at open.
    info.authLevel = doc->authLevel();

    // Empty-user-password bypass. An encrypted document that does NOT need a
    // password is one whose USER password is the empty string: the owner
    // password restricts permissions, but the content itself is readable by
    // anyone, with no prompt. Permission bits are advisory in most viewers, so
    // the practical protection of such a file is close to none. Surface it as
    // a first-class field rather than leaving the caller to infer it from
    // (isEncrypted && !needsPassword).
    if (info.isEncrypted) {
        info.emptyUserPassword = !needsPassword(doc);
    }

    return info;
}

PermissionFlags EncryptionManager::getPermissions(const pdfe::core::Document* doc) const {
    return getEncryptionInfo(doc).permissions;
}

bool EncryptionManager::isEncrypted(const pdfe::core::Document* doc) const {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    pdf_document* pdfDoc = impl_->getPdfDoc(doc);
    if (!pdfDoc) {
        return false;
    }

    return impl_->context_->executeReturn<bool>([pdfDoc](fz_context* ctx) -> bool {
        pdf_obj* trailer = pdf_trailer(ctx, pdfDoc);
        const pdf_obj* encryptObj = pdf_dict_get(ctx, trailer, PDF_NAME(Encrypt));
        return encryptObj != nullptr;
    });
}

bool EncryptionManager::needsPassword(const pdfe::core::Document* doc) const {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    fz_document* fzDoc = impl_->getFzDoc(doc);
    if (!fzDoc) {
        return false;
    }

    return impl_->context_->executeReturn<bool>([fzDoc](fz_context* ctx) -> bool {
        return fz_needs_password(ctx, fzDoc) != 0;
    });
}

int EncryptionManager::authenticatePassword(const pdfe::core::Document* doc, const std::string& password) {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    fz_document* fzDoc = impl_->getFzDoc(doc);
    if (!fzDoc) {
        return 0; // Not a PDF, no password needed
    }

    return impl_->context_->executeReturn<int>([fzDoc, &password](fz_context* ctx) -> int {
        return fz_authenticate_password(ctx, fzDoc, password.c_str());
    });
}

EncryptionSaveReport EncryptionManager::previewPermissionMapping(
    const EncryptionSettings& settings
) {
    EncryptionSaveReport report;
    report.handlerWritten = handlerName(settings.algorithm);

    // F24 — owner-only protection, reported at the moment the settings are
    // applied rather than only from the Document Permissions dialog nothing
    // prompts the user to open. Evaluated before the R2 early-return below so
    // every handler reports it, not just RC4-40.
    if (settings.isOwnerOnlyProtection()) {
        report.emptyUserPassword = true;
        report.emptyUserPasswordWarning = emptyUserPasswordWarningText();
    }

    if (handlerExpressesAllEightPermissionBits(settings.algorithm)) {
        return report;  // nothing is lost
    }

    // RC4-40 / R2. QPDF's setR2EncryptionParametersInsecure takes exactly four
    // booleans; the remaining four permission bits are not defined at R2 and
    // are written as "allowed". Enumerate them, then narrow to the ones the
    // caller actually asked to restrict — those are the requests the produced
    // file does not honour.
    const struct { const char* name; bool requested; } r2Unrepresentable[] = {
        { "accessibility",    settings.permissions.accessibility    },
        { "formFill",         settings.permissions.formFill         },
        { "assemble",         settings.permissions.assemble         },
        { "printHighQuality", settings.permissions.printHighQuality },
    };

    for (const auto& bit : r2Unrepresentable) {
        report.unrepresentablePermissions.emplace_back(bit.name);
        if (!bit.requested) {
            report.droppedRestrictions.emplace_back(bit.name);
        }
    }

    report.lossyPermissionMapping = !report.droppedRestrictions.empty();
    if (report.lossyPermissionMapping) {
        std::string names;
        for (std::size_t i = 0; i < report.droppedRestrictions.size(); ++i) {
            if (i) names += ", ";
            names += report.droppedRestrictions[i];
        }
        report.summary =
            report.handlerWritten +
            " cannot express these permission bits, so the requested "
            "restriction on them is NOT applied to the saved file: " + names +
            ". Use RC4-128 or stronger to restrict them.";
    }
    return report;
}

void EncryptionManager::saveEncrypted(
    pdfe::core::Document* doc,
    const std::filesystem::path& outputPath,
    const EncryptionSettings& settings,
    EncryptionSaveReport* report
) {
    if (report) {
        // PDF-27: fill the report BEFORE any validation throw, so a caller that
        // wants the mapping loss still gets it on the paths that do write.
        *report = previewPermissionMapping(settings);
    }
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }
    // Encryption restructures the document's security handler; flag the
    // session needsFullGc so both the in-memory doc's next save and this
    // function's own write below use do_garbage=3 (pipeline b2e47d91, Tiers
    // 1+2). saveEncrypted itself writes to outputPath separately.
    doc->markNeedsFullGc();

    if (!settings.isEncrypted()) {
        throw std::invalid_argument("Encryption settings have no encryption algorithm set");
    }

    if (!settings.hasOwnerPassword()) {
        throw std::invalid_argument("Owner password is required for encryption");
    }

    pdf_document* pdfDoc = impl_->getPdfDoc(doc);
    if (!pdfDoc) {
        throw core::PdfeException("Not a PDF document");
    }

    // Pipeline (P12-INFRA-1 sub-issue 1: re-encrypt path).
    // 1. MuPDF writes the document to an in-memory fz_buffer with
    //    do_encrypt = PDF_ENCRYPT_NONE.  This handles re-encrypt
    //    cleanly: an already-encrypted source is decrypted (in
    //    memory, never on disk) before QPDF re-applies new
    //    encryption.  Keeping the intermediate in memory (never
    //    touching disk) avoids leaving a plaintext file in the
    //    target directory.
    // 2. QPDF::processMemoryFile ingests the unencrypted buffer.
    // 3. QPDFWriter writes the re-encrypted output to disk.

    // Step 1: Serialize the document to an in-memory fz_buffer
    // without encryption.  Use a local capture struct since the
    // lambda cannot capture by reference and outlive setjmp/longjmp.
    std::vector<unsigned char> intermediate;
    // Tier 2 (pipeline b2e47d91): routed through the helper per the conditional-
    // GC contract; encryption set needsFullGc above, so this resolves to 3.
    const int gcLevel = doc->effectiveGarbageLevel();
    try {
        impl_->context_->execute([pdfDoc, &intermediate, gcLevel](fz_context* ctx) {
            fz_buffer* buf = nullptr;
            fz_output* out = nullptr;
            fz_try(ctx) {
                buf = fz_new_buffer(ctx, 64 * 1024);
                out = fz_new_output_with_buffer(ctx, buf);
                pdf_write_options opts{};
                opts.do_garbage = gcLevel;
                opts.do_compress = 1;
                // Consistent with Document::save() (pipeline 3f8a1c72 Phase 1):
                // MuPDF's copystream() only Flate-compresses filterless streams,
                // so already-DCT/JPX/Flate images and already-subset fonts are
                // copied byte-for-byte. The encrypt-then-save path must not differ
                // from the normal save path, so the flags stay on here too.
                opts.do_compress_images = 1;
                opts.do_compress_fonts = 1;
                opts.do_preserve_metadata = 1;
                opts.do_encrypt = PDF_ENCRYPT_NONE;
                pdf_write_document(ctx, pdfDoc, out, &opts);
                fz_close_output(ctx, out);
                unsigned char* data = nullptr;
                size_t len = fz_buffer_storage(ctx, buf, &data);
                if (data && len > 0) {
                    intermediate.assign(data, data + len);
                }
            }
            fz_always(ctx) {
                if (out) fz_drop_output(ctx, out);
                if (buf) fz_drop_buffer(ctx, buf);
            }
            fz_catch(ctx) {
                fz_rethrow(ctx);
            }
        });
    } catch (const std::exception& e) {
        // Wipe any partial intermediate before propagating; this
        // never contained passwords but may contain document
        // content.
        if (!intermediate.empty()) {
            secureWipe(intermediate.data(), intermediate.size());
            intermediate.clear();
        }
        throw core::PdfeException(
            std::string("Failed to serialize document for re-encryption: ")
            + e.what()
        );
    }

    if (intermediate.empty()) {
        throw core::PdfeException(
            "Failed to serialize document: empty intermediate buffer");
    }

    // RAII guard: wipe and clear the intermediate buffer in all
    // exit paths so the plaintext PDF body does not linger in
    // process memory after this function returns.
    struct IntermediateWiper {
        std::vector<unsigned char>& buf;
        ~IntermediateWiper() {
            if (!buf.empty()) {
                secureWipe(buf.data(), buf.size());
                buf.clear();
                buf.shrink_to_fit();
            }
        }
    } wiper{intermediate};

    try {
        // Step 2 & 3: Apply encryption via QPDF (scoped to release file handle)
        {
            QPDF qpdf;
            qpdf.processMemoryFile(
                "pdfe-reencrypt-intermediate",
                reinterpret_cast<const char*>(intermediate.data()),
                intermediate.size());

            // QPDFWriter stores the filename pointer; the std::string
            // returned by .string() must outlive the writer or .write()
            // dereferences a freed buffer (caught by ASAN).
            const std::string outputPathStr = outputPath.string();
            QPDFWriter writer(qpdf, outputPathStr.c_str());

            // Map print permissions
            qpdf_r3_print_e printPerm = qpdf_r3p_none;
            if (settings.permissions.print) {
                if (settings.permissions.printHighQuality) {
                    printPerm = qpdf_r3p_full;
                } else {
                    printPerm = qpdf_r3p_low;
                }
            }

            auto userPw = settings.userPassword.c_str();
            auto ownerPw = settings.ownerPassword.c_str();

            switch (settings.algorithm) {
                case EncryptionAlgorithm::RC4_40:
                    writer.setR2EncryptionParametersInsecure(
                        userPw, ownerPw,
                        settings.permissions.print,
                        settings.permissions.modify,
                        settings.permissions.copy,
                        settings.permissions.annotate
                    );
                    break;

                case EncryptionAlgorithm::RC4_128:
                    writer.setR3EncryptionParametersInsecure(
                        userPw, ownerPw,
                        settings.permissions.accessibility,
                        settings.permissions.copy,
                        settings.permissions.assemble,
                        settings.permissions.annotate,
                        settings.permissions.formFill,
                        settings.permissions.modify,
                        printPerm
                    );
                    break;

                case EncryptionAlgorithm::AES_128:
                    writer.setR4EncryptionParametersInsecure(
                        userPw, ownerPw,
                        settings.permissions.accessibility,
                        settings.permissions.copy,
                        settings.permissions.assemble,
                        settings.permissions.annotate,
                        settings.permissions.formFill,
                        settings.permissions.modify,
                        printPerm,
                        settings.encryptMetadata,
                        true    // use_aes
                    );
                    break;

                case EncryptionAlgorithm::AES_256:
                    writer.setR6EncryptionParameters(
                        userPw, ownerPw,
                        settings.permissions.accessibility,
                        settings.permissions.copy,
                        settings.permissions.assemble,
                        settings.permissions.annotate,
                        settings.permissions.formFill,
                        settings.permissions.modify,
                        printPerm,
                        settings.encryptMetadata
                    );
                    break;

                default:
                    break;
            }

            writer.write();
        } // QPDF goes out of scope, releasing file handle

    } catch (const std::exception& e) {
        // No on-disk temp to clean up — intermediate is in-memory
        // and the RAII wiper above zeroes it on scope exit.
        throw core::PdfeException(
            std::string("Failed to save encrypted document: ") + e.what()
        );
    }
}

namespace {

/// F26 — POD-only carrier for the source-integrity probe.
///
/// Every field is trivially destructible ON PURPOSE. The probe runs inside
/// MuPdfContext::execute(), whose fz_try frame lives in the CALLER, so a
/// longjmp out of the lambda would skip any destructor. The std::string that
/// becomes the warning is built OUTSIDE the lambda, from these values.
struct SourceIntegrityProbe {
    bool certified = false;
    int signatureCount = 0;
};

SourceIntegrityProbe probeSourceIntegrity(fz_context* ctx,
                                          pdf_document* pdfDoc) {
    SourceIntegrityProbe probe;
    if (!pdfDoc) return probe;

    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
    if (!root) return probe;

    // Certification: /Root /Perms /DocMDP. Presence is the whole question here
    // — the /P LEVEL does not change the answer, because a full rewrite breaks
    // the certifying signature's byte range at every level. (DocMdpEnforcer's
    // readDocMdpLevel decodes the level for the guards that need it; this path
    // deliberately asks the cheaper question rather than taking a dependency on
    // pdfe_signatures for a boolean.)
    pdf_obj* perms = pdf_dict_get(ctx, root, PDF_NAME(Perms));
    if (perms) {
        pdf_obj* docMdp = pdf_dict_get(ctx, perms, PDF_NAME(DocMDP));
        probe.certified = (docMdp != nullptr);
    }

    // F26 REMEDIATION (security-audit gate). This used to walk ONLY
    // /Root /AcroForm /Fields, which is narrower than every other
    // signature-enumeration path in this codebase — SignatureManager lists via
    // pdf_first_widget/pdf_next_widget (page annotations) and DocMdpEnforcer
    // walks page /Annots directly. A document whose signature widget is on a
    // page but not reachable through /AcroForm/Fields — no /AcroForm at all,
    // an orphaned widget, a broken /Kids chain — was therefore INVISIBLE here,
    // and removeEncryption destroyed the signature with no warning on either
    // channel. That is the security-relevant direction: under-warning loses a
    // user's signatures silently, over-warning is merely annoying, and this
    // call is deliberately exempt from the DocMDP direct-write guard precisely
    // so that malformed and adversarial documents reach it.
    //
    // pdfe::core::countAppliedSignatures is the UNION of both routes,
    // de-duplicated by the /V object number. It lives in pdfe_core rather than
    // pdfe_signatures: this library links only pdfe_core, and taking a
    // dependency on pdfe_signatures for a pdf_obj* question would pull
    // OpenSSL, WinHTTP/libcurl and Qt6::Network into every consumer of
    // pdfe_encryption.
    probe.signatureCount = pdfe::core::countAppliedSignatures(ctx, pdfDoc);
    return probe;
}

thread_local std::string t_lastRemoveEncryptionWarning;

} // namespace

const std::string& lastRemoveEncryptionWarning() {
    return t_lastRemoveEncryptionWarning;
}

void clearLastRemoveEncryptionWarning() {
    t_lastRemoveEncryptionWarning.clear();
}

void EncryptionManager::removeEncryption(
    const pdfe::core::Document* doc,
    const std::filesystem::path& outputPath,
    RemoveEncryptionReport* report
) {
    // Cleared unconditionally and FIRST: a caller must never be able to read a
    // previous call's warning as this call's, not even on the throw paths below.
    clearLastRemoveEncryptionWarning();
    if (report) *report = RemoveEncryptionReport{};

    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    // R2-11: stripping a document's encryption is an owner-only operation.
    // A caller who only holds the USER password can open and view the file but
    // must NOT be able to produce an unprotected copy — that would defeat the
    // owner's permission restrictions entirely. Refuse unless owner authority
    // (authLevel owner or both) was established at open. Unencrypted / non-PDF
    // documents report owner-equivalent authority, so this never blocks the
    // legitimate "save a copy" of an unprotected file.
    if (!doc->isOwnerAuthenticated()) {
        throw core::PdfeException(
            "Removing encryption requires owner authentication; the document "
            "was opened with user-level access only");
    }

    // Removing encryption restructures the document's security; force a
    // full-rewrite + do_garbage=3 path (pipeline b2e47d91, Tiers 1+2).
    doc->markNeedsFullGc();

    pdf_document* pdfDoc = impl_->getPdfDoc(doc);
    if (!pdfDoc) {
        throw core::PdfeException("Not a PDF document");
    }

    // F26 — probe the SOURCE before the rewrite destroys the evidence.
    //
    // The write below uses do_garbage + do_compress: a full rewrite that
    // rebuilds the file from the object graph, moving every byte offset in it.
    // Any /ByteRange on an existing signature then describes a file that no
    // longer exists, so the signature stops verifying — silently, because the
    // signature dictionary itself is copied across intact.
    //
    // This WARNS. It does not refuse. See RemoveEncryptionReport.
    SourceIntegrityProbe probe;
    impl_->context_->execute([pdfDoc, &probe](fz_context* ctx) {
        probe = probeSourceIntegrity(ctx, pdfDoc);
    });

    if (probe.certified || probe.signatureCount > 0) {
        std::string what;
        if (probe.signatureCount > 0) {
            what = std::to_string(probe.signatureCount)
                 + (probe.signatureCount == 1 ? " signature" : " signatures");
            if (probe.certified) what += " and a certification (/Perms /DocMDP)";
        } else {
            what = "a certification (/Perms /DocMDP)";
        }

        // Built here, OUTSIDE any fz_try frame, and not routed through
        // fz_throw — MuPdfContext::execute truncates error text at 256 bytes
        // and the actionable tail is the first thing to fall off the end.
        std::string warning =
            "WARNING: the unencrypted copy's signatures will NOT verify.\n\n"
            "This document carries " + what
            + ". Removing encryption rewrites the whole file (object "
              "garbage-collection and stream re-compression), which discards "
              "the incremental revision chain and moves every byte offset. The "
              "/ByteRange each signature was computed over no longer describes "
              "the file, so \""
            + outputPath.filename().string()
            + "\" will be reported as altered-or-corrupted by any validating "
              "reader even though the signature dictionaries are still "
              "present.\n\nThe original file is unchanged. Keep it if the "
              "signatures must remain verifiable.";

        t_lastRemoveEncryptionWarning = warning;
        if (report) {
            report->sourceSigned = (probe.signatureCount > 0);
            report->sourceCertified = probe.certified;
            report->signatureCount = probe.signatureCount;
            report->rewriteBreaksSignatures = true;
            report->rewriteIntegrityWarning = std::move(warning);
        }
    }

    // Save with do_encrypt = PDF_ENCRYPT_NONE to strip encryption
    std::string pathStr = outputPath.string();
    // Tier 2: helper resolves to 3 (removeEncryption set needsFullGc above).
    const int gcLevel = doc->effectiveGarbageLevel();
    impl_->context_->execute([pdfDoc, &pathStr, gcLevel](fz_context* ctx) {
        pdf_write_options opts{};
        opts.do_garbage = gcLevel;
        opts.do_compress = 1;
        // Same MuPDF /Filter guard as Document::save() (pipeline 3f8a1c72 Phase 1):
        // removing encryption rewrites the file but does not re-encode already-
        // compressed images or re-subset fonts. Kept consistent with saveEncrypted.
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        opts.do_preserve_metadata = 1;
        opts.do_encrypt = PDF_ENCRYPT_NONE;
        pdf_save_document(ctx, pdfDoc, pathStr.c_str(), &opts);
    });
}

} // namespace pdfe::encryption
