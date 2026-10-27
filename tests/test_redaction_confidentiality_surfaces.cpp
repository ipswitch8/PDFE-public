// -*- coding: utf-8 -*-
//
// Redaction confidentiality-surface tests (meta-audit gaps PDF-53, PDF-60..68).
//
// PDFE's redaction removes page text, the OCR/hidden text layer and image
// rasters — those already have real save->reopen->assert-absent oracles. What
// nothing covered was every OTHER surface a redacted string can survive in:
//
//   PDF-62  text inside form XObjects overlapping the redaction rect
//   PDF-63  annotations overlapping the redacted region
//   PDF-61  structure tree / tagged content (/ActualText, /Alt)
//   PDF-60  document metadata + XMP
//   PDF-64  embedded / attached files
//   PDF-65  outlines / bookmarks
//   PDF-66  document JavaScript
//   PDF-67  single-revision assertion on PDFE's OWN redact+save output
//   PDF-68  RedactionVerifier::checkSingleRevision self-described as "a
//           heuristic" — it is now a real structural test
//   PDF-53  "Flatten Annotations" was applyPageRedactions() with no marks —
//           a silent no-op with no oracle
//
// Every test here:
//   * plants a UNIQUE sentinel in the surface under test,
//   * drives the REAL product path (RedactionManager::applyPageRedactions /
//     applyAllRedactions / BatchEngine),
//   * saves, REOPENS, and asserts the sentinel is absent from the
//     DECOMPRESSED bytes of that surface (a raw grep of a Flate stream is a
//     false-green — see RedactedTextAbsentFromBytes), and
//   * asserts a CONTROL that must SURVIVE. A test that passes because
//     everything was deleted is not a redaction test.

#include <gtest/gtest.h>

#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "redaction/RedactionData.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionVerifier.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QRectF>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// ── Sentinels ────────────────────────────────────────────────────────────────
// The document-scoped surfaces are scrubbed WHERE THEY CARRY THE REDACTED
// TEXT, so each document-surface sentinel is also a word printed inside the
// redacted region of page 1. Each token is planted in exactly ONE surface
// besides that line, so an assertion on it cannot be satisfied by accident.
constexpr const char* kPageSecret    = "PAGETXT9f3a";
constexpr const char* kXmpSecret     = "XMPTOK4b7e";
constexpr const char* kMetaSecret    = "METATOK3e8d";
constexpr const char* kEmbedSecret   = "EMBTOK8d3f";
constexpr const char* kOutlineSecret = "BMKTOK1e4d";
constexpr const char* kJsSecret      = "JSTOK6a8c";
constexpr const char* kStructSecret  = "ACTTOK5f2a";
constexpr const char* kXObjSecret    = "XOBJTOK7c1d";
constexpr const char* kAnnotSecret   = "ANNOTTOK2d9e";

constexpr const char* kPageControl    = "CONTROLPAGE1b2c";
constexpr const char* kXmpControl     = "CONTROLXMP2f6a";
constexpr const char* kMetaControl    = "CONTROLMETA9a4c";
constexpr const char* kEmbedControl   = "CONTROLEMB5c9b";
constexpr const char* kOutlineControl = "CONTROLBMK7b2a";
constexpr const char* kJsControl      = "CONTROLJS3d5e";
constexpr const char* kStructControl  = "CONTROLACT0d1e";
constexpr const char* kXObjControl    = "CONTROLXOBJ4e5f";
constexpr const char* kAnnotControl   = "CONTROLANNOT8a7b";

// The redaction rectangle, in the page/device space MuPDF's annotation API
// uses (y-down, origin top-left; page is 612x792). It covers device y 70..150,
// i.e. PDF user-space y 642..722 — the secret text line (baseline y=700), the
// secret form XObject (baseline y=660) and the overlapping annotation
// (rect y 640..720). All CONTROL content sits at user-space y <= 300
// (device y >= 492) and is far outside.
inline QRectF redactRect() { return QRectF(0.0, 70.0, 612.0, 80.0); }

