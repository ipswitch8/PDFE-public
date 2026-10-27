#pragma once
// -*- coding: utf-8 -*-
// F6 — shared test-support oracle for DOCUMENT-LEVEL ARTIFACT PRESERVATION
// across page operations.
//
// Every reader here works on a SAVED FILE that it opens itself. That is
// deliberate: an in-memory `Document` still holds the objects a page operation
// was supposed to rewrite, so asserting against it cannot distinguish "the
// artifact survived the save" from "the artifact is still in RAM". A
// preservation claim is only true if it is true of the bytes on disk.
//
// The file also carries `buildArtifactRichPdf`, which writes a small PDF that
// carries all eight tracked artifacts with per-page unique sentinels, so a
// test can tell "page 2's bookmark survived" apart from "some bookmark
// survived".
//
// Header-only; include from a test .cpp. Requires gtest (for
// ::testing::AssertionResult) and the MuPDF headers, which test .cpp files
// already pull in transitively via the linked pdfe_* libraries.

#include <gtest/gtest.h>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pdfe::testsupport {

// ---------------------------------------------------------------------------
// The eight artifacts this phase tracks.
// ---------------------------------------------------------------------------

enum class Artifact {
    Outline,          // /Outlines
    PageLabels,       // /PageLabels
    FieldValues,      // /AcroForm field /V values
    OcProperties,     // /OCProperties
    StructTreeRoot,   // /StructTreeRoot
    EmbeddedFiles,    // /Names /EmbeddedFiles
    NamedDests,       // /Names /Dests
    Annotations       // per-page /Annots
};

