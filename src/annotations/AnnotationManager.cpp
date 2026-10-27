#include "AnnotationManager.h"
#include "attachments/AttachmentManager.h"  // sanitizeFilename
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/Error.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace pdfe::annotations {

// Convert AnnotationType to MuPDF pdf_annot_type integer
int toPdfAnnotType(AnnotationType type) {
    switch (type) {
        case AnnotationType::Text:       return PDF_ANNOT_TEXT;
        case AnnotationType::Highlight:  return PDF_ANNOT_HIGHLIGHT;
        case AnnotationType::Underline:  return PDF_ANNOT_UNDERLINE;
        case AnnotationType::StrikeOut:  return PDF_ANNOT_STRIKE_OUT;
        case AnnotationType::Ink:        return PDF_ANNOT_INK;
        case AnnotationType::FreeText:   return PDF_ANNOT_FREE_TEXT;
        case AnnotationType::Square:     return PDF_ANNOT_SQUARE;
        case AnnotationType::Circle:     return PDF_ANNOT_CIRCLE;
        case AnnotationType::Line:       return PDF_ANNOT_LINE;
        case AnnotationType::Stamp:      return PDF_ANNOT_STAMP;
        case AnnotationType::Link:       return PDF_ANNOT_LINK;
        case AnnotationType::FileAttachment: return PDF_ANNOT_FILE_ATTACHMENT;
        default:                         return PDF_ANNOT_UNKNOWN;
    }
}

// Convert MuPDF pdf_annot_type integer to AnnotationType
AnnotationType fromPdfAnnotType(int pdfType) {
    switch (pdfType) {
        case PDF_ANNOT_TEXT:       return AnnotationType::Text;
        case PDF_ANNOT_HIGHLIGHT:  return AnnotationType::Highlight;
        case PDF_ANNOT_UNDERLINE:  return AnnotationType::Underline;
        case PDF_ANNOT_STRIKE_OUT: return AnnotationType::StrikeOut;
        case PDF_ANNOT_INK:        return AnnotationType::Ink;
        case PDF_ANNOT_FREE_TEXT:  return AnnotationType::FreeText;
        case PDF_ANNOT_SQUARE:     return AnnotationType::Square;
        case PDF_ANNOT_CIRCLE:     return AnnotationType::Circle;
        case PDF_ANNOT_LINE:       return AnnotationType::Line;
        case PDF_ANNOT_STAMP:      return AnnotationType::Stamp;
        case PDF_ANNOT_LINK:       return AnnotationType::Link;
        case PDF_ANNOT_FILE_ATTACHMENT: return AnnotationType::FileAttachment;
        default:                   return AnnotationType::Unknown;
    }
}

// Get display name for annotation type
std::string annotationTypeName(AnnotationType type) {
    switch (type) {
        case AnnotationType::Text:       return "Text";
        case AnnotationType::Highlight:  return "Highlight";
        case AnnotationType::Underline:  return "Underline";
        case AnnotationType::StrikeOut:  return "Strike Out";
        case AnnotationType::Ink:        return "Ink";
        case AnnotationType::FreeText:   return "Free Text";
        case AnnotationType::Square:     return "Square";
        case AnnotationType::Circle:     return "Circle";
        case AnnotationType::Line:       return "Line";
        case AnnotationType::Stamp:      return "Stamp";
        case AnnotationType::Link:       return "Link";
        case AnnotationType::FileAttachment: return "File Attachment";
        default:                         return "Unknown";
    }
}

// Helper: Get the nth annotation from a page
static pdf_annot* getNthAnnot(fz_context* ctx, pdf_page* page, int index) {
    pdf_annot* annot = pdf_first_annot(ctx, page);
    for (int i = 0; i < index && annot; i++) {
        annot = pdf_next_annot(ctx, annot);
    }
    return annot;
}

// Count the annotations pdf_first_annot exposes. Used to split
// `annotIndex` into "iterator-visible" vs "link-only from /Annots".
static int countIteratorAnnots(fz_context* ctx, pdf_page* page) {
    int n = 0;
    for (pdf_annot* a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a)) {
        ++n;
    }
    return n;
}

// Resolve `annotIndex` to a raw pdf_obj* on the page. Indices from 0
// to `iteratorCount-1` map to pdf_first_annot order; indices from
// `iteratorCount` onward map into /Annots-only /Link entries (the
// ones getAnnotations appends after the iterator pass). Returns null
// if the index is out of range or the entry is not a Link (the
// extended range is link-only by construction).
static pdf_obj* getNthAnnotObj(fz_context* ctx, pdf_page* page, int index) {
    // First try the iterator path: it covers all non-Link annotations
    // plus any Links that the iterator happens to surface.
    pdf_annot* iter = getNthAnnot(ctx, page, index);
    if (iter) return pdf_annot_obj(ctx, iter);

    const int iteratorCount = countIteratorAnnots(ctx, page);
    if (index < iteratorCount) return nullptr;

    // Extended range — walk /Annots for /Link entries not already seen.
    pdf_obj* pageObj = page->obj;
    if (!pageObj) return nullptr;
    pdf_obj* arr = pdf_dict_get(ctx, pageObj, PDF_NAME(Annots));
    if (!arr || !pdf_is_array(ctx, arr)) return nullptr;

    std::vector<int> seen;
    for (pdf_annot* a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a)) {
        pdf_obj* ao = pdf_annot_obj(ctx, a);
        if (ao) seen.push_back(pdf_to_num(ctx, ao));
    }

    int extendedIdx = iteratorCount;
    const int n = pdf_array_len(ctx, arr);
    for (int i = 0; i < n; ++i) {
        pdf_obj* entry = pdf_array_get(ctx, arr, i);
        if (!entry) continue;
        int num = pdf_to_num(ctx, entry);
        if (std::find(seen.begin(), seen.end(), num) != seen.end()) continue;
        pdf_obj* subtype = pdf_dict_get(ctx, entry, PDF_NAME(Subtype));
        if (!pdf_name_eq(ctx, subtype, PDF_NAME(Link))) continue;
        if (extendedIdx == index) return entry;
        ++extendedIdx;
    }
    return nullptr;
}

