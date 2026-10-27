#include "SignatureManager.h"
#include "DocMdpEnforcer.h"
#include "Pkcs7OpenSslBridge.h"
#include "SignatureData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/Error.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <openssl/crypto.h>
#include <QRectF>

namespace pdfe::signatures {

// ============================================================================
// SignatureManager::Impl
// ============================================================================

class SignatureManager::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::unique_ptr<Pkcs7OpenSslBridge> bridge;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx))
        , bridge(std::make_unique<Pkcs7OpenSslBridge>())
    {}

    fz_document* getFzDoc(const pdfe::core::Document* doc) {
        if (!doc) throw pdfe::core::PdfeException("Document is null");
        return doc->fzDocument();
    }

    pdf_document* getPdfDoc(const pdfe::core::Document* doc) {
        fz_document* fzDoc = getFzDoc(doc);
        if (!fzDoc) return nullptr;
        return context->executeReturn<pdf_document*>([fzDoc](fz_context* ctx) {
            return pdf_document_from_fz_document(ctx, fzDoc);
        });
    }

    pdf_page* getPdfPage(const pdfe::core::Page* page) {
        fz_page* fzPage = page->fzPage();
        return context->executeReturn<pdf_page*>([fzPage](fz_context* ctx) {
            return pdf_page_from_fz_page(ctx, fzPage);
        });
    }

    // Get the Nth signature widget (skipping non-signature widgets)
    pdf_annot* getSignatureWidget(fz_context* ctx, pdf_page* pdfPage, int widgetIndex) {
        pdf_annot* widget = pdf_first_widget(ctx, pdfPage);
        int sigIdx = 0;
        while (widget) {
            if (pdf_widget_type(ctx, widget) == PDF_WIDGET_TYPE_SIGNATURE) {
                if (sigIdx == widgetIndex) return widget;
                sigIdx++;
            }
            widget = pdf_next_widget(ctx, widget);
        }
        return nullptr;
    }

    // Count signature widgets on a page
    int countPageSignatures(fz_context* ctx, pdf_page* pdfPage) {
        int count = 0;
        pdf_annot* widget = pdf_first_widget(ctx, pdfPage);
        while (widget) {
            if (pdf_widget_type(ctx, widget) == PDF_WIDGET_TYPE_SIGNATURE) {
                count++;
            }
            widget = pdf_next_widget(ctx, widget);
        }
        return count;
    }

    // Extract SignatureData from a widget
    SignatureData extractWidgetData(fz_context* ctx, pdf_annot* widget,
                                    pdf_document* pdfDoc, int pageIdx, int widgetIdx) {
        SignatureData data;
        data.pageIndex = pageIdx;
        data.widgetIndex = widgetIdx;

        // Get rect
        fz_rect r = pdf_annot_rect(ctx, widget);
        data.rect = QRectF(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);

        // Get field name
        const char* name = pdf_annot_field_label(ctx, widget);
        if (name) data.fieldName = name;

        // Check if signed
        data.isSigned = pdf_widget_is_signed(ctx, widget) != 0;

        return data;
    }
};

// ============================================================================
// SignatureManager
// ============================================================================

SignatureManager::SignatureManager(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{
}

SignatureManager::~SignatureManager() = default;

SignatureData SignatureManager::createSignatureField(
    const pdfe::core::Page* page, const std::string& name, QRectF rect)
{
    if (!page) throw pdfe::core::PdfeException("Page is null");

    pdf_page* pdfPage = impl_->getPdfPage(page);
    SignatureData result;

    // Copy name outside the lambda -- no RAII objects on stack across fz_throw (longjmp).
    // nameCopy lives in createSignatureField's frame, properly unwound by C++ exceptions.
    std::string nameCopy = name;
    impl_->context->execute([&](fz_context* ctx) {
        // Create the signature widget
        // pdf_create_signature_widget takes a char* (non-const in older API)
        pdf_annot* widget = pdf_create_signature_widget(ctx, pdfPage,
            const_cast<char*>(nameCopy.c_str()));
        if (!widget) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Failed to create signature widget");
        }

        // Set the rect
        fz_rect r;
        r.x0 = static_cast<float>(rect.x());
        r.y0 = static_cast<float>(rect.y());
        r.x1 = static_cast<float>(rect.x() + rect.width());
        r.y1 = static_cast<float>(rect.y() + rect.height());
        pdf_set_annot_rect(ctx, widget, r);
        pdf_update_annot(ctx, widget);

        // Extract data
        pdf_document* pdfDoc = pdfPage->doc;
        int widgetIdx = impl_->countPageSignatures(ctx, pdfPage) - 1;
        result = impl_->extractWidgetData(ctx, widget, pdfDoc, page->index(), widgetIdx);
    });

    return result;
}