inline long pdfeProcessId()
{
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

std::filesystem::path uniqueDir(const char* stem)
{
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    auto d = std::filesystem::temp_directory_path() / "pdfe_redact_surfaces"
             / (std::string(stem) + "_" + std::to_string(pdfeProcessId())
                + "_" + std::to_string(n));
    std::filesystem::create_directories(d);
    return d;
}

// ── Fixture builder ─────────────────────────────────────────────────────────
// Programmatically builds a PDF carrying EVERY confidentiality surface.
void buildConfidentialityFixture(const std::filesystem::path& out)
{
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    ASSERT_NE(ctx, nullptr);
    fz_register_document_handlers(ctx);

    pdf_document* doc = nullptr;
    fz_buffer* pageBuf = nullptr;
    fz_buffer* formSecretBuf = nullptr;
    fz_buffer* formCtrlBuf = nullptr;
    fz_buffer* xmpBuf = nullptr;
    fz_buffer* embSecretBuf = nullptr;
    fz_buffer* embCtrlBuf = nullptr;
    fz_var(doc);
    fz_var(pageBuf);
    fz_var(formSecretBuf);
    fz_var(formCtrlBuf);
    fz_var(xmpBuf);
    fz_var(embSecretBuf);
    fz_var(embCtrlBuf);

    // Built before fz_try: no C++ object may be constructed inside the
    // setjmp/longjmp region.
    char pageContent[1024];
    char formSecret[256];
    char formCtrl[256];
    char xmp[1024];
    char embSecret[128];
    char embCtrl[128];
    char jsSecret[128];
    char jsCtrl[128];
    char metaSubject[128];
    char outlineSecretTitle[128];
    char structAlt[128];

    snprintf(pageContent, sizeof(pageContent),
             "BT /F1 12 Tf 72 700 Td (%s %s %s %s %s %s %s) Tj ET\n"
             "BT /F1 12 Tf 72 300 Td (%s stays visible) Tj ET\n"
             // Marked-content /ActualText INSIDE the content stream — the other
             // half of PDF-61. Not a text-showing operator, so pdf_redact_page
             // does not touch it, yet PDFE's extractor reads /ActualText.
             "/Span << /ActualText (%s) >> BDC\n"
             "BT /F1 12 Tf 72 680 Td (tagged span) Tj ET\n"
             "EMC\n"
             "/Span << /ActualText (%s) >> BDC\n"
             "BT /F1 12 Tf 72 280 Td (control span) Tj ET\n"
             "EMC\n"
             "q 1 0 0 1 0 0 cm /FxSecret Do Q\n"
             "q 1 0 0 1 0 0 cm /FxControl Do Q\n",
             kPageSecret, kXmpSecret, kMetaSecret, kEmbedSecret,
             kOutlineSecret, kJsSecret, kStructSecret, kPageControl,
             kStructSecret, kStructControl);
    snprintf(formSecret, sizeof(formSecret),
             "BT /F1 12 Tf 72 660 Td (%s) Tj ET\n", kXObjSecret);
    snprintf(formCtrl, sizeof(formCtrl),
             "BT /F1 12 Tf 72 250 Td (%s) Tj ET\n", kXObjControl);
    snprintf(xmp, sizeof(xmp),
             "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
             "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
             "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
             "<rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
             "<dc:description>%s</dc:description>"
             "<dc:creator>%s</dc:creator>"
             "</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>",
             kXmpSecret, kXmpControl);
    snprintf(embSecret, sizeof(embSecret), "attachment body %s\n", kEmbedSecret);
    snprintf(embCtrl, sizeof(embCtrl), "harmless notes %s\n", kEmbedControl);
    snprintf(jsSecret, sizeof(jsSecret), "var leak = '%s';", kJsSecret);
    snprintf(jsCtrl, sizeof(jsCtrl), "var keep = '%s';", kJsControl);
    snprintf(metaSubject, sizeof(metaSubject), "Report about %s", kMetaSecret);
    snprintf(outlineSecretTitle, sizeof(outlineSecretTitle), "Chapter %s", kOutlineSecret);
    snprintf(structAlt, sizeof(structAlt), "alt text for %s", kStructSecret);

    fz_try(ctx) {
        doc = pdf_create_document(ctx);

        // --- font + form XObjects -------------------------------------------
        pdf_obj* font = pdf_add_new_dict(ctx, doc, 4);
        pdf_dict_put(ctx, font, PDF_NAME(Type), PDF_NAME(Font));
        pdf_dict_put(ctx, font, PDF_NAME(Subtype), PDF_NAME(Type1));
        pdf_dict_put_drop(ctx, font, PDF_NAME(BaseFont), pdf_new_name(ctx, "Helvetica"));

        auto makeFormResources = [&]() {
            pdf_obj* res = pdf_add_new_dict(ctx, doc, 2);
            pdf_obj* fonts = pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 1);
            pdf_dict_puts(ctx, fonts, "F1", font);
            return res;
        };

        auto makeForm = [&](fz_buffer* buf) {
            pdf_obj* form = pdf_add_new_dict(ctx, doc, 6);
            pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
            pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
            pdf_dict_put_rect(ctx, form, PDF_NAME(BBox), fz_make_rect(0, 0, 612, 792));
            pdf_dict_put_drop(ctx, form, PDF_NAME(Resources), makeFormResources());
            pdf_update_stream(ctx, doc, form, buf, 0);
            return form;
        };

        formSecretBuf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(formSecret), strlen(formSecret));
        formCtrlBuf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(formCtrl), strlen(formCtrl));
        pdf_obj* formSecretObj = makeForm(formSecretBuf);
        pdf_obj* formCtrlObj = makeForm(formCtrlBuf);

        // --- page resources + page -------------------------------------------
        pdf_obj* resources = pdf_add_new_dict(ctx, doc, 4);
        pdf_obj* fonts = pdf_dict_put_dict(ctx, resources, PDF_NAME(Font), 1);
        pdf_dict_puts(ctx, fonts, "F1", font);
        pdf_obj* xobjs = pdf_dict_put_dict(ctx, resources, PDF_NAME(XObject), 2);
        pdf_dict_puts(ctx, xobjs, "FxSecret", formSecretObj);
        pdf_dict_puts(ctx, xobjs, "FxControl", formCtrlObj);

        pageBuf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(pageContent), strlen(pageContent));
        pdf_obj* pageObj = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 612, 792), 0,
                                        resources, pageBuf);
        pdf_insert_page(ctx, doc, -1, pageObj);

        // --- PDF-63: annotations (one overlapping, one disjoint control) -----
        pdf_obj* annots = pdf_dict_put_array(ctx, pageObj, PDF_NAME(Annots), 2);

        pdf_obj* overAnnot = pdf_add_new_dict(ctx, doc, 6);
        pdf_dict_put(ctx, overAnnot, PDF_NAME(Type), PDF_NAME(Annot));
        pdf_dict_put(ctx, overAnnot, PDF_NAME(Subtype), PDF_NAME(Square));
        pdf_dict_put_rect(ctx, overAnnot, PDF_NAME(Rect), fz_make_rect(72, 640, 300, 720));
        pdf_dict_put_text_string(ctx, overAnnot, PDF_NAME(Contents), kAnnotSecret);
        pdf_array_push(ctx, annots, overAnnot);

        pdf_obj* ctrlAnnot = pdf_add_new_dict(ctx, doc, 6);
        pdf_dict_put(ctx, ctrlAnnot, PDF_NAME(Type), PDF_NAME(Annot));
        pdf_dict_put(ctx, ctrlAnnot, PDF_NAME(Subtype), PDF_NAME(Text));
        pdf_dict_put_rect(ctx, ctrlAnnot, PDF_NAME(Rect), fz_make_rect(72, 200, 96, 224));
        pdf_dict_put_text_string(ctx, ctrlAnnot, PDF_NAME(Contents), kAnnotControl);
        pdf_array_push(ctx, annots, ctrlAnnot);

        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));

        // --- PDF-61: structure tree ------------------------------------------
        pdf_obj* structRoot = pdf_add_new_dict(ctx, doc, 4);
        pdf_dict_put_drop(ctx, structRoot, PDF_NAME(Type),
                          pdf_new_name(ctx, "StructTreeRoot"));
        pdf_obj* kids = pdf_dict_put_array(ctx, structRoot, PDF_NAME(K), 2);

        pdf_obj* secretElem = pdf_add_new_dict(ctx, doc, 6);
        pdf_dict_put_drop(ctx, secretElem, PDF_NAME(Type), pdf_new_name(ctx, "StructElem"));
        pdf_dict_put(ctx, secretElem, PDF_NAME(S), PDF_NAME(P));
        pdf_dict_put_text_string(ctx, secretElem, PDF_NAME(ActualText), kStructSecret);
        pdf_dict_put_text_string(ctx, secretElem, PDF_NAME(Alt), structAlt);
        pdf_array_push(ctx, kids, secretElem);

        pdf_obj* ctrlElem = pdf_add_new_dict(ctx, doc, 6);
        pdf_dict_put_drop(ctx, ctrlElem, PDF_NAME(Type), pdf_new_name(ctx, "StructElem"));
        pdf_dict_put(ctx, ctrlElem, PDF_NAME(S), PDF_NAME(P));
        pdf_dict_put_text_string(ctx, ctrlElem, PDF_NAME(ActualText), kStructControl);
        pdf_array_push(ctx, kids, ctrlElem);

        pdf_dict_put(ctx, root, PDF_NAME(StructTreeRoot), structRoot);

        // --- PDF-60: /Info + XMP ---------------------------------------------
        pdf_obj* info = pdf_add_new_dict(ctx, doc, 4);
        pdf_dict_put_text_string(ctx, info, PDF_NAME(Subject), metaSubject);
        pdf_dict_put_text_string(ctx, info, PDF_NAME(Author), kMetaControl);
        pdf_dict_put(ctx, pdf_trailer(ctx, doc), PDF_NAME(Info), info);

        xmpBuf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(xmp), strlen(xmp));
        pdf_obj* metaObj = pdf_add_new_dict(ctx, doc, 4);
        pdf_dict_put(ctx, metaObj, PDF_NAME(Type), PDF_NAME(Metadata));
        pdf_dict_put_drop(ctx, metaObj, PDF_NAME(Subtype), pdf_new_name(ctx, "XML"));
        pdf_update_stream(ctx, doc, metaObj, xmpBuf, 0);
        pdf_dict_put(ctx, root, PDF_NAME(Metadata), metaObj);

        // --- PDF-64 + PDF-66: /Names tree (EmbeddedFiles, JavaScript) --------
        pdf_obj* names = pdf_dict_put_dict(ctx, root, PDF_NAME(Names), 2);

        auto makeFileSpec = [&](const char* name, fz_buffer* buf) {
            pdf_obj* stream = pdf_add_new_dict(ctx, doc, 2);
            pdf_update_stream(ctx, doc, stream, buf, 0);
            pdf_obj* spec = pdf_add_new_dict(ctx, doc, 4);
            pdf_dict_put(ctx, spec, PDF_NAME(Type), PDF_NAME(Filespec));
            pdf_dict_put_text_string(ctx, spec, PDF_NAME(F), name);
            pdf_dict_put_text_string(ctx, spec, PDF_NAME(UF), name);
            pdf_obj* ef = pdf_dict_put_dict(ctx, spec, PDF_NAME(EF), 1);
            pdf_dict_put(ctx, ef, PDF_NAME(F), stream);
            return spec;
        };

        embCtrlBuf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(embCtrl), strlen(embCtrl));
        embSecretBuf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(embSecret), strlen(embSecret));

        pdf_obj* ef = pdf_dict_put_dict(ctx, names, PDF_NAME(EmbeddedFiles), 1);
        pdf_obj* efNames = pdf_dict_put_array(ctx, ef, PDF_NAME(Names), 4);
        // Name trees are sorted by key: "notes.txt" < "secret.txt".
        pdf_array_push_drop(ctx, efNames, pdf_new_text_string(ctx, "notes.txt"));
        pdf_array_push(ctx, efNames, makeFileSpec("notes.txt", embCtrlBuf));
        pdf_array_push_drop(ctx, efNames, pdf_new_text_string(ctx, "secret.txt"));
        pdf_array_push(ctx, efNames, makeFileSpec("secret.txt", embSecretBuf));

        auto makeJsAction = [&](const char* body) {
            pdf_obj* act = pdf_add_new_dict(ctx, doc, 2);
            pdf_dict_put_drop(ctx, act, PDF_NAME(S), pdf_new_name(ctx, "JavaScript"));
            pdf_dict_put_text_string(ctx, act, PDF_NAME(JS), body);
            return act;
        };

        pdf_obj* js = pdf_dict_put_dict(ctx, names, PDF_NAME(JavaScript), 1);
        pdf_obj* jsNames = pdf_dict_put_array(ctx, js, PDF_NAME(Names), 4);
        pdf_array_push_drop(ctx, jsNames, pdf_new_text_string(ctx, "AA_keep"));
        pdf_array_push_drop(ctx, jsNames, makeJsAction(jsCtrl));
        pdf_array_push_drop(ctx, jsNames, pdf_new_text_string(ctx, "BB_leak"));
        pdf_array_push_drop(ctx, jsNames, makeJsAction(jsSecret));

        // --- PDF-65: outlines -------------------------------------------------
        pdf_obj* outlines = pdf_add_new_dict(ctx, doc, 4);
        pdf_dict_put_drop(ctx, outlines, PDF_NAME(Type), pdf_new_name(ctx, "Outlines"));
        pdf_obj* bmSecret = pdf_add_new_dict(ctx, doc, 4);
        pdf_obj* bmCtrl = pdf_add_new_dict(ctx, doc, 4);
        pdf_dict_put_text_string(ctx, bmSecret, PDF_NAME(Title), outlineSecretTitle);
        pdf_dict_put_text_string(ctx, bmCtrl, PDF_NAME(Title), kOutlineControl);
        pdf_dict_put(ctx, bmSecret, PDF_NAME(Parent), outlines);
        pdf_dict_put(ctx, bmCtrl, PDF_NAME(Parent), outlines);
        pdf_dict_put(ctx, bmSecret, PDF_NAME(Next), bmCtrl);
        pdf_dict_put(ctx, bmCtrl, PDF_NAME(Prev), bmSecret);
        pdf_dict_put(ctx, outlines, PDF_NAME(First), bmSecret);
        pdf_dict_put(ctx, outlines, PDF_NAME(Last), bmCtrl);
        pdf_dict_put_int(ctx, outlines, PDF_NAME(Count), 2);
        pdf_dict_put(ctx, root, PDF_NAME(Outlines), outlines);

        pdf_write_options wopts = pdf_default_write_options;
        pdf_save_document(ctx, doc, out.string().c_str(), &wopts);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, pageBuf);
        fz_drop_buffer(ctx, formSecretBuf);
        fz_drop_buffer(ctx, formCtrlBuf);
        fz_drop_buffer(ctx, xmpBuf);
        fz_drop_buffer(ctx, embSecretBuf);
        fz_drop_buffer(ctx, embCtrlBuf);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        fz_drop_context(ctx);
        FAIL() << "failed to build the confidentiality fixture";
    }
    fz_drop_context(ctx);
}