// Helper: Read annotation data into AnnotationData struct
static AnnotationData readAnnotationData(
    fz_context* ctx,
    pdf_annot* annot,
    int pageIndex,
    int annotIndex
) {
    AnnotationData data;
    data.pageIndex = pageIndex;
    data.annotIndex = annotIndex;

    // Type
    int pdfType = pdf_annot_type(ctx, annot);
    data.type = fromPdfAnnotType(pdfType);

    // Bounding rectangle
    if (pdf_annot_has_rect(ctx, annot)) {
        fz_rect r = pdf_bound_annot(ctx, annot);
        data.rect = QRectF(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
    }
    // For /Link annotations loaded from a reopened document,
    // pdf_annot_has_rect returns false (MuPDF's has-rect predicate
    // excludes Link) even though /Rect is populated in the object.
    // Fall back to reading /Rect directly and converting Y-up→Y-down.
    {
        if (data.rect.isEmpty() && data.type == AnnotationType::Link) {
            pdf_obj* obj = pdf_annot_obj(ctx, annot);
            pdf_obj* rectObj = obj ? pdf_dict_get(ctx, obj, PDF_NAME(Rect)) : nullptr;
            if (rectObj && pdf_is_array(ctx, rectObj) &&
                pdf_array_len(ctx, rectObj) == 4) {
                float rr[4];
                for (int k = 0; k < 4; ++k) {
                    rr[k] = pdf_to_real(ctx,
                        pdf_array_get(ctx, rectObj, k));
                }
                pdf_obj* pageObj = pdf_annot_page(ctx, annot)->obj;
                pdf_obj* mbox = pdf_dict_get_inheritable(
                    ctx, pageObj, PDF_NAME(MediaBox));
                float pageH = 792.0f;
                if (mbox && pdf_is_array(ctx, mbox) &&
                    pdf_array_len(ctx, mbox) >= 4) {
                    float my0 = pdf_to_real(ctx,
                        pdf_array_get(ctx, mbox, 1));
                    float my1 = pdf_to_real(ctx,
                        pdf_array_get(ctx, mbox, 3));
                    pageH = my1 - my0;
                }
                const float y0d = pageH - rr[3];
                const float y1d = pageH - rr[1];
                data.rect = QRectF(QPointF(rr[0], y0d),
                                   QPointF(rr[2], y1d)).normalized();
            }
        }
    }

    // Contents
    const char* contents = pdf_annot_contents(ctx, annot);
    if (contents) {
        data.contents = contents;
    }

    // Author: read /T directly from the annotation dictionary so it
    // works for all annotation types, not just those pdf_annot_has_author
    // considers "supported".
    {
        pdf_obj* annot_obj = pdf_annot_obj(ctx, annot);
        pdf_obj* t_obj = pdf_dict_get(ctx, annot_obj, PDF_NAME(T));
        if (t_obj && pdf_is_string(ctx, t_obj)) {
            const char* t_str = pdf_to_text_string(ctx, t_obj);
            if (t_str && *t_str) {
                data.author = t_str;
            }
        }
    }

    // Color
    int n = 0;
    float color[4] = {0};
    pdf_annot_color(ctx, annot, &n, color);
    if (n >= 3) {
        data.color = QColor::fromRgbF(color[0], color[1], color[2]);
    }

    // Interior color (only for types that support it)
    if (pdf_annot_has_interior_color(ctx, annot)) {
        n = 0;
        pdf_annot_interior_color(ctx, annot, &n, color);
        if (n >= 3) {
            data.interiorColor = QColor::fromRgbF(color[0], color[1], color[2]);
        }
    }

    // Opacity
    data.opacity = pdf_annot_opacity(ctx, annot);

    // Border width
    if (pdf_annot_has_border(ctx, annot)) {
        data.borderWidth = pdf_annot_border(ctx, annot);
    }

    // Flags (/F). F15 / PDF-51: the raw MuPDF int is converted to the named
    // AnnotationFlags mask HERE, at the engine boundary, and nowhere else.
    data.flags = AnnotationFlags::fromRaw(pdf_annot_flags(ctx, annot));

    // Quad points (for text markup annotations)
    if (pdf_annot_has_quad_points(ctx, annot)) {
        int quadCount = pdf_annot_quad_point_count(ctx, annot);
        for (int i = 0; i < quadCount; i++) {
            fz_quad q = pdf_annot_quad_point(ctx, annot, i);
            QRectF quadRect(
                QPointF(q.ul.x, q.ul.y),
                QPointF(q.lr.x, q.lr.y)
            );
            data.quadPoints.push_back(quadRect);
        }
    }

    // Ink strokes (for ink annotations)
    if (pdf_annot_has_ink_list(ctx, annot)) {
        int strokeCount = pdf_annot_ink_list_count(ctx, annot);
        for (int i = 0; i < strokeCount; i++) {
            int vertexCount = pdf_annot_ink_list_stroke_count(ctx, annot, i);
            std::vector<QPointF> stroke;
            for (int j = 0; j < vertexCount; j++) {
                fz_point pt = pdf_annot_ink_list_stroke_vertex(ctx, annot, i, j);
                stroke.push_back(QPointF(pt.x, pt.y));
            }
            data.inkStrokes.push_back(stroke);
        }
    }

    // Line endpoints
    if (pdf_annot_has_line(ctx, annot)) {
        fz_point a, b;
        pdf_annot_line(ctx, annot, &a, &b);
        data.lineStart = QPointF(a.x, a.y);
        data.lineEnd = QPointF(b.x, b.y);
    }

    // Icon name
    if (pdf_annot_has_icon_name(ctx, annot)) {
        const char* icon = pdf_annot_icon_name(ctx, annot);
        if (icon) {
            data.iconName = icon;
        }
    }

    // Rotation. Stored as a REAL under a PDFE-private key because the
    // standard /Rotate is an integer and cannot carry an arbitrary angle.
    // Absent on every annotation that has never been rotated, which leaves
    // the field at its 0.0 default.
    {
        pdf_obj* obj = pdf_annot_obj(ctx, annot);
        pdf_obj* rotObj = pdf_dict_gets(ctx, obj, "PDFERotationDegrees");
        if (rotObj) {
            data.rotationDegrees = pdf_to_real(ctx, rotObj);
        }
        // The pre-rotation content box travels with the angle: a path that
        // rebuilds an annotation from this struct needs both, and cannot
        // derive one from the other.
        pdf_obj* boxObj = pdf_dict_gets(ctx, obj, "PDFEUnrotatedRect");
        if (boxObj) {
            const fz_rect b = pdf_to_rect(ctx, boxObj);
            if (b.x1 > b.x0 && b.y1 > b.y0) {
                data.rotationContentRect =
                    QRectF(QPointF(b.x0, b.y0), QPointF(b.x1, b.y1));
            }
        }
    }

    // Default appearance (for FreeText annotations)
    if (pdf_annot_has_default_appearance(ctx, annot)) {
        const char* font = nullptr;
        float daSize = 0;
        int daN = 0;
        float daColor[4] = {0};
        pdf_annot_default_appearance(ctx, annot, &font, &daSize, &daN, daColor);
        if (font) {
            data.daFontName = font;
        }
        data.daFontSize = daSize;
        if (daN >= 3) {
            data.daColor = QColor::fromRgbF(daColor[0], daColor[1], daColor[2]);
        } else if (daN == 1) {
            data.daColor = QColor::fromRgbF(daColor[0], daColor[0], daColor[0]);
        }
    }

    // Timestamps
    data.creationDate = pdf_annot_creation_date(ctx, annot);
    data.modificationDate = pdf_annot_modification_date(ctx, annot);

    // Phase 8a: /IRT resolution — if this annotation replies to another
    // on the same page, record the parent's annotIndex.
    {
        pdf_obj* obj = pdf_annot_obj(ctx, annot);
        pdf_obj* irt = obj ? pdf_dict_get(ctx, obj, PDF_NAME(IRT)) : nullptr;
        if (irt) {
            // Walk the page's annotations to find which one /IRT points to.
            pdf_page* pg = pdf_annot_page(ctx, annot);
            if (pg) {
                pdf_annot* cur = pdf_first_annot(ctx, pg);
                int i = 0;
                while (cur) {
                    pdf_obj* curObj = pdf_annot_obj(ctx, cur);
                    if (curObj == irt ||
                        (pdf_to_num(ctx, curObj) != 0 &&
                         pdf_to_num(ctx, curObj) == pdf_to_num(ctx, irt))) {
                        data.inReplyToIndex = i;
                        break;
                    }
                    cur = pdf_next_annot(ctx, cur);
                    ++i;
                }
            }
        }
    }

    return data;
}

// Pimpl implementation
class AnnotationManager::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    pdf_page* getPdfPage(const pdfe::core::Page* page) {
        fz_page* fzPage = page->fzPage();
        return context->executeReturn<pdf_page*>([fzPage](fz_context* ctx) {
            return pdf_page_from_fz_page(ctx, fzPage);
        });
    }
};

// AnnotationManager implementation

AnnotationManager::AnnotationManager(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : pImpl(std::make_unique<Impl>(std::move(ctx))) {}

AnnotationManager::~AnnotationManager() = default;

AnnotationManager::AnnotationManager(AnnotationManager&&) noexcept = default;
AnnotationManager& AnnotationManager::operator=(AnnotationManager&&) noexcept = default;

AnnotationData AnnotationManager::createAnnotation(
    const pdfe::core::Page* page,
    AnnotationType type,
    QRectF rect
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();
    int newAnnotIndex = 0;

    pImpl->context->execute([&](fz_context* ctx) {
        // Create the annotation
        pdf_annot* annot = pdf_create_annot(
            ctx,
            pdfPage,
            (enum pdf_annot_type)toPdfAnnotType(type)
        );

        // Set bounding rectangle. pdf_annot_has_rect filters out some
        // types (specifically /Link, which MuPDF treats as read-only
        // for pdf_set_annot_rect despite PDF spec requiring /Rect on
        // all annotations). Fall back to writing /Rect directly to
        // the annotation's object for those subtypes.
        {
            fz_rect r = {
                static_cast<float>(rect.left()),
                static_cast<float>(rect.top()),
                static_cast<float>(rect.right()),
                static_cast<float>(rect.bottom())
            };
            if (pdf_annot_has_rect(ctx, annot)) {
                pdf_set_annot_rect(ctx, annot, r);
            } else {
                // Direct /Rect write in PDF Y-up convention. Other
                // MuPDF code paths expect /Rect as a 4-float array;
                // pdf_bound_annot for these types won't return our
                // rect (it only works when pdf_annot_has_rect is
                // true), so readers must fall back to /Rect reading.
                pdf_obj* obj = pdf_annot_obj(ctx, annot);
                pdf_obj* pageObj = pdf_annot_page(ctx, annot)->obj;
                pdf_obj* mbox = pdf_dict_get_inheritable(
                    ctx, pageObj, PDF_NAME(MediaBox));
                float pageH = 792.0f;
                if (mbox && pdf_is_array(ctx, mbox) &&
                    pdf_array_len(ctx, mbox) >= 4) {
                    float my0 = pdf_to_real(ctx, pdf_array_get(ctx, mbox, 1));
                    float my1 = pdf_to_real(ctx, pdf_array_get(ctx, mbox, 3));
                    pageH = my1 - my0;
                }
                // Convert Y-down (rect as passed) to Y-up (PDF /Rect).
                pdf_obj* rectArr = pdf_new_array(ctx,
                    pdf_get_bound_document(ctx, obj), 4);
                pdf_array_push_real(ctx, rectArr, r.x0);
                pdf_array_push_real(ctx, rectArr, pageH - r.y1);
                pdf_array_push_real(ctx, rectArr, r.x1);
                pdf_array_push_real(ctx, rectArr, pageH - r.y0);
                pdf_dict_put_drop(ctx, obj, PDF_NAME(Rect), rectArr);
            }
        }

        // Update the annotation
        pdf_update_annot(ctx, annot);

        // Find the index of the newly created annotation
        pdf_annot* current = pdf_first_annot(ctx, pdfPage);
        int index = 0;
        while (current) {
            if (current == annot) {
                newAnnotIndex = index;
                break;
            }
            current = pdf_next_annot(ctx, current);
            index++;
        }
    });

    return getAnnotation(page, newAnnotIndex);
}

AnnotationData AnnotationManager::deleteAnnotation(
    const pdfe::core::Page* page,
    int annotIndex
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();

    // First, get the annotation data for undo
    AnnotationData data = getAnnotation(page, annotIndex);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        pdf_delete_annot(ctx, pdfPage, annot);
    });

    return data;
}