SignatureData SignatureManager::removeSignatureField(const pdfe::core::Page* page, int widgetIndex)
{
    if (!page) throw pdfe::core::PdfeException("Page is null");

    pdf_page* pdfPage = impl_->getPdfPage(page);
    SignatureData result;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_annot* widget = impl_->getSignatureWidget(ctx, pdfPage, widgetIndex);
        if (!widget) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Signature widget not found at index %d", widgetIndex);
        }

        pdf_document* pdfDoc = pdfPage->doc;
        result = impl_->extractWidgetData(ctx, widget, pdfDoc, page->index(), widgetIndex);

        pdf_delete_annot(ctx, pdfPage, widget);
    });

    return result;
}

int SignatureManager::restoreSignatureField(const pdfe::core::Page* page, const SignatureData& data)
{
    // Re-create the field with the same name and rect
    SignatureData restored = createSignatureField(page, data.fieldName, data.rect);
    return restored.widgetIndex;
}

void SignatureManager::signField(const pdfe::core::Document* doc, const pdfe::core::Page* page,
                                  int widgetIndex, const SigningOptions& optsIn)
{
    // Take a mutable copy so we can zero the password after use
    SigningOptions opts = optsIn;
    if (!doc) throw pdfe::core::PdfeException("Document is null");
    if (!page) throw pdfe::core::PdfeException("Page is null");
    if (opts.pfxPath.empty()) throw pdfe::core::PdfeException("PFX path is empty");

    pdf_page* pdfPage = impl_->getPdfPage(page);
    pdf_document* pdfDoc = impl_->getPdfDoc(doc);
    std::string savePath = doc->filePath().string();

    impl_->context->execute([&](fz_context* ctx) {
        pdf_annot* widget = impl_->getSignatureWidget(ctx, pdfPage, widgetIndex);
        if (!widget) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Signature widget not found at index %d", widgetIndex);
        }

        // Load the PFX signer
        pdf_pkcs7_signer* signer = impl_->bridge->readPfx(ctx, opts.pfxPath.c_str(), opts.pfxPassword.c_str());
        if (!signer) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Failed to load PFX signer");
        }

        // Configure TSA timestamping if requested
        if (opts.useTimestamp && !opts.tsaUrl.empty()) {
            Pkcs7OpenSslBridge::configureTsa(signer, opts.tsaUrl);
        }

        // FieldMDP (/Lock) — meta-audit follow-up F21, gap PDF-33.
        //
        // Written HERE, before pdf_sign_signature, and onto the signature
        // FIELD dictionary rather than the signature value dictionary /V.
        // Both choices matter: the lock has to be inside the signed byte range
        // to be worth anything, and /V must not be touched after the byte-range
        // layout is planned (see the note below — F20 reproduced that
        // corruption twice).
        //
        // DEFECT FOUND BY F21, fixed here: MuPDF's
        // pdf_create_signature_widget stamps /Lock << /Action /All >> onto
        // every signature field it creates. That is a FieldMDP lock over EVERY
        // form field in the document, and PDFE never asked for it. It was
        // invisible only because nothing in PDFE read /Lock back until F21 —
        // a conforming reader does, and would refuse post-signature form
        // filling on a document PDFE had certified at /P 2 in the same save,
        // /P 2 being the level whose entire purpose is to permit it. Now that
        // SigningOptions can express a lock, the ABSENCE of one is expressible
        // too: no lock requested, no lock written.
        {
            pdf_obj* fieldObj = pdf_annot_obj(ctx, widget);
            if (!fieldObj) {
                fz_throw(ctx, FZ_ERROR_GENERIC,
                    "Cannot apply the field lock policy: signature widget has "
                    "no object");
            }
            pdf_dict_del(ctx, fieldObj, PDF_NAME(Lock));
        }
        if (opts.fieldLockAction != FieldLockAction::None) {
            pdf_obj* fieldObj = pdf_annot_obj(ctx, widget);
            pdf_obj* lock = pdf_dict_put_dict(ctx, fieldObj, PDF_NAME(Lock), 3);
            // /SigFieldLock is not in MuPDF's static name table.
            pdf_dict_put_name(ctx, lock, PDF_NAME(Type), "SigFieldLock");
            switch (opts.fieldLockAction) {
            case FieldLockAction::All:
                pdf_dict_put(ctx, lock, PDF_NAME(Action), PDF_NAME(All));
                break;
            case FieldLockAction::Include:
                pdf_dict_put(ctx, lock, PDF_NAME(Action), PDF_NAME(Include));
                break;
            case FieldLockAction::Exclude:
                pdf_dict_put(ctx, lock, PDF_NAME(Action), PDF_NAME(Exclude));
                break;
            case FieldLockAction::None:
                break;
            }
            if (opts.fieldLockAction != FieldLockAction::All) {
                pdf_obj* fields = pdf_dict_put_array(
                    ctx, lock, PDF_NAME(Fields),
                    static_cast<int>(opts.lockedFields.size()));
                for (const std::string& name : opts.lockedFields) {
                    pdf_array_push_text_string(ctx, fields, name.c_str());
                }
            }
        }

        // pdf_sign_signature prepares the signature in memory (stores signer
        // in unsaved_sig list). The actual PKCS7 digest is computed and written
        // during the incremental save below.
        //
        // Invisible signature: pass appearanceFlags=0 so MuPDF skips the
        // visual appearance stream. The widget rect stays as whatever the
        // caller placed (typically zero-area for true invisibility).
        const int appearanceFlags = opts.invisible ? 0 : opts.appearanceFlags;

        pdf_sign_signature(ctx, widget, signer,
                          appearanceFlags,
                          nullptr, // no graphic image
                          opts.reason.empty() ? nullptr : opts.reason.c_str(),
                          opts.location.empty() ? nullptr : opts.location.c_str());

        // Certification signature: stamp /Perms /DocMDP on /Root so
        // readers enforce the certification level. The DocMDP dict is
        // a standalone dict under /Perms carrying /Type /Sig plus
        // /TransformMethod /DocMDP and /TransformParams /P <level>.
        //
        // IMPORTANT: We must NOT modify the signature value dict (/V)
        // after pdf_sign_signature() has planned the byte-range layout.
        // Adding a /Reference array to /V (as the original code did)
        // increases /V's serialized size, causing MuPDF to compute
        // negative byte-range lengths during incremental save. The
        // /Root/Perms/DocMDP entry alone is sufficient for PDF reader
        // certification enforcement per ISO 32000-1:2008 §12.8.2.2.
        if (opts.certification != CertificationLevel::NotCertification) {
            pdf_obj* trailer = pdf_trailer(ctx, pdfDoc);
            pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
            if (!root) {
                fz_throw(ctx, FZ_ERROR_GENERIC,
                    "Cannot certify: document has no /Root");
            }
            pdf_obj* perms = pdf_dict_get(ctx, root, PDF_NAME(Perms));
            if (!perms) {
                perms = pdf_dict_put_dict(ctx, root, PDF_NAME(Perms), 1);
            }
            pdf_obj* docMdp = pdf_dict_put_dict(ctx, perms, PDF_NAME(DocMDP), 4);
            pdf_dict_put(ctx, docMdp, PDF_NAME(Type), PDF_NAME(Sig));
            pdf_dict_put(ctx, docMdp, PDF_NAME(TransformMethod), PDF_NAME(DocMDP));
            pdf_obj* params = pdf_dict_put_dict(ctx, docMdp, PDF_NAME(TransformParams), 2);
            pdf_dict_put_int(ctx, params, PDF_NAME(P),
                             static_cast<int>(opts.certification));
            pdf_dict_put_name(ctx, params, PDF_NAME(V), "1.2");
        }

        // Incremental save to write the signature digest to the file
        pdf_write_options writeOpts{};
        writeOpts.do_incremental = 1;
        pdf_save_document(ctx, pdfDoc, savePath.c_str(), &writeOpts);

        // NOTE: pdf_save_document drops the signer via the unsaved_sig list,
        // so we must NOT call pdf_drop_signer here (it was already dropped
        // when the save completed).
    });

    // Zero password in our copy (defense-in-depth)
    if (!opts.pfxPassword.empty()) {
        OPENSSL_cleanse(opts.pfxPassword.data(), opts.pfxPassword.size());
    }
}