inline const char* artifactName(Artifact a) {
    switch (a) {
        case Artifact::Outline:        return "outline";
        case Artifact::PageLabels:     return "PageLabels";
        case Artifact::FieldValues:    return "AcroFormFieldValues";
        case Artifact::OcProperties:   return "OCProperties";
        case Artifact::StructTreeRoot: return "StructTreeRoot";
        case Artifact::EmbeddedFiles:  return "NamesEmbeddedFiles";
        case Artifact::NamedDests:     return "NamesDests";
        case Artifact::Annotations:    return "annotations";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Observation model
// ---------------------------------------------------------------------------

struct AnnotTuple {
    int page = -1;
    std::string subtype;
    std::string contents;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool operator<(const AnnotTuple& o) const {
        if (page != o.page) return page < o.page;
        if (subtype != o.subtype) return subtype < o.subtype;
        return contents < o.contents;
    }
    std::string str() const {
        return "p" + std::to_string(page) + "/" + subtype + "/" + contents;
    }
};

struct OutlineItem {
    std::string title;
    int depth = 0;
    int destPage = -1;      // resolved 0-based page index, -1 = none/unresolvable
    bool hasDestSpec = false;
};

/// Everything the preservation matrix observes about one saved PDF.
struct DocArtifacts {
    bool valid = false;
    std::string error;
    int pageCount = 0;

    std::vector<OutlineItem> outline;
    std::vector<std::string> pageLabels;              // one per page, "" = none
    bool hasPageLabelsKey = false;
    /// Effective /Rotate per page, inherited per the spec, normalised to
    /// [0,360). Without this the eight Rotate cells have NOTHING that varies
    /// with the operation under test: every artifact assertion they make is
    /// equally true before the rotate, after it, and after a no-op (#224).
    std::vector<int> pageRotations;
    std::map<std::string, std::string> fieldValues;   // fully-qualified name -> /V
    std::vector<std::string> ocgNames;
    bool hasOcProperties = false;
    bool hasStructTreeRoot = false;
    int structElemCount = 0;
    std::vector<std::string> structPageRoles;         // roles of page-anchored elems
    std::vector<std::string> embeddedFileNames;
    std::vector<std::string> javascriptNames;
    std::vector<std::string> namedDests;
    std::map<std::string, int> namedDestPage;         // name -> page index (-1 dangling)
    std::vector<AnnotTuple> annots;

    // --- convenience predicates -------------------------------------------
    std::vector<std::string> outlineTitles() const {
        std::vector<std::string> t;
        t.reserve(outline.size());
        for (const auto& o : outline) t.push_back(o.title);
        return t;
    }
    bool hasOutlineTitle(const std::string& s) const {
        for (const auto& o : outline) if (o.title == s) return true;
        return false;
    }
    bool hasOcg(const std::string& s) const {
        return std::find(ocgNames.begin(), ocgNames.end(), s) != ocgNames.end();
    }
    bool hasEmbeddedFile(const std::string& s) const {
        for (const auto& n : embeddedFileNames) if (n.rfind(s, 0) == 0) return true;
        return false;
    }
    bool hasJavaScript(const std::string& s) const {
        for (const auto& n : javascriptNames) if (n.rfind(s, 0) == 0) return true;
        return false;
    }
    bool hasNamedDest(const std::string& s) const {
        for (const auto& n : namedDests) if (n.rfind(s, 0) == 0) return true;
        return false;
    }
    bool hasFieldValue(const std::string& value) const {
        for (const auto& kv : fieldValues) if (kv.second == value) return true;
        return false;
    }
    bool hasAnnotContents(const std::string& c) const {
        for (const auto& a : annots) if (a.contents == c) return true;
        return false;
    }
    /// Outline entries that carry a destination which no longer resolves to a
    /// page of this document. A preserved artifact must never leave one behind.
    int danglingOutlineDests() const {
        int n = 0;
        for (const auto& o : outline) if (o.hasDestSpec && o.destPage < 0) ++n;
        return n;
    }
    int danglingNamedDests() const {
        int n = 0;
        for (const auto& kv : namedDestPage) if (kv.second < 0) ++n;
        return n;
    }
    std::string summary() const {
        std::string s = "pages=" + std::to_string(pageCount);
        s += " outline=" + std::to_string(outline.size());
        s += " labels=" + std::to_string(hasPageLabelsKey ? 1 : 0);
        s += " fields=" + std::to_string(fieldValues.size());
        s += " ocgs=" + std::to_string(ocgNames.size());
        s += " struct=" + std::to_string(structElemCount);
        s += " embedded=" + std::to_string(embeddedFileNames.size());
        s += " dests=" + std::to_string(namedDests.size());
        s += " annots=" + std::to_string(annots.size());
        return s;
    }
};

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

namespace detail {

inline std::string nameOrEmpty(fz_context* ctx, pdf_obj* o) {
    const char* s = pdf_to_name(ctx, o);
    return s ? std::string(s) : std::string();
}

inline std::string textOrEmpty(fz_context* ctx, pdf_obj* o) {
    if (!o) return {};
    if (pdf_is_string(ctx, o)) {
        const char* s = pdf_to_text_string(ctx, o);
        return s ? std::string(s) : std::string();
    }
    if (pdf_is_name(ctx, o)) return nameOrEmpty(ctx, o);
    if (pdf_is_int(ctx, o)) return std::to_string(pdf_to_int(ctx, o));
    if (pdf_is_bool(ctx, o)) return pdf_to_bool(ctx, o) ? "true" : "false";
    return {};
}

/// 0-based page index of a destination value, or -1.
inline int destPageIndex(fz_context* ctx, pdf_document* doc, pdf_obj* v,
                         const std::map<int, int>& pageNumToIndex, int depth = 0) {
    if (!v || depth > 4) return -1;
    if (pdf_is_name(ctx, v) || pdf_is_string(ctx, v)) {
        pdf_obj* key = pdf_is_name(ctx, v)
            ? pdf_new_name(ctx, pdf_to_name(ctx, v))
            : pdf_new_string(ctx, pdf_to_text_string(ctx, v),
                             strlen(pdf_to_text_string(ctx, v)));
        pdf_obj* found = pdf_lookup_dest(ctx, doc, key);
        pdf_drop_obj(ctx, key);
        return destPageIndex(ctx, doc, found, pageNumToIndex, depth + 1);
    }
    pdf_obj* r = pdf_resolve_indirect(ctx, v);
    if (pdf_is_dict(ctx, r)) {
        return destPageIndex(ctx, doc, pdf_dict_get(ctx, r, PDF_NAME(D)),
                             pageNumToIndex, depth + 1);
    }
    if (!pdf_is_array(ctx, r) || pdf_array_len(ctx, r) < 1) return -1;
    pdf_obj* p = pdf_array_get(ctx, r, 0);
    if (pdf_is_indirect(ctx, p)) {
        auto it = pageNumToIndex.find(pdf_to_num(ctx, p));
        return it == pageNumToIndex.end() ? -1 : it->second;
    }
    if (pdf_is_int(ctx, p)) {
        const int idx = pdf_to_int(ctx, p);
        return (idx >= 0 && idx < (int)pageNumToIndex.size()) ? idx : -1;
    }
    return -1;
}

inline void walkOutline(fz_context* ctx, pdf_document* doc, pdf_obj* first, int depth,
                        const std::map<int, int>& pageNumToIndex,
                        std::vector<OutlineItem>& out) {
    if (depth > 24) return;
    pdf_obj* cur = first;
    for (int guard = 0; cur && guard < 10000; ++guard) {
        pdf_obj* r = pdf_resolve_indirect(ctx, cur);
        if (!pdf_is_dict(ctx, r)) break;
        OutlineItem item;
        item.depth = depth;
        item.title = textOrEmpty(ctx, pdf_dict_get(ctx, r, PDF_NAME(Title)));
        pdf_obj* dest = pdf_dict_get(ctx, r, PDF_NAME(Dest));
        pdf_obj* act = pdf_dict_get(ctx, r, PDF_NAME(A));
        if (dest) {
            item.hasDestSpec = true;
            item.destPage = destPageIndex(ctx, doc, dest, pageNumToIndex);
        } else if (act) {
            pdf_obj* ar = pdf_resolve_indirect(ctx, act);
            pdf_obj* s = pdf_dict_get(ctx, ar, PDF_NAME(S));
            if (pdf_name_eq(ctx, s, PDF_NAME(GoTo))) {
                item.hasDestSpec = true;
                item.destPage = destPageIndex(ctx, doc, pdf_dict_get(ctx, ar, PDF_NAME(D)),
                                              pageNumToIndex);
            }
        }
        out.push_back(item);
        walkOutline(ctx, doc, pdf_dict_get(ctx, r, PDF_NAME(First)), depth + 1,
                    pageNumToIndex, out);
        cur = pdf_dict_get(ctx, r, PDF_NAME(Next));
    }
}

inline void walkFields(fz_context* ctx, pdf_obj* fields, const std::string& prefix,
                       std::map<std::string, std::string>& out, int depth) {
    if (depth > 16 || !pdf_is_array(ctx, fields)) return;
    const int n = pdf_array_len(ctx, fields);
    for (int i = 0; i < n; ++i) {
        pdf_obj* f = pdf_resolve_indirect(ctx, pdf_array_get(ctx, fields, i));
        if (!pdf_is_dict(ctx, f)) continue;
        std::string name = textOrEmpty(ctx, pdf_dict_get(ctx, f, PDF_NAME(T)));
        const std::string full = prefix.empty() ? name
                                                : (name.empty() ? prefix : prefix + "." + name);
        pdf_obj* kids = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, f, PDF_NAME(Kids)));
        const bool hasFieldKids = pdf_is_array(ctx, kids) && pdf_array_len(ctx, kids) > 0;
        pdf_obj* v = pdf_dict_get(ctx, f, PDF_NAME(V));
        if (v || !hasFieldKids) {
            if (!full.empty()) out[full] = textOrEmpty(ctx, v);
        }
        if (hasFieldKids) walkFields(ctx, kids, full, out, depth + 1);
    }
}

inline void walkStruct(fz_context* ctx, pdf_obj* node, int depth,
                       const std::map<int, int>& pageNumToIndex,
                       DocArtifacts& a) {
    if (!node || depth > 48) return;
    pdf_obj* r = pdf_resolve_indirect(ctx, node);
    if (pdf_is_array(ctx, r)) {
        const int n = pdf_array_len(ctx, r);
        for (int i = 0; i < n; ++i) walkStruct(ctx, pdf_array_get(ctx, r, i), depth, pageNumToIndex, a);
        return;
    }
    if (!pdf_is_dict(ctx, r)) return;
    pdf_obj* type = pdf_dict_get(ctx, r, PDF_NAME(Type));
    const std::string t = nameOrEmpty(ctx, type);
    if (t == "OBJR" || t == "MCR") return;
    ++a.structElemCount;
    pdf_obj* pg = pdf_dict_gets(ctx, r, "Pg");
    if (pdf_is_indirect(ctx, pg) && pageNumToIndex.count(pdf_to_num(ctx, pg))) {
        a.structPageRoles.push_back(nameOrEmpty(ctx, pdf_dict_get(ctx, r, PDF_NAME(S))));
    }
    walkStruct(ctx, pdf_dict_get(ctx, r, PDF_NAME(K)), depth + 1, pageNumToIndex, a);
}

inline void readNameTreeKeys(fz_context* ctx, pdf_document* doc, const char* which,
                             std::vector<std::string>& out) {
    pdf_obj* key = pdf_new_name(ctx, which);
    pdf_obj* tree = pdf_load_name_tree(ctx, doc, key);
    pdf_drop_obj(ctx, key);
    if (!tree) return;
    const int n = pdf_dict_len(ctx, tree);
    for (int i = 0; i < n; ++i) {
        const char* k = pdf_to_name(ctx, pdf_dict_get_key(ctx, tree, i));
        if (k) out.emplace_back(k);
    }
    pdf_drop_obj(ctx, tree);
    std::sort(out.begin(), out.end());
}

} // namespace detail