namespace {

// Rotation keys and the re-assert helper. The helper is DEFINED further down,
// next to setRotation() where the reasoning lives; it and these keys are
// declared here because callers above that point need them -- every property
// setter can make MuPDF resynthesise a FreeText appearance and drop the
// rotation, and restoreAnnotation has to write the keys back by hand.
//
// PDFE-private keys on the annotation dictionary. Extra entries are legal
// (ISO 32000-1 7.3.7) and viewers ignore what they do not know.
//
//   /PDFERotationDegrees  the angle, as a REAL. The standard /Rotate is an
//                         integer, so it cannot carry 37.4 degrees; /Rotate is
//                         still written alongside for interoperability.
//   /PDFEUnrotatedRect    the box the content is laid out in BEFORE rotating.
//
// The unrotated box has to be stored, not derived. Recovering it from the
// widened /Rect and the angle means solving
//     W = hw|cos| + hh|sin|,  H = hw|sin| + hh|cos|
// which is singular at 45 degrees and ill-conditioned near it. Storing four
// numbers avoids an inverse problem that has no answer at one angle.
const char* kRotationDegreesKey = "PDFERotationDegrees";
const char* kUnrotatedRectKey = "PDFEUnrotatedRect";

constexpr double kRotationEpsilonDeg = 1e-6;

void reapplyRotation(fz_context* ctx, pdf_annot* annot);

} // namespace

int AnnotationManager::restoreAnnotation(
    const pdfe::core::Page* page,
    const AnnotationData& data
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int newAnnotIndex = 0;

    pImpl->context->execute([&](fz_context* ctx) {
        // Create the annotation
        pdf_annot* annot = pdf_create_annot(
            ctx,
            pdfPage,
            (enum pdf_annot_type)toPdfAnnotType(data.type)
        );

        // Set bounding rectangle (if supported)
        if (pdf_annot_has_rect(ctx, annot)) {
            fz_rect r = {
                static_cast<float>(data.rect.left()),
                static_cast<float>(data.rect.top()),
                static_cast<float>(data.rect.right()),
                static_cast<float>(data.rect.bottom())
            };
            pdf_set_annot_rect(ctx, annot, r);
        }

        // Set contents
        if (!data.contents.empty()) {
            pdf_set_annot_contents(ctx, annot, data.contents.c_str());
        }

        // Set author (only for supported types)
        if (!data.author.empty() && pdf_annot_has_author(ctx, annot)) {
            pdf_set_annot_author(ctx, annot, data.author.c_str());
        }

        // Set color
        float rgb[3] = {
            static_cast<float>(data.color.redF()),
            static_cast<float>(data.color.greenF()),
            static_cast<float>(data.color.blueF())
        };
        pdf_set_annot_color(ctx, annot, 3, rgb);

        // Set interior color (only for supported types)
        if (data.interiorColor.alpha() > 0 && pdf_annot_has_interior_color(ctx, annot)) {
            float interiorRgb[3] = {
                static_cast<float>(data.interiorColor.redF()),
                static_cast<float>(data.interiorColor.greenF()),
                static_cast<float>(data.interiorColor.blueF())
            };
            pdf_set_annot_interior_color(ctx, annot, 3, interiorRgb);
        }

        // Set opacity
        pdf_set_annot_opacity(ctx, annot, data.opacity);

        // Set border width (only for supported types)
        if (pdf_annot_has_border(ctx, annot)) {
            pdf_set_annot_border(ctx, annot, data.borderWidth);
        }

        // Set flags (named mask -> raw int at the engine boundary only)
        pdf_set_annot_flags(ctx, annot, data.flags.toRaw());

        // Set quad points (only for markup annotations)
        if (!data.quadPoints.empty() && pdf_annot_has_quad_points(ctx, annot)) {
            pdf_clear_annot_quad_points(ctx, annot);
            for (const auto& quad : data.quadPoints) {
                fz_quad q;
                q.ul = {static_cast<float>(quad.left()), static_cast<float>(quad.top())};
                q.ur = {static_cast<float>(quad.right()), static_cast<float>(quad.top())};
                q.ll = {static_cast<float>(quad.left()), static_cast<float>(quad.bottom())};
                q.lr = {static_cast<float>(quad.right()), static_cast<float>(quad.bottom())};
                pdf_add_annot_quad_point(ctx, annot, q);
            }
        }

        // Set ink strokes (only for ink annotations)
        if (!data.inkStrokes.empty() && pdf_annot_has_ink_list(ctx, annot)) {
            pdf_clear_annot_ink_list(ctx, annot);
            for (const auto& stroke : data.inkStrokes) {
                pdf_add_annot_ink_list_stroke(ctx, annot);
                for (const auto& pt : stroke) {
                    fz_point p = {static_cast<float>(pt.x()), static_cast<float>(pt.y())};
                    pdf_add_annot_ink_list_stroke_vertex(ctx, annot, p);
                }
            }
        }

        // Set line endpoints
        if (pdf_annot_has_line(ctx, annot)) {
            fz_point a = {static_cast<float>(data.lineStart.x()), static_cast<float>(data.lineStart.y())};
            fz_point b = {static_cast<float>(data.lineEnd.x()), static_cast<float>(data.lineEnd.y())};
            pdf_set_annot_line(ctx, annot, a, b);
        }

        // Set icon name (only for supported types)
        if (!data.iconName.empty() && pdf_annot_has_icon_name(ctx, annot)) {
            pdf_set_annot_icon_name(ctx, annot, data.iconName.c_str());
        }

        // Set default appearance (for FreeText annotations)
        if (!data.daFontName.empty() && pdf_annot_has_default_appearance(ctx, annot)) {
            float daRgb[3] = {
                static_cast<float>(data.daColor.redF()),
                static_cast<float>(data.daColor.greenF()),
                static_cast<float>(data.daColor.blueF())
            };
            pdf_set_annot_default_appearance(ctx, annot,
                data.daFontName.c_str(), data.daFontSize, 3, daRgb);
        }

        // Update the annotation
        pdf_update_annot(ctx, annot);

        // Restore rotation, if the deleted annotation had any.
        //
        // This path REBUILDS an annotation rather than mutating one, so it
        // does not go through setRotation and the private keys have to be
        // written back explicitly. Missed in the first pass of the rotation
        // work and found by a gate agent: delete a rotated text box, press
        // Ctrl+Z, and the text came back upright while the box kept its
        // widened bounds.
        //
        // Both keys are required. The content box cannot be recomputed from
        // the restored /Rect and the angle -- that inversion is singular at
        // 45 degrees -- which is why AnnotationData carries it.
        if (std::abs(data.rotationDegrees) > kRotationEpsilonDeg &&
            data.rotationContentRect.isValid()) {
            pdf_obj* obj = pdf_annot_obj(ctx, annot);
            const fz_rect box = fz_make_rect(
                static_cast<float>(data.rotationContentRect.left()),
                static_cast<float>(data.rotationContentRect.top()),
                static_cast<float>(data.rotationContentRect.right()),
                static_cast<float>(data.rotationContentRect.bottom()));
            pdf_dict_puts_drop(ctx, obj, kUnrotatedRectKey,
                               pdf_new_rect(ctx, pdf_get_bound_document(ctx, obj),
                                            box));
            pdf_dict_puts_drop(ctx, obj, kRotationDegreesKey,
                               pdf_new_real(ctx, data.rotationDegrees));
            pdf_dict_put_int(ctx, obj, PDF_NAME(Rotate),
                             static_cast<int64_t>(std::lround(data.rotationDegrees)) % 360);
            pdf_annot_request_resynthesis(ctx, annot);
            reapplyRotation(ctx, annot);
        }

        // Find the index of the newly created annotation
        pdf_annot* current = pdf_first_annot(ctx, pdfPage);
        int index = 0;
        while (current) {
            if (current == annot) {
                newAnnotIndex = index;
                break;
            }
            current = pdf_next_annot(ctx, current);
            index++;
        }
    });

    return newAnnotIndex;
}

void AnnotationManager::setContents(
    const pdfe::core::Page* page,
    int annotIndex,
    const std::string& text
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        // F15 / PDF-51: /LockedContents (bit 10) and /ReadOnly (bit 7) mean the
        // annotation's text must not be altered by the user. Before this the
        // flag was read into an int nobody consulted, so PDFE happily rewrote
        // /Contents on a locked comment.
        const AnnotationFlags flags =
            AnnotationFlags::fromRaw(pdf_annot_flags(ctx, annot));
        if (flags.isContentsLocked()) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT,
                     "Annotation contents are locked (/F %s); refusing to edit",
                     describeAnnotationFlags(flags).c_str());
        }

        pdf_set_annot_contents(ctx, annot, text.c_str());
        pdf_update_annot(ctx, annot);
        // Rotation survives nothing on its own: MuPDF carves out Stamps
        // with custom appearances from resynthesis but NOT FreeText, so the
        // update above may have just replaced a rotated appearance with an
        // upright one. No-op when nothing is rotated.
        reapplyRotation(ctx, annot);
    });
}

void AnnotationManager::setAuthor(
    const pdfe::core::Page* page,
    int annotIndex,
    const std::string& author
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        // Set the /T (title/author) field directly in the annotation
        // dictionary.  pdf_set_annot_author is type-guarded in MuPDF and
        // silently does nothing for annotation types it considers to not
        // "have" an author (e.g. Square, Circle).  Writing via the PDF
        // dictionary works for ALL annotation types.
        pdf_obj* annot_obj = pdf_annot_obj(ctx, annot);
        if (author.empty()) {
            pdf_dict_del(ctx, annot_obj, PDF_NAME(T));
        } else {
            pdf_obj* t_str = pdf_new_text_string(ctx, author.c_str());
            pdf_dict_put(ctx, annot_obj, PDF_NAME(T), t_str);
            pdf_drop_obj(ctx, t_str);
        }
        pdf_update_annot(ctx, annot);
        // Rotation survives nothing on its own: MuPDF carves out Stamps
        // with custom appearances from resynthesis but NOT FreeText, so the
        // update above may have just replaced a rotated appearance with an
        // upright one. No-op when nothing is rotated.
        reapplyRotation(ctx, annot);
    });
}