void SignatureManager::clearSignature(const pdfe::core::Page* page, int widgetIndex)
{
    if (!page) throw pdfe::core::PdfeException("Page is null");

    pdf_page* pdfPage = impl_->getPdfPage(page);

    impl_->context->execute([&](fz_context* ctx) {
        pdf_annot* widget = impl_->getSignatureWidget(ctx, pdfPage, widgetIndex);
        if (!widget) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Signature widget not found at index %d", widgetIndex);
        }

        // Only attempt to clear if the widget is actually signed
        if (pdf_widget_is_signed(ctx, widget)) {
            pdf_clear_signature(ctx, widget);
        }
    });
}

SignatureVerifyResult SignatureManager::verifySignature(
    const pdfe::core::Document* doc, const pdfe::core::Page* page, int widgetIndex)
{
    if (!doc) throw pdfe::core::PdfeException("Document is null");
    if (!page) throw pdfe::core::PdfeException("Page is null");

    pdf_page* pdfPage = impl_->getPdfPage(page);
    SignatureVerifyResult result;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_annot* widget = impl_->getSignatureWidget(ctx, pdfPage, widgetIndex);
        if (!widget) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Signature widget not found at index %d", widgetIndex);
        }

        if (!pdf_widget_is_signed(ctx, widget)) {
            result.digestStatus = SignatureError::NotSigned;
            result.certificateStatus = SignatureError::NotSigned;
            result.digestMessage = "Not signed";
            result.certificateMessage = "Not signed";
            return;
        }

        // Create verifier
        pdf_pkcs7_verifier* verifier = impl_->bridge->createVerifier(ctx);
        if (!verifier) {
            result.digestStatus = SignatureError::Unknown;
            result.certificateStatus = SignatureError::Unknown;
            result.digestMessage = "Verifier unavailable";
            result.certificateMessage = "Verifier unavailable";
            return;
        }

        // Check digest
        pdf_signature_error digestErr = pdf_check_widget_digest(ctx, verifier, widget);
        result.digestStatus = signatureErrorFromMuPdf(static_cast<int>(digestErr));
        result.digestValid = (digestErr == PDF_SIGNATURE_ERROR_OKAY);
        char* digestDesc = pdf_signature_error_description(digestErr);
        if (digestDesc) {
            result.digestMessage = digestDesc;
        }

        // Check certificate
        pdf_signature_error certErr = pdf_check_widget_certificate(ctx, verifier, widget);
        result.certificateStatus = signatureErrorFromMuPdf(static_cast<int>(certErr));
        result.certificateValid = (certErr == PDF_SIGNATURE_ERROR_OKAY);
        char* certDesc = pdf_signature_error_description(certErr);
        if (certDesc) {
            result.certificateMessage = certDesc;
        }

        // Check if modified after signing
        result.modifiedAfterSigning =
            pdf_incremental_change_since_signing_widget(ctx, widget) != 0;

        pdf_drop_verifier(ctx, verifier);
    });

    return result;
}

