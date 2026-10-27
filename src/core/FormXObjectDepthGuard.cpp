// -*- coding: utf-8 -*-
#include "FormXObjectDepthGuard.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pdfe::core {

namespace {

// 256: see FormXObjectDepthGuard.h. Deliberately BELOW MuPDF's own catchable
// limit as well as far below the native-stack ceiling, because a bound that
// only trips where MuPDF already copes would be a guard that never runs on the
// case it was written for.
constexpr int kMaxFormXObjectDepth = 256;

// ONE FRAME OF THE ITERATIVE WALK.
//
// `xdict` is the `/XObject` sub-dictionary being iterated; `i` is the next
// entry; `best` is the deepest value any child has reported so far; `num` is
// the object number of the form that OWNS these resources, or -1 for the page's
// own resources (the root, which contributes no level of its own).
struct Frame {
    pdf_obj* xdict = nullptr;
    int i = 0;
    int best = 0;
    int num = -1;
};

// WHY THESE ARE thread_local AND NOT LOCALS.
//
// This function runs inside a MuPdfContext execute()/executeReturn() scope,
// i.e. inside an fz_try. MuPDF reports failure with fz_throw, which is
// longjmp, and longjmp does not run C++ destructors — a std::vector on this
// stack would leak its buffer on any malformed-object error raised by
// pdf_resolve_indirect or pdf_dict_get. Reusing thread_local storage that is
// cleared on entry has no destructor to skip: the buffers survive the longjmp
// and are reused (and correctly sized down) by the next call.
//
// Bounded by construction: the walk returns as soon as the path exceeds the
// limit, so `stack` never exceeds limit + 2 frames.
thread_local std::vector<Frame> tlStack;
thread_local std::unordered_map<int, int> tlMemo;
thread_local std::unordered_set<int> tlOnPath;

}  // namespace

// ---------------------------------------------------------------------------
// GitHub #176 -- the BISECT SEAM, and why the limit could not be measured
// without one.
//
// The limit this guard enforces was argued, never measured: the prior pipeline
// established that MuPDF's own catchable guard copes from depth 64 through 1024
// and that 4096 kills the process, then picked 256 with reasoning about margin.
// The actual native-stack failure depth was never bisected, so "256 has ample
// margin" was a judgement, not a number.
//
// It CANNOT be bisected through the shipping code, because the guard itself
// refuses at 257 -- every probe above the limit is answered by the guard long
// before the native stack is anywhere near exhausted. Measuring where the stack
// actually dies requires raising the guard out of the way first.
//
// So: in a PDFE_TESTING build only, the limit may be overridden through the
// environment. `kTestPipesCompiledIn` in src/ui/TestDialogSeam.h documents why
// this is a COMPILE-TIME capability rather than a runtime one -- a shipped
// binary must not expose a control channel that lets a local process weaken a
// security guard.
//
// STATE THE POSTURE ACCURATELY, because the neighbouring seam does not.
// PDFE_TESTING is `option(... ON)` (CMakeLists.txt:150) and NO build path in
// this repository passes -DPDFE_TESTING=OFF -- not a preset, not a workflow,
// not a packaging script. So today this getenv IS compiled into every artifact,
// including the packaged one. TestDialogSeam.h:64-65 claims the opposite
// polarity ("false unless ... -DPDFE_TESTING=ON"), which is backwards and is
// probably why nobody noticed; filed as GitHub #182.
//
// The gate is still the right one to hang this on: fixing #182 closes this
// override along with every other seam, which is exactly why reusing the
// established mechanism beat inventing a second. But a comment here claiming
// the override "does not exist to be found" in a release build would be false
// as the tree stands, and this file is a security guard -- it is the last place
// a reassuring inaccuracy belongs.
//
// A value <= 0, unparseable, or absent leaves the shipped constant in force.
//
// THE 1,000,000 CEILING IS NOT A SECURITY CONTROL, and must not be read as one.
// The measured native-stack failure band is around 3994 (see
// docs/audit/p83-outstanding/phase-6/native-stack-depth.json), which is three
// orders of magnitude BELOW the ceiling -- so any value that matters for safety
// is already permitted. The ceiling only rejects absurd input.
//
// Capping at or below kMaxFormXObjectDepth was considered and rejected: the
// entire purpose of this seam is to raise the guard ABOVE the failure point so
// the failure point can be measured, and a seam that cannot exceed the shipped
// limit cannot measure past it. The mitigation for a caller who sets this
// maliciously is GitHub #182 (close PDFE_TESTING for shipped builds), not a
// narrower number here.
// ---------------------------------------------------------------------------
int maxFormXObjectDepth()
{
#ifdef PDFE_TESTING
    if (const char* ov = std::getenv("PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE")) {
        const long v = std::strtol(ov, nullptr, 10);
        // Bounds reject absurd/degenerate input only -- see the note above on
        // why this is deliberately NOT a safety bound.
        if (v > 0 && v <= 1000000L) return static_cast<int>(v);
    }
#endif
    return kMaxFormXObjectDepth;
}