void AnnotationManager::setColor(
    const pdfe::core::Page* page,
    int annotIndex,
    QColor color
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        float rgb[3] = {
            static_cast<float>(color.redF()),
            static_cast<float>(color.greenF()),
            static_cast<float>(color.blueF())
        };
        pdf_set_annot_color(ctx, annot, 3, rgb);
        pdf_update_annot(ctx, annot);
        // Rotation survives nothing on its own: MuPDF carves out Stamps
        // with custom appearances from resynthesis but NOT FreeText, so the
        // update above may have just replaced a rotated appearance with an
        // upright one. No-op when nothing is rotated.
        reapplyRotation(ctx, annot);
    });
}

void AnnotationManager::setOpacity(
    const pdfe::core::Page* page,
    int annotIndex,
    float opacity
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        pdf_set_annot_opacity(ctx, annot, opacity);
        pdf_update_annot(ctx, annot);
        // Rotation survives nothing on its own: MuPDF carves out Stamps
        // with custom appearances from resynthesis but NOT FreeText, so the
        // update above may have just replaced a rotated appearance with an
        // upright one. No-op when nothing is rotated.
        reapplyRotation(ctx, annot);
    });
}

namespace {

// Rotation support -----------------------------------------------------------
//
// PDFE-private keys on the annotation dictionary. Extra entries are legal
// (ISO 32000-1 7.3.7) and viewers ignore what they do not know.
//
//   /PDFERotationDegrees  the angle, as a REAL. The standard /Rotate is an
//                         integer, so it cannot carry 37.4 degrees; /Rotate is
//                         still written alongside for interoperability.
//   /PDFEUnrotatedRect    the box the content is laid out in BEFORE rotating.
//
// The unrotated box has to be stored, not derived. Recovering it from the
// widened /Rect and the angle means solving
//     W = hw|cos| + hh|sin|,  H = hw|sin| + hh|cos|
// which is singular at 45 degrees and ill-conditioned near it. Storing four
// numbers avoids an inverse problem that has no answer at one angle.

/// Axis-aligned bounds of `r` rotated by `degrees` about its own centre.
fz_rect rotatedBounds(const fz_rect& r, double degrees) {
    const double rad = degrees * 3.14159265358979323846 / 180.0;
    const double c = std::abs(std::cos(rad));
    const double s = std::abs(std::sin(rad));
    const double cx = (r.x0 + r.x1) / 2.0;
    const double cy = (r.y0 + r.y1) / 2.0;
    const double hw = (r.x1 - r.x0) / 2.0;
    const double hh = (r.y1 - r.y0) / 2.0;
    const double nw = hw * c + hh * s;
    const double nh = hw * s + hh * c;
    return fz_make_rect(static_cast<float>(cx - nw), static_cast<float>(cy - nh),
                        static_cast<float>(cx + nw), static_cast<float>(cy + nh));
}

/// Rotation about (cx, cy) as a PDF matrix.
fz_matrix rotationAbout(double degrees, double cx, double cy) {
    fz_matrix m = fz_rotate(static_cast<float>(degrees));
    // T(c) . R . T(-c)
    m.e = static_cast<float>(cx - (m.a * cx + m.c * cy));
    m.f = static_cast<float>(cy - (m.b * cx + m.d * cy));
    return m;
}

/// Re-apply the stored rotation to the annotation's appearance stream.
///
/// MUST be called after anything that lets MuPDF resynthesise the appearance.
/// pdf-appearance.c has an explicit carve-out that refuses to resynthesise a
/// Stamp carrying a custom appearance, and NO such carve-out for FreeText: any
/// later edit (contents, colour, rect) sets needs_new_ap, MuPDF regenerates
/// the FreeText appearance with fz_identity, and the rotation is silently
/// gone. Re-asserting is the only way to survive that.
///
/// No-op when the annotation carries no stored rotation, so it is safe to call
/// unconditionally.
void reapplyRotation(fz_context* ctx, pdf_annot* annot) {
    if (!annot) return;
    pdf_obj* obj = pdf_annot_obj(ctx, annot);
    if (!obj) return;

    pdf_obj* degreesObj = pdf_dict_gets(ctx, obj, kRotationDegreesKey);
    if (!degreesObj) return;
    const double degrees = pdf_to_real(ctx, degreesObj);

    pdf_obj* unrotObj = pdf_dict_gets(ctx, obj, kUnrotatedRectKey);
    if (!unrotObj) return;
    const fz_rect unrotated = pdf_to_rect(ctx, unrotObj);
    if (unrotated.x1 <= unrotated.x0 || unrotated.y1 <= unrotated.y0) return;

    // Lay the content out in the UNROTATED box, then rotate the result. The
    // appearance MuPDF generates depends on /Rect, so the rect must be the
    // unrotated one at the moment of synthesis.
    pdf_set_annot_rect(ctx, annot, unrotated);
    pdf_update_annot(ctx, annot);

    if (std::abs(degrees) < kRotationEpsilonDeg) {
        return;  // stored as zero: leave the appearance upright.
    }

    pdf_obj* ap = pdf_annot_ap(ctx, annot);
    if (!ap) return;  // nothing synthesised; nothing to rotate.

    const double cx = (unrotated.x0 + unrotated.x1) / 2.0;
    const double cy = (unrotated.y0 + unrotated.y1) / 2.0;
    pdf_dict_put_matrix(ctx, ap, PDF_NAME(Matrix),
                        rotationAbout(degrees, cx, cy));

    // pdf_annot_transform maps (BBox x Matrix) onto /Rect, so /Rect must be
    // the rotated bounds or the appearance would be squashed back into the
    // upright box instead of rotated.
    pdf_set_annot_rect(ctx, annot, rotatedBounds(unrotated, degrees));

    // CRITICAL, and the reason this line exists at all: pdf_set_annot_rect
    // dirties the annotation, so it is left REQUESTING resynthesis with our
    // rotation matrix already installed. The next pdf_update_annot from
    // anywhere then regenerates the appearance with fz_identity and the
    // rotation is gone -- and pdf-write.c requests resynthesis during save,
    // so "anywhere" includes simply saving the file.
    //
    // This was not hypothetical. It made the rotation survive or not
    // depending on what else had run first, which is the worst possible
    // shape for a defect: the tests passed once, by ordering luck, before
    // failing. Declaring the annotation already-resynthesised closes the
    // window, because the appearance it now carries IS the one we want.
    pdf_set_annot_resynthesised(ctx, annot);
}

} // namespace

void AnnotationManager::setRotation(
    const pdfe::core::Page* page,
    int annotIndex,
    double degrees,
    QRectF rect
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        // Rotating is a geometry change, so it is refused under the same
        // flags as moving or resizing (ISO 32000-1 table 165).
        const AnnotationFlags flags =
            AnnotationFlags::fromRaw(pdf_annot_flags(ctx, annot));
        if (flags.isGeometryLocked()) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT,
                     "Annotation geometry is locked (/F %s); refusing to "
                     "rotate", describeAnnotationFlags(flags).c_str());
        }

        pdf_obj* obj = pdf_annot_obj(ctx, annot);

        // The unrotated box. `rect` from the caller is the axis-aligned
        // bounds the rotated content occupies; the box the content is laid
        // out in is whatever it was before this rotation. On the first
        // rotation that is the annotation's current /Rect; on a subsequent
        // one it is the box already recorded, because re-deriving it from a
        // rect that has already been widened would compound the widening on
        // every drag of the rotate handle.
        fz_rect unrotated;
        pdf_obj* existing = pdf_dict_gets(ctx, obj, kUnrotatedRectKey);
        if (existing) {
            unrotated = pdf_to_rect(ctx, existing);
        } else {
            unrotated = pdf_annot_rect(ctx, annot);
        }
        if (unrotated.x1 <= unrotated.x0 || unrotated.y1 <= unrotated.y0) {
            // Nothing sane recorded; fall back to the caller's rect so the
            // annotation still has a usable box.
            unrotated = fz_make_rect(
                static_cast<float>(rect.left()), static_cast<float>(rect.top()),
                static_cast<float>(rect.right()), static_cast<float>(rect.bottom()));
        }

        pdf_dict_puts_drop(ctx, obj, kUnrotatedRectKey,
                           pdf_new_rect(ctx, pdf_get_bound_document(ctx, obj),
                                        unrotated));
        pdf_dict_puts_drop(ctx, obj, kRotationDegreesKey,
                           pdf_new_real(ctx, degrees));

        // /Rotate as well, for viewers that read Adobe's undocumented key.
        // Rounded, because it is an integer; it is NOT what we render from.
        pdf_dict_put_int(ctx, obj, PDF_NAME(Rotate),
                         static_cast<int64_t>(std::lround(degrees)) % 360);

        // Force a fresh appearance, then rotate it.
        pdf_annot_request_resynthesis(ctx, annot);
        reapplyRotation(ctx, annot);
    });
}