SignatureSummary SignatureManager::verifyAllSignatures(pdfe::core::Document* doc)
{
    if (!doc) throw pdfe::core::PdfeException("Document is null");

    SignatureSummary summary;
    auto signatures = listSignatures(doc);

    summary.totalCount = static_cast<int>(signatures.size());
    summary.signedCount = 0;
    summary.unsignedCount = 0;
    summary.validCount = 0;
    summary.invalidCount = 0;

    for (auto& sig : signatures) {
        if (sig.isSigned) {
            summary.signedCount++;
            auto page = doc->page(sig.pageIndex);
            sig.verification = verifySignature(doc, page.get(), sig.widgetIndex);
            if (sig.verification.isFullyValid()) {
                summary.validCount++;
            } else {
                summary.invalidCount++;
            }
        } else {
            summary.unsignedCount++;
        }
    }

    summary.signatures = std::move(signatures);
    return summary;
}

DocMdpStatus SignatureManager::checkDocMdp(const pdfe::core::Document* doc) const
{
    // PDF-31 / PDF-32: the read-side counterpart to the /Perms /DocMDP stamp
    // written in signField() above. Delegated to DocMdpEnforcer so the /P
    // semantics live in exactly one place.
    DocMdpEnforcer enforcer(impl_->context);
    return enforcer.evaluate(doc);
}