// ── Surface readers (all DECOMPRESS before returning bytes) ─────────────────

template <typename Fn>
void withPdf(const std::filesystem::path& p, const Fn& fn)
{
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return;
    fz_register_document_handlers(ctx);
    fz_document* fdoc = nullptr;
    fz_var(fdoc);
    fz_try(ctx) {
        fdoc = fz_open_document(ctx, p.string().c_str());
        pdf_document* doc = pdf_document_from_fz_document(ctx, fdoc);
        if (doc) fn(ctx, doc);
    }
    fz_always(ctx) {
        fz_drop_document(ctx, fdoc);
    }
    fz_catch(ctx) {
    }
    fz_drop_context(ctx);
}

void appendStream(fz_context* ctx, pdf_obj* stm, std::string& out)
{
    if (!stm || !pdf_is_stream(ctx, stm)) return;
    fz_buffer* b = nullptr;
    fz_var(b);
    fz_try(ctx) {
        b = pdf_load_stream(ctx, stm);
        unsigned char* d = nullptr;
        size_t n = fz_buffer_storage(ctx, b, &d);
        if (d && n) out.append(reinterpret_cast<const char*>(d), n);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, b);
    }
    fz_catch(ctx) {
    }
}

void appendText(fz_context* ctx, pdf_obj* o, std::string& out)
{
    if (!o) return;
    if (pdf_is_string(ctx, o)) {
        const char* s = pdf_to_text_string(ctx, o);
        if (s) { out.append(s); out.push_back('\n'); }
    }
}