void AnnotationManager::setRect(
    const pdfe::core::Page* page,
    int annotIndex,
    QRectF rect
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        // F15 / PDF-51: /Locked (bit 8) and /ReadOnly (bit 7) forbid the user
        // moving or resizing the annotation (ISO 32000-1 table 165).
        const AnnotationFlags flags =
            AnnotationFlags::fromRaw(pdf_annot_flags(ctx, annot));
        if (flags.isGeometryLocked()) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT,
                     "Annotation geometry is locked (/F %s); refusing to move "
                     "or resize", describeAnnotationFlags(flags).c_str());
        }

        fz_rect r = {
            static_cast<float>(rect.left()),
            static_cast<float>(rect.top()),
            static_cast<float>(rect.right()),
            static_cast<float>(rect.bottom())
        };

        // A rotated annotation carries a separate content box, and /Rect is
        // the axis-aligned bounds of that box once rotated. Writing the
        // caller's rect straight into /Rect and then re-asserting the
        // rotation would silently snap the annotation back to where the
        // content box says it is, i.e. the move would be discarded. Translate
        // the content box by the same delta instead.
        //
        // A MOVE translates the content box; a RESIZE has to solve for a new
        // one, because `r` is the axis-aligned BOUNDS of the rotated content,
        // not the content itself.
        //
        // Forward, for a content half-size (hw, hh) at angle t:
        //     W = hw|cos t| + hh|sin t|
        //     H = hw|sin t| + hh|cos t|
        // Inverting that 2x2 for a desired (W, H) gives
        //     hw = (W|cos| - H|sin|) / cos 2t
        //     hh = (H|cos| - W|sin|) / cos 2t
        // whose determinant cos 2t VANISHES AT 45 DEGREES -- the same
        // singularity that made storing the content box necessary in the
        // first place. Near it, and whenever the solution comes out
        // non-positive (an over-tall drag on a wide box can), fall back to
        // scaling the existing box by the area ratio: not the exact inverse,
        // but stable, monotonic in the drag, and never degenerate.
        pdf_obj* obj = pdf_annot_obj(ctx, annot);
        pdf_obj* unrotObj = pdf_dict_gets(ctx, obj, kUnrotatedRectKey);
        if (unrotObj) {
            const fz_rect oldBounds = pdf_annot_rect(ctx, annot);
            const fz_rect content = pdf_to_rect(ctx, unrotObj);
            const double dx = ((r.x0 + r.x1) - (oldBounds.x0 + oldBounds.x1)) / 2.0;
            const double dy = ((r.y0 + r.y1) - (oldBounds.y0 + oldBounds.y1)) / 2.0;

            const double oldW = oldBounds.x1 - oldBounds.x0;
            const double oldH = oldBounds.y1 - oldBounds.y0;
            const double newW = r.x1 - r.x0;
            const double newH = r.y1 - r.y0;
            const bool resized = oldW > 0.0 && oldH > 0.0
                && (std::abs(newW - oldW) > 0.5 || std::abs(newH - oldH) > 0.5);

            double cw = (content.x1 - content.x0) / 2.0;
            double ch = (content.y1 - content.y0) / 2.0;

            if (resized) {
                pdf_obj* degObj = pdf_dict_gets(ctx, obj, kRotationDegreesKey);
                const double deg = degObj ? pdf_to_real(ctx, degObj) : 0.0;
                const double rad = deg * 3.14159265358979323846 / 180.0;
                const double c = std::abs(std::cos(rad));
                const double s = std::abs(std::sin(rad));
                const double det = c * c - s * s;   // == cos 2t
                bool solved = false;
                if (std::abs(det) > 0.02) {
                    const double hw = ((newW / 2.0) * c - (newH / 2.0) * s) / det;
                    const double hh = ((newH / 2.0) * c - (newW / 2.0) * s) / det;
                    if (hw > 0.5 && hh > 0.5) {
                        cw = hw;
                        ch = hh;
                        solved = true;
                    }
                }
                if (!solved) {
                    const double ratio = std::sqrt(
                        std::max(1e-6, (newW * newH) / (oldW * oldH)));
                    cw *= ratio;
                    ch *= ratio;
                }
            }

            const double ccx = (content.x0 + content.x1) / 2.0 + dx;
            const double ccy = (content.y0 + content.y1) / 2.0 + dy;
            const fz_rect moved = fz_make_rect(
                static_cast<float>(ccx - cw),
                static_cast<float>(ccy - ch),
                static_cast<float>(ccx + cw),
                static_cast<float>(ccy + ch));
            pdf_dict_puts_drop(ctx, obj, kUnrotatedRectKey,
                               pdf_new_rect(ctx, pdf_get_bound_document(ctx, obj),
                                            moved));
            pdf_annot_request_resynthesis(ctx, annot);
            reapplyRotation(ctx, annot);
            return;
        }

        pdf_set_annot_rect(ctx, annot, r);
        pdf_update_annot(ctx, annot);
    });
}

// ─── F15 / PDF-51: named annotation flags ────────────────────────────────

AnnotationFlags AnnotationManager::getAnnotationFlags(
    const pdfe::core::Page* page,
    int annotIndex
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    const int raw = pImpl->context->executeReturn<int>(
        [pdfPage, annotIndex](fz_context* ctx) -> int {
            pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
            if (!annot) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
            }
            return pdf_annot_flags(ctx, annot);
        });
    return AnnotationFlags::fromRaw(raw);
}

void AnnotationManager::setAnnotationFlags(
    const pdfe::core::Page* page,
    int annotIndex,
    AnnotationFlags flags
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    const int raw = flags.toRaw();

    pImpl->context->execute([pdfPage, annotIndex, raw](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }
        pdf_set_annot_flags(ctx, annot, raw);
        pdf_update_annot(ctx, annot);
        // pdf_set_annot_flags -> pdf_dirty_annot -> request_resynthesis, so
        // this reaches the appearance exactly like the other setters do. It
        // was MISSED in the first pass of this fix -- the claim "every setter
        // re-asserts" was written while five of the six did -- and a gate
        // caught it by reading the MuPDF call chain rather than the claim.
        // No UI path reaches this method today, which is precisely why it
        // would have sat broken until one did.
        reapplyRotation(ctx, annot);
    });
}

void AnnotationManager::setAnnotationFlag(
    const pdfe::core::Page* page,
    int annotIndex,
    AnnotationFlag flag,
    bool on
) {
    AnnotationFlags flags = getAnnotationFlags(page, annotIndex);
    flags.set(flag, on);
    setAnnotationFlags(page, annotIndex, flags);
}

void AnnotationManager::setQuadPoints(
    const pdfe::core::Page* page,
    int annotIndex,
    const std::vector<QRectF>& quads
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        pdf_clear_annot_quad_points(ctx, annot);
        for (const auto& quad : quads) {
            fz_quad q;
            q.ul = {static_cast<float>(quad.left()), static_cast<float>(quad.top())};
            q.ur = {static_cast<float>(quad.right()), static_cast<float>(quad.top())};
            q.ll = {static_cast<float>(quad.left()), static_cast<float>(quad.bottom())};
            q.lr = {static_cast<float>(quad.right()), static_cast<float>(quad.bottom())};
            pdf_add_annot_quad_point(ctx, annot, q);
        }
        pdf_update_annot(ctx, annot);
    });
}

void AnnotationManager::setInkStrokes(
    const pdfe::core::Page* page,
    int annotIndex,
    const std::vector<std::vector<QPointF>>& strokes
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        pdf_clear_annot_ink_list(ctx, annot);
        for (const auto& stroke : strokes) {
            pdf_add_annot_ink_list_stroke(ctx, annot);
            for (const auto& pt : stroke) {
                fz_point p = {static_cast<float>(pt.x()), static_cast<float>(pt.y())};
                pdf_add_annot_ink_list_stroke_vertex(ctx, annot, p);
            }
        }
        pdf_update_annot(ctx, annot);
    });
}

void AnnotationManager::setLineEndpoints(
    const pdfe::core::Page* page,
    int annotIndex,
    QPointF start,
    QPointF end
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        if (pdf_annot_has_line(ctx, annot)) {
            fz_point a = {static_cast<float>(start.x()), static_cast<float>(start.y())};
            fz_point b = {static_cast<float>(end.x()), static_cast<float>(end.y())};
            pdf_set_annot_line(ctx, annot, a, b);
            pdf_update_annot(ctx, annot);
        }
    });
}

void AnnotationManager::setDefaultAppearance(
    const pdfe::core::Page* page,
    int annotIndex,
    const std::string& font,
    float size,
    QColor color
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        if (pdf_annot_has_default_appearance(ctx, annot)) {
            float rgb[3] = {
                static_cast<float>(color.redF()),
                static_cast<float>(color.greenF()),
                static_cast<float>(color.blueF())
            };
            pdf_set_annot_default_appearance(ctx, annot, font.c_str(), size, 3, rgb);
            pdf_update_annot(ctx, annot);
            // Rotation survives nothing on its own: MuPDF carves out Stamps
            // with custom appearances from resynthesis but NOT FreeText, so the
            // update above may have just replaced a rotated appearance with an
            // upright one. No-op when nothing is rotated.
            reapplyRotation(ctx, annot);
        }
    });
}

// Phase 8c: /Alt and /ActualText accessibility fields on annotations.
// These are stored directly on the annotation's pdf_obj as text strings.
namespace {
void setAnnotStringField(fz_context* ctx, pdf_document* /*doc*/,
                         pdf_annot* annot, pdf_obj* key, const std::string& value)
{
    pdf_obj* obj = pdf_annot_obj(ctx, annot);
    if (!obj) return;
    if (value.empty()) {
        pdf_dict_del(ctx, obj, key);
    } else {
        pdf_dict_put_text_string(ctx, obj, key, value.c_str());
    }
    pdf_update_annot(ctx, annot);
}

std::string getAnnotStringField(fz_context* ctx, pdf_annot* annot, pdf_obj* key)
{
    pdf_obj* obj = pdf_annot_obj(ctx, annot);
    if (!obj) return {};
    pdf_obj* val = pdf_dict_get(ctx, obj, key);
    if (!val) return {};
    const char* s = pdf_to_text_string(ctx, val);
    return s ? std::string(s) : std::string();
}
} // namespace