/// Read every tracked artifact out of a SAVED PDF file.
inline DocArtifacts readArtifacts(const std::filesystem::path& pdf)
{
    DocArtifacts a;
    if (!std::filesystem::exists(pdf)) { a.error = "file does not exist"; return a; }

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) { a.error = "fz_new_context failed"; return a; }
    fz_register_document_handlers(ctx);

    fz_document* fdoc = nullptr;
    std::map<int, int> pageNumToIndex;
    fz_try(ctx) {
        fdoc = fz_open_document(ctx, pdf.string().c_str());
        pdf_document* doc = pdf_document_from_fz_document(ctx, fdoc);
        if (!doc) fz_throw(ctx, FZ_ERROR_GENERIC, "not a PDF");
        a.pageCount = pdf_count_pages(ctx, doc);
        for (int i = 0; i < a.pageCount; ++i) {
            pdf_obj* p = pdf_lookup_page_obj(ctx, doc, i);
            if (p) pageNumToIndex[pdf_to_num(ctx, p)] = i;
        }
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));

        // Outline.
        pdf_obj* outlines = pdf_dict_get(ctx, root, PDF_NAME(Outlines));
        if (outlines) {
            detail::walkOutline(ctx, doc,
                pdf_dict_get(ctx, pdf_resolve_indirect(ctx, outlines), PDF_NAME(First)),
                0, pageNumToIndex, a.outline);
        }

        // Page labels -- resolved through MuPDF's own /PageLabels logic.
        a.hasPageLabelsKey = pdf_dict_get(ctx, root, PDF_NAME(PageLabels)) != nullptr;
        for (int i = 0; i < a.pageCount; ++i) {
            char buf[128] = {0};
            fz_page* pg = fz_load_page(ctx, fdoc, i);
            if (pg) {
                fz_page_label(ctx, pg, buf, sizeof(buf));
                fz_drop_page(ctx, pg);
            }
            a.pageLabels.emplace_back(buf);
        }

        // Effective page rotation. /Rotate is inheritable, so read it through
        // the page tree rather than off the page dict, and normalise: the spec
        // allows any multiple of 90, including negatives, so a raw -270 and a
        // 90 are the same page and must compare equal.
        for (int i = 0; i < a.pageCount; ++i) {
            int rot = 0;
            pdf_obj* p = pdf_lookup_page_obj(ctx, doc, i);
            if (p) {
                pdf_obj* r = pdf_dict_get_inheritable(ctx, p, PDF_NAME(Rotate));
                if (pdf_is_number(ctx, r)) rot = pdf_to_int(ctx, r);
            }
            rot %= 360;
            if (rot < 0) rot += 360;
            a.pageRotations.push_back(rot);
        }

        // Form field values.
        pdf_obj* acro = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, root, PDF_NAME(AcroForm)));
        if (pdf_is_dict(ctx, acro)) {
            detail::walkFields(ctx,
                pdf_resolve_indirect(ctx, pdf_dict_get(ctx, acro, PDF_NAME(Fields))),
                "", a.fieldValues, 0);
        }

        // Optional content.
        pdf_obj* ocp = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, root, PDF_NAME(OCProperties)));
        if (pdf_is_dict(ctx, ocp)) {
            a.hasOcProperties = true;
            pdf_obj* ocgs = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, ocp, PDF_NAME(OCGs)));
            const int n = pdf_array_len(ctx, ocgs);
            for (int i = 0; i < n; ++i) {
                pdf_obj* g = pdf_resolve_indirect(ctx, pdf_array_get(ctx, ocgs, i));
                a.ocgNames.push_back(detail::textOrEmpty(ctx, pdf_dict_get(ctx, g, PDF_NAME(Name))));
            }
            std::sort(a.ocgNames.begin(), a.ocgNames.end());
        }

        // Structure tree.
        pdf_obj* st = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, root, PDF_NAME(StructTreeRoot)));
        if (pdf_is_dict(ctx, st)) {
            a.hasStructTreeRoot = true;
            detail::walkStruct(ctx, pdf_dict_get(ctx, st, PDF_NAME(K)), 0, pageNumToIndex, a);
            std::sort(a.structPageRoles.begin(), a.structPageRoles.end());
        }

        // Name trees.
        detail::readNameTreeKeys(ctx, doc, "EmbeddedFiles", a.embeddedFileNames);
        detail::readNameTreeKeys(ctx, doc, "JavaScript", a.javascriptNames);
        detail::readNameTreeKeys(ctx, doc, "Dests", a.namedDests);
        {
            pdf_obj* key = pdf_new_name(ctx, "Dests");
            pdf_obj* tree = pdf_load_name_tree(ctx, doc, key);
            pdf_drop_obj(ctx, key);
            if (tree) {
                const int n = pdf_dict_len(ctx, tree);
                for (int i = 0; i < n; ++i) {
                    const char* k = pdf_to_name(ctx, pdf_dict_get_key(ctx, tree, i));
                    if (!k) continue;
                    a.namedDestPage[k] = detail::destPageIndex(
                        ctx, doc, pdf_dict_get_val(ctx, tree, i), pageNumToIndex);
                }
                pdf_drop_obj(ctx, tree);
            }
        }

        // Annotations, straight off each page dict (no MuPDF synthesis).
        for (int i = 0; i < a.pageCount; ++i) {
            pdf_obj* p = pdf_lookup_page_obj(ctx, doc, i);
            pdf_obj* annots = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, p, PDF_NAME(Annots)));
            const int n = pdf_array_len(ctx, annots);
            for (int j = 0; j < n; ++j) {
                pdf_obj* an = pdf_resolve_indirect(ctx, pdf_array_get(ctx, annots, j));
                if (!pdf_is_dict(ctx, an)) continue;
                AnnotTuple t;
                t.page = i;
                t.subtype = detail::nameOrEmpty(ctx, pdf_dict_get(ctx, an, PDF_NAME(Subtype)));
                t.contents = detail::textOrEmpty(ctx, pdf_dict_get(ctx, an, PDF_NAME(Contents)));
                fz_rect rc = pdf_dict_get_rect(ctx, an, PDF_NAME(Rect));
                t.x0 = rc.x0; t.y0 = rc.y0; t.x1 = rc.x1; t.y1 = rc.y1;
                a.annots.push_back(t);
            }
        }
        std::sort(a.annots.begin(), a.annots.end());
        a.valid = true;
    }
    fz_always(ctx) { if (fdoc) fz_drop_document(ctx, fdoc); }
    fz_catch(ctx) { a.valid = false; a.error = "open/parse failed"; }
    fz_drop_context(ctx);
    return a;
}