/// Concatenated DECOMPRESSED bytes of every form XObject on page `pageNum`.
std::string formXObjectBytes(const std::filesystem::path& p, int pageNum = 0)
{
    std::string out;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_page* page = pdf_load_page(ctx, doc, pageNum);
        pdf_obj* res = pdf_dict_get(ctx, page->obj, PDF_NAME(Resources));
        pdf_obj* xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
        const int n = pdf_dict_len(ctx, xobjs);
        for (int i = 0; i < n; ++i) {
            pdf_obj* v = pdf_dict_get_val(ctx, xobjs, i);
            if (pdf_dict_get(ctx, v, PDF_NAME(Subtype)) == PDF_NAME(Form)) {
                appendStream(ctx, v, out);
            }
        }
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
    });
    return out;
}

int formXObjectCount(const std::filesystem::path& p, int pageNum = 0)
{
    int count = 0;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_page* page = pdf_load_page(ctx, doc, pageNum);
        pdf_obj* res = pdf_dict_get(ctx, page->obj, PDF_NAME(Resources));
        pdf_obj* xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
        const int n = pdf_dict_len(ctx, xobjs);
        for (int i = 0; i < n; ++i) {
            if (pdf_dict_get(ctx, pdf_dict_get_val(ctx, xobjs, i), PDF_NAME(Subtype))
                == PDF_NAME(Form)) {
                ++count;
            }
        }
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
    });
    return count;
}

/// Every annotation dictionary's /Contents plus its DECOMPRESSED appearance
/// stream, for page `pageNum`.
std::string annotationBytes(const std::filesystem::path& p, int pageNum = 0)
{
    std::string out;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_page* page = pdf_load_page(ctx, doc, pageNum);
        pdf_obj* annots = pdf_dict_get(ctx, page->obj, PDF_NAME(Annots));
        const int n = pdf_array_len(ctx, annots);
        for (int i = 0; i < n; ++i) {
            pdf_obj* a = pdf_array_get(ctx, annots, i);
            appendText(ctx, pdf_dict_get(ctx, a, PDF_NAME(Contents)), out);
            pdf_obj* ap = pdf_dict_get(ctx, a, PDF_NAME(AP));
            pdf_obj* normal = pdf_dict_get(ctx, ap, PDF_NAME(N));
            appendStream(ctx, normal, out);
        }
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
    });
    return out;
}