DocumentCoverage SignatureManager::analyzeSignatureCoverage(
    const std::filesystem::path& path) const
{
    // PDF-36 / PDF-37. Delegated so the coverage rule lives in exactly one
    // place, the same way checkDocMdp delegates the /P matrix.
    SignatureCoverageAnalyzer analyzer(impl_->context);
    return analyzer.analyze(path);
}

int SignatureManager::countSignatures(const pdfe::core::Document* doc) const
{
    if (!doc) return 0;

    pdf_document* pdfDoc = impl_->getPdfDoc(doc);
    if (!pdfDoc) return 0;

    return impl_->context->executeReturn<int>([pdfDoc](fz_context* ctx) {
        return pdf_count_signatures(ctx, pdfDoc);
    });
}

std::vector<SignatureData> SignatureManager::listSignatures(pdfe::core::Document* doc)
{
    if (!doc) return {};

    pdf_document* pdfDoc = impl_->getPdfDoc(doc);
    if (!pdfDoc) return {};

    std::vector<SignatureData> result;
    int pageCount = doc->pageCount();

    for (int pi = 0; pi < pageCount; ++pi) {
        auto page = doc->page(pi);
        pdf_page* pdfPage = impl_->getPdfPage(page.get());

        impl_->context->execute([&](fz_context* ctx) {
            pdf_annot* widget = pdf_first_widget(ctx, pdfPage);
            int sigIdx = 0;
            while (widget) {
                if (pdf_widget_type(ctx, widget) == PDF_WIDGET_TYPE_SIGNATURE) {
                    SignatureData data = impl_->extractWidgetData(
                        ctx, widget, pdfDoc, pi, sigIdx);

                    // If signed, try to get signer info
                    if (data.isSigned) {
                        pdf_pkcs7_verifier* verifier = impl_->bridge->createVerifier(ctx);
                        if (verifier) {
                            pdf_pkcs7_distinguished_name* dn =
                                pdf_signature_get_widget_signatory(ctx, verifier, widget);
                            if (dn) {
                                if (dn->cn) data.signer.commonName = dn->cn;
                                if (dn->o) data.signer.organization = dn->o;
                                if (dn->ou) data.signer.orgUnit = dn->ou;
                                if (dn->email) data.signer.email = dn->email;
                                if (dn->c) data.signer.country = dn->c;

                                char* formatted = pdf_signature_format_distinguished_name(ctx, dn);
                                if (formatted) {
                                    data.signer.formattedName = formatted;
                                    fz_free(ctx, formatted);
                                }
                                pdf_signature_drop_distinguished_name(ctx, dn);
                            }
                            pdf_drop_verifier(ctx, verifier);
                        }
                    }

                    result.push_back(std::move(data));
                    sigIdx++;
                }
                widget = pdf_next_widget(ctx, widget);
            }
        });
    }

    return result;
}

