// -*- coding: utf-8 -*-
#pragma once

// Reads every image placement (`Do`) of a page and rewrites ONE of them in
// place (docs/image-placement-model.md, GitHub #417/#419/#421/#424/#427-#430).
//
// The scanner tokenises the page's raw content bytes itself rather than going
// through a MuPDF filter pass. A filter pass re-serialises the whole stream;
// splicing the bytes of the one targeted `Do` leaves every other operator -
// other placements, q/Q, gs, BDC/EMC (MCID, /OC), clips and the drawing order
// - byte-for-byte as it was.
//
// PDFE's placement wrapper, the only shape the editor writes:
//
//     q <F> cm [u0 v0 du dv re W n] /Name Do Q
//
// The clip part is present only when the placement is cropped. It is given in
// the image's unit square (it sits INSIDE the cm), so it moves, scales and
// rotates with the image. F is chosen so the effective CTM at the Do is the
// requested placement matrix, given the CTM in force where the wrapper sits.
// `q <M> cm /Name Do Q` - what insert writes - is the same shape without a
// clip. When the targeted Do already sits in such a wrapper, the whole
// wrapper is replaced, so repeated edits never nest.
//
// Clip to page (ui-tweaks phase-4): when a unit-square crop cannot keep a
// placement inside its page (a rotation that is not a multiple of 90
// degrees), the save wraps the whole wrapper in a page-rect clip,
// `q x y w h re W n <wrapper> Q`. scanPage reports it (DoSite::enclosingClip);
// later edits replace the inner wrapper and leave the clip, which is the page
// box and so stays right wherever the image is moved on its page.
//
// Legacy crop: the editor before the fix wrote a page-space clip OUTSIDE the
// cm, `q x y w h re W n <M> cm /Name Do Q`. The scanner recognises that
// shape, reports its clip converted to the unit square, and the next edit
// replaces it with the unit-space wrapper.

#include "image/ImagePlacement.h"

#include <cstddef>
#include <string>
#include <vector>

struct fz_context;
struct pdf_document;
struct pdf_page;
struct pdf_obj;

namespace pdfe::image::placement {

enum class WrapperKind {
    None,            ///< the Do is not in a recognised wrapper
    Pdfe,            ///< q F cm [u0 v0 du dv re W n] /Name Do Q
    LegacyPageClip,  ///< q x y w h re W n M cm /Name Do Q (old cropImage)
};

/// One `Do` of an XObject, in content order.
struct DoSite {
    std::string name;
    int occurrence = 0;          ///< n-th Do of `name` in the PAGE content; -1 inside a form
    int formDepth = 0;           ///< 0 = page content; >0 = inside a Form XObject
    bool isImage = false;        ///< resolved to an Image XObject
    pdf_obj* xobject = nullptr;  ///< borrowed; valid while the page is
    pdf_obj* resources = nullptr;  ///< borrowed: the resource dict the name was looked up in
    Affine ctm;                  ///< effective PDF user-space CTM at the Do
    Affine ctmOuter;             ///< user-space CTM where a replacement wrapper will sit
    UnitCrop crop;               ///< unit-square crop read from the wrapper (full if none)
    bool cropped = false;
    WrapperKind wrapper = WrapperKind::None;
    int stream = -1;             ///< index into the page /Contents (page level only)
    std::size_t replaceStart = 0;  ///< byte range a rewrite replaces
    std::size_t replaceEnd = 0;
    std::size_t nameStart = 0;   ///< byte range of the Do's name operand ("/Name")
    std::size_t nameEnd = 0;
    /// The placement's PDFE wrapper sits alone inside a rect clip,
    ///     q x y w h re W n <wrapper> Q
    /// - the shape rewriteSiteInClip writes to keep a placement inside its
    /// page (ui-tweaks phase-4). An edit replaces only the inner wrapper
    /// (replaceStart/End), so the clip stays; removeSite takes it along.
    bool enclosingClip = false;
    double clipRect[4] = {0, 0, 0, 0};  ///< x y w h of that `re`, in clipCtm's space
    Affine clipCtm;                     ///< user-space CTM in force at that `re`
    std::size_t clipStart = 0;          ///< byte range of the whole clip wrapper
    std::size_t clipEnd = 0;
};

/// Every Do on the page in content order. With recurseForms, Form XObjects are
/// followed (their /Matrix composed in) and the Do's inside them are reported
/// with formDepth > 0 (for enumeration only - they are not editable here).
std::vector<DoSite> scanPage(fz_context* ctx, pdf_page* page, bool recurseForms);

/// Page user space -> rendered fitz frame (pdf_page_transform: /Rotate, box
/// origin, Y flip).
Affine pageTransform(fz_context* ctx, pdf_page* page);

/// Replace the bytes of `site` (a page-level site from scanPage) with the PDFE
/// wrapper drawing it at effective user-space matrix `placement` with `crop`.
/// The changed content stream is written as a NEW stream object (the old one
/// may be shared); every other byte of the page content is unchanged.
/// Throws (fz_throw) on a MuPDF error; returns false if the geometry is
/// degenerate.
bool rewriteSite(fz_context* ctx, pdf_document* doc, pdf_page* page,
                 const DoSite& site, const Affine& placement, const UnitCrop& crop);

/// rewriteSite, with the new wrapper put inside a rect clip:
///     q x y w h re W n <wrapper> Q
/// `clipRe` is the clip's `re` operands (x y w h) in the space of the CTM where
/// the wrapper sits (site.ctmOuter). Refused (false) when the site already has
/// an enclosing clip, so clips never stack.
bool rewriteSiteInClip(fz_context* ctx, pdf_document* doc, pdf_page* page,
                       const DoSite& site, const Affine& placement, const UnitCrop& crop,
                       const double clipRe[4]);

/// Remove `site` (a page-level site from scanPage): its Do, or the whole PDFE
/// wrapper around it (with its enclosing clip, if any), is cut from the
/// content; every other byte stays.
bool removeSite(fz_context* ctx, pdf_document* doc, pdf_page* page, const DoSite& site);

/// Point `site` (a page-level site from scanPage) at another XObject: only its
/// name operand is replaced by "/<newName>" (an unescaped, regular-character
/// name), in a NEW content stream object. Every other byte of the page
/// content, the wrapper and the drawing order stay as they are.
bool renameSite(fz_context* ctx, pdf_document* doc, pdf_page* page, const DoSite& site,
                const std::string& newName);

/// Replace the byte range of `site` with `replacement` (possibly empty) in a
/// NEW content stream object. Shared by rewriteSite and removeSite.
bool spliceSite(fz_context* ctx, pdf_document* doc, pdf_page* page,
                const DoSite& site, const std::string& replacement);

/// Format a number for a content stream: fixed point, no exponent, trailing
/// zeros trimmed.
std::string formatNumber(double v);

} // namespace pdfe::image::placement