int annotationCount(const std::filesystem::path& p, int pageNum = 0)
{
    int count = 0;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_page* page = pdf_load_page(ctx, doc, pageNum);
        count = pdf_array_len(ctx, pdf_dict_get(ctx, page->obj, PDF_NAME(Annots)));
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
    });
    return count;
}

void walkStruct(fz_context* ctx, pdf_obj* node, std::string& out, int depth)
{
    if (!node || depth > 32) return;
    if (pdf_is_array(ctx, node)) {
        const int n = pdf_array_len(ctx, node);
        for (int i = 0; i < n; ++i) walkStruct(ctx, pdf_array_get(ctx, node, i), out, depth + 1);
        return;
    }
    if (!pdf_is_dict(ctx, node)) return;
    appendText(ctx, pdf_dict_get(ctx, node, PDF_NAME(ActualText)), out);
    appendText(ctx, pdf_dict_get(ctx, node, PDF_NAME(Alt)), out);
    walkStruct(ctx, pdf_dict_get(ctx, node, PDF_NAME(K)), out, depth + 1);
}

std::string structTreeBytes(const std::filesystem::path& p)
{
    std::string out;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        walkStruct(ctx, pdf_dict_get(ctx, root, PDF_NAME(StructTreeRoot)), out, 0);
    });
    return out;
}

std::string infoDictBytes(const std::filesystem::path& p)
{
    std::string out;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_obj* info = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Info));
        const int n = pdf_dict_len(ctx, info);
        for (int i = 0; i < n; ++i) appendText(ctx, pdf_dict_get_val(ctx, info, i), out);
    });
    return out;
}

std::string xmpBytes(const std::filesystem::path& p)
{
    std::string out;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        appendStream(ctx, pdf_dict_get(ctx, root, PDF_NAME(Metadata)), out);
    });
    return out;
}

void walkNameTree(fz_context* ctx, pdf_obj* node, std::string& out, int depth,
                  void (*emitFn)(fz_context*, pdf_obj*, pdf_obj*, std::string&))
{
    if (!node || depth > 32) return;
    pdf_obj* kids = pdf_dict_get(ctx, node, PDF_NAME(Kids));
    const int kn = pdf_array_len(ctx, kids);
    for (int i = 0; i < kn; ++i) {
        walkNameTree(ctx, pdf_array_get(ctx, kids, i), out, depth + 1, emitFn);
    }
    pdf_obj* names = pdf_dict_get(ctx, node, PDF_NAME(Names));
    const int n = pdf_array_len(ctx, names);
    for (int i = 0; i + 1 < n; i += 2) {
        emitFn(ctx, pdf_array_get(ctx, names, i), pdf_array_get(ctx, names, i + 1), out);
    }
}

void emitEmbedded(fz_context* ctx, pdf_obj* key, pdf_obj* val, std::string& out)
{
    appendText(ctx, key, out);
    appendText(ctx, pdf_dict_get(ctx, val, PDF_NAME(F)), out);
    appendText(ctx, pdf_dict_get(ctx, val, PDF_NAME(UF)), out);
    pdf_obj* ef = pdf_dict_get(ctx, val, PDF_NAME(EF));
    const int n = pdf_dict_len(ctx, ef);
    for (int i = 0; i < n; ++i) appendStream(ctx, pdf_dict_get_val(ctx, ef, i), out);
}

void emitJs(fz_context* ctx, pdf_obj* key, pdf_obj* val, std::string& out)
{
    appendText(ctx, key, out);
    pdf_obj* body = pdf_dict_get(ctx, val, PDF_NAME(JS));
    appendText(ctx, body, out);
    appendStream(ctx, body, out);
}

std::string embeddedFileBytes(const std::filesystem::path& p)
{
    std::string out;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* names = pdf_dict_get(ctx, root, PDF_NAME(Names));
        walkNameTree(ctx, pdf_dict_get(ctx, names, PDF_NAME(EmbeddedFiles)), out, 0,
                     emitEmbedded);
    });
    return out;
}

std::string javascriptBytes(const std::filesystem::path& p)
{
    std::string out;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* names = pdf_dict_get(ctx, root, PDF_NAME(Names));
        walkNameTree(ctx, pdf_dict_get(ctx, names, PDF_NAME(JavaScript)), out, 0, emitJs);
    });
    return out;
}

std::string outlineBytes(const std::filesystem::path& p)
{
    std::string out;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* item = pdf_dict_get(ctx,
            pdf_dict_get(ctx, root, PDF_NAME(Outlines)), PDF_NAME(First));
        int guard = 0;
        while (item && guard++ < 256) {
            appendText(ctx, pdf_dict_get(ctx, item, PDF_NAME(Title)), out);
            pdf_obj* child = pdf_dict_get(ctx, item, PDF_NAME(First));
            int cguard = 0;
            while (child && cguard++ < 256) {
                appendText(ctx, pdf_dict_get(ctx, child, PDF_NAME(Title)), out);
                child = pdf_dict_get(ctx, child, PDF_NAME(Next));
            }
            item = pdf_dict_get(ctx, item, PDF_NAME(Next));
        }
    });
    return out;
}

