// -*- coding: utf-8 -*-
#include "DocMdpEnforcer.h"

#include "core/Document.h"
#include "core/Error.h"
#include "core/MuPdfContext.h"
#include "core/PdfSignatureProbe.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <map>
#include <set>
#include <string>
#include <vector>

namespace pdfe::signatures {

const char* docMdpLayoutToString(DocMdpLayout l) {
    switch (l) {
    case DocMdpLayout::None:                  return "none";
    case DocMdpLayout::DirectTransformParams: return "/Perms /DocMDP /TransformParams";
    case DocMdpLayout::IsoSigRefReference:    return "/Perms /DocMDP /Reference /SigRef";
    }
    return "unknown";
}

const char* docMdpChangeClassToString(DocMdpChangeClass c) {
    switch (c) {
    case DocMdpChangeClass::FormFieldValue: return "form field value";
    case DocMdpChangeClass::Annotation:     return "annotation";
    case DocMdpChangeClass::Signature:      return "signature";
    case DocMdpChangeClass::Other:          return "document structure or content";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The /P matrix. ISO 32000-1 Table 254. THIS is what the byte-flip proxies in
// test_signature_roundtrip.cpp never encoded: a change is judged against the
// LEVEL, so the same edit is a violation at P1 and lawful at P3.
// ---------------------------------------------------------------------------
bool DocMdpEnforcer::changePermittedAt(DocMdpChangeClass changeClass,
                                       CertificationLevel level) {
    switch (level) {
    case CertificationLevel::NotCertification:
        // No certification in force — nothing to violate.
        return true;
    case CertificationLevel::NoChanges:
        // P1: "no changes to the document shall be permitted". Not form fill,
        // not annotation, not a further signature.
        return false;
    case CertificationLevel::FormFillOnly:
        // P2: filling in forms, instantiating page templates, and signing.
        return changeClass == DocMdpChangeClass::FormFieldValue
            || changeClass == DocMdpChangeClass::Signature;
    case CertificationLevel::AnnotationsAllowed:
        // P3: the P2 set plus annotation create/delete/modify.
        return changeClass == DocMdpChangeClass::FormFieldValue
            || changeClass == DocMdpChangeClass::Signature
            || changeClass == DocMdpChangeClass::Annotation;
    }
    return false;
}

namespace {

// Object-number sets built from the CURRENT state of the document, used to
// classify a revised object without loading an older revision of it.
struct ObjectRoles {
    std::set<int> pageDicts;
    std::set<int> contentStreams;   // every page's /Contents
    std::set<int> pageResources;    // every page's /Resources (content-side)
    std::set<int> annots;           // non-widget annotations
    std::set<int> annotArrays;      // indirect /Annots arrays
    std::set<int> widgets;          // widget annotations (form fields on a page)
    std::set<int> sigObjects;       // signature fields, their widgets and /V
    std::set<int> formFields;       // AcroForm field dicts (incl. /Kids)
    std::set<int> appearanceStreams;// /AP streams reachable from an annot/widget
    /// Objects hanging off the AcroForm /DR (the form's DEFAULT RESOURCES) and
    /// off an /AP stream's own /Resources. These are the fonts and XObjects a
    /// widget appearance is drawn WITH, so generating the appearance for a
    /// permitted form fill legitimately creates or revises them.
    std::set<int> formResources;
    /// page-dict object number -> the object numbers hanging off its /Annots.
    /// Used to tell an /Annots edit apart from any other page-dict edit
    /// (a /Rotate change, a /Contents swap, a /MediaBox change) without
    /// needing the pre-certification revision of the page.
    std::map<int, std::set<int>> pageAnnots;
    int acroFormNum = 0;
    int catalogNum = 0;

    // --- FieldMDP (/Lock), gap PDF-33 ---
    /// Field names named by a signature's /Lock /Fields.
    std::set<std::string> lockNames;
    bool lockAll = false;
    bool lockExclude = false;
    bool lockPresent = false;
    /// Object numbers belonging to a NON-signature form field, keyed by the
    /// field's fully-qualified name: the field dict, its kid widgets, their
    /// /AP streams and its /V value object.
    std::map<std::string, std::set<int>> fieldObjects;
    /// Resolved from the three above once the whole form has been walked.
    std::set<int> lockedObjects;
};

int refNum(fz_context* ctx, pdf_obj* o) {
    if (!o) return 0;
    return pdf_to_num(ctx, o);
}

// NOTE: a whole-xref "does anything reference this object?" scan used to live
// here, used to suppress the structural-change report for objects that looked
// orphaned. It has been REMOVED, not merely disabled. It was introduced to stop
// a permitted DocMDP /P 2 form fill being reported BROKEN by the appearance font
// that regeneration writes — but that font was never orphaned; the scan was two
// levels deep and could not see the 3-hop AP-stream -> /Resources -> /Font chain.
// Suppressing unreferenced-looking objects would also have been a standing hole
// in the enforcement. The real fix classifies appearance RESOURCES as appearance
// objects (collectApResources). Do not reintroduce a reachability escape hatch.

// Collect every indirect object number reachable through the INLINE structure
// of `o` (dicts/arrays written directly inside it). An indirect value is
// recorded and NOT followed — the object it names gets classified on its own
// terms; we only want the edge, not the whole reachable graph.
void collectInlineRefs(fz_context* ctx, pdf_obj* o, std::set<int>& out,
                       int depth) {
    constexpr int kMaxDepth = 32;
    if (!o || depth > kMaxDepth) return;
    if (pdf_is_indirect(ctx, o)) {
        const int n = pdf_to_num(ctx, o);
        if (n > 0) out.insert(n);
        return;
    }
    if (pdf_is_dict(ctx, o)) {
        const int kn = pdf_dict_len(ctx, o);
        for (int k = 0; k < kn; ++k) {
            collectInlineRefs(ctx, pdf_dict_get_val(ctx, o, k), out, depth + 1);
        }
    } else if (pdf_is_array(ctx, o)) {
        const int an = pdf_array_len(ctx, o);
        for (int j = 0; j < an; ++j) {
            collectInlineRefs(ctx, pdf_array_get(ctx, o, j), out, depth + 1);
        }
    }
}

// An appearance stream's /Resources are PART OF THE APPEARANCE. Regenerating a
// widget's appearance on a permitted form fill legitimately creates or revises
// the font it draws with, so those objects must classify as appearance objects
// too — otherwise a DocMDP /P 2 document is reported BROKEN by the very fill
// that /P 2 exists to permit.
//
// This replaces an earlier attempt that instead suppressed the report whenever
// the object appeared to have no referrer. That was wrong twice over: the
// referrer scan was too shallow to see a 3-hop inline chain, so the font only
// LOOKED orphaned; and suppressing unreferenced-looking objects would have been
// a general hole in the enforcement rather than a fact about appearances.
// (phase-15 karen gate.)
void collectApResources(fz_context* ctx, pdf_obj* stream, std::set<int>& out) {
    pdf_obj* s = pdf_resolve_indirect(ctx, stream);
    if (!pdf_is_dict(ctx, s)) return;
    pdf_obj* resources = pdf_dict_get(ctx, s, PDF_NAME(Resources));
    if (!resources) return;
    const int rnum = refNum(ctx, resources);
    if (rnum > 0) out.insert(rnum);
    collectInlineRefs(ctx, resources, out, 0);
    // If /Resources is itself indirect, its CONTENTS are inline within that
    // object and still name the fonts/XObjects the appearance draws with.
    pdf_obj* rres = pdf_resolve_indirect(ctx, resources);
    if (rres != resources) collectInlineRefs(ctx, rres, out, 0);
}

void collectApStreams(fz_context* ctx, pdf_obj* annot, std::set<int>& out) {
    pdf_obj* ap = pdf_dict_get(ctx, annot, PDF_NAME(AP));
    if (!pdf_is_dict(ctx, ap)) return;
    const int n = pdf_dict_len(ctx, ap);
    for (int i = 0; i < n; ++i) {
        pdf_obj* v = pdf_dict_get_val(ctx, ap, i);
        if (!v) continue;
        const int num = refNum(ctx, v);
        if (num > 0) out.insert(num);
        // Pull in this appearance's own /Resources. Called unconditionally on
        // the value: pdf_is_stream() must be given the INDIRECT object (it
        // resolves the number against the xref) and answers FALSE for an
        // already-resolved dict, so gating on it here silently skipped every
        // AP stream — which is precisely why the appearance font kept
        // classifying as a structural change. collectApResources is a no-op
        // for anything without /Resources, so calling it always is safe.
        collectApResources(ctx, v, out);

        pdf_obj* res = pdf_resolve_indirect(ctx, v);
        if (pdf_is_dict(ctx, res)) {
            // /N may be a sub-dictionary of appearance states, each value of
            // which is itself an appearance stream.
            const int m = pdf_dict_len(ctx, res);
            for (int j = 0; j < m; ++j) {
                pdf_obj* sv = pdf_dict_get_val(ctx, res, j);
                const int sub = refNum(ctx, sv);
                if (sub > 0) out.insert(sub);
                collectApResources(ctx, sv, out);
            }
        }
    }
}

// /FT is inheritable through /Parent. The traversal used to live here as a
// private copy; meta-audit F26's security-audit gate found that the THIRD
// implementation of "where are the signatures?" (EncryptionManager's
// rewrite-integrity probe) had drifted narrower than this one and under-warned
// as a result. There is now one home for the predicate — pdfe::core — and this
// file reads it from there rather than keeping its own.
using pdfe::core::pdfFieldIsSignature;

inline bool fieldIsSignature(fz_context* ctx, pdf_obj* field) {
    return pdfFieldIsSignature(ctx, field);
}

// FieldMDP (gap PDF-33). Read a signature field's /Lock dictionary
// (ISO 32000-1 §12.7.4.5, Table 233) into `roles`. The lock is authored BEFORE
// signing and therefore lies inside the signed byte range, which is what makes
// it trustworthy: an attacker who edits it invalidates the digest.
//
// Both spellings are read. Acrobat and pyHanko write the /Lock dict on the
// signature FIELD and mirror it as a FieldMDP transform in the signature value
// dictionary's /Reference array; either one alone is enough to establish the
// lock, so a document carrying only the /Reference form is still enforced.
void collectFieldLock(fz_context* ctx, pdf_obj* sigField, ObjectRoles& roles) {
    auto absorb = [&](pdf_obj* action, pdf_obj* fields) {
        if (!action) return;
        roles.lockPresent = true;
        if (pdf_name_eq(ctx, action, PDF_NAME(All))) {
            roles.lockAll = true;
        } else if (pdf_name_eq(ctx, action, PDF_NAME(Exclude))) {
            roles.lockExclude = true;
        }
        pdf_obj* arr = pdf_resolve_indirect(ctx, fields);
        if (!pdf_is_array(ctx, arr)) return;
        const int n = pdf_array_len(ctx, arr);
        for (int i = 0; i < n; ++i) {
            const char* s = pdf_array_get_text_string(ctx, arr, i);
            if (s && *s) roles.lockNames.insert(std::string(s));
        }
    };

    pdf_obj* lock = pdf_dict_get(ctx, sigField, PDF_NAME(Lock));
    if (pdf_is_dict(ctx, lock)) {
        absorb(pdf_dict_get(ctx, lock, PDF_NAME(Action)),
               pdf_dict_get(ctx, lock, PDF_NAME(Fields)));
    }

    pdf_obj* v = pdf_resolve_indirect(ctx,
                                      pdf_dict_get(ctx, sigField, PDF_NAME(V)));
    pdf_obj* refs = pdf_resolve_indirect(ctx,
                                         pdf_dict_get(ctx, v, PDF_NAME(Reference)));
    if (!pdf_is_array(ctx, refs)) return;
    const int rn = pdf_array_len(ctx, refs);
    for (int i = 0; i < rn; ++i) {
        pdf_obj* entry = pdf_resolve_indirect(ctx, pdf_array_get(ctx, refs, i));
        if (!pdf_is_dict(ctx, entry)) continue;
        pdf_obj* method = pdf_dict_get(ctx, entry, PDF_NAME(TransformMethod));
        if (!pdf_name_eq(ctx, method, PDF_NAME(FieldMDP))) continue;
        pdf_obj* params = pdf_resolve_indirect(
            ctx, pdf_dict_get(ctx, entry, PDF_NAME(TransformParams)));
        absorb(pdf_dict_get(ctx, params, PDF_NAME(Action)),
               pdf_dict_get(ctx, params, PDF_NAME(Fields)));
    }
}

void collectFields(fz_context* ctx, pdf_obj* fieldsArray, ObjectRoles& roles,
                   int depth, const std::string& prefix) {
    if (depth > 16 || !pdf_is_array(ctx, fieldsArray)) return;
    const int n = pdf_array_len(ctx, fieldsArray);
    for (int i = 0; i < n; ++i) {
        pdf_obj* ref = pdf_array_get(ctx, fieldsArray, i);
        const int num = refNum(ctx, ref);
        pdf_obj* field = pdf_resolve_indirect(ctx, ref);
        if (!pdf_is_dict(ctx, field)) continue;
        const bool isSig = fieldIsSignature(ctx, field);
        if (num > 0) {
            roles.formFields.insert(num);
            if (isSig) roles.sigObjects.insert(num);
        }
        // Fully-qualified name: partial names joined by '.' down the /Kids
        // chain, exactly as /Lock /Fields names them.
        std::string name = prefix;
        const char* partial = pdf_dict_get_text_string(ctx, field, PDF_NAME(T));
        if (partial && *partial) {
            if (!name.empty()) name.push_back('.');
            name.append(partial);
        }
        // The signature value dictionary itself.
        pdf_obj* v = pdf_dict_get(ctx, field, PDF_NAME(V));
        if (v && isSig) {
            const int vnum = refNum(ctx, v);
            if (vnum > 0) roles.sigObjects.insert(vnum);
        }
        if (!isSig && !name.empty()) {
            std::set<int>& owned = roles.fieldObjects[name];
            if (num > 0) owned.insert(num);
            const int vnum = refNum(ctx, v);
            if (vnum > 0) owned.insert(vnum);
            collectApStreams(ctx, field, owned);
        }
        if (isSig) collectFieldLock(ctx, field, roles);
        collectApStreams(ctx, field, roles.appearanceStreams);
        collectFields(ctx, pdf_dict_get(ctx, field, PDF_NAME(Kids)), roles,
                      depth + 1, name);
    }
}

/// Resolve /Lock /Action + /Fields into the concrete object numbers a change
/// to which breaks the field lock.
void resolveLockedObjects(ObjectRoles& roles) {
    if (!roles.lockPresent) return;
    for (const auto& [name, objs] : roles.fieldObjects) {
        bool locked;
        if (roles.lockAll) {
            locked = true;
        } else if (roles.lockExclude) {
            locked = roles.lockNames.count(name) == 0;
        } else {
            // /Action /Include — and a named parent locks its children, which
            // is how a hierarchical form names a whole group in one entry.
            locked = roles.lockNames.count(name) != 0;
            if (!locked) {
                for (const std::string& n : roles.lockNames) {
                    if (name.size() > n.size() && name.compare(0, n.size(), n) == 0
                        && name[n.size()] == '.') {
                        locked = true;
                        break;
                    }
                }
            }
        }
        if (locked) roles.lockedObjects.insert(objs.begin(), objs.end());
    }
}

void buildObjectRoles(fz_context* ctx, pdf_document* doc, ObjectRoles& roles) {
    pdf_obj* trailer = pdf_trailer(ctx, doc);
    pdf_obj* rootRef = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
    roles.catalogNum = refNum(ctx, rootRef);
    pdf_obj* root = pdf_resolve_indirect(ctx, rootRef);

    pdf_obj* acroRef = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
    roles.acroFormNum = refNum(ctx, acroRef);
    pdf_obj* acro = pdf_resolve_indirect(ctx, acroRef);
    if (pdf_is_dict(ctx, acro)) {
        collectFields(ctx, pdf_dict_get(ctx, acro, PDF_NAME(Fields)), roles, 0,
                      std::string());
        // The AcroForm /DR — default resources. MuPDF's appearance generator
        // puts the font it draws a filled field with HERE, not (only) in the
        // /AP stream's own /Resources, so without this a permitted /P 2 fill
        // reports its own appearance font as a structural change and calls the
        // certification BROKEN. Verified by instrumentation: the object left
        // unclassified was exactly a /Type /Subtype /BaseFont /Encoding font
        // dict reachable through /DR.
        pdf_obj* dr = pdf_dict_get(ctx, acro, PDF_NAME(DR));
        if (dr) {
            const int drnum = refNum(ctx, dr);
            if (drnum > 0) roles.formResources.insert(drnum);
            collectInlineRefs(ctx, dr, roles.formResources, 0);
            pdf_obj* drRes = pdf_resolve_indirect(ctx, dr);
            if (drRes != dr) {
                collectInlineRefs(ctx, drRes, roles.formResources, 0);
            }
        }
    }

    const int pageCount = pdf_count_pages(ctx, doc);
    for (int p = 0; p < pageCount; ++p) {
        pdf_obj* page = pdf_lookup_page_obj(ctx, doc, p);
        if (!pdf_is_dict(ctx, page)) continue;
        const int pnum = pdf_to_num(ctx, page);
        if (pnum > 0) roles.pageDicts.insert(pnum);

        pdf_obj* contents = pdf_dict_get(ctx, page, PDF_NAME(Contents));
        if (pdf_is_array(ctx, contents)) {
            const int n = pdf_array_len(ctx, contents);
            for (int i = 0; i < n; ++i) {
                const int num = refNum(ctx, pdf_array_get(ctx, contents, i));
                if (num > 0) roles.contentStreams.insert(num);
            }
        } else if (contents) {
            const int num = refNum(ctx, contents);
            if (num > 0) roles.contentStreams.insert(num);
        }

        const int resNum = refNum(ctx, pdf_dict_get(ctx, page, PDF_NAME(Resources)));
        if (resNum > 0) roles.pageResources.insert(resNum);

        pdf_obj* annots = pdf_dict_get(ctx, page, PDF_NAME(Annots));
        const int annotsArrNum = refNum(ctx, annots);
        if (annotsArrNum > 0) {
            roles.annotArrays.insert(annotsArrNum);
            if (pnum > 0) roles.pageAnnots[pnum].insert(annotsArrNum);
        }
        pdf_obj* annotsArr = pdf_resolve_indirect(ctx, annots);
        if (pdf_is_array(ctx, annotsArr)) {
            const int n = pdf_array_len(ctx, annotsArr);
            for (int i = 0; i < n; ++i) {
                pdf_obj* aref = pdf_array_get(ctx, annotsArr, i);
                const int num = refNum(ctx, aref);
                pdf_obj* annot = pdf_resolve_indirect(ctx, aref);
                if (!pdf_is_dict(ctx, annot)) continue;
                pdf_obj* subtype = pdf_dict_get(ctx, annot, PDF_NAME(Subtype));
                const bool isWidget = pdf_name_eq(ctx, subtype, PDF_NAME(Widget));
                if (num > 0) {
                    if (isWidget) {
                        roles.widgets.insert(num);
                        if (fieldIsSignature(ctx, annot)) roles.sigObjects.insert(num);
                    } else {
                        roles.annots.insert(num);
                    }
                    if (pnum > 0) roles.pageAnnots[pnum].insert(num);
                }
                collectApStreams(ctx, annot, roles.appearanceStreams);
                if (isWidget && !fieldIsSignature(ctx, annot)) {
                    // A widget reached from the page may be the merged
                    // field/widget object of a field named through /Parent.
                    // Attribute it to that field so a /Lock naming the field
                    // also covers the widget and its appearance.
                    std::string name;
                    pdf_obj* node = annot;
                    for (int d = 0; node && d < 32; ++d) {
                        const char* t = pdf_dict_get_text_string(ctx, node,
                                                                 PDF_NAME(T));
                        if (t && *t) {
                            name = name.empty() ? std::string(t)
                                                : std::string(t) + "." + name;
                        }
                        node = pdf_dict_get(ctx, node, PDF_NAME(Parent));
                    }
                    if (!name.empty() && num > 0) {
                        std::set<int>& owned = roles.fieldObjects[name];
                        owned.insert(num);
                        collectApStreams(ctx, annot, owned);
                        const int vnum = refNum(ctx, pdf_dict_get(ctx, annot,
                                                                  PDF_NAME(V)));
                        if (vnum > 0) owned.insert(vnum);
                    }
                }
                if (isWidget) {
                    pdf_obj* v = pdf_dict_get(ctx, annot, PDF_NAME(V));
                    const int vnum = refNum(ctx, v);
                    if (vnum > 0 && fieldIsSignature(ctx, annot)) {
                        roles.sigObjects.insert(vnum);
                    }
                }
            }
        }
    }

    resolveLockedObjects(roles);
}

// Classify a single revised object. Allocation-free: it is invoked from inside
// an fz_try, so a longjmp past it must not skip a destructor. `revised` is the
// full set of object numbers written after the certifying signature, needed to
// disambiguate a page-dictionary revision.
DocMdpChangeClass classifyObject(fz_context* ctx, pdf_document* doc, int num,
                                 const ObjectRoles& roles,
                                 const std::set<int>& revised) {
    // Order matters. A page's content stream is checked before the page dict,
    // and the signature set before the generic widget set.
    if (roles.contentStreams.count(num)) return DocMdpChangeClass::Other;
    if (roles.pageResources.count(num))  return DocMdpChangeClass::Other;
    if (roles.sigObjects.count(num))     return DocMdpChangeClass::Signature;
    if (roles.annots.count(num))         return DocMdpChangeClass::Annotation;
    if (roles.annotArrays.count(num))    return DocMdpChangeClass::Annotation;
    if (roles.widgets.count(num))        return DocMdpChangeClass::FormFieldValue;
    if (roles.formFields.count(num))     return DocMdpChangeClass::FormFieldValue;
    if (num == roles.acroFormNum && num != 0) return DocMdpChangeClass::FormFieldValue;
    if (roles.appearanceStreams.count(num)) return DocMdpChangeClass::FormFieldValue;
    // Appearance RESOURCES (AcroForm /DR + an /AP stream's own /Resources).
    // Scope note, stated deliberately: this makes a revision to a form's
    // default font permitted at /P 2. That is inherent to permitting form fill
    // at all — the appearance cannot be regenerated without the font it draws
    // with — and it is confined to FORM resources. Page /Resources are a
    // separate set (roles.pageResources) and remain structural, so this does
    // not make page content re-styleable under a certification.
    if (roles.formResources.count(num)) return DocMdpChangeClass::FormFieldValue;

    if (roles.pageDicts.count(num)) {
        // A page-dictionary revision is ambiguous on its face: adding an
        // annotation rewrites /Annots, but so does a /Rotate change, a
        // /MediaBox change or a /Contents swap — and those are structural
        // edits no certification level permits.
        //
        // Disambiguate WITHOUT needing the pre-certification revision of the
        // page: a page dict revised *together with* one of its own annotation
        // objects (or its /Annots array) is an annotation edit; a page dict
        // revised alone is a page-level structural edit. This is what makes
        // the /P discrimination real — a P3 document survives an annotation
        // add and is still reported broken by a page rotate.
        auto it = roles.pageAnnots.find(num);
        if (it != roles.pageAnnots.end()) {
            for (int annotNum : it->second) {
                if (revised.count(annotNum)) return DocMdpChangeClass::Annotation;
            }
        }
        return DocMdpChangeClass::Other;
    }

    // Fall back to inspecting the object itself.
    pdf_obj* o = pdf_load_object(ctx, doc, num);
    DocMdpChangeClass result = DocMdpChangeClass::Other;
    if (pdf_is_dict(ctx, o)) {
        pdf_obj* type = pdf_dict_get(ctx, o, PDF_NAME(Type));
        pdf_obj* subtype = pdf_dict_get(ctx, o, PDF_NAME(Subtype));
        if (pdf_name_eq(ctx, type, PDF_NAME(Annot))) {
            if (pdf_name_eq(ctx, subtype, PDF_NAME(Widget))) {
                result = fieldIsSignature(ctx, o)
                             ? DocMdpChangeClass::Signature
                             : DocMdpChangeClass::FormFieldValue;
            } else {
                result = DocMdpChangeClass::Annotation;
            }
        } else if (pdf_name_eq(ctx, type, PDF_NAME(Sig))) {
            result = DocMdpChangeClass::Signature;
        } else if (pdf_dict_get(ctx, o, PDF_NAME(FT))) {
            result = fieldIsSignature(ctx, o)
                         ? DocMdpChangeClass::Signature
                         : DocMdpChangeClass::FormFieldValue;
        }
    }
    pdf_drop_obj(ctx, o);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------

DocMdpEnforcer::DocMdpEnforcer(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : context_(std::move(context)) {}

DocMdpEnforcer::~DocMdpEnforcer() = default;

DocMdpLevelReading readDocMdpLevel(fz_context* ctx, pdf_document* doc) {
    DocMdpLevelReading r;
    if (!ctx || !doc) return r;

    int levelValue = 0;
    pdf_obj* trailer = pdf_trailer(ctx, doc);
    pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
    pdf_obj* perms = pdf_dict_get(ctx, root, PDF_NAME(Perms));
    pdf_obj* docMdp = pdf_resolve_indirect(
        ctx, pdf_dict_get(ctx, perms, PDF_NAME(DocMDP)));
    if (!pdf_is_dict(ctx, docMdp)) return r;
    r.certified = true;

    pdf_obj* params = pdf_resolve_indirect(
        ctx, pdf_dict_get(ctx, docMdp, PDF_NAME(TransformParams)));
    if (pdf_is_dict(ctx, params)) {
        r.layout = DocMdpLayout::DirectTransformParams;
        levelValue = pdf_dict_get_int_default(ctx, params, PDF_NAME(P), 2);
    } else {
        pdf_obj* refs = pdf_resolve_indirect(
            ctx, pdf_dict_get(ctx, docMdp, PDF_NAME(Reference)));
        bool found = false;
        if (pdf_is_array(ctx, refs)) {
            const int n = pdf_array_len(ctx, refs);
            for (int i = 0; i < n && !found; ++i) {
                pdf_obj* entry =
                    pdf_resolve_indirect(ctx, pdf_array_get(ctx, refs, i));
                if (!pdf_is_dict(ctx, entry)) continue;
                pdf_obj* method =
                    pdf_dict_get(ctx, entry, PDF_NAME(TransformMethod));
                if (!pdf_name_eq(ctx, method, PDF_NAME(DocMDP))) continue;
                pdf_obj* tp = pdf_resolve_indirect(
                    ctx, pdf_dict_get(ctx, entry, PDF_NAME(TransformParams)));
                r.layout = DocMdpLayout::IsoSigRefReference;
                levelValue = pdf_dict_get_int_default(ctx, tp, PDF_NAME(P), 2);
                found = true;
            }
        }
        if (!found) {
            // Certified, but neither layout carries a /P. ISO 32000-1
            // Table 254: /P is optional and defaults to 2.
            levelValue = 2;
        }
    }

    if (levelValue < 1 || levelValue > 3) levelValue = 2;
    r.level = static_cast<CertificationLevel>(levelValue);
    return r;
}

DocMdpStatus DocMdpEnforcer::evaluate(const pdfe::core::Document* doc) const {
    DocMdpStatus status;
    if (!doc) {
        status.message = "No document";
        return status;
    }
    fz_document* fzDoc = doc->fzDocument();
    if (!fzDoc) {
        status.message = "Not a PDF document";
        return status;
    }

    pdf_document* pdfDoc = context_->executeReturn<pdf_document*>(
        [fzDoc](fz_context* ctx) { return pdf_document_from_fz_document(ctx, fzDoc); });
    if (!pdfDoc) {
        status.message = "Not a PDF document";
        return status;
    }

    // --- 1. Is a certification in force, and at what level? ---
    //
    // TWO layouts exist and PDFE used to understand only the first:
    //
    //   a) PDFE's own writer stamps /Root /Perms /DocMDP as a standalone dict
    //      carrying /TransformMethod and /TransformParams DIRECTLY.
    //   b) ISO 32000-1 §12.8.2.2 — and Acrobat, and pyHanko — make /Perms
    //      /DocMDP the SIGNATURE VALUE dictionary, whose /Reference array
    //      holds a /SigRef with /TransformMethod /DocMDP and the /P inside its
    //      /TransformParams.
    //
    // Reading only (a) meant a third-party P1 ("no changes") document fell
    // through to the /P default of 2 and was enforced as though form filling
    // and further signing were permitted — under-enforcement, silently. Found
    // by the F21 adversarial corpus (gap PDF-36/PDF-37 fixtures are authored
    // in layout (b), because that is what the rest of the world writes).
    //
    // The decode itself lives in readDocMdpLevel() so that the direct-write
    // guard (the engines that call pdf_save_document themselves) shares it
    // rather than growing a second, drift-prone reading of the same bytes.
    DocMdpLevelReading reading;
    context_->execute([&](fz_context* ctx) {
        reading = readDocMdpLevel(ctx, pdfDoc);
    });

    status.certified = reading.certified;
    status.layout = reading.layout;
    if (!reading.certified) {
        status.level = CertificationLevel::NotCertification;
        status.message = "Document is not certified (no /Perms /DocMDP)";
        return status;
    }
    status.level = reading.level;

    // --- 2. Which objects were revised after the certifying signature? ---
    //
    // MuPDF version numbering: 0 = the newest revision, 1 = the one before it,
    // and so on. The certifying signature's value dictionary was written in
    // the same incremental update as the /Perms /DocMDP stamp, so any object
    // whose version number is SMALLER than the signature's was written later.
    struct RawChange { int num; int version; };
    std::vector<RawChange> raw;
    int sigVersion = -1;
    int catalogVersion = -1;
    int unsaved = 0;
    ObjectRoles roles;

    int totalVersions = 0;
    std::vector<int> sigObjVersions;
    context_->execute([&](fz_context* ctx) {
        unsaved = pdf_count_unsaved_versions(ctx, pdfDoc);
        totalVersions = pdf_count_versions(ctx, pdfDoc);
        buildObjectRoles(ctx, pdfDoc, roles);

        // Version of the certifying signature. Prefer the signature value
        // dictionary; fall back to the catalog, which the DocMDP stamp itself
        // rewrote.
        for (int num : roles.sigObjects) {
            pdf_obj* ind = pdf_new_indirect(ctx, pdfDoc, num, 0);
            int v = -1;
            fz_try(ctx) {
                v = pdf_find_version_for_obj(ctx, pdfDoc, ind);
            }
            fz_always(ctx) {
                pdf_drop_obj(ctx, ind);
            }
            fz_catch(ctx) {
                v = -1;
            }
            sigObjVersions.push_back(v);
            if (v >= 0 && (sigVersion < 0 || v < sigVersion)) sigVersion = v;
        }
        // Version of the catalog, which is where the /Perms /DocMDP stamp
        // lives. MuPDF's signing save emits the stamp in a SEPARATE incremental
        // xref section from the signature value dictionary (verified: a
        // certified test_single.pdf has three revisions — base, signature,
        // then a catalog-only section carrying /Perms). Certification is not
        // established until BOTH exist, so the anchor is the newer of the two.
        //
        // The advance is deliberately capped at ONE section: the stamp is
        // written by the same save operation as the signature, so it can never
        // be more than one revision newer. A LATER update that rewrites the
        // catalog therefore does not move the anchor — it is reported as the
        // structural change it is, instead of silently becoming the new
        // baseline and hiding everything after it.
        if (roles.catalogNum > 0) {
            pdf_obj* ind = pdf_new_indirect(ctx, pdfDoc, roles.catalogNum, 0);
            int v = -1;
            fz_try(ctx) {
                v = pdf_find_version_for_obj(ctx, pdfDoc, ind);
            }
            fz_always(ctx) {
                pdf_drop_obj(ctx, ind);
            }
            fz_catch(ctx) {
                v = -1;
            }
            catalogVersion = v;
        }
        if (sigVersion < 0) {
            sigVersion = catalogVersion;
        } else if (catalogVersion >= 0 && catalogVersion == sigVersion - 1) {
            sigVersion = catalogVersion;
        }
        if (sigVersion < 0) return;

        const int xrefLen = pdf_xref_len(ctx, pdfDoc);
        for (int num = 1; num < xrefLen; ++num) {
            pdf_obj* ind = pdf_new_indirect(ctx, pdfDoc, num, 0);
            int v = -1;
            fz_try(ctx) {
                v = pdf_find_version_for_obj(ctx, pdfDoc, ind);
            }
            fz_always(ctx) {
                pdf_drop_obj(ctx, ind);
            }
            fz_catch(ctx) {
                v = -1;
            }
            if (v < 0 || v >= sigVersion) continue;
            raw.push_back(RawChange{num, v});
        }
    });

    status.unsavedVersions = unsaved;
    status.certificationVersion = sigVersion;
    status.totalVersions = totalVersions;

    if (sigVersion < 0) {
        status.message = "Certified, but the certifying signature's revision "
                         "could not be located";
        return status;
    }

    // The full revised set, needed to disambiguate page-dictionary revisions.
    std::set<int> revisedSet;
    for (const auto& rc : raw) revisedSet.insert(rc.num);

    for (const auto& rc : raw) {
        DocMdpChangeClass cls = DocMdpChangeClass::Other;
        context_->execute([&](fz_context* ctx) {
            fz_try(ctx) {
                cls = classifyObject(ctx, pdfDoc, rc.num, roles, revisedSet);
            }
            fz_catch(ctx) {
                cls = DocMdpChangeClass::Other;
            }
        });

        // NOTE: there is deliberately NO "is this object referenced anywhere?"
        // escape hatch here. An earlier version suppressed the report for any
        // Other-classified object that appeared unreferenced, to stop a
        // permitted /P 2 form fill being called BROKEN. That was the wrong
        // remedy: the object (an appearance font) was not actually orphaned —
        // the referrer scan was simply too shallow to see it — and suppressing
        // unreferenced-looking objects would have let a real structural change
        // hide behind the same rule. The correct fix classifies appearance
        // RESOURCES as appearance objects; see collectApResources.

        // FieldMDP (PDF-33): a locked field's objects may not be revised at
        // ANY level. This is what /P alone cannot express — /P 2 permits form
        // filling in general, and a /Lock is how the signer says "not THIS
        // field".
        const bool locked = roles.lockedObjects.count(rc.num) != 0;

        DocMdpChange change;
        change.objectNumber = rc.num;
        change.version = rc.version;
        change.changeClass = cls;
        change.touchesLockedField = locked;
        change.description = std::string("object ") + std::to_string(rc.num)
                             + " revised after certification ("
                             + docMdpChangeClassToString(cls)
                             + (locked ? ", FIELD IS LOCKED" : "") + ")";
        status.changes.push_back(change);
        if (locked) {
            status.lockedFieldChanges.push_back(change);
            status.disallowedChanges.push_back(change);
        } else if (!changePermittedAt(cls, status.level)) {
            status.disallowedChanges.push_back(change);
        }
    }

    status.allFieldsLocked = roles.lockAll;
    status.fieldLockIsExclusion = roles.lockExclude;
    for (const std::string& n : roles.lockNames) {
        status.lockedFields.push_back(n);
    }
    status.fieldLockViolated = !status.lockedFieldChanges.empty();

    status.changedSinceCertification = !status.changes.empty();
    status.violated = !status.disallowedChanges.empty();

    if (status.fieldLockViolated) {
        status.message = "FieldMDP: a LOCKED form field was changed after "
                         "signing (" + std::to_string(
                             status.lockedFieldChanges.size())
                       + " object(s)); certification (DocMDP /P "
                       + std::to_string(static_cast<int>(status.level)) + ") is BROKEN";
    } else if (status.violated) {
        status.message = "Certification (DocMDP /P "
                         + std::to_string(static_cast<int>(status.level)) + ") is BROKEN: "
                         + std::to_string(status.disallowedChanges.size())
                         + " disallowed change(s), first is "
                         + status.disallowedChanges.front().description;
    } else if (status.changedSinceCertification) {
        status.message = "Certification (DocMDP /P " + std::to_string(static_cast<int>(status.level))
                         + ") intact: " + std::to_string(status.changes.size())
                         + " change(s), all permitted at this level";
    } else {
        status.message = "Certification (DocMDP /P " + std::to_string(static_cast<int>(status.level))
                         + ") intact: no changes since signing";
    }
    return status;
}

} // namespace pdfe::signatures