void AnnotationManager::setAltText(
    const pdfe::core::Page* page, int annotIndex, const std::string& altText)
{
    if (!page) throw std::invalid_argument("Page cannot be null");
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) fz_throw(ctx, FZ_ERROR_ARGUMENT,
            "Annotation index out of range");
        setAnnotStringField(ctx, nullptr, annot, PDF_NAME(Alt), altText);
    });
}

std::string AnnotationManager::getAltText(
    const pdfe::core::Page* page, int annotIndex)
{
    if (!page) throw std::invalid_argument("Page cannot be null");
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    return pImpl->context->executeReturn<std::string>(
        [&](fz_context* ctx) -> std::string {
            pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
            if (!annot) return {};
            return getAnnotStringField(ctx, annot, PDF_NAME(Alt));
        });
}

void AnnotationManager::setActualText(
    const pdfe::core::Page* page, int annotIndex, const std::string& actualText)
{
    if (!page) throw std::invalid_argument("Page cannot be null");
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) fz_throw(ctx, FZ_ERROR_ARGUMENT,
            "Annotation index out of range");
        setAnnotStringField(ctx, nullptr, annot, PDF_NAME(ActualText), actualText);
    });
}

std::string AnnotationManager::getActualText(
    const pdfe::core::Page* page, int annotIndex)
{
    if (!page) throw std::invalid_argument("Page cannot be null");
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    return pImpl->context->executeReturn<std::string>(
        [&](fz_context* ctx) -> std::string {
            pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
            if (!annot) return {};
            return getAnnotStringField(ctx, annot, PDF_NAME(ActualText));
        });
}

// Phase 8a: /IRT reply chains. The PDF spec expresses "this annotation
// replies to that one" via an /IRT dict entry on the reply, holding an
// indirect reference to the parent annotation's object. Walk the same
// page's annotations to find the parent by index or (on read) to map
// an IRT reference back to an annotIndex.
void AnnotationManager::setInReplyTo(
    const pdfe::core::Page* page,
    int replyAnnotIndex,
    int parentAnnotIndex
) {
    if (!page) throw std::invalid_argument("Page cannot be null");
    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* reply = getNthAnnot(ctx, pdfPage, replyAnnotIndex);
        if (!reply) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT,
                "Reply annotation index out of range");
        }
        pdf_obj* replyObj = pdf_annot_obj(ctx, reply);
        if (!replyObj) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "Reply annotation has no object");
        }
        if (parentAnnotIndex < 0) {
            pdf_dict_del(ctx, replyObj, PDF_NAME(IRT));
            pdf_update_annot(ctx, reply);
            return;
        }
        pdf_annot* parent = getNthAnnot(ctx, pdfPage, parentAnnotIndex);
        if (!parent) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT,
                "Parent annotation index out of range");
        }
        pdf_obj* parentObj = pdf_annot_obj(ctx, parent);
        if (!parentObj) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "Parent annotation has no object");
        }
        pdf_dict_put(ctx, replyObj, PDF_NAME(IRT), parentObj);
        pdf_update_annot(ctx, reply);
    });
}

int AnnotationManager::getInReplyTo(
    const pdfe::core::Page* page,
    int replyAnnotIndex
) {
    if (!page) throw std::invalid_argument("Page cannot be null");
    pdf_page* pdfPage = pImpl->getPdfPage(page);

    return pImpl->context->executeReturn<int>([&](fz_context* ctx) -> int {
        pdf_annot* reply = getNthAnnot(ctx, pdfPage, replyAnnotIndex);
        if (!reply) return -1;
        pdf_obj* replyObj = pdf_annot_obj(ctx, reply);
        if (!replyObj) return -1;
        pdf_obj* irt = pdf_dict_get(ctx, replyObj, PDF_NAME(IRT));
        if (!irt) return -1;
        // Walk the page's annotations and compare object identity.
        pdf_annot* cur = pdf_first_annot(ctx, pdfPage);
        int idx = 0;
        while (cur) {
            pdf_obj* curObj = pdf_annot_obj(ctx, cur);
            if (curObj == irt) return idx;
            // Also compare via object number for indirect references.
            if (pdf_to_num(ctx, curObj) != 0 &&
                pdf_to_num(ctx, curObj) == pdf_to_num(ctx, irt)) {
                return idx;
            }
            cur = pdf_next_annot(ctx, cur);
            ++idx;
        }
        return -1;
    });
}

void AnnotationManager::setLinkAction(
    const pdfe::core::Page* page,
    int annotIndex,
    const LinkAction& action
) {
    if (!page) throw std::invalid_argument("Page cannot be null");
    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }
        pdf_obj* obj = pdf_annot_obj(ctx, annot);
        if (!obj) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "Annotation has no object");
        }
        pdf_document* doc = pdf_get_bound_document(ctx, obj);
        if (!doc) {
            fz_throw(ctx, FZ_ERROR_GENERIC,
                "Annotation not bound to a document");
        }

        // Remove any existing /A and /Dest so the new action is the
        // sole source of truth. PDF spec: if both are present /A wins,
        // but dropping /Dest keeps the object clean.
        pdf_dict_del(ctx, obj, PDF_NAME(A));
        pdf_dict_del(ctx, obj, PDF_NAME(Dest));

        pdf_obj* aDict = pdf_dict_put_dict(ctx, obj, PDF_NAME(A), 4);
        switch (action.type) {
        case LinkActionType::Uri: {
            pdf_dict_put(ctx, aDict, PDF_NAME(S), PDF_NAME(URI));
            pdf_dict_put_text_string(ctx, aDict, PDF_NAME(URI),
                                     action.uri.c_str());
            // Clear /Contents: legacy code stored the URI there; /A is
            // authoritative, /Contents is just alt text.
            pdf_set_annot_contents(ctx, annot, "");
            break;
        }
        case LinkActionType::GoTo: {
            pdf_dict_put(ctx, aDict, PDF_NAME(S), PDF_NAME(GoTo));
            // /D is an array: [<page-ref> /XYZ null null null] — use the
            // page's indirect reference and a /Fit view to land the
            // viewport on the whole page without specifying coords.
            pdf_obj* pageObj = pdf_lookup_page_obj(ctx, doc, action.pageIndex);
            if (!pageObj) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT,
                    "GoTo page index out of range");
            }
            pdf_obj* dest = pdf_dict_put_array(ctx, aDict, PDF_NAME(D), 2);
            pdf_array_push(ctx, dest, pageObj);
            pdf_array_push(ctx, dest, PDF_NAME(Fit));
            break;
        }
        case LinkActionType::Named: {
            pdf_dict_put(ctx, aDict, PDF_NAME(S), PDF_NAME(Named));
            pdf_dict_put_name(ctx, aDict, PDF_NAME(N),
                              action.namedAction.c_str());
            break;
        }
        case LinkActionType::Launch: {
            // Launch action: PDF spec /S /Launch + /F filespec. The /F
            // can be either a plain string (legacy) or a filespec dict
            // with /F + /UF for Unicode. Use the dict form for parity
            // with how modern Acrobat writes these.
            pdf_dict_put(ctx, aDict, PDF_NAME(S), PDF_NAME(Launch));
            pdf_obj* fs = pdf_dict_put_dict(ctx, aDict, PDF_NAME(F), 3);
            pdf_dict_put(ctx, fs, PDF_NAME(Type), PDF_NAME(Filespec));
            pdf_dict_put_text_string(ctx, fs, PDF_NAME(F),
                                     action.launchPath.c_str());
            pdf_dict_put_text_string(ctx, fs, PDF_NAME(UF),
                                     action.launchPath.c_str());
            break;
        }
        }
        pdf_update_annot(ctx, annot);
    });
}