std::string pageContentBytes(const std::filesystem::path& p, int pageNum = 0)
{
    std::string out;
    withPdf(p, [&](fz_context* ctx, pdf_document* doc) {
        pdf_page* page = pdf_load_page(ctx, doc, pageNum);
        pdf_obj* contents = pdf_dict_get(ctx, page->obj, PDF_NAME(Contents));
        if (pdf_is_array(ctx, contents)) {
            const int n = pdf_array_len(ctx, contents);
            for (int i = 0; i < n; ++i) appendStream(ctx, pdf_array_get(ctx, contents, i), out);
        } else {
            appendStream(ctx, contents, out);
        }
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
    });
    return out;
}

} // namespace

// ============================================================================
// Fixture
// ============================================================================

class RedactionConfidentialitySurfaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        dir_ = uniqueDir("surfaces");
        src_ = dir_ / "surfaces_src.pdf";
        out_ = dir_ / "surfaces_redacted.pdf";
        buildConfidentialityFixture(src_);
        ASSERT_TRUE(std::filesystem::exists(src_));
    }

    void TearDown() override {
        pages_.clear();
        doc_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// Drive the REAL product path: mark the region, apply the redaction via
    /// RedactionManager::applyPageRedactions, save, and leave the saved file at
    /// out_ ready to be reopened.
    void redactAndSave(const pdfe::redaction::RedactionApplyOptions& opts = {}) {
        doc_ = pdfe::core::Document::open(context_, src_);
        ASSERT_NE(doc_, nullptr);
        auto page = doc_->page(0);
        ASSERT_NE(page, nullptr);

        pdfe::redaction::RedactionManager mgr(context_);
        auto mark = mgr.createRedactionMark(page.get(), redactRect());
        ASSERT_NE(mark.annotIndex, -1);

        ASSERT_TRUE(mgr.applyPageRedactions(page.get(), opts))
            << "applyPageRedactions must report that it applied a mark";

        page.reset();
        doc_->saveAs(out_);
        doc_.reset();
        ASSERT_TRUE(std::filesystem::exists(out_));
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> doc_;
    std::vector<std::unique_ptr<pdfe::core::Page>> pages_;
    std::filesystem::path dir_;
    std::filesystem::path src_;
    std::filesystem::path out_;
};

// ── Fixture self-check ──────────────────────────────────────────────────────
// If the builder ever stops planting a sentinel, every test below would pass
// vacuously. This locks the pre-condition for all of them.
TEST_F(RedactionConfidentialitySurfaceTest, FixturePlantsEverySentinel) {
    EXPECT_NE(pageContentBytes(src_).find(kPageSecret), std::string::npos);
    EXPECT_NE(formXObjectBytes(src_).find(kXObjSecret), std::string::npos);
    EXPECT_NE(annotationBytes(src_).find(kAnnotSecret), std::string::npos);
    EXPECT_NE(structTreeBytes(src_).find(kStructSecret), std::string::npos);
    EXPECT_NE(infoDictBytes(src_).find(kMetaSecret), std::string::npos);
    EXPECT_NE(xmpBytes(src_).find(kXmpSecret), std::string::npos);
    EXPECT_NE(embeddedFileBytes(src_).find(kEmbedSecret), std::string::npos);
    EXPECT_NE(outlineBytes(src_).find(kOutlineSecret), std::string::npos);
    EXPECT_NE(javascriptBytes(src_).find(kJsSecret), std::string::npos);

    EXPECT_NE(formXObjectBytes(src_).find(kXObjControl), std::string::npos);
    EXPECT_NE(annotationBytes(src_).find(kAnnotControl), std::string::npos);
    EXPECT_NE(structTreeBytes(src_).find(kStructControl), std::string::npos);
    EXPECT_NE(infoDictBytes(src_).find(kMetaControl), std::string::npos);
    EXPECT_NE(xmpBytes(src_).find(kXmpControl), std::string::npos);
    EXPECT_NE(embeddedFileBytes(src_).find(kEmbedControl), std::string::npos);
    EXPECT_NE(outlineBytes(src_).find(kOutlineControl), std::string::npos);
    EXPECT_NE(javascriptBytes(src_).find(kJsControl), std::string::npos);
}

// ── PDF-62: form XObjects ───────────────────────────────────────────────────
TEST_F(RedactionConfidentialitySurfaceTest, FormXObjectTextRemoved) {
    redactAndSave();

    const std::string forms = formXObjectBytes(out_);
    ASSERT_FALSE(forms.empty()) << "no decompressed form-XObject bytes to check";

    EXPECT_EQ(forms.find(kXObjSecret), std::string::npos)
        << "PDF-62: text inside a form XObject overlapping the redaction region "
           "survives in the DECOMPRESSED XObject stream — the classic "
           "'covered but not removed' leak.";
    EXPECT_NE(forms.find(kXObjControl), std::string::npos)
        << "CONTROL: the form XObject outside the redaction region was destroyed; "
           "redaction must not blank unrelated XObject text.";
}

// ── PDF-63: overlapping annotations ─────────────────────────────────────────
// NOTE: the complement of this test is RedactionPreservesAnnotations in
// test_redaction_edge_cases.cpp, which locks that a DISJOINT annotation
// survives. That behaviour is correct and must keep passing; this test locks
// the missing half — an OVERLAPPING annotation must be removed.
TEST_F(RedactionConfidentialitySurfaceTest, OverlappingAnnotationRemoved) {
    redactAndSave();

    const std::string annots = annotationBytes(out_);

    EXPECT_EQ(annots.find(kAnnotSecret), std::string::npos)
        << "PDF-63: an annotation overlapping the redacted region survived "
           "(its /Contents is still readable after save+reopen).";
    EXPECT_NE(annots.find(kAnnotControl), std::string::npos)
        << "CONTROL: the disjoint annotation was destroyed. Redaction must only "
           "remove annotations that overlap the redacted region.";
}

// ── PDF-61: structure tree / tagged content ─────────────────────────────────
TEST_F(RedactionConfidentialitySurfaceTest, StructTreeActualTextRemoved) {
    redactAndSave();

    const std::string tree = structTreeBytes(out_);

    EXPECT_EQ(tree.find(kStructSecret), std::string::npos)
        << "PDF-61: the redacted word survives in the structure tree "
           "(/ActualText or /Alt). PDFE's own extractor reads /ActualText, so "
           "the 'redacted' word is still extractable.";
    EXPECT_NE(tree.find(kStructControl), std::string::npos)
        << "CONTROL: an unrelated struct element's /ActualText was destroyed. "
           "Redaction must not flatten the whole tagged-content tree.";

    // The other half of PDF-61: /ActualText carried as a marked-content
    // property inside the page content stream itself.
    const std::string content = pageContentBytes(out_);
    ASSERT_FALSE(content.empty()) << "no decompressed page content to check";
    EXPECT_EQ(content.find(kStructSecret), std::string::npos)
        << "PDF-61: the redacted word survives as a marked-content "
           "/ActualText property in the DECOMPRESSED page content stream "
           "(`/Span << /ActualText (...) >> BDC`), so it is still extractable.";
    EXPECT_NE(content.find(kStructControl), std::string::npos)
        << "CONTROL: the unrelated marked-content /ActualText outside the "
           "redacted region was destroyed.";
}

// ── PDF-60: metadata + XMP ──────────────────────────────────────────────────
TEST_F(RedactionConfidentialitySurfaceTest, MetadataAndXmpScrubbed) {
    redactAndSave();

    const std::string info = infoDictBytes(out_);
    const std::string xmp = xmpBytes(out_);

    EXPECT_EQ(info.find(kMetaSecret), std::string::npos)
        << "PDF-60: the redacted word survives in the document information "
           "dictionary.";
    EXPECT_EQ(xmp.find(kXmpSecret), std::string::npos)
        << "PDF-60: the redacted word survives in the DECOMPRESSED XMP packet.";

    EXPECT_NE(info.find(kMetaControl), std::string::npos)
        << "CONTROL: unrelated /Info metadata was deleted. The default must "
           "remove only the entries carrying the redacted text.";
    EXPECT_NE(xmp.find(kXmpControl), std::string::npos)
        << "CONTROL: unrelated XMP properties were deleted — the packet must be "
           "scrubbed in place, not dropped wholesale.";
}

// ── PDF-64: embedded / attached files ───────────────────────────────────────
TEST_F(RedactionConfidentialitySurfaceTest, EmbeddedFilesScrubbed) {
    redactAndSave();

    const std::string embedded = embeddedFileBytes(out_);

    EXPECT_EQ(embedded.find(kEmbedSecret), std::string::npos)
        << "PDF-64: the redacted word survives inside the DECOMPRESSED bytes of "
           "an embedded/attached file.";
    EXPECT_NE(embedded.find(kEmbedControl), std::string::npos)
        << "CONTROL: the unrelated attachment was deleted. Redacting one page "
           "must not strip every attachment.";
}

// ── PDF-65: outlines / bookmarks ────────────────────────────────────────────
TEST_F(RedactionConfidentialitySurfaceTest, OutlinesScrubbed) {
    redactAndSave();

    const std::string outlines = outlineBytes(out_);

    EXPECT_EQ(outlines.find(kOutlineSecret), std::string::npos)
        << "PDF-65: the redacted word survives in a bookmark /Title.";
    EXPECT_NE(outlines.find(kOutlineControl), std::string::npos)
        << "CONTROL: the unrelated bookmark was deleted. Deleting every bookmark "
           "because one page was redacted is its own defect.";
}

// ── PDF-66: document JavaScript ─────────────────────────────────────────────
TEST_F(RedactionConfidentialitySurfaceTest, DocumentJavaScriptScrubbed) {
    redactAndSave();

    const std::string js = javascriptBytes(out_);

    EXPECT_EQ(js.find(kJsSecret), std::string::npos)
        << "PDF-66: the redacted word survives in document-level JavaScript.";
    EXPECT_NE(js.find(kJsControl), std::string::npos)
        << "CONTROL: unrelated document JavaScript was deleted.";
}

// ── Opt-in full sanitize ────────────────────────────────────────────────────
// The wholesale option must remove the document-scoped surfaces ENTIRELY,
// including the controls — while the page's unrelated CONTENT still survives
// (full sanitize is about document-scoped carriers, not about nuking the page).
TEST_F(RedactionConfidentialitySurfaceTest, FullSanitizeRemovesDocumentSurfacesWholesale) {
    pdfe::redaction::RedactionApplyOptions opts;
    opts.fullSanitize = true;
    redactAndSave(opts);

    EXPECT_EQ(infoDictBytes(out_).find(kMetaControl), std::string::npos)
        << "fullSanitize must drop the whole /Info dictionary";
    EXPECT_EQ(xmpBytes(out_).find(kXmpControl), std::string::npos)
        << "fullSanitize must drop the whole XMP packet";
    EXPECT_EQ(embeddedFileBytes(out_).find(kEmbedControl), std::string::npos)
        << "fullSanitize must drop all embedded files";
    EXPECT_EQ(outlineBytes(out_).find(kOutlineControl), std::string::npos)
        << "fullSanitize must drop all outlines";
    EXPECT_EQ(javascriptBytes(out_).find(kJsControl), std::string::npos)
        << "fullSanitize must drop all document JavaScript";

    // CONTROL: the page content outside the redaction region is untouched.
    EXPECT_NE(pageContentBytes(out_).find(kPageControl), std::string::npos)
        << "fullSanitize wrongly destroyed unrelated PAGE content";
}

// ── PDF-67: single revision on PDFE's OWN redact+save output ────────────────
TEST_F(RedactionConfidentialitySurfaceTest, OwnRedactOutputIsSingleRevision) {
    // applyAllRedactions(rewriteDocument=true) is the product's full pipeline:
    // apply -> MuPDF bake -> QPDF single-revision rewrite onto filePath().
    auto doc = pdfe::core::Document::open(context_, src_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::redaction::RedactionManager mgr(context_);
    auto mark = mgr.createRedactionMark(page.get(), redactRect());
    page.reset();

    pdfe::redaction::RedactionApplyOptions opts;
    opts.rewriteDocument = true;
    auto result = mgr.applyAllRedactions(doc.get(), opts);
    ASSERT_TRUE(result.success) << result.errorMessage;

    doc->saveAs(out_);
    doc.reset();

    pdfe::redaction::RedactionVerifier verifier(context_);
    EXPECT_TRUE(verifier.checkSingleRevision(out_))
        << "PDF-67: PDFE's own redact+save output is NOT a single revision — a "
           "previous (pre-redaction) revision of the objects is still on disk.";

    std::vector<pdfe::redaction::RedactionData> regions{mark};
    auto vr = verifier.verify(out_, regions);
    EXPECT_TRUE(vr.textRemoved)
        << "redacted text still present in the redacted region of our own output";

    // CONTROL: the output is a real, readable PDF whose unrelated page content
    // survived — "single revision" must not be satisfied by an empty file.
    EXPECT_NE(pageContentBytes(out_).find(kPageControl), std::string::npos)
        << "CONTROL: unrelated page content missing from the rewritten output";
}

// ── PDF-68: the single-revision check is real, not a heuristic ──────────────
TEST_F(RedactionConfidentialitySurfaceTest, SingleRevisionCheckDetectsAppendedRevision) {
    pdfe::redaction::RedactionVerifier verifier(context_);

    // Our freshly written fixture is a single revision.
    ASSERT_TRUE(verifier.checkSingleRevision(src_))
        << "a freshly written single-revision PDF must verify as such "
           "(CONTROL for the fail case below)";

    // Append a genuine incremental update: a second body + xref section
    // chaining to the first via /Prev. The generation numbers stay at 0, which
    // is exactly what the old generation-only heuristic could not see.
    auto appended = dir_ / "appended_revision.pdf";
    std::filesystem::copy_file(src_, appended,
                               std::filesystem::copy_options::overwrite_existing);
    {
        auto doc = pdfe::core::Document::open(context_, appended);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        pdfe::annotations::AnnotationManager annMgr(context_);
        annMgr.createAnnotation(page.get(), pdfe::annotations::AnnotationType::Text,
                                QRectF(300.0, 300.0, 24.0, 24.0));
        page.reset();
        doc->saveIncremental();
    }

    EXPECT_FALSE(verifier.checkSingleRevision(appended))
        << "PDF-68: checkSingleRevision must detect an appended incremental "
           "revision (a second cross-reference section chained via /Prev). The "
           "previous implementation only looked at object generation numbers, "
           "which an incremental update leaves at 0.";
}

// ── PDF-53: Flatten Annotations ─────────────────────────────────────────────
// Previously implemented as applyPageRedactions() with no redaction marks —
// pdf_redact_page() with zero redaction annotations does nothing, so the
// action was a silent no-op and no test asserted otherwise.
TEST_F(RedactionConfidentialitySurfaceTest, FlattenAnnotationsActuallyBakesAnnotations) {
    auto work = dir_ / "flatten_work.pdf";

    // Give the page an annotation with a real appearance stream (the fixture's
    // hand-built annotation dictionaries carry no /AP, and there is nothing to
    // bake from an appearance-less annotation).
    {
        auto doc = pdfe::core::Document::open(context_, src_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        pdfe::annotations::AnnotationManager annMgr(context_);
        auto a = annMgr.createAnnotation(page.get(),
                                         pdfe::annotations::AnnotationType::Square,
                                         QRectF(360.0, 360.0, 80.0, 40.0));
        ASSERT_NE(a.annotIndex, -1);
        page.reset();
        doc->saveAs(work);
    }

    const int annotsBefore = annotationCount(work);
    ASSERT_GE(annotsBefore, 3) << "fixture must carry annotations to flatten";
    const int formsBefore = formXObjectCount(work);

    pdfe::batch::BatchEngine engine(context_);
    std::vector<std::filesystem::path> files{work};
    std::vector<pdfe::batch::BatchAction> actions{
        {pdfe::batch::BatchActionType::FlattenAnnotations, std::monostate{}}};
    auto result = engine.process(files, actions);
    ASSERT_EQ(result.errorCount, 0) << "batch flatten reported an error";
    ASSERT_EQ(result.successCount, 1);

    // BatchEngine writes to <input dir>/batch_output/<input name>.
    const auto flattened = dir_ / "batch_output" / work.filename();
    ASSERT_TRUE(std::filesystem::exists(flattened)) << flattened.string();

    EXPECT_EQ(annotationCount(flattened), 0)
        << "PDF-53: 'Flatten Annotations' left " << annotationCount(flattened)
        << " annotation object(s) on the page — the action is a no-op.";
    EXPECT_GT(formXObjectCount(flattened), formsBefore)
        << "PDF-53: flattening must BAKE each annotation's appearance into the "
           "page as a form XObject; the XObject count did not grow, so nothing "
           "was baked (the annotations were merely dropped, or nothing happened).";

    // CONTROL: flattening must not disturb the page's own content.
    EXPECT_NE(pageContentBytes(flattened).find(kPageControl), std::string::npos)
        << "CONTROL: flattening destroyed unrelated page content";
    EXPECT_NE(pageContentBytes(flattened).find(kPageSecret), std::string::npos)
        << "CONTROL: flattening is not redaction — page text must be preserved";
}