// ---------------------------------------------------------------------------
// Assertion predicates
// ---------------------------------------------------------------------------

inline ::testing::AssertionResult DocReadable(const DocArtifacts& a) {
    if (a.valid) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "document not readable: " << a.error;
}

inline ::testing::AssertionResult HasOutlineTitles(
    const DocArtifacts& a, const std::vector<std::string>& expected)
{
    for (const auto& t : expected) {
        if (!a.hasOutlineTitle(t)) {
            std::string got;
            for (const auto& o : a.outline) got += "[" + o.title + "]";
            return ::testing::AssertionFailure()
                << "outline is missing '" << t << "'; present: " << got;
        }
    }
    return ::testing::AssertionSuccess();
}

inline ::testing::AssertionResult LacksOutlineTitles(
    const DocArtifacts& a, const std::vector<std::string>& forbidden)
{
    for (const auto& t : forbidden) {
        if (a.hasOutlineTitle(t)) {
            return ::testing::AssertionFailure()
                << "outline still contains '" << t << "', which targets a page "
                   "that no longer exists";
        }
    }
    return ::testing::AssertionSuccess();
}

inline ::testing::AssertionResult NoDanglingDestinations(const DocArtifacts& a) {
    if (a.danglingOutlineDests() == 0 && a.danglingNamedDests() == 0) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
        << "dangling destinations: " << a.danglingOutlineDests()
        << " outline, " << a.danglingNamedDests() << " named";
}

inline ::testing::AssertionResult HasFieldValues(
    const DocArtifacts& a, const std::vector<std::string>& values)
{
    for (const auto& v : values) {
        if (!a.hasFieldValue(v)) {
            std::string got;
            for (const auto& kv : a.fieldValues) got += "[" + kv.first + "=" + kv.second + "]";
            return ::testing::AssertionFailure()
                << "form-field value '" << v << "' did not survive; present: " << got;
        }
    }
    return ::testing::AssertionSuccess();
}

inline ::testing::AssertionResult HasAnnotContents(
    const DocArtifacts& a, const std::vector<std::string>& contents)
{
    for (const auto& c : contents) {
        if (!a.hasAnnotContents(c)) {
            std::string got;
            for (const auto& t : a.annots) got += "[" + t.str() + "]";
            return ::testing::AssertionFailure()
                << "annotation '" << c << "' did not survive; present: " << got;
        }
    }
    return ::testing::AssertionSuccess();
}