LinkAction AnnotationManager::getLinkAction(
    const pdfe::core::Page* page,
    int annotIndex
) {
    if (!page) throw std::invalid_argument("Page cannot be null");
    pdf_page* pdfPage = pImpl->getPdfPage(page);

    return pImpl->context->executeReturn<LinkAction>(
        [&](fz_context* ctx) -> LinkAction {
            LinkAction result{};  // default: Uri, empty
            // Try iterator first (covers in-session links + all
            // non-Link annotations); fall back to extended /Annots
            // walk for links-on-reopen which pdf_first_annot skips.
            pdf_obj* obj = getNthAnnotObj(ctx, pdfPage, annotIndex);
            if (!obj) return result;
            pdf_obj* aDict = pdf_dict_get(ctx, obj, PDF_NAME(A));
            if (!aDict) return result;
            pdf_obj* subtype = pdf_dict_get(ctx, aDict, PDF_NAME(S));
            if (pdf_name_eq(ctx, subtype, PDF_NAME(URI))) {
                result.type = LinkActionType::Uri;
                pdf_obj* uriObj = pdf_dict_get(ctx, aDict, PDF_NAME(URI));
                if (uriObj) {
                    const char* s = pdf_to_text_string(ctx, uriObj);
                    if (s) result.uri = s;
                }
            } else if (pdf_name_eq(ctx, subtype, PDF_NAME(GoTo))) {
                result.type = LinkActionType::GoTo;
                pdf_obj* dest = pdf_dict_get(ctx, aDict, PDF_NAME(D));
                if (dest && pdf_is_array(ctx, dest) && pdf_array_len(ctx, dest) > 0) {
                    pdf_obj* pageRef = pdf_array_get(ctx, dest, 0);
                    pdf_document* doc = pdf_get_bound_document(ctx, obj);
                    if (doc) {
                        result.pageIndex = pdf_lookup_page_number(
                            ctx, doc, pageRef);
                    }
                }
            } else if (pdf_name_eq(ctx, subtype, PDF_NAME(Named))) {
                result.type = LinkActionType::Named;
                pdf_obj* n = pdf_dict_get(ctx, aDict, PDF_NAME(N));
                if (n) {
                    const char* s = pdf_to_name(ctx, n);
                    if (s) result.namedAction = s;
                }
            } else if (pdf_name_eq(ctx, subtype, PDF_NAME(Launch))) {
                result.type = LinkActionType::Launch;
                // /F can be a string or a filespec dict; prefer /UF
                // from the dict form, fall back to /F string.
                pdf_obj* fEntry = pdf_dict_get(ctx, aDict, PDF_NAME(F));
                if (fEntry) {
                    if (pdf_is_dict(ctx, fEntry)) {
                        pdf_obj* uf = pdf_dict_get(ctx, fEntry, PDF_NAME(UF));
                        pdf_obj* f  = pdf_dict_get(ctx, fEntry, PDF_NAME(F));
                        const char* s = uf ? pdf_to_text_string(ctx, uf)
                                           : (f ? pdf_to_text_string(ctx, f) : nullptr);
                        if (s) result.launchPath = s;
                    } else if (pdf_is_string(ctx, fEntry)) {
                        const char* s = pdf_to_text_string(ctx, fEntry);
                        if (s) result.launchPath = s;
                    }
                }
            }
            return result;
        });
}

std::vector<AnnotationData> AnnotationManager::getAnnotations(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();
    std::vector<AnnotationData> annotations;

    pImpl->context->execute([&](fz_context* ctx) {
        std::vector<int> seenObjNums;
        pdf_annot* annot = pdf_first_annot(ctx, pdfPage);
        int idx = 0;
        while (annot) {
            AnnotationData data = readAnnotationData(ctx, annot, pageIndex, idx);
            annotations.push_back(data);
            pdf_obj* ao = pdf_annot_obj(ctx, annot);
            if (ao) seenObjNums.push_back(pdf_to_num(ctx, ao));
            annot = pdf_next_annot(ctx, annot);
            idx++;
        }

        // MuPDF's pdf_first_annot iterator historically skips /Link
        // annotations after a fresh document load — they're kept in
        // the raw /Annots array but not promoted into page->annots
        // unless a pdf_annot is explicitly created for them in-session.
        // Walk /Annots directly to pick up any /Link entries we would
        // otherwise miss, so callers can query and edit links on
        // reopened documents.
        pdf_obj* pageObj = pdfPage->obj;
        if (pageObj) {
            pdf_obj* annotsArr = pdf_dict_get(ctx, pageObj, PDF_NAME(Annots));
            if (annotsArr && pdf_is_array(ctx, annotsArr)) {
                int n = pdf_array_len(ctx, annotsArr);
                for (int i = 0; i < n; ++i) {
                    pdf_obj* entry = pdf_array_get(ctx, annotsArr, i);
                    if (!entry) continue;
                    int num = pdf_to_num(ctx, entry);
                    if (std::find(seenObjNums.begin(), seenObjNums.end(), num)
                        != seenObjNums.end()) {
                        continue;
                    }
                    pdf_obj* subtype = pdf_dict_get(ctx, entry, PDF_NAME(Subtype));
                    if (!pdf_name_eq(ctx, subtype, PDF_NAME(Link))) continue;

                    AnnotationData data;
                    data.pageIndex = pageIndex;
                    data.annotIndex = idx++;
                    data.type = AnnotationType::Link;
                    pdf_obj* rectObj = pdf_dict_get(ctx, entry, PDF_NAME(Rect));
                    if (rectObj && pdf_is_array(ctx, rectObj) &&
                        pdf_array_len(ctx, rectObj) == 4) {
                        float r[4];
                        for (int k = 0; k < 4; ++k) {
                            r[k] = pdf_to_real(ctx,
                                pdf_array_get(ctx, rectObj, k));
                        }
                        // /Rect is in PDF Y-up convention (origin
                        // bottom-left). Other AnnotationData.rect
                        // values come from pdf_bound_annot which is
                        // already converted to MuPDF Y-down. Convert
                        // y via page height so both match.
                        pdf_obj* mbox = pdf_dict_get_inheritable(
                            ctx, pageObj, PDF_NAME(MediaBox));
                        float pageH = 792.0f;
                        if (mbox && pdf_is_array(ctx, mbox) &&
                            pdf_array_len(ctx, mbox) >= 4) {
                            float my0 = pdf_to_real(ctx,
                                pdf_array_get(ctx, mbox, 1));
                            float my1 = pdf_to_real(ctx,
                                pdf_array_get(ctx, mbox, 3));
                            pageH = my1 - my0;
                        }
                        const float y0d = pageH - r[3];
                        const float y1d = pageH - r[1];
                        data.rect = QRectF(QPointF(r[0], y0d),
                                           QPointF(r[2], y1d)).normalized();
                    }
                    pdf_obj* contentsObj = pdf_dict_get(ctx, entry, PDF_NAME(Contents));
                    if (contentsObj) {
                        const char* c = pdf_to_text_string(ctx, contentsObj);
                        if (c) data.contents = c;
                    }
                    annotations.push_back(data);
                }
            }
        }
    });

    return annotations;
}

AnnotationData AnnotationManager::getAnnotation(const pdfe::core::Page* page, int annotIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();
    AnnotationData data;

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        data = readAnnotationData(ctx, annot, pageIndex, annotIndex);
    });

    return data;
}

// ---------- Pipeline E phase-5: /FileAttachment annotations ----------

namespace {

// Read a file from disk into a fz_buffer. Returns nullptr on failure.
fz_buffer* readFileToFzBuffer(fz_context* ctx,
                              const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return nullptr;
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0) return nullptr;
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;
    fz_buffer* buf = fz_new_buffer(ctx, static_cast<size_t>(size));
    fz_append_data(ctx, buf, bytes.data(), bytes.size());
    return buf;
}

} // namespace

AnnotationData AnnotationManager::createFileAttachmentAnnotation(
    const pdfe::core::Page* page,
    QRectF rect,
    const std::filesystem::path& filePath,
    const std::string& iconName,
    const std::string& displayName
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }
    if (!std::filesystem::exists(filePath)) {
        throw std::invalid_argument(
            "File attachment source does not exist: " + filePath.string());
    }

    // Validate icon name: allow only the four standard /FileAttachment
    // icons. Default to PushPin for anything else (matches Acrobat's
    // fallback behavior).
    std::string icon = iconName;
    if (icon != "PaperClip" && icon != "PushPin" &&
        icon != "Graph" && icon != "Tag") {
        icon = "PushPin";
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();
    int newAnnotIndex = 0;

    // Filename to record in the filespec /UF (and used by extract).
    const std::string baseName = displayName.empty()
        ? filePath.filename().string()
        : displayName;
    const std::string sanitized =
        pdfe::attachments::AttachmentManager::sanitizeFilename(baseName);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_get_bound_document(ctx, pdfPage->obj);
        if (!pdoc) {
            fz_throw(ctx, FZ_ERROR_GENERIC,
                     "Could not resolve PDF document from page");
        }

        // 1. Read the file bytes into a fz_buffer.
        fz_buffer* contents = readFileToFzBuffer(ctx, filePath);
        if (!contents) {
            fz_throw(ctx, FZ_ERROR_GENERIC,
                     "Failed to read attachment source file");
        }

        pdf_obj* filespec = nullptr;
        pdf_annot* annot = nullptr;
        fz_var(filespec);
        fz_var(annot);
        fz_try(ctx) {
            // 2. Build an embedded-file stream + filespec dict.
            filespec = pdf_add_embedded_file(ctx, pdoc,
                sanitized.c_str(),
                nullptr,         // mimetype unknown
                contents,
                -1, -1,          // created/modified unknown
                0);              // no checksum

            // 3. Create the annotation and configure its rect / icon
            //    / filespec.
            annot = pdf_create_annot(ctx, pdfPage,
                                     PDF_ANNOT_FILE_ATTACHMENT);

            fz_rect r = {
                static_cast<float>(rect.left()),
                static_cast<float>(rect.top()),
                static_cast<float>(rect.right()),
                static_cast<float>(rect.bottom())
            };
            pdf_set_annot_rect(ctx, annot, r);
            pdf_set_annot_icon_name(ctx, annot, icon.c_str());
            pdf_set_annot_filespec(ctx, annot, filespec);
            // /Contents is convention for file attachments to hold a
            // human-readable description; default to the file's name.
            pdf_set_annot_contents(ctx, annot, sanitized.c_str());
            pdf_update_annot(ctx, annot);
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, contents);
            // filespec ref is held by the annotation now via /FS;
            // pdf_add_embedded_file returns a borrowed indirect ref
            // (lives in /Catalog tree), so we do NOT drop it here.
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }

        // Locate the index of the freshly-created annotation.
        pdf_annot* current = pdf_first_annot(ctx, pdfPage);
        int index = 0;
        while (current) {
            if (current == annot) {
                newAnnotIndex = index;
                break;
            }
            current = pdf_next_annot(ctx, current);
            index++;
        }
    });

    return getAnnotation(page, newAnnotIndex);
}