int measureFormXObjectDepth(fz_context* ctx, pdf_obj* resources, int limit)
{
    if (!ctx || !resources || limit < 0) return 0;
    if (!pdf_is_dict(ctx, resources)) return 0;

    tlStack.clear();
    tlMemo.clear();
    tlOnPath.clear();

    pdf_obj* rootX = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
    if (!pdf_is_dict(ctx, rootX)) return 0;

    tlStack.push_back(Frame{rootX, 0, 0, -1});

    while (!tlStack.empty()) {
        Frame& f = tlStack.back();
        const int len = pdf_is_dict(ctx, f.xdict) ? pdf_dict_len(ctx, f.xdict) : 0;

        if (f.i >= len) {
            // Frame complete. A form contributes one level of its own on top of
            // the deepest chain beneath it; the root resources contribute none.
            const int val = (f.num >= 0) ? (f.best + 1) : f.best;
            const int num = f.num;
            tlStack.pop_back();
            if (num >= 0) {
                tlMemo[num] = val;
                tlOnPath.erase(num);
            }
            if (tlStack.empty()) return std::min(val, limit + 1);
            Frame& parent = tlStack.back();
            parent.best = std::max(parent.best, val);
            if (parent.best > limit) return limit + 1;
            continue;
        }

        pdf_obj* raw = pdf_dict_get_val(ctx, f.xdict, f.i);
        ++f.i;

        // Every Form XObject is a stream, and a stream must be an indirect
        // object, so a direct value here cannot be one. Skipping it also means
        // the cycle guard below never has to reason about unnumbered objects.
        const int num = pdf_to_num(ctx, raw);
        if (num <= 0) continue;

        pdf_obj* val = pdf_resolve_indirect(ctx, raw);
        if (!pdf_is_dict(ctx, val)) continue;
        if (!pdf_name_eq(ctx, pdf_dict_get(ctx, val, PDF_NAME(Subtype)),
                         PDF_NAME(Form))) {
            continue;
        }

        // Already resolved elsewhere in the graph: reuse, do not re-walk. A
        // wide document that references the same form from many places is
        // otherwise exponential.
        auto memoIt = tlMemo.find(num);
        if (memoIt != tlMemo.end()) {
            Frame& cur = tlStack.back();
            cur.best = std::max(cur.best, memoIt->second);
            if (cur.best > limit) return limit + 1;
            continue;
        }

        // CYCLE. /X0 may resolve to a form whose own resources name /X0 again.
        // The edge is counted once and not followed, so the walk terminates
        // instead of counting to infinity — and, importantly, a cyclic
        // document is not misreported as unbounded and refused outright.
        if (tlOnPath.count(num) != 0) {
            Frame& cur = tlStack.back();
            cur.best = std::max(cur.best, 1);
            continue;
        }

        // EARLY OUT. Frames on the stack are a path from the page's resources,
        // so the current path length already exceeds the bound; nothing deeper
        // can change the answer.
        if (static_cast<int>(tlStack.size()) > limit) return limit + 1;

        pdf_obj* childRes = pdf_dict_get(ctx, val, PDF_NAME(Resources));
        pdf_obj* childX = pdf_is_dict(ctx, childRes)
                              ? pdf_dict_get(ctx, childRes, PDF_NAME(XObject))
                              : nullptr;
        tlOnPath.insert(num);
        tlStack.push_back(Frame{childX, 0, 0, num});
    }

    return 0;
}

bool formXObjectDepthExceeded(fz_context* ctx, pdf_document* doc, int pageIndex)
{
    if (!ctx || !doc || pageIndex < 0) return false;
    // Through the accessor, NOT the constant. Reading kMaxFormXObjectDepth
    // directly here would leave the #176 bisect seam inert at the one site that
    // decides anything -- a seam nothing consults is decoration.
    const int limit = maxFormXObjectDepth();
    pdf_obj* pageObj = pdf_lookup_page_obj(ctx, doc, pageIndex);
    if (!pageObj) return false;
    pdf_obj* res = pdf_dict_get_inheritable(ctx, pageObj, PDF_NAME(Resources));
    return measureFormXObjectDepth(ctx, res, limit) > limit;
}

namespace {

void throwIfTooDeep(fz_context* ctx, pdf_document* pdoc, int pageIndex)
{
    if (!formXObjectDepthExceeded(ctx, pdoc, pageIndex)) return;
    fz_throw(ctx, FZ_ERROR_LIMIT,
             "page %d nests Form XObjects more than %d levels deep; refusing "
             "to load it because the recursion would exhaust the stack",
             pageIndex, maxFormXObjectDepth());
}

}  // namespace

fz_page* safeLoadPage(fz_context* ctx, fz_document* doc, int pageIndex)
{
    // A non-PDF document has no Form XObjects; pdf_document_from_fz_document
    // answers null and the load proceeds unchecked.
    throwIfTooDeep(ctx, pdf_document_from_fz_document(ctx, doc), pageIndex);
    return fz_load_page(ctx, doc, pageIndex);
}

pdf_page* safeLoadPdfPage(fz_context* ctx, pdf_document* doc, int pageIndex)
{
    throwIfTooDeep(ctx, doc, pageIndex);
    return pdf_load_page(ctx, doc, pageIndex);
}

}  // namespace pdfe::core
