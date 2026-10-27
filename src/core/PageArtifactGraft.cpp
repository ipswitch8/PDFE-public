// -*- coding: utf-8 -*-
// F6 / PDF-88, PDF-89: document-level artifact preservation for cross-document
// page grafting. See PageArtifactGraft.h for the contract.
//
// MuPDF gotchas observed here:
//  * No C++ exception may cross an fz_try frame -- every failure path uses
//    fz_throw and the caller's MuPdfContext::execute() converts it.
//  * Every std::-container member is constructed by the ArtifactGrafter ctor,
//    which runs before any fz_try in this translation unit.

#include "PageArtifactGraft.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pdfe::core {
namespace {

// ---------------------------------------------------------------------------
// Number-tree helpers (used for /PageLabels and /StructTreeRoot /ParentTree).
// ---------------------------------------------------------------------------

void collectNumsTree(fz_context* ctx, pdf_obj* node,
                     std::vector<std::pair<int, pdf_obj*>>& out, int depth)
{
    if (!node || depth > 32) return;
    pdf_obj* nums = pdf_dict_get(ctx, node, PDF_NAME(Nums));
    if (pdf_is_array(ctx, nums)) {
        const int n = pdf_array_len(ctx, nums);
        for (int i = 0; i + 1 < n; i += 2) {
            pdf_obj* k = pdf_array_get(ctx, nums, i);
            pdf_obj* v = pdf_array_get(ctx, nums, i + 1);
            if (pdf_is_int(ctx, k)) out.emplace_back(pdf_to_int(ctx, k), v);
        }
    }
    pdf_obj* kids = pdf_dict_get(ctx, node, PDF_NAME(Kids));
    if (pdf_is_array(ctx, kids)) {
        const int n = pdf_array_len(ctx, kids);
        for (int i = 0; i < n; ++i) {
            collectNumsTree(ctx, pdf_array_get(ctx, kids, i), out, depth + 1);
        }
    }
}

/// The effective /PageLabels descriptor for one page index.
struct LabelDesc {
    bool has = false;
    std::string style;   // "D", "R", "r", "A", "a"; empty = no numeric portion
    std::string prefix;
    int number = 1;

    bool sameSeries(const LabelDesc& o) const {
        return has == o.has && style == o.style && prefix == o.prefix;
    }
};

// ---------------------------------------------------------------------------

class ArtifactGrafter {
public:
    ArtifactGrafter(fz_context* ctx, pdf_document* dst, pdf_document* src)
        : ctx_(ctx), dst_(dst), src_(src) {}

    ~ArtifactGrafter() { release(); }

    void run(const std::vector<int>& srcPageIndices, int dstInsertAt);

private:
    // --- small helpers -----------------------------------------------------
    pdf_obj* own(pdf_obj* o) { if (o) owned_.push_back(o); return o; }
    void release() {
        names_.clear();
        for (pdf_obj* o : owned_) pdf_drop_obj(ctx_, o);
        owned_.clear();
        if (map_) { pdf_drop_graft_map(ctx_, map_); map_ = nullptr; }
    }
    pdf_obj* catalogOf(pdf_document* doc) const {
        return pdf_dict_get(ctx_, pdf_trailer(ctx_, doc), PDF_NAME(Root));
    }
    pdf_obj* dstRoot() const { return catalogOf(dst_); }
    pdf_obj* srcRoot() const { return catalogOf(src_); }
    pdf_obj* graft(pdf_obj* v) { return own(pdf_graft_mapped_object(ctx_, map_, v)); }
    pdf_obj* refTo(int num) { return own(pdf_new_indirect(ctx_, dst_, num, 0)); }
    /// Name object for a key MuPDF's static PDF_NAME table does not carry.
    pdf_obj* nm(const char* s) {
        auto it = names_.find(s);
        if (it != names_.end()) return it->second;
        pdf_obj* n = own(pdf_new_name(ctx_, s));
        names_[s] = n;
        return n;
    }

    // --- destinations ------------------------------------------------------
    int srcPageNumOfDest(pdf_obj* destArray) const;
    pdf_obj* graftDestValue(pdf_obj* v);
    pdf_obj* resolveDestForLink(pdf_obj* d);
    pdf_obj* graftAction(pdf_obj* a);

    // --- annotations and form fields ---------------------------------------
    pdf_obj* graftAnnot(pdf_obj* srcAnnotRef, int dstPageNum);
    pdf_obj* graftField(pdf_obj* srcFieldRef);
    void finishAcroForm();

    // --- catalog artifacts --------------------------------------------------
    void collectSrcDests();
    void mergeNamedDests();
    void mergeOtherNameTrees();
    void mergeOutline();
    void copyOutlineChain(pdf_obj* srcFirst, pdf_obj* parentRef,
                          std::vector<std::pair<int, pdf_obj*>>& out, int depth);
    std::vector<LabelDesc> capturePageLabels(pdf_document* doc) const;
    void mergePageLabels(const std::vector<LabelDesc>& dstBefore,
                         const std::vector<int>& srcPageIndices, int dstInsertAt);
    void mergeOcProperties();
    void attachStructTree();
    pdf_obj* copyStructElem(pdf_obj* srcRef, pdf_obj* parentRef, int depth);
    void copySimpleCatalogKeys();
    void writeNameTree(pdf_obj* which, const std::map<std::string, pdf_obj*>& entries);

    fz_context* ctx_;
    pdf_document* dst_;
    pdf_document* src_;
    pdf_graft_map* map_ = nullptr;