std::vector<unsigned char> AnnotationManager::readFileAttachmentBytes(
    const pdfe::core::Page* page,
    int annotIndex
) {
    std::vector<unsigned char> out;
    if (!page) return out;
    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) return;
        if (pdf_annot_type(ctx, annot) != PDF_ANNOT_FILE_ATTACHMENT) return;
        pdf_obj* fs = pdf_annot_filespec(ctx, annot);
        if (!fs) return;
        if (!pdf_is_embedded_file(ctx, fs)) return;

        fz_buffer* buf = pdf_load_embedded_file_contents(ctx, fs);
        if (!buf) return;

        // Same shape as extractFileAttachmentPayload() below: buf was dropped
        // only on the fall-through path, so a throw out of fz_buffer_storage
        // leaked the whole decompressed attachment. Found by the phase-68
        // security-audit gate as a sibling of the fix directly below it --
        // this project's recurring miss is the instance one function over from
        // the one being repaired.
        //
        // out.assign() can also throw std::bad_alloc, which would be a C++
        // exception unwinding through MuPdfContext::execute's fz_try frame.
        // It is inside the guarded block so the buffer is still released, and
        // `out` itself lives in the caller's frame.
        fz_var(buf);
        fz_try(ctx) {
            unsigned char* data = nullptr;
            size_t n = fz_buffer_storage(ctx, buf, &data);
            out.assign(data, data + n);
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
    return out;
}

std::filesystem::path AnnotationManager::extractFileAttachmentPayload(
    const pdfe::core::Page* page,
    int annotIndex,
    const std::filesystem::path& outDir
) {
    if (!page) return {};
    std::filesystem::create_directories(outDir);

    std::filesystem::path written;
    pdf_page* pdfPage = pImpl->getPdfPage(page);

    // Every owning local this operation needs is constructed HERE, in the
    // caller's frame, and captured by reference. The lambda body below runs
    // inside MuPdfContext::execute's fz_try, which is setjmp/longjmp: any of
    // the pdf_* calls can longjmp straight out, and a longjmp does not run
    // destructors for objects declared inside the guarded block. Declared out
    // here they are destroyed normally whichever way execute() returns.
    std::string fileName;
    std::string safe;
    std::filesystem::path target;
    std::ofstream ofs;

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) return;
        if (pdf_annot_type(ctx, annot) != PDF_ANNOT_FILE_ATTACHMENT) return;
        pdf_obj* fs = pdf_annot_filespec(ctx, annot);
        if (!fs) return;
        if (!pdf_is_embedded_file(ctx, fs)) return;

        // Determine display name from the filespec params; fall back
        // to /Contents and finally to a generic name.
        pdf_filespec_params params{};
        pdf_get_filespec_params(ctx, fs, &params);
        fileName = params.filename
            ? std::string(params.filename) : std::string();
        if (fileName.empty()) {
            const char* contents = pdf_annot_contents(ctx, annot);
            if (contents) fileName = contents;
        }
        if (fileName.empty()) fileName = "attachment.bin";
        safe = pdfe::attachments::AttachmentManager::sanitizeFilename(fileName);

        fz_buffer* buf = pdf_load_embedded_file_contents(ctx, fs);
        if (!buf) return;

        // buf was dropped only on the fall-through path, so a throw out of
        // fz_buffer_storage leaked the whole decompressed attachment.
        //
        // fz_var is not strictly required -- buf is assigned before fz_try and
        // never reassigned inside it -- but MuPDF's rule is that any local read
        // from fz_always is fz_var'd, and a later edit that moved the
        // assignment inside the block would otherwise introduce a silent,
        // optimisation-dependent bug.
        fz_var(buf);
        fz_try(ctx) {
            target = outDir / safe;
            unsigned char* data = nullptr;
            size_t n = fz_buffer_storage(ctx, buf, &data);
            ofs.open(target, std::ios::binary);
            if (ofs) {
                ofs.write(reinterpret_cast<const char*>(data),
                          static_cast<std::streamsize>(n));
                ofs.close();
                if (ofs.good()) written = target;
            }
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
    return written;
}

void AnnotationManager::setStampImageAppearance(
    const pdfe::core::Page* page,
    int annotIndex,
    const QImage& imageIn
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }
    if (imageIn.isNull()) {
        throw std::invalid_argument("Image is null");
    }

    QImage img = imageIn;
    if (img.format() != QImage::Format_RGB32 && img.format() != QImage::Format_ARGB32) {
        img = img.convertToFormat(
            img.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    }
    const int width = img.width();
    const int height = img.height();
    const bool hasAlpha = img.hasAlphaChannel();

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        pdf_obj* annotObj = pdf_annot_obj(ctx, annot);
        pdf_document* pdfDoc = pdf_get_bound_document(ctx, annotObj);

        // Use annotation rect as appearance bbox extent (in points).
        fz_rect annotRect = pdf_annot_rect(ctx, annot);
        const float w_pt = annotRect.x1 - annotRect.x0;
        const float h_pt = annotRect.y1 - annotRect.y0;
        if (w_pt <= 0.0f || h_pt <= 0.0f) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation rect has zero size");
        }

        // Build fz_pixmap from QImage (RGB or RGBA) -- same pattern as
        // ImageManager::insertImage.
        const int n = hasAlpha ? 4 : 3;
        fz_colorspace* cs = fz_device_rgb(ctx);
        fz_pixmap* pix = fz_new_pixmap(ctx, cs, width, height, nullptr,
                                       hasAlpha ? 1 : 0);
        pix->xres = 72;
        pix->yres = 72;

        unsigned char* samples = pix->samples;
        for (int y = 0; y < height; ++y) {
            const uchar* scanline = img.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                std::uint32_t pixel;
                std::memcpy(&pixel, scanline + x * 4, 4);
                const std::uint8_t b = pixel & 0xFF;
                const std::uint8_t g = (pixel >> 8) & 0xFF;
                const std::uint8_t r = (pixel >> 16) & 0xFF;
                const std::uint8_t a = (pixel >> 24) & 0xFF;
                const int idx = (y * width + x) * n;
                samples[idx]     = r;
                samples[idx + 1] = g;
                samples[idx + 2] = b;
                if (hasAlpha) samples[idx + 3] = a;
            }
        }

        fz_image* fzImage = nullptr;
        pdf_obj* imageObj = nullptr;
        pdf_obj* xobjDict = nullptr;
        pdf_obj* resources = nullptr;
        fz_buffer* contents = nullptr;

        fz_var(fzImage);
        fz_var(imageObj);
        fz_var(xobjDict);
        fz_var(resources);
        fz_var(contents);

        fz_try(ctx) {
            fzImage = fz_new_image_from_pixmap(ctx, pix, nullptr);
            // pixmap ref now owned by fzImage; drop ours.
            fz_drop_pixmap(ctx, pix);
            pix = nullptr;

            imageObj = pdf_add_image(ctx, pdfDoc, fzImage);

            // Build resources bottom-up using *_drop variants so each
            // child ref is transferred to its parent atomically. Once
            // a pointer is transferred we set it to nullptr so the
            // fz_always cleanup path is unambiguous: any non-null
            // pointer there represents a ref we still own.
            xobjDict = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_dict_puts_drop(ctx, xobjDict, "Im0", imageObj);
            imageObj = nullptr;

            resources = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_dict_put_drop(ctx, resources, PDF_NAME(XObject), xobjDict);
            xobjDict = nullptr;

            // Build a content stream that scales the unit-image to
            // the annotation rect's points-space dimensions. The
            // Form XObject's bbox is (0,0,w_pt,h_pt) and Matrix
            // translates to the annotation origin.
            char ops[128];
            std::snprintf(ops, sizeof(ops),
                "q %g 0 0 %g 0 0 cm /Im0 Do Q\n",
                static_cast<double>(w_pt),
                static_cast<double>(h_pt));
            contents = fz_new_buffer_from_copied_data(
                ctx,
                reinterpret_cast<const unsigned char*>(ops),
                std::strlen(ops));

            fz_rect bbox = {0.0f, 0.0f, w_pt, h_pt};
            fz_matrix matrix = fz_make_matrix(
                1, 0, 0, 1, annotRect.x0, annotRect.y0);

            pdf_set_annot_appearance(ctx, annot, "N", nullptr,
                                     matrix, bbox, resources, contents);

            pdf_update_annot(ctx, annot);
        }
        fz_always(ctx) {
            if (pix) fz_drop_pixmap(ctx, pix);
            if (fzImage) fz_drop_image(ctx, fzImage);
            if (contents) fz_drop_buffer(ctx, contents);
            if (resources) pdf_drop_obj(ctx, resources);
            if (xobjDict) pdf_drop_obj(ctx, xobjDict);
            if (imageObj) pdf_drop_obj(ctx, imageObj);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

std::optional<int> AnnotationManager::getAnnotationAt(
    const pdfe::core::Page* page,
    QPointF pdfPoint
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    std::optional<int> result;

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = pdf_first_annot(ctx, pdfPage);
        int idx = 0;
        while (annot) {
            fz_rect r = pdf_bound_annot(ctx, annot);
            QRectF rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
            if (rect.contains(pdfPoint)) {
                result = idx;
                return; // Found it
            }
            annot = pdf_next_annot(ctx, annot);
            idx++;
        }
    });

    return result;
}

} // namespace pdfe::annotations