SignatureData SignatureManager::getSignatureDetails(
    const pdfe::core::Document* doc, const pdfe::core::Page* page, int widgetIndex)
{
    if (!doc) throw pdfe::core::PdfeException("Document is null");
    if (!page) throw pdfe::core::PdfeException("Page is null");

    pdf_page* pdfPage = impl_->getPdfPage(page);
    pdf_document* pdfDoc = impl_->getPdfDoc(doc);
    SignatureData result;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_annot* widget = impl_->getSignatureWidget(ctx, pdfPage, widgetIndex);
        if (!widget) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Signature widget not found at index %d", widgetIndex);
        }

        result = impl_->extractWidgetData(ctx, widget, pdfDoc, page->index(), widgetIndex);

        if (result.isSigned) {
            pdf_pkcs7_verifier* verifier = impl_->bridge->createVerifier(ctx);
            if (verifier) {
                pdf_pkcs7_distinguished_name* dn =
                    pdf_signature_get_widget_signatory(ctx, verifier, widget);
                if (dn) {
                    if (dn->cn) result.signer.commonName = dn->cn;
                    if (dn->o) result.signer.organization = dn->o;
                    if (dn->ou) result.signer.orgUnit = dn->ou;
                    if (dn->email) result.signer.email = dn->email;
                    if (dn->c) result.signer.country = dn->c;

                    char* formatted = pdf_signature_format_distinguished_name(ctx, dn);
                    if (formatted) {
                        result.signer.formattedName = formatted;
                        fz_free(ctx, formatted);
                    }
                    pdf_signature_drop_distinguished_name(ctx, dn);
                }
                pdf_drop_verifier(ctx, verifier);
            }
        }
    });

    return result;
}

// saveIncremental() was HERE. Removed in the phase-53 adversarial-review
// remediation (finding 3) — see the note at its former declaration in
// SignatureManager.h for what it was, why it went rather than being hardened,
// and what replaced it (pdfe::core::Document::saveIncrementalStaged).
//
// Recorded at the site, deliberately, because that is what phase-49 did for
// LtvDssWriter::writeTo() and the value of the tombstone is that it is where
// someone looking for the function will land.

QImage SignatureManager::previewSignatureAppearance(int w, int h, const SigningOptions& opts)
{
    if (w <= 0 || h <= 0) return {};

    QImage result;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_pkcs7_signer* signer = nullptr;
        if (!opts.pfxPath.empty()) {
            signer = impl_->bridge->readPfx(ctx, opts.pfxPath.c_str(), opts.pfxPassword.c_str());
        }

        fz_pixmap* pix = pdf_preview_signature_as_pixmap(ctx, w, h,
            FZ_LANG_UNSET, signer, opts.appearanceFlags, nullptr,
            opts.reason.empty() ? nullptr : opts.reason.c_str(),
            opts.location.empty() ? nullptr : opts.location.c_str());

        if (pix) {
            int pw = fz_pixmap_width(ctx, pix);
            int ph = fz_pixmap_height(ctx, pix);
            int stride = fz_pixmap_stride(ctx, pix);
            int n = fz_pixmap_components(ctx, pix);
            unsigned char* samples = fz_pixmap_samples(ctx, pix);

            QImage::Format fmt = QImage::Format_Invalid;
            if (n == 4) fmt = QImage::Format_RGBA8888;
            else if (n == 3) fmt = QImage::Format_RGB888;
            else if (n == 2) fmt = QImage::Format_Grayscale8; // gray+alpha, approximate
            else if (n == 1) fmt = QImage::Format_Grayscale8;

            if (fmt != QImage::Format_Invalid && pw > 0 && ph > 0) {
                result = QImage(samples, pw, ph, stride, fmt).copy();
            }
            fz_drop_pixmap(ctx, pix);
        }

        if (signer) pdf_drop_signer(ctx, signer);
    });

    return result;
}

bool SignatureManager::isPkcs7BridgeAvailable() const
{
    return impl_->bridge && impl_->bridge->isAvailable();
}

} // namespace pdfe::signatures