    std::vector<pdf_obj*> owned_;
    std::map<std::string, pdf_obj*> names_;
    std::map<int, int> pageNumMap_;    // src page obj num -> dst page obj num
    std::map<int, int> annotMap_;      // src annot obj num -> dst obj num
    std::map<int, int> fieldMap_;      // src field obj num -> dst obj num
    std::map<int, int> structMap_;     // src StructElem obj num -> dst obj num
    std::vector<int> rootFieldNums_;   // dst obj nums to append to /AcroForm /Fields
    std::map<std::string, pdf_obj*> srcDests_;        // name -> src dest value
    std::map<std::string, std::string> destNameRemap_;// src name -> dst name
    std::set<std::string> liveDestNames_;             // names present in dst after merge
    bool attachStructTree_ = false;
};

// ---------------------------------------------------------------------------
// Destinations
// ---------------------------------------------------------------------------

int ArtifactGrafter::srcPageNumOfDest(pdf_obj* destArray) const
{
    if (!pdf_is_array(ctx_, destArray) || pdf_array_len(ctx_, destArray) < 1) return -1;
    pdf_obj* p = pdf_array_get(ctx_, destArray, 0);
    if (pdf_is_indirect(ctx_, p)) return pdf_to_num(ctx_, p);
    if (pdf_is_int(ctx_, p)) {
        const int idx = pdf_to_int(ctx_, p);
        if (idx >= 0 && idx < pdf_count_pages(ctx_, src_)) {
            pdf_obj* po = pdf_lookup_page_obj(ctx_, src_, idx);
            if (po) return pdf_to_num(ctx_, po);
        }
    }
    return -1;
}

pdf_obj* ArtifactGrafter::graftDestValue(pdf_obj* v)
{
    pdf_obj* arr = v;
    if (pdf_is_dict(ctx_, pdf_resolve_indirect(ctx_, v))) {
        arr = pdf_dict_get(ctx_, v, PDF_NAME(D));
    }
    arr = pdf_resolve_indirect(ctx_, arr);
    if (!pdf_is_array(ctx_, arr)) return nullptr;

    const int srcNum = srcPageNumOfDest(arr);
    auto it = pageNumMap_.find(srcNum);
    if (it == pageNumMap_.end()) return nullptr;   // destination page not grafted

    const int len = pdf_array_len(ctx_, arr);
    pdf_obj* out = own(pdf_new_array(ctx_, dst_, len > 0 ? len : 1));
    pdf_array_push_drop(ctx_, out, pdf_new_indirect(ctx_, dst_, it->second, 0));
    for (int i = 1; i < len; ++i) {
        pdf_array_push_drop(ctx_, out,
            pdf_graft_mapped_object(ctx_, map_, pdf_array_get(ctx_, arr, i)));
    }
    return out;
}

pdf_obj* ArtifactGrafter::resolveDestForLink(pdf_obj* d)
{
    if (!d) return nullptr;
    if (pdf_is_name(ctx_, d) || pdf_is_string(ctx_, d)) {
        const char* raw = pdf_is_name(ctx_, d) ? pdf_to_name(ctx_, d)
                                               : pdf_to_text_string(ctx_, d);
        if (!raw) return nullptr;
        std::string name(raw);
        auto remap = destNameRemap_.find(name);
        const std::string target = (remap != destNameRemap_.end()) ? remap->second : name;
        if (liveDestNames_.count(target)) {
            return own(pdf_new_text_string(ctx_, target.c_str()));
        }
        // Named destination that did not survive the page selection: fall back
        // to an explicit destination if its page did survive after all.
        auto sd = srcDests_.find(name);
        if (sd != srcDests_.end()) return graftDestValue(sd->second);
        return nullptr;
    }
    return graftDestValue(d);
}

pdf_obj* ArtifactGrafter::graftAction(pdf_obj* a)
{
    pdf_obj* r = pdf_resolve_indirect(ctx_, a);
    if (!pdf_is_dict(ctx_, r)) return nullptr;
    pdf_obj* s = pdf_dict_get(ctx_, r, PDF_NAME(S));
    if (pdf_name_eq(ctx_, s, PDF_NAME(GoTo))) {
        pdf_obj* nd = resolveDestForLink(pdf_dict_get(ctx_, r, PDF_NAME(D)));
        if (!nd) return nullptr;   // dangling GoTo -> drop the action entirely
        pdf_obj* out = own(pdf_new_dict(ctx_, dst_, 3));
        pdf_dict_put(ctx_, out, PDF_NAME(Type), PDF_NAME(Action));
        pdf_dict_put(ctx_, out, PDF_NAME(S), PDF_NAME(GoTo));
        pdf_dict_put(ctx_, out, PDF_NAME(D), nd);
        return out;
    }
    // Actions that cannot reference a page in this document are safe to copy.
    if (pdf_name_eq(ctx_, s, PDF_NAME(URI)) ||
        pdf_name_eq(ctx_, s, PDF_NAME(GoToR)) ||
        pdf_name_eq(ctx_, s, PDF_NAME(Launch)) ||
        pdf_name_eq(ctx_, s, PDF_NAME(Named)) ||
        pdf_name_eq(ctx_, s, PDF_NAME(JavaScript)) ||
        pdf_name_eq(ctx_, s, nm("SubmitForm")) ||
        pdf_name_eq(ctx_, s, PDF_NAME(ResetForm))) {
        return graft(r);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Annotations and form fields
// ---------------------------------------------------------------------------

pdf_obj* ArtifactGrafter::graftAnnot(pdf_obj* srcAnnotRef, int dstPageNum)
{
    if (!srcAnnotRef) return nullptr;
    const int srcNum = pdf_is_indirect(ctx_, srcAnnotRef) ? pdf_to_num(ctx_, srcAnnotRef) : 0;
    if (srcNum) {
        auto it = annotMap_.find(srcNum);
        if (it != annotMap_.end()) return refTo(it->second);
    }
    pdf_obj* r = pdf_resolve_indirect(ctx_, srcAnnotRef);
    if (!pdf_is_dict(ctx_, r)) return nullptr;

    const int newNum = pdf_create_object(ctx_, dst_);
    if (srcNum) annotMap_[srcNum] = newNum;

    pdf_obj* d = own(pdf_new_dict(ctx_, dst_, 16));
    const int n = pdf_dict_len(ctx_, r);
    for (int i = 0; i < n; ++i) {
        pdf_obj* key = pdf_dict_get_key(ctx_, r, i);
        pdf_obj* val = pdf_dict_get_val(ctx_, r, i);
        if (pdf_name_eq(ctx_, key, PDF_NAME(P)) ||
            pdf_name_eq(ctx_, key, PDF_NAME(Popup)) ||
            pdf_name_eq(ctx_, key, PDF_NAME(Parent)) ||
            pdf_name_eq(ctx_, key, PDF_NAME(IRT)) ||
            pdf_name_eq(ctx_, key, PDF_NAME(Dest)) ||
            pdf_name_eq(ctx_, key, PDF_NAME(A)) ||
            pdf_name_eq(ctx_, key, PDF_NAME(StructParent))) {
            continue;   // handled below (or deliberately dropped)
        }
        pdf_dict_put_drop(ctx_, d, key, pdf_graft_mapped_object(ctx_, map_, val));
    }
    pdf_dict_put_drop(ctx_, d, PDF_NAME(P), pdf_new_indirect(ctx_, dst_, dstPageNum, 0));

    pdf_obj* nd = resolveDestForLink(pdf_dict_get(ctx_, r, PDF_NAME(Dest)));
    if (nd) pdf_dict_put(ctx_, d, PDF_NAME(Dest), nd);
    pdf_obj* na = graftAction(pdf_dict_get(ctx_, r, PDF_NAME(A)));
    if (na) pdf_dict_put(ctx_, d, PDF_NAME(A), na);

    if (attachStructTree_) {
        pdf_obj* sp = pdf_dict_get(ctx_, r, PDF_NAME(StructParent));
        if (pdf_is_int(ctx_, sp)) {
            pdf_dict_put_drop(ctx_, d, PDF_NAME(StructParent),
                              pdf_new_int(ctx_, pdf_to_int(ctx_, sp)));
        }
    }

    pdf_update_object(ctx_, dst_, newNum, d);
    pdf_obj* ref = refTo(newNum);

    // Cyclic links, resolved after the object exists so recursion terminates.
    pdf_obj* popup = pdf_dict_get(ctx_, r, PDF_NAME(Popup));
    if (popup) {
        pdf_obj* pr = graftAnnot(popup, dstPageNum);
        if (pr) pdf_dict_put(ctx_, d, PDF_NAME(Popup), pr);
    }
    pdf_obj* irt = pdf_dict_get(ctx_, r, PDF_NAME(IRT));
    if (irt) {
        pdf_obj* ir = graftAnnot(irt, dstPageNum);
        if (ir) pdf_dict_put(ctx_, d, PDF_NAME(IRT), ir);
    }

    const bool isWidget = pdf_name_eq(ctx_, pdf_dict_get(ctx_, r, PDF_NAME(Subtype)),
                                      PDF_NAME(Widget));
    pdf_obj* parent = pdf_dict_get(ctx_, r, PDF_NAME(Parent));
    if (parent && isWidget) {
        pdf_obj* fr = graftField(parent);
        if (fr) {
            pdf_dict_put(ctx_, d, PDF_NAME(Parent), fr);
            pdf_obj* kids = pdf_dict_get(ctx_, fr, PDF_NAME(Kids));
            if (pdf_is_array(ctx_, kids)) pdf_array_push(ctx_, kids, ref);
        }
    } else if (parent) {
        pdf_obj* ar = graftAnnot(parent, dstPageNum);
        if (ar) pdf_dict_put(ctx_, d, PDF_NAME(Parent), ar);
    } else if (isWidget && pdf_dict_get(ctx_, r, PDF_NAME(FT))) {
        // Merged field/widget: the annotation IS a root form field.
        rootFieldNums_.push_back(newNum);
    }
    return ref;
}

pdf_obj* ArtifactGrafter::graftField(pdf_obj* srcFieldRef)
{
    if (!srcFieldRef) return nullptr;
    const int srcNum = pdf_is_indirect(ctx_, srcFieldRef) ? pdf_to_num(ctx_, srcFieldRef) : 0;
    if (srcNum) {
        auto it = fieldMap_.find(srcNum);
        if (it != fieldMap_.end()) return refTo(it->second);
    }
    pdf_obj* r = pdf_resolve_indirect(ctx_, srcFieldRef);
    if (!pdf_is_dict(ctx_, r)) return nullptr;

    const int newNum = pdf_create_object(ctx_, dst_);
    if (srcNum) fieldMap_[srcNum] = newNum;

    // A signature VALUE can never be valid over a different page set, so the
    // field is carried but its /V and /DV are not.
    const bool isSig = pdf_name_eq(ctx_, pdf_dict_get(ctx_, r, PDF_NAME(FT)), PDF_NAME(Sig));

    pdf_obj* d = own(pdf_new_dict(ctx_, dst_, 12));
    const int n = pdf_dict_len(ctx_, r);
    for (int i = 0; i < n; ++i) {
        pdf_obj* key = pdf_dict_get_key(ctx_, r, i);
        pdf_obj* val = pdf_dict_get_val(ctx_, r, i);
        if (pdf_name_eq(ctx_, key, PDF_NAME(Kids)) ||
            pdf_name_eq(ctx_, key, PDF_NAME(Parent)) ||
            pdf_name_eq(ctx_, key, PDF_NAME(P))) {
            continue;
        }
        if (isSig && (pdf_name_eq(ctx_, key, PDF_NAME(V)) ||
                      pdf_name_eq(ctx_, key, PDF_NAME(DV)))) {
            continue;
        }
        pdf_dict_put_drop(ctx_, d, key, pdf_graft_mapped_object(ctx_, map_, val));
    }
    pdf_dict_put_drop(ctx_, d, PDF_NAME(Kids), pdf_new_array(ctx_, dst_, 4));
    pdf_update_object(ctx_, dst_, newNum, d);
    pdf_obj* ref = refTo(newNum);

    pdf_obj* parent = pdf_dict_get(ctx_, r, PDF_NAME(Parent));
    pdf_obj* pr = parent ? graftField(parent) : nullptr;
    if (pr) {
        pdf_dict_put(ctx_, d, PDF_NAME(Parent), pr);
        pdf_obj* kids = pdf_dict_get(ctx_, pr, PDF_NAME(Kids));
        if (pdf_is_array(ctx_, kids)) pdf_array_push(ctx_, kids, ref);
    } else {
        rootFieldNums_.push_back(newNum);
    }
    return ref;
}

void ArtifactGrafter::finishAcroForm()
{
    pdf_obj* sacro = pdf_dict_get(ctx_, srcRoot(), PDF_NAME(AcroForm));
    if (rootFieldNums_.empty() && !sacro) return;

    pdf_obj* droot = dstRoot();
    pdf_obj* acro = pdf_dict_get(ctx_, droot, PDF_NAME(AcroForm));
    if (!pdf_is_dict(ctx_, pdf_resolve_indirect(ctx_, acro))) {
        pdf_obj* fresh = own(pdf_new_dict(ctx_, dst_, 4));
        pdf_obj* aref = own(pdf_add_object(ctx_, dst_, fresh));
        pdf_dict_put(ctx_, droot, PDF_NAME(AcroForm), aref);
        acro = aref;
    }
    pdf_obj* fields = pdf_dict_get(ctx_, acro, PDF_NAME(Fields));
    if (!pdf_is_array(ctx_, pdf_resolve_indirect(ctx_, fields))) {
        pdf_obj* arr = own(pdf_new_array(ctx_, dst_, 8));
        pdf_dict_put(ctx_, acro, PDF_NAME(Fields), arr);
        fields = arr;
    }
    for (int num : rootFieldNums_) {
        pdf_array_push_drop(ctx_, fields, pdf_new_indirect(ctx_, dst_, num, 0));
    }
    if (sacro) {
        static const char* const kCarry[] = { "DR", "DA", "Q", "NeedAppearances" };
        for (const char* key : kCarry) {
            pdf_obj* sv = pdf_dict_gets(ctx_, sacro, key);
            if (sv && !pdf_dict_gets(ctx_, acro, key)) {
                pdf_dict_puts_drop(ctx_, acro, key,
                                   pdf_graft_mapped_object(ctx_, map_, sv));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Name trees
// ---------------------------------------------------------------------------

void ArtifactGrafter::collectSrcDests()
{
    pdf_obj* tree = pdf_load_name_tree(ctx_, src_, PDF_NAME(Dests));
    if (tree) {
        own(tree);
        const int n = pdf_dict_len(ctx_, tree);
        for (int i = 0; i < n; ++i) {
            const char* k = pdf_to_name(ctx_, pdf_dict_get_key(ctx_, tree, i));
            if (k) srcDests_[k] = pdf_dict_get_val(ctx_, tree, i);
        }
    }
    // Legacy PDF-1.1 catalog /Dests dictionary.
    pdf_obj* legacy = pdf_dict_get(ctx_, srcRoot(), PDF_NAME(Dests));
    if (pdf_is_dict(ctx_, pdf_resolve_indirect(ctx_, legacy))) {
        pdf_obj* r = pdf_resolve_indirect(ctx_, legacy);
        const int n = pdf_dict_len(ctx_, r);
        for (int i = 0; i < n; ++i) {
            const char* k = pdf_to_name(ctx_, pdf_dict_get_key(ctx_, r, i));
            if (k && !srcDests_.count(k)) srcDests_[k] = pdf_dict_get_val(ctx_, r, i);
        }
    }
}

void ArtifactGrafter::writeNameTree(pdf_obj* which,
                                    const std::map<std::string, pdf_obj*>& entries)
{
    pdf_obj* droot = dstRoot();
    pdf_obj* names = pdf_dict_get(ctx_, droot, PDF_NAME(Names));
    if (!pdf_is_dict(ctx_, pdf_resolve_indirect(ctx_, names))) {
        pdf_obj* fresh = own(pdf_new_dict(ctx_, dst_, 4));
        pdf_obj* nref = own(pdf_add_object(ctx_, dst_, fresh));
        pdf_dict_put(ctx_, droot, PDF_NAME(Names), nref);
        names = nref;
    }
    if (entries.empty()) return;

    pdf_obj* arr = own(pdf_new_array(ctx_, dst_, static_cast<int>(entries.size()) * 2));
    for (const auto& kv : entries) {   // std::map keeps the required sort order
        pdf_array_push_drop(ctx_, arr, pdf_new_text_string(ctx_, kv.first.c_str()));
        pdf_array_push(ctx_, arr, kv.second);
    }
    pdf_obj* node = own(pdf_new_dict(ctx_, dst_, 1));
    pdf_dict_put(ctx_, node, PDF_NAME(Names), arr);
    pdf_dict_put_drop(ctx_, names, which, pdf_add_object(ctx_, dst_, node));
}

void ArtifactGrafter::mergeNamedDests()
{
    std::map<std::string, pdf_obj*> merged;

    pdf_obj* dstTree = pdf_load_name_tree(ctx_, dst_, PDF_NAME(Dests));
    if (dstTree) {
        own(dstTree);
        const int n = pdf_dict_len(ctx_, dstTree);
        for (int i = 0; i < n; ++i) {
            const char* k = pdf_to_name(ctx_, pdf_dict_get_key(ctx_, dstTree, i));
            if (k) merged[k] = pdf_dict_get_val(ctx_, dstTree, i);
        }
    }
    const size_t preExisting = merged.size();

    for (const auto& kv : srcDests_) {
        pdf_obj* nv = graftDestValue(kv.second);
        if (!nv) continue;                       // target page was not grafted
        std::string name = kv.first;
        if (merged.count(name)) {
            std::string candidate;
            for (int suffix = 2; suffix < 1000; ++suffix) {
                candidate = name + "_" + std::to_string(suffix);
                if (!merged.count(candidate)) break;
            }
            destNameRemap_[name] = candidate;
            name = candidate;
        }
        merged[name] = nv;
    }
    for (const auto& kv : merged) liveDestNames_.insert(kv.first);

    if (merged.empty() && preExisting == 0) return;
    writeNameTree(PDF_NAME(Dests), merged);
}

void ArtifactGrafter::mergeOtherNameTrees()
{
    // Page-independent catalog name trees: a union by name is always correct;
    // only the key may collide, and a suffixed key keeps both payloads reachable.
    static const char* const kTreeNames[] = {
        "EmbeddedFiles", "JavaScript", "AP", "IDS", "URLS", "Renditions"
    };
    for (const char* whichName : kTreeNames) {
        pdf_obj* which = nm(whichName);
        pdf_obj* srcTree = pdf_load_name_tree(ctx_, src_, which);
        if (!srcTree) continue;
        own(srcTree);

        std::map<std::string, pdf_obj*> merged;
        pdf_obj* dstTree = pdf_load_name_tree(ctx_, dst_, which);
        if (dstTree) {
            own(dstTree);
            const int dn = pdf_dict_len(ctx_, dstTree);
            for (int i = 0; i < dn; ++i) {
                const char* k = pdf_to_name(ctx_, pdf_dict_get_key(ctx_, dstTree, i));
                if (k) merged[k] = pdf_dict_get_val(ctx_, dstTree, i);
            }
        }
        const int sn = pdf_dict_len(ctx_, srcTree);
        for (int i = 0; i < sn; ++i) {
            const char* k = pdf_to_name(ctx_, pdf_dict_get_key(ctx_, srcTree, i));
            if (!k) continue;
            std::string name(k);
            if (merged.count(name)) {
                for (int suffix = 2; suffix < 1000; ++suffix) {
                    std::string candidate = name + "_" + std::to_string(suffix);
                    if (!merged.count(candidate)) { name = candidate; break; }
                }
            }
            merged[name] = graft(pdf_dict_get_val(ctx_, srcTree, i));
        }
        writeNameTree(which, merged);
    }
}

// ---------------------------------------------------------------------------
// Outline
// ---------------------------------------------------------------------------

void ArtifactGrafter::copyOutlineChain(pdf_obj* srcFirst, pdf_obj* parentRef,
                                       std::vector<std::pair<int, pdf_obj*>>& out,
                                       int depth)
{
    if (depth > 32) return;
    pdf_obj* cur = srcFirst;
    for (int guard = 0; cur && guard < 20000; ++guard) {
        pdf_obj* r = pdf_resolve_indirect(ctx_, cur);
        if (!pdf_is_dict(ctx_, r)) break;
        pdf_obj* next = pdf_dict_get(ctx_, r, PDF_NAME(Next));

        const int newNum = pdf_create_object(ctx_, dst_);
        pdf_obj* d = own(pdf_new_dict(ctx_, dst_, 8));
        pdf_obj* ref = refTo(newNum);

        std::vector<std::pair<int, pdf_obj*>> kids;
        pdf_obj* kFirst = pdf_dict_get(ctx_, r, PDF_NAME(First));
        if (kFirst) copyOutlineChain(kFirst, ref, kids, depth + 1);

        pdf_obj* sDest = pdf_dict_get(ctx_, r, PDF_NAME(Dest));
        pdf_obj* sAct  = pdf_dict_get(ctx_, r, PDF_NAME(A));
        const bool hasDestSpec = (sDest != nullptr) || (sAct != nullptr);
        pdf_obj* nDest = sDest ? resolveDestForLink(sDest) : nullptr;
        pdf_obj* nAct  = (!nDest && sAct) ? graftAction(sAct) : nullptr;

        // An item whose destination page was dropped is itself dropped, UNLESS
        // it still has surviving children -- then it is kept as a pure grouping
        // node with the dangling destination removed.
        const bool survives = (!hasDestSpec) || nDest || nAct || !kids.empty();
        if (!survives) { cur = next; continue; }

        pdf_obj* title = pdf_dict_get(ctx_, r, PDF_NAME(Title));
        if (title) pdf_dict_put_drop(ctx_, d, PDF_NAME(Title),
                                     pdf_graft_mapped_object(ctx_, map_, title));
        pdf_obj* colour = pdf_dict_get(ctx_, r, PDF_NAME(C));
        if (colour) pdf_dict_put_drop(ctx_, d, PDF_NAME(C),
                                      pdf_graft_mapped_object(ctx_, map_, colour));
        pdf_obj* flags = pdf_dict_get(ctx_, r, PDF_NAME(F));
        if (pdf_is_int(ctx_, flags)) {
            pdf_dict_put_drop(ctx_, d, PDF_NAME(F), pdf_new_int(ctx_, pdf_to_int(ctx_, flags)));
        }
        if (nDest) pdf_dict_put(ctx_, d, PDF_NAME(Dest), nDest);
        if (nAct)  pdf_dict_put(ctx_, d, PDF_NAME(A), nAct);
        if (parentRef) pdf_dict_put(ctx_, d, PDF_NAME(Parent), parentRef);
        if (!kids.empty()) {
            pdf_dict_put_drop(ctx_, d, PDF_NAME(First),
                              pdf_new_indirect(ctx_, dst_, kids.front().first, 0));
            pdf_dict_put_drop(ctx_, d, PDF_NAME(Last),
                              pdf_new_indirect(ctx_, dst_, kids.back().first, 0));
            pdf_dict_put_drop(ctx_, d, PDF_NAME(Count),
                              pdf_new_int(ctx_, static_cast<int>(kids.size())));
        }
        pdf_update_object(ctx_, dst_, newNum, d);
        out.emplace_back(newNum, d);
        cur = next;
    }
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) {
            pdf_dict_put_drop(ctx_, out[i].second, PDF_NAME(Prev),
                              pdf_new_indirect(ctx_, dst_, out[i - 1].first, 0));
        }
        if (i + 1 < out.size()) {
            pdf_dict_put_drop(ctx_, out[i].second, PDF_NAME(Next),
                              pdf_new_indirect(ctx_, dst_, out[i + 1].first, 0));
        }
    }
}

void ArtifactGrafter::mergeOutline()
{
    pdf_obj* sOut = pdf_dict_get(ctx_, srcRoot(), PDF_NAME(Outlines));
    if (!sOut) return;
    pdf_obj* sFirst = pdf_dict_get(ctx_, pdf_resolve_indirect(ctx_, sOut), PDF_NAME(First));
    if (!sFirst) return;

    pdf_obj* droot = dstRoot();
    pdf_obj* dOutRef = pdf_dict_get(ctx_, droot, PDF_NAME(Outlines));
    if (!pdf_is_indirect(ctx_, dOutRef)) {
        pdf_obj* fresh = own(pdf_new_dict(ctx_, dst_, 4));
        pdf_dict_put(ctx_, fresh, PDF_NAME(Type), PDF_NAME(Outlines));
        pdf_obj* ref = own(pdf_add_object(ctx_, dst_, fresh));
        pdf_dict_put(ctx_, droot, PDF_NAME(Outlines), ref);
        dOutRef = ref;
    }

    std::vector<std::pair<int, pdf_obj*>> top;
    copyOutlineChain(sFirst, dOutRef, top, 0);
    if (top.empty()) return;

    pdf_obj* outRoot = pdf_resolve_indirect(ctx_, dOutRef);
    pdf_obj* existingLast = pdf_dict_get(ctx_, outRoot, PDF_NAME(Last));
    if (pdf_is_indirect(ctx_, existingLast)) {
        pdf_obj* lastDict = pdf_resolve_indirect(ctx_, existingLast);
        pdf_dict_put_drop(ctx_, lastDict, PDF_NAME(Next),
                          pdf_new_indirect(ctx_, dst_, top.front().first, 0));
        pdf_dict_put(ctx_, top.front().second, PDF_NAME(Prev), existingLast);
    } else {
        pdf_dict_put_drop(ctx_, outRoot, PDF_NAME(First),
                          pdf_new_indirect(ctx_, dst_, top.front().first, 0));
    }
    pdf_dict_put_drop(ctx_, outRoot, PDF_NAME(Last),
                      pdf_new_indirect(ctx_, dst_, top.back().first, 0));
    pdf_obj* count = pdf_dict_get(ctx_, outRoot, PDF_NAME(Count));
    const int prev = pdf_is_int(ctx_, count) ? std::abs(pdf_to_int(ctx_, count)) : 0;
    pdf_dict_put_drop(ctx_, outRoot, PDF_NAME(Count),
                      pdf_new_int(ctx_, prev + static_cast<int>(top.size())));
}

// ---------------------------------------------------------------------------
// Page labels
// ---------------------------------------------------------------------------

std::vector<LabelDesc> ArtifactGrafter::capturePageLabels(pdf_document* doc) const
{
    std::vector<LabelDesc> out;
    const int pages = pdf_count_pages(ctx_, doc);
    pdf_obj* pl = pdf_dict_get(ctx_, catalogOf(doc), PDF_NAME(PageLabels));
    std::vector<std::pair<int, pdf_obj*>> nums;
    if (pl) collectNumsTree(ctx_, pdf_resolve_indirect(ctx_, pl), nums, 0);
    std::sort(nums.begin(), nums.end(),
              [](const std::pair<int, pdf_obj*>& a, const std::pair<int, pdf_obj*>& b) {
                  return a.first < b.first;
              });
    out.reserve(static_cast<size_t>(pages));
    for (int i = 0; i < pages; ++i) {
        LabelDesc L;
        int best = -1;
        for (size_t k = 0; k < nums.size(); ++k) {
            if (nums[k].first <= i) best = static_cast<int>(k); else break;
        }
        if (best >= 0) {
            pdf_obj* e = pdf_resolve_indirect(ctx_, nums[static_cast<size_t>(best)].second);
            if (pdf_is_dict(ctx_, e)) {
                L.has = true;
                const char* s = pdf_to_name(ctx_, pdf_dict_get(ctx_, e, PDF_NAME(S)));
                if (s) L.style = s;
                pdf_obj* p = pdf_dict_get(ctx_, e, PDF_NAME(P));
                if (pdf_is_string(ctx_, p)) {
                    const char* t = pdf_to_text_string(ctx_, p);
                    if (t) L.prefix = t;
                }
                pdf_obj* st = pdf_dict_get(ctx_, e, PDF_NAME(St));
                const int start = pdf_is_int(ctx_, st) ? pdf_to_int(ctx_, st) : 1;
                L.number = start + (i - nums[static_cast<size_t>(best)].first);
            }
        }
        out.push_back(L);
    }
    return out;
}

void ArtifactGrafter::mergePageLabels(const std::vector<LabelDesc>& dstBefore,
                                      const std::vector<int>& srcPageIndices,
                                      int dstInsertAt)
{
    const bool srcHas = pdf_dict_get(ctx_, srcRoot(), PDF_NAME(PageLabels)) != nullptr;
    bool dstHas = false;
    for (const auto& L : dstBefore) if (L.has) { dstHas = true; break; }
    if (!srcHas && !dstHas) return;

    const std::vector<LabelDesc> srcLabels = capturePageLabels(src_);

    std::vector<LabelDesc> merged;
    const size_t at = std::min(static_cast<size_t>(std::max(dstInsertAt, 0)), dstBefore.size());
    merged.insert(merged.end(), dstBefore.begin(),
                  dstBefore.begin() + static_cast<std::ptrdiff_t>(at));
    for (int si : srcPageIndices) {
        if (si >= 0 && si < static_cast<int>(srcLabels.size())) merged.push_back(srcLabels[static_cast<size_t>(si)]);
        else merged.push_back(LabelDesc{});
    }
    merged.insert(merged.end(), dstBefore.begin() + static_cast<std::ptrdiff_t>(at),
                  dstBefore.end());

    bool any = false;
    for (const auto& L : merged) if (L.has) { any = true; break; }
    if (!any) return;

    pdf_obj* nums = own(pdf_new_array(ctx_, dst_, 8));
    for (size_t i = 0; i < merged.size(); ++i) {
        const LabelDesc& L = merged[i];
        if (i > 0) {
            const LabelDesc& p = merged[i - 1];
            if (p.sameSeries(L) && (!L.has || L.number == p.number + 1)) continue;
        }
        pdf_array_push_drop(ctx_, nums, pdf_new_int(ctx_, static_cast<int>(i)));
        pdf_obj* e = pdf_new_dict(ctx_, dst_, 3);
        if (L.has) {
            if (!L.style.empty()) {
                pdf_dict_put_drop(ctx_, e, PDF_NAME(S), pdf_new_name(ctx_, L.style.c_str()));
            }
            if (!L.prefix.empty()) {
                pdf_dict_put_drop(ctx_, e, PDF_NAME(P), pdf_new_text_string(ctx_, L.prefix.c_str()));
            }
            pdf_dict_put_drop(ctx_, e, PDF_NAME(St), pdf_new_int(ctx_, L.number));
        }
        pdf_array_push_drop(ctx_, nums, e);
    }
    pdf_obj* tree = own(pdf_new_dict(ctx_, dst_, 1));
    pdf_dict_put(ctx_, tree, PDF_NAME(Nums), nums);
    pdf_dict_put_drop(ctx_, dstRoot(), PDF_NAME(PageLabels),
                      pdf_add_object(ctx_, dst_, tree));
}

// ---------------------------------------------------------------------------
// Optional content
// ---------------------------------------------------------------------------

void ArtifactGrafter::mergeOcProperties()
{
    pdf_obj* sOC = pdf_dict_get(ctx_, srcRoot(), PDF_NAME(OCProperties));
    if (!sOC) return;
    pdf_obj* droot = dstRoot();
    pdf_obj* dOC = pdf_dict_get(ctx_, droot, PDF_NAME(OCProperties));
    if (!pdf_is_dict(ctx_, pdf_resolve_indirect(ctx_, dOC))) {
        pdf_dict_put_drop(ctx_, droot, PDF_NAME(OCProperties),
                          pdf_graft_mapped_object(ctx_, map_, sOC));
        return;
    }
    dOC = pdf_resolve_indirect(ctx_, dOC);
    pdf_obj* sOCr = pdf_resolve_indirect(ctx_, sOC);

    auto appendArray = [&](pdf_obj* dstOwner, pdf_obj* srcOwner, pdf_obj* key) {
        pdf_obj* sArr = pdf_resolve_indirect(ctx_, pdf_dict_get(ctx_, srcOwner, key));
        if (!pdf_is_array(ctx_, sArr)) return;
        pdf_obj* dArr = pdf_resolve_indirect(ctx_, pdf_dict_get(ctx_, dstOwner, key));
        if (!pdf_is_array(ctx_, dArr)) {
            pdf_obj* fresh = own(pdf_new_array(ctx_, dst_, pdf_array_len(ctx_, sArr)));
            pdf_dict_put(ctx_, dstOwner, key, fresh);
            dArr = fresh;
        }
        const int n = pdf_array_len(ctx_, sArr);
        for (int i = 0; i < n; ++i) {
            pdf_array_push_drop(ctx_, dArr,
                pdf_graft_mapped_object(ctx_, map_, pdf_array_get(ctx_, sArr, i)));
        }
    };
    appendArray(dOC, sOCr, PDF_NAME(OCGs));

    pdf_obj* sD = pdf_resolve_indirect(ctx_, pdf_dict_get(ctx_, sOCr, PDF_NAME(D)));
    if (pdf_is_dict(ctx_, sD)) {
        pdf_obj* dD = pdf_resolve_indirect(ctx_, pdf_dict_get(ctx_, dOC, PDF_NAME(D)));
        if (!pdf_is_dict(ctx_, dD)) {
            pdf_obj* fresh = own(pdf_new_dict(ctx_, dst_, 4));
            pdf_dict_put(ctx_, dOC, PDF_NAME(D), fresh);
            dD = fresh;
        }
        appendArray(dD, sD, PDF_NAME(Order));
        appendArray(dD, sD, PDF_NAME(ON));
        appendArray(dD, sD, PDF_NAME(OFF));
    }
}

// ---------------------------------------------------------------------------
// Structure tree
// ---------------------------------------------------------------------------

pdf_obj* ArtifactGrafter::copyStructElem(pdf_obj* srcRef, pdf_obj* parentRef, int depth)
{
    if (!srcRef || depth > 64) return nullptr;
    const int srcNum = pdf_is_indirect(ctx_, srcRef) ? pdf_to_num(ctx_, srcRef) : 0;
    if (srcNum) {
        auto it = structMap_.find(srcNum);
        if (it != structMap_.end()) return refTo(it->second);
    }
    pdf_obj* r = pdf_resolve_indirect(ctx_, srcRef);
    if (!pdf_is_dict(ctx_, r)) return nullptr;

    const int newNum = pdf_create_object(ctx_, dst_);
    pdf_obj* d = own(pdf_new_dict(ctx_, dst_, 8));
    pdf_obj* ref = refTo(newNum);

    int dstPgNum = 0;
    pdf_obj* sPg = pdf_dict_get(ctx_, r, nm("Pg"));
    if (pdf_is_indirect(ctx_, sPg)) {
        auto it = pageNumMap_.find(pdf_to_num(ctx_, sPg));
        if (it != pageNumMap_.end()) dstPgNum = it->second;
    }
    const bool pgLive = dstPgNum != 0;

    pdf_obj* newK = own(pdf_new_array(ctx_, dst_, 4));
    auto handleChild = [&](pdf_obj* child) {
        if (!child) return;
        if (pdf_is_int(ctx_, child)) {
            if (pgLive) pdf_array_push_drop(ctx_, newK, pdf_new_int(ctx_, pdf_to_int(ctx_, child)));
            return;
        }
        pdf_obj* cr = pdf_resolve_indirect(ctx_, child);
        if (!pdf_is_dict(ctx_, cr)) return;
        pdf_obj* type = pdf_dict_get(ctx_, cr, PDF_NAME(Type));
        if (pdf_name_eq(ctx_, type, nm("OBJR"))) {
            pdf_obj* obj = pdf_dict_get(ctx_, cr, nm("Obj"));
            if (!pdf_is_indirect(ctx_, obj)) return;
            auto it = annotMap_.find(pdf_to_num(ctx_, obj));
            if (it == annotMap_.end()) return;   // annotation not grafted
            pdf_obj* o = pdf_new_dict(ctx_, dst_, 3);
            pdf_dict_put(ctx_, o, PDF_NAME(Type), nm("OBJR"));
            pdf_dict_put_drop(ctx_, o, nm("Obj"), pdf_new_indirect(ctx_, dst_, it->second, 0));
            if (pgLive) pdf_dict_put_drop(ctx_, o, nm("Pg"),
                                          pdf_new_indirect(ctx_, dst_, dstPgNum, 0));
            pdf_array_push_drop(ctx_, newK, o);
            return;
        }
        if (pdf_name_eq(ctx_, type, nm("MCR"))) {
            pdf_obj* cpg = pdf_dict_get(ctx_, cr, nm("Pg"));
            int mcrPg = dstPgNum;
            if (pdf_is_indirect(ctx_, cpg)) {
                auto it = pageNumMap_.find(pdf_to_num(ctx_, cpg));
                mcrPg = (it == pageNumMap_.end()) ? 0 : it->second;
            }
            if (!mcrPg) return;
            pdf_obj* o = pdf_new_dict(ctx_, dst_, 3);
            pdf_dict_put(ctx_, o, PDF_NAME(Type), nm("MCR"));
            pdf_dict_put_drop(ctx_, o, nm("Pg"), pdf_new_indirect(ctx_, dst_, mcrPg, 0));
            pdf_obj* mcid = pdf_dict_get(ctx_, cr, PDF_NAME(MCID));
            if (pdf_is_int(ctx_, mcid)) {
                pdf_dict_put_drop(ctx_, o, PDF_NAME(MCID), pdf_new_int(ctx_, pdf_to_int(ctx_, mcid)));
            }
            pdf_array_push_drop(ctx_, newK, o);
            return;
        }
        pdf_obj* kid = copyStructElem(child, ref, depth + 1);
        if (kid) pdf_array_push(ctx_, newK, kid);
    };

    pdf_obj* sK = pdf_dict_get(ctx_, r, PDF_NAME(K));
    pdf_obj* sKr = pdf_resolve_indirect(ctx_, sK);
    if (pdf_is_array(ctx_, sKr)) {
        const int n = pdf_array_len(ctx_, sKr);
        for (int i = 0; i < n; ++i) handleChild(pdf_array_get(ctx_, sKr, i));
    } else if (sK) {
        handleChild(sK);
    }

    const bool hasContent = pdf_array_len(ctx_, newK) > 0;
    if (!hasContent && !pgLive) return nullptr;   // orphaned to a dropped page

    if (srcNum) structMap_[srcNum] = newNum;

    const int n = pdf_dict_len(ctx_, r);
    for (int i = 0; i < n; ++i) {
        pdf_obj* key = pdf_dict_get_key(ctx_, r, i);
        if (pdf_name_eq(ctx_, key, PDF_NAME(K)) ||
            pdf_name_eq(ctx_, key, PDF_NAME(P)) ||
            pdf_name_eq(ctx_, key, nm("Pg"))) {
            continue;
        }
        pdf_dict_put_drop(ctx_, d, key,
            pdf_graft_mapped_object(ctx_, map_, pdf_dict_get_val(ctx_, r, i)));
    }
    if (pgLive) pdf_dict_put_drop(ctx_, d, nm("Pg"), pdf_new_indirect(ctx_, dst_, dstPgNum, 0));
    if (parentRef) pdf_dict_put(ctx_, d, PDF_NAME(P), parentRef);
    if (hasContent) pdf_dict_put(ctx_, d, PDF_NAME(K), newK);
    pdf_update_object(ctx_, dst_, newNum, d);
    return ref;
}

void ArtifactGrafter::attachStructTree()
{
    pdf_obj* sSt = pdf_resolve_indirect(
        ctx_, pdf_dict_get(ctx_, srcRoot(), PDF_NAME(StructTreeRoot)));
    if (!pdf_is_dict(ctx_, sSt)) return;

    const int newNum = pdf_create_object(ctx_, dst_);
    pdf_obj* d = own(pdf_new_dict(ctx_, dst_, 6));
    pdf_obj* ref = refTo(newNum);
    pdf_dict_put(ctx_, d, PDF_NAME(Type), PDF_NAME(StructTreeRoot));
    for (const char* key : {"RoleMap", "ClassMap"}) {
        pdf_obj* v = pdf_dict_gets(ctx_, sSt, key);
        if (v) pdf_dict_puts_drop(ctx_, d, key, pdf_graft_mapped_object(ctx_, map_, v));
    }

    pdf_obj* newK = own(pdf_new_array(ctx_, dst_, 4));
    pdf_obj* sK = pdf_dict_get(ctx_, sSt, PDF_NAME(K));
    pdf_obj* sKr = pdf_resolve_indirect(ctx_, sK);
    if (pdf_is_array(ctx_, sKr)) {
        const int n = pdf_array_len(ctx_, sKr);
        for (int i = 0; i < n; ++i) {
            pdf_obj* kid = copyStructElem(pdf_array_get(ctx_, sKr, i), ref, 0);
            if (kid) pdf_array_push(ctx_, newK, kid);
        }
    } else if (sK) {
        pdf_obj* kid = copyStructElem(sK, ref, 0);
        if (kid) pdf_array_push(ctx_, newK, kid);
    }
    if (pdf_array_len(ctx_, newK) == 0) return;   // nothing survived the prune
    pdf_dict_put(ctx_, d, PDF_NAME(K), newK);

    // /ParentTree: keep only entries that still resolve to a copied element.
    pdf_obj* sPT = pdf_resolve_indirect(ctx_, pdf_dict_get(ctx_, sSt, PDF_NAME(ParentTree)));
    if (pdf_is_dict(ctx_, sPT)) {
        std::vector<std::pair<int, pdf_obj*>> nums;
        collectNumsTree(ctx_, sPT, nums, 0);
        std::sort(nums.begin(), nums.end(),
                  [](const std::pair<int, pdf_obj*>& a, const std::pair<int, pdf_obj*>& b) {
                      return a.first < b.first;
                  });
        pdf_obj* outNums = own(pdf_new_array(ctx_, dst_, 8));
        int maxKey = -1;
        auto mapElem = [&](pdf_obj* v) -> int {
            if (!pdf_is_indirect(ctx_, v)) return 0;
            auto it = structMap_.find(pdf_to_num(ctx_, v));
            return it == structMap_.end() ? 0 : it->second;
        };
        for (const auto& kv : nums) {
            pdf_obj* v = kv.second;
            pdf_obj* vr = pdf_resolve_indirect(ctx_, v);
            if (pdf_is_array(ctx_, vr) && !pdf_is_indirect(ctx_, v)) {
                pdf_obj* arr = pdf_new_array(ctx_, dst_, pdf_array_len(ctx_, vr));
                int kept = 0;
                const int n = pdf_array_len(ctx_, vr);
                for (int i = 0; i < n; ++i) {
                    const int num = mapElem(pdf_array_get(ctx_, vr, i));
                    if (num) { pdf_array_push_drop(ctx_, arr, pdf_new_indirect(ctx_, dst_, num, 0)); ++kept; }
                    else pdf_array_push(ctx_, arr, PDF_NULL);
                }
                if (kept == 0) { pdf_drop_obj(ctx_, arr); continue; }
                pdf_array_push_drop(ctx_, outNums, pdf_new_int(ctx_, kv.first));
                pdf_array_push_drop(ctx_, outNums, arr);
            } else {
                const int num = mapElem(v);
                if (!num) continue;
                pdf_array_push_drop(ctx_, outNums, pdf_new_int(ctx_, kv.first));
                pdf_array_push_drop(ctx_, outNums, pdf_new_indirect(ctx_, dst_, num, 0));
            }
            maxKey = std::max(maxKey, kv.first);
        }
        if (pdf_array_len(ctx_, outNums) > 0) {
            pdf_obj* tree = own(pdf_new_dict(ctx_, dst_, 1));
            pdf_dict_put(ctx_, tree, PDF_NAME(Nums), outNums);
            pdf_dict_put_drop(ctx_, d, PDF_NAME(ParentTree), pdf_add_object(ctx_, dst_, tree));
            pdf_dict_puts_drop(ctx_, d, "ParentTreeNextKey", pdf_new_int(ctx_, maxKey + 1));
        }
    }

    pdf_update_object(ctx_, dst_, newNum, d);
    pdf_dict_put(ctx_, dstRoot(), PDF_NAME(StructTreeRoot), ref);

    pdf_obj* mi = pdf_dict_gets(ctx_, srcRoot(), "MarkInfo");
    if (mi && !pdf_dict_gets(ctx_, dstRoot(), "MarkInfo")) {
        pdf_dict_puts_drop(ctx_, dstRoot(), "MarkInfo",
                           pdf_graft_mapped_object(ctx_, map_, mi));
    }
}

// ---------------------------------------------------------------------------

void ArtifactGrafter::copySimpleCatalogKeys()
{
    // Page-position-INDEPENDENT catalog entries. /OpenAction, the legacy
    // catalog /Dests dict and /Threads are deliberately NOT carried: each can
    // dangle to a page that was not grafted. /Perms is not carried either -- a
    // certification over a different page set is meaningless.
    static const char* const kKeys[] = {
        "Metadata", "Lang", "MarkInfo", "ViewerPreferences",
        "PageLayout", "PageMode", "Version", "Extensions", "Collection", "AF",
    };
    pdf_obj* sroot = srcRoot();
    pdf_obj* droot = dstRoot();
    for (const char* key : kKeys) {
        pdf_obj* v = pdf_dict_gets(ctx_, sroot, key);
        if (v && !pdf_dict_gets(ctx_, droot, key)) {
            pdf_dict_puts_drop(ctx_, droot, key, pdf_graft_mapped_object(ctx_, map_, v));
        }
    }
}

void ArtifactGrafter::run(const std::vector<int>& srcPageIndices, int dstInsertAt)
{
    if (srcPageIndices.empty()) return;

    // Snapshot the destination's own page labels BEFORE the page indices move.
    const std::vector<LabelDesc> dstLabelsBefore = capturePageLabels(dst_);

    // Two structure trees cannot be concatenated without renumbering both
    // /ParentTree number trees and every page's /StructParents key, so the
    // source tree is attached only when the destination has none of its own.
    attachStructTree_ =
        pdf_dict_get(ctx_, srcRoot(), PDF_NAME(StructTreeRoot)) != nullptr &&
        pdf_dict_get(ctx_, dstRoot(), PDF_NAME(StructTreeRoot)) == nullptr;

    map_ = pdf_new_graft_map(ctx_, dst_);
    for (size_t i = 0; i < srcPageIndices.size(); ++i) {
        pdf_graft_mapped_page(ctx_, map_, dstInsertAt + static_cast<int>(i),
                              src_, srcPageIndices[i]);
    }
    for (size_t i = 0; i < srcPageIndices.size(); ++i) {
        pdf_obj* s = pdf_lookup_page_obj(ctx_, src_, srcPageIndices[i]);
        pdf_obj* dpg = pdf_lookup_page_obj(ctx_, dst_, dstInsertAt + static_cast<int>(i));
        if (s && dpg) pageNumMap_[pdf_to_num(ctx_, s)] = pdf_to_num(ctx_, dpg);
    }

    collectSrcDests();
    mergeNamedDests();      // fills destNameRemap_/liveDestNames_ for links below
    mergeOtherNameTrees();

    // Per-page keys pdf_graft_mapped_page does not copy.
    for (size_t i = 0; i < srcPageIndices.size(); ++i) {
        pdf_obj* s = pdf_lookup_page_obj(ctx_, src_, srcPageIndices[i]);
        pdf_obj* dpg = pdf_lookup_page_obj(ctx_, dst_, dstInsertAt + static_cast<int>(i));
        if (!s || !dpg) continue;
        const int dstPageNum = pdf_to_num(ctx_, dpg);

        pdf_obj* sAnnots = pdf_resolve_indirect(ctx_, pdf_dict_get(ctx_, s, PDF_NAME(Annots)));
        if (pdf_is_array(ctx_, sAnnots)) {
            const int n = pdf_array_len(ctx_, sAnnots);
            pdf_obj* arr = own(pdf_new_array(ctx_, dst_, n > 0 ? n : 1));
            for (int a = 0; a < n; ++a) {
                pdf_obj* got = graftAnnot(pdf_array_get(ctx_, sAnnots, a), dstPageNum);
                if (got) pdf_array_push(ctx_, arr, got);
            }
            if (pdf_array_len(ctx_, arr) > 0) pdf_dict_put(ctx_, dpg, PDF_NAME(Annots), arr);
        }
        for (const char* key : {"Group", "Tabs"}) {
            pdf_obj* v = pdf_dict_gets(ctx_, s, key);
            if (v && !pdf_dict_gets(ctx_, dpg, key)) {
                pdf_dict_puts_drop(ctx_, dpg, key, pdf_graft_mapped_object(ctx_, map_, v));
            }
        }
        if (attachStructTree_) {
            pdf_obj* sp = pdf_dict_get(ctx_, s, PDF_NAME(StructParents));
            if (pdf_is_int(ctx_, sp)) {
                pdf_dict_put_drop(ctx_, dpg, PDF_NAME(StructParents),
                                  pdf_new_int(ctx_, pdf_to_int(ctx_, sp)));
            }
        }
    }

    finishAcroForm();
    mergeOutline();
    mergePageLabels(dstLabelsBefore, srcPageIndices, dstInsertAt);
    mergeOcProperties();
    if (attachStructTree_) attachStructTree();
    copySimpleCatalogKeys();

    release();
}

} // namespace

void graftPagesWithArtifacts(::fz_context* ctx,
                             ::pdf_document* dst,
                             ::pdf_document* src,
                             const std::vector<int>& srcPageIndices,
                             int dstInsertAt)
{
    if (!ctx) return;
    if (!dst || !src) {
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "graftPagesWithArtifacts: null document");
    }
    ArtifactGrafter grafter(ctx, dst, src);
    grafter.run(srcPageIndices, dstInsertAt);
}

} // namespace pdfe::core