/// Per-annotation {page, subtype, rect, contents} tuple-set equality (PDF-97):
/// an operation that keeps the COUNT but moves an annotation to the wrong page
/// or resets its rect fails this, where a count check would pass.
inline ::testing::AssertionResult AnnotTuplesEqual(
    const std::vector<AnnotTuple>& expected, const std::vector<AnnotTuple>& actual,
    double rectTol = 0.01)
{
    if (expected.size() != actual.size()) {
        return ::testing::AssertionFailure()
            << "annotation count " << actual.size() << " != expected " << expected.size();
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        const AnnotTuple& e = expected[i];
        const AnnotTuple& g = actual[i];
        if (e.page != g.page || e.subtype != g.subtype || e.contents != g.contents) {
            return ::testing::AssertionFailure()
                << "annotation tuple mismatch at " << i << ": expected " << e.str()
                << " got " << g.str();
        }
        if (std::abs(e.x0 - g.x0) > rectTol || std::abs(e.y0 - g.y0) > rectTol ||
            std::abs(e.x1 - g.x1) > rectTol || std::abs(e.y1 - g.y1) > rectTol) {
            return ::testing::AssertionFailure()
                << "annotation " << e.str() << " rect changed: expected ["
                << e.x0 << " " << e.y0 << " " << e.x1 << " " << e.y1 << "] got ["
                << g.x0 << " " << g.y0 << " " << g.x1 << " " << g.y1 << "]";
        }
    }
    return ::testing::AssertionSuccess();
}

/// Generic "is this artifact present at all" probe, used by the matrix to state
/// a wont-fix cell as a LOCK-IN NEGATIVE assertion rather than as a silent skip.
inline bool artifactPresent(const DocArtifacts& a, Artifact art) {
    switch (art) {
        case Artifact::Outline:        return !a.outline.empty();
        case Artifact::PageLabels:     return a.hasPageLabelsKey;
        case Artifact::FieldValues:    return !a.fieldValues.empty();
        case Artifact::OcProperties:   return a.hasOcProperties;
        case Artifact::StructTreeRoot: return a.hasStructTreeRoot;
        case Artifact::EmbeddedFiles:  return !a.embeddedFileNames.empty();
        case Artifact::NamedDests:     return !a.namedDests.empty();
        case Artifact::Annotations:    return !a.annots.empty();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Fixture builder
// ---------------------------------------------------------------------------

/// Names the sentinels a built fixture carries, so tests never hard-code them.
struct ArtifactFixtureNames {
    std::string tag;
    std::string contentMarker(int page) const { return "MARK_" + tag + "_p" + std::to_string(page); }
    std::string bookmark(int page) const     { return "BM_" + tag + "_p" + std::to_string(page); }
    std::string fieldName(int page) const    { return "fld_" + tag + "_p" + std::to_string(page); }
    std::string fieldValue(int page) const   { return "VAL_" + tag + "_p" + std::to_string(page); }
    std::string annotContents(int page) const{ return "ANNOT_" + tag + "_p" + std::to_string(page); }
    std::string destName(int page) const     { return "DEST_" + tag + "_p" + std::to_string(page); }
    std::string ocgName() const              { return "LAYER_" + tag; }
    std::string embeddedFile() const         { return "attach_" + tag + ".txt"; }
    std::string javascriptName() const       { return "JS_" + tag; }
    std::string labelPrefix() const          { return "L" + tag + "-"; }
};

/// Write a PDF carrying ALL EIGHT tracked artifacts, with per-page sentinels.
/// Returns the sentinel names. Throws nothing; returns names with an empty tag
/// on failure (callers assert the file exists).
inline ArtifactFixtureNames buildArtifactRichPdf(const std::filesystem::path& out,
                                                 int pageCount,
                                                 const std::string& tag)
{
    ArtifactFixtureNames names;
    names.tag = tag;

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return names;
    fz_register_document_handlers(ctx);

    // Every std:: container is constructed BEFORE the fz_try frame.
    std::vector<std::string> contents;
    std::vector<std::string> bookmarks;
    std::vector<std::string> fieldNames;
    std::vector<std::string> fieldValues;
    std::vector<std::string> annotTexts;
    std::vector<std::string> destNames;
    for (int i = 0; i < pageCount; ++i) {
        contents.push_back(
            "/P <</MCID 0>> BDC BT /F1 18 Tf 72 700 Td (" + names.contentMarker(i) +
            ") Tj ET EMC\n");
        bookmarks.push_back(names.bookmark(i));
        fieldNames.push_back(names.fieldName(i));
        fieldValues.push_back(names.fieldValue(i));
        annotTexts.push_back(names.annotContents(i));
        destNames.push_back(names.destName(i));
    }
    const std::string ocgName = names.ocgName();
    const std::string embName = names.embeddedFile();
    const std::string jsName = names.javascriptName();
    const std::string labelPrefix = names.labelPrefix();
    const std::string outStr = out.string();

    pdf_document* doc = nullptr;
    fz_try(ctx) {
        doc = pdf_create_document(ctx);
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));

        // One optional-content group, referenced from every page's resources.
        pdf_obj* ocg = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put_drop(ctx, ocg, PDF_NAME(Type), pdf_new_name(ctx, "OCG"));
        pdf_dict_put_drop(ctx, ocg, PDF_NAME(Name), pdf_new_text_string(ctx, ocgName.c_str()));
        pdf_obj* ocgRef = pdf_add_object_drop(ctx, doc, ocg);

        std::vector<pdf_obj*> pageRefs;
        for (int i = 0; i < pageCount; ++i) {
            fz_buffer* buf = fz_new_buffer_from_copied_data(
                ctx, (const unsigned char*)contents[(size_t)i].data(),
                contents[(size_t)i].size());
            pdf_obj* res = pdf_new_dict(ctx, doc, 3);
            pdf_obj* fonts = pdf_new_dict(ctx, doc, 1);
            pdf_obj* f1 = pdf_new_dict(ctx, doc, 3);
            pdf_dict_put(ctx, f1, PDF_NAME(Type), PDF_NAME(Font));
            pdf_dict_put(ctx, f1, PDF_NAME(Subtype), PDF_NAME(Type1));
            pdf_dict_put_drop(ctx, f1, PDF_NAME(BaseFont), pdf_new_name(ctx, "Helvetica"));
            pdf_dict_puts_drop(ctx, fonts, "F1", f1);
            pdf_dict_put_drop(ctx, res, PDF_NAME(Font), fonts);
            pdf_obj* props = pdf_new_dict(ctx, doc, 1);
            pdf_dict_puts(ctx, props, "MC0", ocgRef);
            pdf_dict_put_drop(ctx, res, PDF_NAME(Properties), props);

            pdf_obj* pageObj = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 612, 792), 0, res, buf);
            fz_drop_buffer(ctx, buf);
            pdf_drop_obj(ctx, res);
            pdf_insert_page(ctx, doc, -1, pageObj);
            pageRefs.push_back(pageObj);   // owned; dropped below
            pdf_dict_puts_drop(ctx, pageObj, "StructParents", pdf_new_int(ctx, i));
        }

        // Annotations: one Square markup annot and one merged widget/text field
        // per page.
        std::vector<pdf_obj*> widgetRefs;
        for (int i = 0; i < pageCount; ++i) {
            pdf_obj* annots = pdf_new_array(ctx, doc, 2);

            pdf_obj* sq = pdf_new_dict(ctx, doc, 6);
            pdf_dict_put(ctx, sq, PDF_NAME(Type), PDF_NAME(Annot));
            pdf_dict_put(ctx, sq, PDF_NAME(Subtype), PDF_NAME(Square));
            pdf_dict_put_drop(ctx, sq, PDF_NAME(Rect),
                              pdf_new_rect(ctx, doc, fz_make_rect(100, 100, 200, 160)));
            pdf_dict_put_drop(ctx, sq, PDF_NAME(Contents),
                              pdf_new_text_string(ctx, annotTexts[(size_t)i].c_str()));
            pdf_dict_put(ctx, sq, PDF_NAME(P), pageRefs[(size_t)i]);
            pdf_array_push_drop(ctx, annots, pdf_add_object_drop(ctx, doc, sq));

            pdf_obj* w = pdf_new_dict(ctx, doc, 10);
            pdf_dict_put(ctx, w, PDF_NAME(Type), PDF_NAME(Annot));
            pdf_dict_put(ctx, w, PDF_NAME(Subtype), PDF_NAME(Widget));
            pdf_dict_put(ctx, w, PDF_NAME(FT), PDF_NAME(Tx));
            pdf_dict_put_drop(ctx, w, PDF_NAME(T),
                              pdf_new_text_string(ctx, fieldNames[(size_t)i].c_str()));
            pdf_dict_put_drop(ctx, w, PDF_NAME(V),
                              pdf_new_text_string(ctx, fieldValues[(size_t)i].c_str()));
            pdf_dict_put_drop(ctx, w, PDF_NAME(DA),
                              pdf_new_string(ctx, "/Helv 12 Tf 0 g", 15));
            pdf_dict_put_drop(ctx, w, PDF_NAME(Rect),
                              pdf_new_rect(ctx, doc, fz_make_rect(300, 100, 500, 130)));
            pdf_dict_put_drop(ctx, w, PDF_NAME(F), pdf_new_int(ctx, 4));
            pdf_dict_put(ctx, w, PDF_NAME(P), pageRefs[(size_t)i]);
            pdf_obj* wRef = pdf_add_object_drop(ctx, doc, w);
            widgetRefs.push_back(wRef);
            pdf_array_push(ctx, annots, wRef);

            pdf_dict_put_drop(ctx, pageRefs[(size_t)i], PDF_NAME(Annots), annots);
        }

        // /AcroForm.
        pdf_obj* acro = pdf_new_dict(ctx, doc, 3);
        pdf_obj* fields = pdf_new_array(ctx, doc, pageCount);
        for (pdf_obj* w : widgetRefs) pdf_array_push(ctx, fields, w);
        pdf_dict_put_drop(ctx, acro, PDF_NAME(Fields), fields);
        pdf_dict_put_drop(ctx, acro, PDF_NAME(DA), pdf_new_string(ctx, "/Helv 0 Tf 0 g", 14));
        pdf_dict_put_drop(ctx, root, PDF_NAME(AcroForm), pdf_add_object_drop(ctx, doc, acro));

        // /Outlines: one flat item per page.
        pdf_obj* outlines = pdf_new_dict(ctx, doc, 4);
        pdf_dict_put(ctx, outlines, PDF_NAME(Type), PDF_NAME(Outlines));
        pdf_obj* outRef = pdf_add_object_drop(ctx, doc, outlines);
        std::vector<pdf_obj*> items;
        for (int i = 0; i < pageCount; ++i) {
            pdf_obj* it = pdf_new_dict(ctx, doc, 6);
            pdf_dict_put_drop(ctx, it, PDF_NAME(Title),
                              pdf_new_text_string(ctx, bookmarks[(size_t)i].c_str()));
            pdf_obj* dest = pdf_new_array(ctx, doc, 5);
            pdf_array_push(ctx, dest, pageRefs[(size_t)i]);
            pdf_array_push(ctx, dest, PDF_NAME(XYZ));
            pdf_array_push(ctx, dest, PDF_NULL);
            pdf_array_push(ctx, dest, PDF_NULL);
            pdf_array_push(ctx, dest, PDF_NULL);
            pdf_dict_put_drop(ctx, it, PDF_NAME(Dest), dest);
            pdf_dict_put(ctx, it, PDF_NAME(Parent), outRef);
            items.push_back(pdf_add_object_drop(ctx, doc, it));
        }
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) pdf_dict_put(ctx, items[i], PDF_NAME(Prev), items[i - 1]);
            if (i + 1 < items.size()) pdf_dict_put(ctx, items[i], PDF_NAME(Next), items[i + 1]);
        }
        if (!items.empty()) {
            pdf_dict_put(ctx, outRef, PDF_NAME(First), items.front());
            pdf_dict_put(ctx, outRef, PDF_NAME(Last), items.back());
            pdf_dict_put_drop(ctx, outRef, PDF_NAME(Count), pdf_new_int(ctx, (int)items.size()));
        }
        pdf_dict_put(ctx, root, PDF_NAME(Outlines), outRef);

        // /PageLabels: lowercase roman with a per-fixture prefix from page 0.
        pdf_obj* labels = pdf_new_dict(ctx, doc, 1);
        pdf_obj* nums = pdf_new_array(ctx, doc, 2);
        pdf_array_push_drop(ctx, nums, pdf_new_int(ctx, 0));
        pdf_obj* le = pdf_new_dict(ctx, doc, 3);
        pdf_dict_put_drop(ctx, le, PDF_NAME(S), pdf_new_name(ctx, "r"));
        pdf_dict_put_drop(ctx, le, PDF_NAME(P), pdf_new_text_string(ctx, labelPrefix.c_str()));
        pdf_dict_put_drop(ctx, le, PDF_NAME(St), pdf_new_int(ctx, 1));
        pdf_array_push_drop(ctx, nums, le);
        pdf_dict_put_drop(ctx, labels, PDF_NAME(Nums), nums);
        pdf_dict_put_drop(ctx, root, PDF_NAME(PageLabels), pdf_add_object_drop(ctx, doc, labels));

        // /OCProperties.
        pdf_obj* ocp = pdf_new_dict(ctx, doc, 2);
        pdf_obj* ocgs = pdf_new_array(ctx, doc, 1);
        pdf_array_push(ctx, ocgs, ocgRef);
        pdf_dict_put(ctx, ocp, PDF_NAME(OCGs), ocgs);
        pdf_obj* dcfg = pdf_new_dict(ctx, doc, 2);
        pdf_obj* order = pdf_new_array(ctx, doc, 1);
        pdf_array_push(ctx, order, ocgRef);
        pdf_dict_put_drop(ctx, dcfg, PDF_NAME(Order), order);
        pdf_obj* on = pdf_new_array(ctx, doc, 1);
        pdf_array_push(ctx, on, ocgRef);
        pdf_dict_put_drop(ctx, dcfg, PDF_NAME(ON), on);
        pdf_dict_put_drop(ctx, ocp, PDF_NAME(D), dcfg);
        pdf_dict_put_drop(ctx, ocp, PDF_NAME(OCGs), ocgs);
        pdf_dict_put_drop(ctx, root, PDF_NAME(OCProperties), pdf_add_object_drop(ctx, doc, ocp));

        // /StructTreeRoot: Document > P per page, each anchored to its page.
        pdf_obj* stRoot = pdf_new_dict(ctx, doc, 4);
        pdf_dict_put(ctx, stRoot, PDF_NAME(Type), PDF_NAME(StructTreeRoot));
        pdf_obj* stRootRef = pdf_add_object_drop(ctx, doc, stRoot);
        pdf_obj* docElem = pdf_new_dict(ctx, doc, 4);
        pdf_dict_put_drop(ctx, docElem, PDF_NAME(Type), pdf_new_name(ctx, "StructElem"));
        pdf_dict_put_drop(ctx, docElem, PDF_NAME(S), pdf_new_name(ctx, "Document"));
        pdf_dict_put(ctx, docElem, PDF_NAME(P), stRootRef);
        pdf_obj* docElemRef = pdf_add_object_drop(ctx, doc, docElem);
        pdf_obj* docKids = pdf_new_array(ctx, doc, pageCount);
        pdf_obj* ptNums = pdf_new_array(ctx, doc, pageCount * 2);
        for (int i = 0; i < pageCount; ++i) {
            pdf_obj* pe = pdf_new_dict(ctx, doc, 5);
            pdf_dict_put_drop(ctx, pe, PDF_NAME(Type), pdf_new_name(ctx, "StructElem"));
            pdf_dict_put(ctx, pe, PDF_NAME(S), PDF_NAME(P));
            pdf_dict_put(ctx, pe, PDF_NAME(P), docElemRef);
            pdf_dict_puts(ctx, pe, "Pg", pageRefs[(size_t)i]);
            pdf_dict_put_drop(ctx, pe, PDF_NAME(K), pdf_new_int(ctx, 0));
            pdf_obj* peRef = pdf_add_object_drop(ctx, doc, pe);
            pdf_array_push(ctx, docKids, peRef);
            pdf_array_push_drop(ctx, ptNums, pdf_new_int(ctx, i));
            pdf_obj* one = pdf_new_array(ctx, doc, 1);
            pdf_array_push(ctx, one, peRef);
            pdf_array_push_drop(ctx, ptNums, one);
            pdf_drop_obj(ctx, peRef);
        }
        pdf_dict_put_drop(ctx, docElemRef, PDF_NAME(K), docKids);
        pdf_obj* stK = pdf_new_array(ctx, doc, 1);
        pdf_array_push(ctx, stK, docElemRef);
        pdf_dict_put_drop(ctx, stRootRef, PDF_NAME(K), stK);
        pdf_obj* pt = pdf_new_dict(ctx, doc, 1);
        pdf_dict_put_drop(ctx, pt, PDF_NAME(Nums), ptNums);
        pdf_dict_put_drop(ctx, stRootRef, PDF_NAME(ParentTree), pdf_add_object_drop(ctx, doc, pt));
        pdf_dict_puts_drop(ctx, stRootRef, "ParentTreeNextKey", pdf_new_int(ctx, pageCount));
        pdf_dict_put(ctx, root, PDF_NAME(StructTreeRoot), stRootRef);
        pdf_obj* mi = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, mi, "Marked", PDF_TRUE);
        pdf_dict_puts_drop(ctx, root, "MarkInfo", pdf_add_object_drop(ctx, doc, mi));
        pdf_drop_obj(ctx, docElemRef);
        pdf_drop_obj(ctx, stRootRef);

        // /Names: EmbeddedFiles, JavaScript and Dests.
        pdf_obj* nameDict = pdf_new_dict(ctx, doc, 3);

        const std::string payload = "EMBEDDED_" + tag + "_SECRET";
        fz_buffer* ebuf = fz_new_buffer_from_copied_data(
            ctx, (const unsigned char*)payload.data(), payload.size());
        pdf_obj* estream = pdf_add_stream(ctx, doc, ebuf, nullptr, 0);
        fz_drop_buffer(ctx, ebuf);
        pdf_obj* filespec = pdf_new_dict(ctx, doc, 4);
        pdf_dict_put(ctx, filespec, PDF_NAME(Type), PDF_NAME(Filespec));
        pdf_dict_put_drop(ctx, filespec, PDF_NAME(F), pdf_new_text_string(ctx, embName.c_str()));
        pdf_dict_put_drop(ctx, filespec, PDF_NAME(UF), pdf_new_text_string(ctx, embName.c_str()));
        pdf_obj* ef = pdf_new_dict(ctx, doc, 1);
        pdf_dict_put_drop(ctx, ef, PDF_NAME(F), estream);
        pdf_dict_put_drop(ctx, filespec, PDF_NAME(EF), ef);
        pdf_obj* efNames = pdf_new_array(ctx, doc, 2);
        pdf_array_push_drop(ctx, efNames, pdf_new_text_string(ctx, embName.c_str()));
        pdf_array_push_drop(ctx, efNames, pdf_add_object_drop(ctx, doc, filespec));
        pdf_obj* efTree = pdf_new_dict(ctx, doc, 1);
        pdf_dict_put_drop(ctx, efTree, PDF_NAME(Names), efNames);
        pdf_dict_put_drop(ctx, nameDict, PDF_NAME(EmbeddedFiles),
                          pdf_add_object_drop(ctx, doc, efTree));

        pdf_obj* jsAction = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put(ctx, jsAction, PDF_NAME(S), PDF_NAME(JavaScript));
        pdf_dict_put_drop(ctx, jsAction, PDF_NAME(JS),
                          pdf_new_text_string(ctx, "var pdfeSentinel = 1;"));
        pdf_obj* jsNames = pdf_new_array(ctx, doc, 2);
        pdf_array_push_drop(ctx, jsNames, pdf_new_text_string(ctx, jsName.c_str()));
        pdf_array_push_drop(ctx, jsNames, pdf_add_object_drop(ctx, doc, jsAction));
        pdf_obj* jsTree = pdf_new_dict(ctx, doc, 1);
        pdf_dict_put_drop(ctx, jsTree, PDF_NAME(Names), jsNames);
        pdf_dict_put_drop(ctx, nameDict, PDF_NAME(JavaScript),
                          pdf_add_object_drop(ctx, doc, jsTree));

        pdf_obj* destNamesArr = pdf_new_array(ctx, doc, pageCount * 2);
        for (int i = 0; i < pageCount; ++i) {
            pdf_array_push_drop(ctx, destNamesArr,
                                pdf_new_text_string(ctx, destNames[(size_t)i].c_str()));
            pdf_obj* dest = pdf_new_array(ctx, doc, 5);
            pdf_array_push(ctx, dest, pageRefs[(size_t)i]);
            pdf_array_push(ctx, dest, PDF_NAME(XYZ));
            pdf_array_push(ctx, dest, PDF_NULL);
            pdf_array_push(ctx, dest, PDF_NULL);
            pdf_array_push(ctx, dest, PDF_NULL);
            pdf_array_push_drop(ctx, destNamesArr, dest);
        }
        pdf_obj* destTree = pdf_new_dict(ctx, doc, 1);
        pdf_dict_put_drop(ctx, destTree, PDF_NAME(Names), destNamesArr);
        pdf_dict_put_drop(ctx, nameDict, PDF_NAME(Dests),
                          pdf_add_object_drop(ctx, doc, destTree));

        pdf_dict_put_drop(ctx, root, PDF_NAME(Names), pdf_add_object_drop(ctx, doc, nameDict));

        for (pdf_obj* p : pageRefs) pdf_drop_obj(ctx, p);
        for (pdf_obj* w : widgetRefs) pdf_drop_obj(ctx, w);
        for (pdf_obj* it : items) pdf_drop_obj(ctx, it);
        pdf_drop_obj(ctx, ocgRef);

        pdf_write_options opts = pdf_default_write_options;
        opts.do_compress = 0;
        opts.do_garbage = 0;
        pdf_save_document(ctx, doc, outStr.c_str(), &opts);
    }
    fz_always(ctx) { if (doc) pdf_drop_document(ctx, doc); }
    fz_catch(ctx) { names.tag.clear(); }
    fz_drop_context(ctx);
    return names;
}

} // namespace pdfe::testsupport
