// -*- coding: utf-8 -*-
// Pipeline image-placement-state, phase 5: the rules decided for redacting
// images and for flattening (docs/image-placement-model.md, "Redaction and
// flatten"). The phase-1 probes are in tests/test_redaction_image_state.cpp;
// these tests pin the rest of the decided behaviour.
//
//   RedactionImageModel.*  (GitHub #440, #441)
//     - Pixels mode maps the mark through the image's FULL placement matrix:
//       a quarter-turned, a flipped, a cropped (PDFE unit-square clip inside
//       the cm) and a clipped (page-space clip) image each lose exactly the
//       pixels under the mark, in the rendered page AND in the stored image
//       data, and keep the rest. The oracle maps every stored pixel to the
//       page through PageImageInfo::pageCtm (the placement model) and checks
//       it against the mark.
//     - An oblique (30 degree) placement loses at least the pixels under the
//       mark; MuPDF blanks the mark's bounding box in the image's own axes,
//       so a few more go (the decided, conservative limit).
//     - A mark covering the whole image removes it.
//     - An image drawn twice: only the marked placement changes. An image
//       shared with another page, and a Form XObject shared with another
//       page: the other page keeps the original.
//     - A soft mask is blanked under the mark too; JPX and JBIG2 images are
//       pixel-redacted, and with their decoders off the original coded data
//       still never survives.
//     - Every per-mark image method is honoured end to end (mark -> apply),
//       read back by the getters, and a method found in a FILE is ignored.
//   FlattenImageModel.*  (GitHub #442, #437)
//     - flattenForm(page) leaves the other pages' objects byte for byte,
//       copies a shared /Resources before adding to it, bakes the widget's
//       appearance, and removes the AcroForm only with its last field.
//     - A flattened image stamp is enumerated and selectable but NOT editable
//       (it is drawn inside a Form XObject: the phase-2 rule).
#include <gtest/gtest.h>
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "image/ImageContentEditor.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionData.h"
#include "forms/FormManager.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/JpxSecurityGate.h"
#include "core/Jbig2SecurityGate.h"
#include <QImage>
#include <QRectF>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;
using pdfe::redaction::RedactionApplyOptions;
using pdfe::redaction::RedactionData;
using pdfe::redaction::RedactionImageMethod;
using pdfe::redaction::RedactionManager;

namespace {

constexpr double kPageW = 612.0;
constexpr double kPageH = 792.0;
constexpr int kImgW = 64;
constexpr int kImgH = 48;

using Bytes = std::string;

Bytes readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool isRedPx(QRgb c) { return qRed(c) > 200 && qGreen(c) < 90 && qBlue(c) < 90; }
bool isWhitePx(QRgb c) { return qRed(c) > 240 && qGreen(c) > 240 && qBlue(c) > 240; }

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}

// "a b c d e f cm" in PDF user space.
std::string cm(double a, double b, double c, double d, double e, double f) {
    return num(a) + " " + num(b) + " " + num(c) + " " + num(d) + " " + num(e) + " " + num(f) + " cm";
}

// A solid RGB image XObject; NEW ref.
pdf_obj* addSolidImage(fz_context* ctx, pdf_document* doc, int w, int h, int r, int g, int b) {
    fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), w, h, nullptr, 0);
    unsigned char* s = fz_pixmap_samples(ctx, pix);
    for (int i = 0; i < w * h; ++i) {
        s[3 * i] = static_cast<unsigned char>(r);
        s[3 * i + 1] = static_cast<unsigned char>(g);
        s[3 * i + 2] = static_cast<unsigned char>(b);
    }
    fz_image* im = fz_new_image_from_pixmap(ctx, pix, nullptr);
    fz_drop_pixmap(ctx, pix);
    pdf_obj* o = pdf_add_image(ctx, doc, im);
    fz_drop_image(ctx, im);
    return o;
}

// A solid gray image XObject (used as a soft mask); NEW ref.
pdf_obj* addGrayImage(fz_context* ctx, pdf_document* doc, int w, int h, int v) {
    fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_gray(ctx), w, h, nullptr, 0);
    std::memset(fz_pixmap_samples(ctx, pix), v, static_cast<size_t>(w) * h);
    fz_image* im = fz_new_image_from_pixmap(ctx, pix, nullptr);
    fz_drop_pixmap(ctx, pix);
    pdf_obj* o = pdf_add_image(ctx, doc, im);
    fz_drop_image(ctx, im);
    return o;
}

void putXObject(fz_context* ctx, pdf_obj* res, const char* name, pdf_obj* obj) {
    pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    if (!xo) xo = pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 4);
    pdf_dict_puts(ctx, xo, name, obj);
}

// A Form XObject with `body` and resources `res`; NEW ref.
pdf_obj* addForm(fz_context* ctx, pdf_document* doc, const char* body, pdf_obj* res) {
    fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(body), std::strlen(body));
    pdf_obj* form = pdf_add_stream(ctx, doc, b, nullptr, 0);
    fz_drop_buffer(ctx, b);
    pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
    pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
    pdf_dict_put_rect(ctx, form, PDF_NAME(BBox), fz_make_rect(0, 0, static_cast<float>(kPageW), static_cast<float>(kPageH)));
    pdf_dict_put(ctx, form, PDF_NAME(Resources), res);
    return form;
}

// Fills page `page`'s resource dictionary. Objects shared between pages live
// in the caller's captured variables.
using ResFill = std::function<void(fz_context*, pdf_document*, pdf_obj* res, int page)>;

// A PDF with one 612 x 792 page per entry of `contents`.
void buildPdf(MuPdfContext& mc, const std::string& path, const std::vector<std::string>& contents,
              const ResFill& fill) {
    mc.execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_create_document(ctx);
        for (size_t i = 0; i < contents.size(); ++i) {
            pdf_obj* res = pdf_new_dict(ctx, doc, 2);
            fill(ctx, doc, res, static_cast<int>(i));
            fz_buffer* b = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(contents[i].data()), contents[i].size());
            pdf_obj* page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, static_cast<float>(kPageW), static_cast<float>(kPageH)),
                                         0, res, b);
            pdf_insert_page(ctx, doc, -1, page);
            pdf_drop_obj(ctx, page);
            fz_drop_buffer(ctx, b);
            pdf_drop_obj(ctx, res);
        }
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, doc, path.c_str(), &wo);
        pdf_drop_document(ctx, doc);
    });
}

// The stored pixels of an image XObject, as RGB, plus its soft mask.
struct Pixels {
    bool ok = false;
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;     // w*h*3
    bool hasMask = false;
    int mw = 0, mh = 0;
    std::vector<unsigned char> mask;    // mw*mh
    QRgb at(int x, int y) const {
        const size_t i = (static_cast<size_t>(y) * w + x) * 3;
        return qRgb(rgb[i], rgb[i + 1], rgb[i + 2]);
    }
};

// Copies `obj`'s decoded samples into `out` (RGB, or gray for a mask).
void decodeInto(fz_context* ctx, pdf_document* doc, pdf_obj* obj, bool gray, int& w, int& h,
                std::vector<unsigned char>& out) {
    fz_image* im = nullptr;
    fz_pixmap* pm = nullptr;
    fz_pixmap* conv = nullptr;
    fz_var(im);
    fz_var(pm);
    fz_var(conv);
    fz_try(ctx) {
        im = pdf_load_image(ctx, doc, obj);
        pm = fz_get_pixmap_from_image(ctx, im, nullptr, nullptr, nullptr, nullptr);
        fz_colorspace* want = gray ? fz_device_gray(ctx) : fz_device_rgb(ctx);
        fz_pixmap* use = pm;
        if (pm->colorspace != want || pm->alpha) {
            conv = fz_convert_pixmap(ctx, pm, want, nullptr, nullptr, fz_default_color_params, 0);
            use = conv;
        }
        w = use->w;
        h = use->h;
        const int n = gray ? 1 : 3;
        out.resize(static_cast<size_t>(w) * h * n);
        for (int y = 0; y < h; ++y)
            std::memcpy(out.data() + static_cast<size_t>(y) * w * n, use->samples + static_cast<size_t>(y) * use->stride,
                        static_cast<size_t>(w) * n);
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, conv);
        fz_drop_pixmap(ctx, pm);
        fz_drop_image(ctx, im);
    }
    fz_catch(ctx) { fz_rethrow(ctx); }
}

// The image named `name` in `res`, or in the resources of a form in it.
pdf_obj* findImage(fz_context* ctx, pdf_obj* res, const char* name, int depth) {
    pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    pdf_obj* hit = pdf_dict_gets(ctx, xo, name);
    if (hit && pdf_name_eq(ctx, pdf_dict_get(ctx, hit, PDF_NAME(Subtype)), PDF_NAME(Image))) return hit;
    if (depth > 4) return nullptr;
    for (int i = 0; i < pdf_dict_len(ctx, xo); ++i) {
        pdf_obj* v = pdf_dict_get_val(ctx, xo, i);
        if (pdf_name_eq(ctx, pdf_dict_get(ctx, v, PDF_NAME(Subtype)), PDF_NAME(Form)))
            if (pdf_obj* f = findImage(ctx, pdf_dict_get(ctx, v, PDF_NAME(Resources)), name, depth + 1)) return f;
    }
    return nullptr;
}

// Reads the image `name` drawn on page `pageIndex` of the saved file `path`.
Pixels readImage(MuPdfContext& mc, const std::string& path, int pageIndex, const std::string& name) {
    Pixels p;
    mc.execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, path.c_str());
        fz_try(ctx) {
            pdf_obj* page = pdf_lookup_page_obj(ctx, doc, pageIndex);
            pdf_obj* img = findImage(ctx, pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources)), name.c_str(), 0);
            if (img) {
                decodeInto(ctx, doc, img, false, p.w, p.h, p.rgb);
                pdf_obj* sm = pdf_dict_get(ctx, img, PDF_NAME(SMask));
                if (pdf_is_stream(ctx, sm)) {
                    decodeInto(ctx, doc, sm, true, p.mw, p.mh, p.mask);
                    p.hasMask = true;
                }
                p.ok = true;
            }
        }
        fz_always(ctx) { pdf_drop_document(ctx, doc); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return p;
}

// Image XObjects anywhere in the saved file that are still entirely red: an
// untouched copy of a solid red original.
int untouchedRedImages(MuPdfContext& mc, const std::string& path) {
    int count = 0;
    std::vector<unsigned char> rgb;
    mc.execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, path.c_str());
        fz_try(ctx) {
            const int n = pdf_xref_len(ctx, doc);
            for (int i = 1; i < n; ++i) {
                pdf_obj* o = pdf_new_indirect(ctx, doc, i, 0);
                fz_try(ctx) {
                    if (pdf_is_stream(ctx, o) &&
                        pdf_name_eq(ctx, pdf_dict_get(ctx, o, PDF_NAME(Subtype)), PDF_NAME(Image))) {
                        int w = 0, h = 0;
                        decodeInto(ctx, doc, o, false, w, h, rgb);
                        bool allRed = w > 0;
                        for (size_t k = 0; allRed && k + 2 < rgb.size(); k += 3)
                            allRed = rgb[k] > 200 && rgb[k + 1] < 90 && rgb[k + 2] < 90;
                        if (allRed) ++count;
                    }
                }
                fz_always(ctx) { pdf_drop_obj(ctx, o); }
                fz_catch(ctx) { fz_report_error(ctx); }
            }
        }
        fz_always(ctx) { pdf_drop_document(ctx, doc); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return count;
}

// Image XObjects anywhere in the saved file whose stored or filter-decoded
// data still contains `needle`. Unlike a search of the file's bytes, this also
// finds a copy that was re-compressed on save.
int imagesHoldingData(MuPdfContext& mc, const std::string& path, const std::string& needle) {
    int count = 0;
    mc.execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, path.c_str());
        fz_try(ctx) {
            const int n = pdf_xref_len(ctx, doc);
            for (int i = 1; i < n; ++i) {
                pdf_obj* o = pdf_new_indirect(ctx, doc, i, 0);
                fz_try(ctx) {
                    if (pdf_is_stream(ctx, o) &&
                        pdf_name_eq(ctx, pdf_dict_get(ctx, o, PDF_NAME(Subtype)), PDF_NAME(Image))) {
                        bool found = false;
                        for (int decoded = 0; decoded < 2 && !found; ++decoded) {
                            fz_buffer* b = nullptr;
                            fz_var(b);
                            fz_try(ctx) {
                                b = decoded ? pdf_load_stream(ctx, o) : pdf_load_raw_stream(ctx, o);
                                unsigned char* data = nullptr;
                                const size_t len = fz_buffer_storage(ctx, b, &data);
                                found = std::string(reinterpret_cast<const char*>(data), len).find(needle) !=
                                        std::string::npos;
                            }
                            fz_always(ctx) { fz_drop_buffer(ctx, b); }
                            fz_catch(ctx) { fz_report_error(ctx); }  // e.g. undecodable DCT
                        }
                        if (found) ++count;
                    }
                }
                fz_always(ctx) { pdf_drop_obj(ctx, o); }
                fz_catch(ctx) { fz_report_error(ctx); }
            }
        }
        fz_always(ctx) { pdf_drop_document(ctx, doc); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return count;
}

// A renumbering-proof text form of `o`: references resolved (to depth 6),
// stream data appended, /P and /Parent (back links to the page) skipped.
void canon(fz_context* ctx, pdf_obj* o, int depth, std::string& out) {
    o = pdf_resolve_indirect(ctx, o);
    char b[64];
    if (depth > 6) { out.append("..."); return; }
    if (!o || pdf_is_null(ctx, o)) { out.append("null "); return; }
    if (pdf_is_name(ctx, o)) { out.append("/").append(pdf_to_name(ctx, o)).append(" "); return; }
    if (pdf_is_int(ctx, o)) { std::snprintf(b, sizeof b, "%lld ", static_cast<long long>(pdf_to_int64(ctx, o))); out.append(b); return; }
    if (pdf_is_real(ctx, o)) { std::snprintf(b, sizeof b, "%.4f ", pdf_to_real(ctx, o)); out.append(b); return; }
    if (pdf_is_bool(ctx, o)) { out.append(pdf_to_bool(ctx, o) ? "true " : "false "); return; }
    if (pdf_is_string(ctx, o)) { out.append("(").append(pdf_to_str_buf(ctx, o), pdf_to_str_len(ctx, o)).append(") "); return; }
    if (pdf_is_array(ctx, o)) {
        out.append("[ ");
        for (int i = 0; i < pdf_array_len(ctx, o); ++i) canon(ctx, pdf_array_get(ctx, o, i), depth + 1, out);
        out.append("] ");
        return;
    }
    if (pdf_is_dict(ctx, o)) {
        out.append("<< ");
        for (int i = 0; i < pdf_dict_len(ctx, o); ++i) {
            pdf_obj* k = pdf_dict_get_key(ctx, o, i);
            // Back links, and the encoding of a stream (its DECODED data is
            // compared below; a save may re-encode it).
            if (pdf_name_eq(ctx, k, PDF_NAME(P)) || pdf_name_eq(ctx, k, PDF_NAME(Parent)) ||
                pdf_name_eq(ctx, k, PDF_NAME(Length)) || pdf_name_eq(ctx, k, PDF_NAME(Filter)) ||
                pdf_name_eq(ctx, k, PDF_NAME(DecodeParms)))
                continue;
            canon(ctx, k, depth + 1, out);
            canon(ctx, pdf_dict_get_val(ctx, o, i), depth + 1, out);
        }
        out.append(">> ");
        if (pdf_is_stream(ctx, o)) {
            fz_buffer* data = pdf_load_stream(ctx, o);
            unsigned char* d = nullptr;
            const size_t n = fz_buffer_storage(ctx, data, &d);
            out.append("stream ").append(reinterpret_cast<const char*>(d), n).append(" endstream ");
            fz_drop_buffer(ctx, data);
        }
    }
}

// Page `i` of `file`: its annotations, resources and content (canon).
std::string pageBytes(MuPdfContext& mc, const std::string& file, int i) {
    std::string out;
    mc.execute([&](fz_context* ctx) {
        pdf_document* d = pdf_open_document(ctx, file.c_str());
        fz_try(ctx) {
            pdf_obj* p = pdf_lookup_page_obj(ctx, d, i);
            for (pdf_obj* key : {PDF_NAME(Annots), PDF_NAME(Resources), PDF_NAME(Contents)})
                canon(ctx, pdf_dict_get_inheritable(ctx, p, key), 0, out);
        }
        fz_always(ctx) { pdf_drop_document(ctx, d); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return out;
}

// A unit-square point through a row-vector matrix [a b c d e f].
QPointF mapUnit(const std::array<float, 6>& m, double u, double v) {
    return QPointF(u * m[0] + v * m[2] + m[4], u * m[1] + v * m[3] + m[5]);
}

// Page point -> unit square, through the inverse of `m`.
bool unmapUnit(const std::array<float, 6>& m, QPointF p, double& u, double& v) {
    const double det = static_cast<double>(m[0]) * m[3] - static_cast<double>(m[1]) * m[2];
    if (std::fabs(det) < 1e-9) return false;
    const double x = p.x() - m[4], y = p.y() - m[5];
    u = (x * m[3] - y * m[2]) / det;
    v = (y * m[0] - x * m[1]) / det;
    return true;
}

// The stored pixels of `px`, placed on the page by `info` (the placement
// model), checked against the mark. A pixel is "under" when its centre is
// inside the mark shrunk by `margin`, "outside" when it is outside the mark
// grown by `margin` (margin: one and a half pixels on the page).
struct PixelVerdict {
    int under = 0, underBlank = 0;
    int outside = 0, outsideKept = 0;
};

PixelVerdict judgePixels(const Pixels& px, const PageImageInfo& info, const QRectF& mark) {
    PixelVerdict v;
    const auto& m = info.pageCtm;
    const double pxSize = std::max(std::hypot(m[0], m[1]) / px.w, std::hypot(m[2], m[3]) / px.h);
    const double margin = 1.5 * pxSize;
    const QRectF in = mark.adjusted(margin, margin, -margin, -margin);
    const QRectF out = mark.adjusted(-margin, -margin, margin, margin);
    for (int y = 0; y < px.h; ++y) {
        for (int x = 0; x < px.w; ++x) {
            const QPointF p = mapUnit(m, (x + 0.5) / px.w, 1.0 - (y + 0.5) / px.h);
            const QRgb c = px.at(x, y);
            if (in.contains(p)) {
                ++v.under;
                if (isWhitePx(c)) ++v.underBlank;
            } else if (!out.contains(p)) {
                ++v.outside;
                if (isRedPx(c)) ++v.outsideKept;
            }
        }
    }
    return v;
}

class RedactionImageModelBase : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tmp_.isValid());
        ctx_ = std::make_shared<MuPdfContext>();
        images_ = std::make_unique<ImageManager>(ctx_);
        redact_ = std::make_unique<RedactionManager>(ctx_);
    }

    std::string path(const char* name) const { return tmp_.filePath(name).toStdString(); }

    // Builds `in`, opens it, marks `marks` on page 0 (fitz space), applies
    // with `options` the way the UI does (no external rewrite), saves `out`
    // and reopens it as saved_.
    void redact(const std::vector<std::string>& contents, const ResFill& fill, const std::vector<QRectF>& marks,
                RedactionApplyOptions options = {}) {
        saved_.reset();
        in_ = path("in.pdf");
        out_ = path("out.pdf");
        buildPdf(*ctx_, in_, contents, fill);
        auto doc = Document::open(ctx_, in_);
        ASSERT_NE(doc, nullptr);
        {
            auto page = doc->page(0);
            for (const QRectF& r : marks) redact_->createRedactionMark(page.get(), r);
        }
        options.rewriteDocument = false;
        const auto r = redact_->applyAllRedactions(doc.get(), options);
        ASSERT_TRUE(r.success) << r.errorMessage;
        doc->saveAs(out_);
        doc.reset();
        saved_ = Document::open(ctx_, out_);
        ASSERT_NE(saved_, nullptr);
    }

    std::vector<PageImageInfo> imagesOn(int page) {
        auto p = saved_->page(page);
        return images_->enumerateImages(p.get(), saved_.get());
    }

    QImage render(int page) {
        auto p = saved_->page(page);
        return p->render(QRectF(), 1.0f);
    }

    static QRgb px(const QImage& img, QPointF p) {
        const int x = static_cast<int>(std::lround(p.x()));
        const int y = static_cast<int>(std::lround(p.y()));
        if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) return qRgb(0, 0, 255);
        return img.pixel(x, y);
    }

    // The rendered page against the placement `info` and the mark: every
    // sampled point that shows the image (inside the placement and inside
    // `visible`) is red when it is outside the mark (by 3 points) and not red
    // inside it (by 3 points). Both sets must be non-empty.
    void expectRenderMatches(const QImage& img, const PageImageInfo& info, const QRectF& mark,
                             const QRectF& visible = QRectF(0, 0, kPageW, kPageH)) {
        int inside = 0, insideOk = 0, outside = 0, outsideOk = 0;
        const QRectF in = mark.adjusted(3, 3, -3, -3), out = mark.adjusted(-3, -3, 3, 3);
        for (double y = 2; y < kPageH; y += 3) {
            for (double x = 2; x < kPageW; x += 3) {
                const QPointF p(x, y);
                double u = 0, v = 0;
                if (!unmapUnit(info.pageCtm, p, u, v)) continue;
                if (u < 0.03 || u > 0.97 || v < 0.03 || v > 0.97) continue;
                if (!visible.adjusted(3, 3, -3, -3).contains(p)) continue;
                if (in.contains(p)) {
                    ++inside;
                    if (!isRedPx(px(img, p))) ++insideOk;
                } else if (!out.contains(p)) {
                    ++outside;
                    if (isRedPx(px(img, p))) ++outsideOk;
                }
            }
        }
        EXPECT_GT(inside, 0) << "oracle: no sample under the mark";
        EXPECT_GT(outside, 0) << "oracle: no sample outside the mark";
        EXPECT_EQ(insideOk, inside) << "the image still shows under the mark";
        EXPECT_EQ(outsideOk, outside) << "the image was lost outside the mark";
    }

    // The stored image data of the single placement on page 0.
    void expectDataMatches(const PageImageInfo& info, const QRectF& mark) {
        const Pixels p = readImage(*ctx_, out_, 0, info.xobjectName);
        ASSERT_TRUE(p.ok) << "image " << info.xobjectName << " not found in the saved file";
        const PixelVerdict v = judgePixels(p, info, mark);
        EXPECT_GT(v.under, 0) << "oracle: no stored pixel under the mark";
        EXPECT_GT(v.outside, 0) << "oracle: no stored pixel outside the mark";
        EXPECT_EQ(v.underBlank, v.under) << "stored pixels under the mark were not blanked";
        EXPECT_EQ(v.outsideKept, v.outside) << "stored pixels outside the mark were lost";
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<ImageManager> images_;
    std::unique_ptr<RedactionManager> redact_;
    std::unique_ptr<Document> saved_;
    std::string in_, out_;
};

ResFill oneRedImage(const char* name = "Im0") {
    return [name](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) {
        pdf_obj* img = addSolidImage(ctx, doc, kImgW, kImgH, 255, 0, 0);
        putXObject(ctx, res, name, img);
        pdf_drop_obj(ctx, img);
    };
}

// fitz (Y-down) rectangle from PDF user-space edges.
QRectF fitzRect(double x0, double y0Pdf, double x1, double y1Pdf) {
    return QRectF(x0, kPageH - y1Pdf, x1 - x0, y1Pdf - y0Pdf);
}

} // namespace

class RedactionImageModel : public RedactionImageModelBase {};

// A mark covering the whole image removes it (Pixels mode keeps the old
// "covered means gone" rule).
TEST_F(RedactionImageModel, FullyCoveredImageIsRemoved) {
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    redact({body}, oneRedImage(), {fitzRect(90, 290, 310, 460)});
    EXPECT_TRUE(imagesOn(0).empty()) << "a fully covered image is still drawn";
    EXPECT_EQ(untouchedRedImages(*ctx_, out_), 0) << "the original image is still in the saved file";
    const QImage img = render(0);
    EXPECT_FALSE(isRedPx(px(img, QPointF(200, kPageH - 375))));
}

// A quarter-turned image: the mark over the top-left of its page extent is
// mapped into the image's own (rotated) axes.
TEST_F(RedactionImageModel, QuarterTurnedImageLosesExactlyThePixelsUnderTheMark) {
    // u -> up (0,200), v -> left (-150,0): covers x 200..350, y 300..500.
    const std::string body = "q " + cm(0, 200, -150, 0, 350, 300) + " /Im0 Do Q";
    const QRectF mark = fitzRect(200, 440, 260, 500);
    redact({body}, oneRedImage(), {mark});
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u) << "a partly covered image must remain";
    EXPECT_NEAR(std::fabs(infos[0].rotationRadians), M_PI / 2, 0.01) << "the placement lost its rotation";
    expectRenderMatches(render(0), infos[0], mark);
    expectDataMatches(infos[0], mark);
    EXPECT_EQ(untouchedRedImages(*ctx_, out_), 0) << "an unredacted copy of the image is still in the file";
}

// A mirrored image: the mark's side of the page is the image's OTHER side.
TEST_F(RedactionImageModel, FlippedImageLosesExactlyThePixelsUnderTheMark) {
    // Mirrored left-right: covers x 200..400, y 300..450.
    const std::string body = "q " + cm(-200, 0, 0, 150, 400, 300) + " /Im0 Do Q";
    const QRectF mark = fitzRect(200, 400, 260, 450);
    redact({body}, oneRedImage(), {mark});
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_TRUE(infos[0].flippedX || infos[0].flippedY) << "the placement lost its flip";
    expectRenderMatches(render(0), infos[0], mark);
    expectDataMatches(infos[0], mark);
    // The mark is at the page-left, which is the image's RIGHT edge: the
    // stored image's top-right corner is blanked, its top-left kept.
    const Pixels p = readImage(*ctx_, out_, 0, infos[0].xobjectName);
    ASSERT_TRUE(p.ok);
    EXPECT_TRUE(isWhitePx(p.at(p.w - 2, 1)));
    EXPECT_TRUE(isRedPx(p.at(1, 1)));
    EXPECT_EQ(untouchedRedImages(*ctx_, out_), 0);
}

// A PDFE crop (a unit-square clip inside the cm): the mark over the visible
// part blanks those pixels and the crop is kept.
TEST_F(RedactionImageModel, CroppedImageLosesThePixelsUnderTheMark) {
    // Full image x 100..500, y 200..500; visible u 0..0.5, v 0.5..1 ->
    // x 100..300, y 350..500.
    const std::string body = "q " + cm(400, 0, 0, 300, 100, 200) + " 0 0.5 0.5 0.5 re W n /Im0 Do Q";
    const QRectF mark = fitzRect(100, 440, 160, 500);
    const QRectF visible = fitzRect(100, 350, 300, 500);
    redact({body}, oneRedImage(), {mark});
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_TRUE(infos[0].cropped) << "the crop was lost";
    expectRenderMatches(render(0), infos[0], mark, visible);
    expectDataMatches(infos[0], mark);
    EXPECT_FALSE(isRedPx(px(render(0), QPointF(400, kPageH - 250)))) << "the crop no longer hides the rest";
    EXPECT_EQ(untouchedRedImages(*ctx_, out_), 0);
}

// A page-space clip before the cm: the mark over the visible part blanks
// those pixels.
TEST_F(RedactionImageModel, ClippedImageLosesThePixelsUnderTheMark) {
    const std::string body = "q 150 250 200 150 re W n " + cm(400, 0, 0, 300, 100, 200) + " /Im0 Do Q";
    const QRectF mark = fitzRect(140, 340, 200, 410);
    const QRectF visible = fitzRect(150, 250, 350, 400);
    redact({body}, oneRedImage(), {mark});
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    expectRenderMatches(render(0), infos[0], mark, visible);
    expectDataMatches(infos[0], mark);
    EXPECT_EQ(untouchedRedImages(*ctx_, out_), 0);
}

// An oblique placement (30 degrees): every pixel under the mark is blanked.
// MuPDF blanks the mark's bounding box in the image's own axes, so pixels
// outside that box are kept and a few pixels next to the mark go too (the
// decided, conservative limit: never fewer than the mark covers).
TEST_F(RedactionImageModel, ObliqueImageLosesAtLeastThePixelsUnderTheMark) {
    const double a = M_PI / 6, w = 240, h = 180;
    const std::string body = "q " + cm(w * std::cos(a), w * std::sin(a), -h * std::sin(a), h * std::cos(a), 250, 250) +
                             " /Im0 Do Q";
    redact({body}, oneRedImage(), {});   // no mark yet: find the placement first
    const auto before = imagesOn(0);
    ASSERT_EQ(before.size(), 1u);
    const QPointF c = mapUnit(before[0].pageCtm, 0.3, 0.6);
    const QRectF mark(c.x() - 25, c.y() - 20, 50, 40);
    redact({body}, oneRedImage(), {mark});
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    const Pixels p = readImage(*ctx_, out_, 0, infos[0].xobjectName);
    ASSERT_TRUE(p.ok);
    const PixelVerdict v = judgePixels(p, infos[0], mark);
    EXPECT_GT(v.under, 0);
    EXPECT_EQ(v.underBlank, v.under) << "a stored pixel under the mark survived";
    // The bounding box of the mark in pixel coordinates; outside it (plus a
    // pixel) every pixel is kept.
    double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (QPointF q : {mark.topLeft(), mark.topRight(), mark.bottomLeft(), mark.bottomRight()}) {
        double u = 0, vv = 0;
        ASSERT_TRUE(unmapUnit(infos[0].pageCtm, q, u, vv));
        minX = std::min(minX, u * p.w);
        maxX = std::max(maxX, u * p.w);
        minY = std::min(minY, (1 - vv) * p.h);
        maxY = std::max(maxY, (1 - vv) * p.h);
    }
    int far = 0, farKept = 0, blanked = 0;
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            if (isWhitePx(p.at(x, y))) ++blanked;
            if (x + 1 < minX - 1 || x > maxX + 1 || y + 1 < minY - 1 || y > maxY + 1) {
                ++far;
                if (isRedPx(p.at(x, y))) ++farKept;
            }
        }
    EXPECT_GT(far, 0);
    EXPECT_EQ(farKept, far) << "pixels outside the mark's box in the image's axes were lost";
    EXPECT_GE(blanked, v.under);
    EXPECT_EQ(untouchedRedImages(*ctx_, out_), 0);
}

// An image drawn twice: only the marked placement loses pixels; the other
// keeps showing the whole image (it is not under any mark, so the original
// pixels legitimately stay in the file for it).
TEST_F(RedactionImageModel, ImageDrawnTwiceOnlyTheMarkedPlacementChanges) {
    const std::string body = "q " + cm(200, 0, 0, 150, 50, 500) + " /Im0 Do Q q " + cm(200, 0, 0, 150, 350, 100) +
                             " /Im0 Do Q";
    const QRectF mark = fitzRect(50, 600, 110, 650);
    redact({body}, oneRedImage(), {mark});
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 2u) << "both placements must remain";
    const PageImageInfo& marked = infos[0].pageCtm[5] < infos[1].pageCtm[5] ? infos[0] : infos[1];
    const PageImageInfo& other = &marked == &infos[0] ? infos[1] : infos[0];
    expectRenderMatches(render(0), marked, mark);
    expectDataMatches(marked, mark);
    const Pixels o = readImage(*ctx_, out_, 0, other.xobjectName);
    ASSERT_TRUE(o.ok);
    int red = 0;
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) red += isRedPx(o.at(x, y)) ? 1 : 0;
    EXPECT_EQ(red, o.w * o.h) << "the unmarked placement was changed";
    EXPECT_TRUE(isRedPx(px(render(0), QPointF(450, kPageH - 175)))) << "the other placement no longer shows";
}

// An image shared with another page: page 1 keeps the original image.
TEST_F(RedactionImageModel, ImageSharedWithAnotherPageIsLeftOnTheOtherPage) {
    pdf_obj* shared = nullptr;
    const ResFill fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res, int page) {
        if (!shared) shared = addSolidImage(ctx, doc, kImgW, kImgH, 255, 0, 0);
        putXObject(ctx, res, "Im0", shared);
        if (page == 1) {
            pdf_drop_obj(ctx, shared);
            shared = nullptr;
        }
    };
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    const QRectF mark = fitzRect(100, 400, 160, 450);
    redact({body, body}, fill, {mark});
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    expectRenderMatches(render(0), infos[0], mark);
    expectDataMatches(infos[0], mark);
    const auto other = imagesOn(1);
    ASSERT_EQ(other.size(), 1u);
    const QImage p1 = render(1);
    EXPECT_TRUE(isRedPx(px(p1, QPointF(130, kPageH - 425)))) << "page 1 lost the pixels page 0's mark covered";
    const Pixels o = readImage(*ctx_, out_, 1, other[0].xobjectName);
    ASSERT_TRUE(o.ok);
    EXPECT_TRUE(isRedPx(o.at(1, 1))) << "page 1's image was changed by page 0's redaction";
}

// An image inside a Form XObject that page 1 also draws: page 0's copy of
// the form gets the redacted image; page 1 keeps the original form.
TEST_F(RedactionImageModel, ImageInsideASharedFormIsRedactedOnlyOnTheMarkedPage) {
    pdf_obj* form = nullptr;
    const ResFill fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res, int page) {
        if (!form) {
            pdf_obj* img = addSolidImage(ctx, doc, kImgW, kImgH, 255, 0, 0);
            pdf_obj* fres = pdf_new_dict(ctx, doc, 1);
            putXObject(ctx, fres, "Im0", img);
            const std::string fbody = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
            form = addForm(ctx, doc, fbody.c_str(), fres);
            pdf_drop_obj(ctx, fres);
            pdf_drop_obj(ctx, img);
        }
        putXObject(ctx, res, "Fm0", form);
        if (page == 1) {
            pdf_drop_obj(ctx, form);
            form = nullptr;
        }
    };
    const QRectF mark = fitzRect(100, 400, 160, 450);
    redact({"/Fm0 Do", "/Fm0 Do"}, fill, {mark});
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_TRUE(infos[0].inFormXObject);
    expectRenderMatches(render(0), infos[0], mark);
    expectDataMatches(infos[0], mark);
    const QImage p1 = render(1);
    EXPECT_TRUE(isRedPx(px(p1, QPointF(130, kPageH - 425)))) << "the shared form was redacted on page 1 too";
}

// A soft mask: its values under the mark are overwritten too (opaque), so the
// shape the mask held there is gone; outside the mark it is unchanged.
TEST_F(RedactionImageModel, SoftMaskIsBlankedUnderTheMarkToo) {
    const ResFill fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) {
        pdf_obj* img = addSolidImage(ctx, doc, kImgW, kImgH, 255, 0, 0);
        pdf_obj* sm = addGrayImage(ctx, doc, kImgW, kImgH, 200);
        pdf_dict_put(ctx, img, PDF_NAME(SMask), sm);
        putXObject(ctx, res, "Im0", img);
        pdf_drop_obj(ctx, sm);
        pdf_drop_obj(ctx, img);
    };
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    const QRectF mark = fitzRect(100, 400, 160, 450);
    redact({body}, fill, {mark});
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    expectDataMatches(infos[0], mark);
    const Pixels p = readImage(*ctx_, out_, 0, infos[0].xobjectName);
    ASSERT_TRUE(p.hasMask) << "the soft mask was dropped";
    ASSERT_EQ(p.mw, kImgW);
    // The mark covers the image's top-left 30% x 33%.
    EXPECT_EQ(p.mask[1 * p.mw + 1], 255) << "the soft mask under the mark still holds its values";
    EXPECT_EQ(p.mask[(p.mh - 2) * p.mw + (p.mw - 2)], 200) << "the soft mask outside the mark changed";
}

// A JPX image is decoded, blanked under the mark and stored again; the
// original JPEG 2000 data is not in the saved file.
TEST_F(RedactionImageModel, JpxImageIsPixelRedacted) {
    const bool was = isJpxDecodingEnabled();
    setJpxDecodingEnabled(true);
    const ResFill fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) {
        fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), kImgW, kImgH, nullptr, 0);
        unsigned char* s = fz_pixmap_samples(ctx, pix);
        for (int i = 0; i < kImgW * kImgH; ++i) { s[3 * i] = 255; s[3 * i + 1] = 0; s[3 * i + 2] = 0; }
        fz_buffer* jpx = fz_new_buffer_from_pixmap_as_jpx(ctx, pix, fz_default_color_params, 100);
        fz_drop_pixmap(ctx, pix);
        pdf_obj* d = pdf_new_dict(ctx, doc, 6);
        pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Image));
        pdf_dict_put_int(ctx, d, PDF_NAME(Width), kImgW);
        pdf_dict_put_int(ctx, d, PDF_NAME(Height), kImgH);
        pdf_dict_put(ctx, d, PDF_NAME(Filter), PDF_NAME(JPXDecode));
        pdf_obj* img = pdf_add_stream(ctx, doc, jpx, d, 1);
        fz_drop_buffer(ctx, jpx);
        pdf_drop_obj(ctx, d);
        putXObject(ctx, res, "Im0", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    const QRectF mark = fitzRect(100, 400, 160, 450);
    redact({body}, fill, {mark});
    {
        auto doc = Document::open(ctx_, in_);
        auto page = doc->page(0);
        const auto orig = images_->enumerateImages(page.get(), doc.get());
        ASSERT_EQ(orig.size(), 1u);
        ASSERT_TRUE(orig[0].isJpx) << "oracle: the input image is JPX";
    }
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    expectRenderMatches(render(0), infos[0], mark);
    expectDataMatches(infos[0], mark);
    EXPECT_EQ(readFile(out_).find("JPXDecode"), Bytes::npos) << "the original JPX data is still in the file";
    setJpxDecodingEnabled(was);
}

// With JPX decoding off (the default) the image cannot be read: MuPDF gives
// a placeholder. A mark that touches the image replaces it with the blanked
// placeholder, so the original JPEG 2000 data never survives (the rest of the
// image is lost too, as with Remove).
TEST_F(RedactionImageModel, JpxImageWithDecodingOffNeverSurvivesAMark) {
    const bool was = isJpxDecodingEnabled();
    setJpxDecodingEnabled(true);
    Bytes jpxData;
    ctx_->execute([&](fz_context* ctx) {
        fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), kImgW, kImgH, nullptr, 0);
        unsigned char* s = fz_pixmap_samples(ctx, pix);
        for (int i = 0; i < kImgW * kImgH; ++i) { s[3 * i] = 255; s[3 * i + 1] = 0; s[3 * i + 2] = 0; }
        fz_buffer* jpx = fz_new_buffer_from_pixmap_as_jpx(ctx, pix, fz_default_color_params, 100);
        unsigned char* d = nullptr;
        const size_t n = fz_buffer_storage(ctx, jpx, &d);
        jpxData.assign(reinterpret_cast<const char*>(d), n);
        fz_drop_buffer(ctx, jpx);
        fz_drop_pixmap(ctx, pix);
    });
    setJpxDecodingEnabled(false);
    const ResFill fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) {
        fz_buffer* jpx = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(jpxData.data()),
                                                         jpxData.size());
        pdf_obj* d = pdf_new_dict(ctx, doc, 6);
        pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Image));
        pdf_dict_put_int(ctx, d, PDF_NAME(Width), kImgW);
        pdf_dict_put_int(ctx, d, PDF_NAME(Height), kImgH);
        pdf_dict_put(ctx, d, PDF_NAME(Filter), PDF_NAME(JPXDecode));
        pdf_obj* img = pdf_add_stream(ctx, doc, jpx, d, 1);
        fz_drop_buffer(ctx, jpx);
        pdf_drop_obj(ctx, d);
        putXObject(ctx, res, "Im0", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    redact({body}, fill, {fitzRect(100, 400, 160, 450)});
    const Bytes saved = readFile(out_);
    EXPECT_EQ(saved.find("JPXDecode"), Bytes::npos) << "the original JPX image is still in the file";
    EXPECT_EQ(saved.find(jpxData.substr(jpxData.size() / 2, 24)), Bytes::npos)
        << "a run of the original JPX codestream is still in the file";
    setJpxDecodingEnabled(was);
}

// Image data that cannot be decoded (a DCT stream of garbage) under a
// partial mark: the apply either fails with an error, or the saved file holds
// no copy of the original data. It is never left in place as if redacted.
TEST_F(RedactionImageModel, UndecodableImageIsNeverLeftInPlace) {
    const Bytes secret = "PDFE-UNDECODABLE-IMAGE-SECRET-0123456789-PDFE-UNDECODABLE-IMAGE-SECRET";
    const ResFill fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) {
        fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(secret.data()),
                                                       secret.size());
        pdf_obj* d = pdf_new_dict(ctx, doc, 8);
        pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Image));
        pdf_dict_put_int(ctx, d, PDF_NAME(Width), kImgW);
        pdf_dict_put_int(ctx, d, PDF_NAME(Height), kImgH);
        pdf_dict_put_int(ctx, d, PDF_NAME(BitsPerComponent), 8);
        pdf_dict_put(ctx, d, PDF_NAME(ColorSpace), PDF_NAME(DeviceRGB));
        pdf_dict_put(ctx, d, PDF_NAME(Filter), PDF_NAME(DCTDecode));
        pdf_obj* img = pdf_add_stream(ctx, doc, b, d, 1);
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, d);
        putXObject(ctx, res, "Im0", img);
        pdf_drop_obj(ctx, img);
    };
    in_ = path("undecodable.pdf");
    out_ = path("undecodable_out.pdf");
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    buildPdf(*ctx_, in_, {body}, fill);
    ASSERT_NE(readFile(in_).find(secret), Bytes::npos) << "oracle: the data is stored as is";
    auto doc = Document::open(ctx_, in_);
    {
        auto page = doc->page(0);
        redact_->createRedactionMark(page.get(), fitzRect(100, 400, 160, 450));
    }
    RedactionApplyOptions o;
    o.rewriteDocument = false;
    const auto r = redact_->applyAllRedactions(doc.get(), o);
    RecordProperty("outcome", r.success ? "applied" : "failed");
    if (!r.success) {
        EXPECT_FALSE(r.errorMessage.empty()) << "a failed apply must say why";
        return;
    }
    doc->saveAs(out_);
    doc.reset();
    EXPECT_EQ(readFile(out_).find(secret), Bytes::npos)
        << "the undecodable image's original data is still in the saved file";
    EXPECT_GE(r.marksApplied, 1) << "the apply reported success but applied no mark";
    // Re-open the saved file and check what it contains: no image in it holds
    // the original data, stored or decoded (a re-compressed copy would pass the
    // byte search above), and every placement still on the page is a
    // readable image rather than a reference to the undecodable data.
    saved_ = Document::open(ctx_, out_);
    ASSERT_NE(saved_, nullptr) << "the saved file does not re-open";
    EXPECT_EQ(saved_->pageCount(), 1);
    EXPECT_EQ(imagesHoldingData(*ctx_, out_, secret), 0)
        << "an image in the re-opened document still holds the undecodable image's data";
    for (const auto& info : imagesOn(0)) {
        const Pixels p = readImage(*ctx_, out_, 0, info.xobjectName);
        EXPECT_TRUE(p.ok) << "a placement left on the page (" << info.xobjectName
                          << ") is not a readable replacement image";
    }
}

// The authored JBIG2 scan (tests/fixtures/jbig2_redaction.pdf): the marked
// black quadrant is blanked, the other black quadrant survives, and the
// original coded data is not in the saved file. With JBIG2 decoding off the
// image cannot be read; the original data still does not survive.
TEST_F(RedactionImageModel, Jbig2ImageIsPixelRedacted) {
    const std::string fixturePath = std::string(TEST_FIXTURES_DIR) + "/jbig2_redaction.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixturePath));
    const Bytes original = readFile(fixturePath);
    ASSERT_NE(original.find("/JBIG2Decode"), Bytes::npos);
    const bool was = isJbig2DecodingEnabled();
    for (bool decode : {true, false}) {
        SCOPED_TRACE(decode ? "JBIG2 decoding on" : "JBIG2 decoding off");
        setJbig2DecodingEnabled(decode);
        const std::string work = path(decode ? "jb_on.pdf" : "jb_off.pdf");
        const std::string out = path(decode ? "jb_on_out.pdf" : "jb_off_out.pdf");
        std::filesystem::copy_file(fixturePath, work, std::filesystem::copy_options::overwrite_existing);
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        {
            auto page = doc->page(0);
            // Top-left quadrant of the 200 x 200 image at (50,50) on a 300 x 300 page.
            redact_->createRedactionMark(page.get(), QRectF(50, 50, 100, 100));
        }
        RedactionApplyOptions o;
        o.rewriteDocument = false;
        o.blackBoxes = false;
        const auto r = redact_->applyAllRedactions(doc.get(), o);
        ASSERT_TRUE(r.success) << r.errorMessage;
        doc->saveAs(out);
        doc.reset();
        const Bytes saved = readFile(out);
        EXPECT_EQ(saved.find("/JBIG2Decode"), Bytes::npos) << "the original JBIG2 data is still in the file";
        if (decode) {
            auto again = Document::open(ctx_, out);
            auto page = again->page(0);
            const QImage img = page->render(QRectF(), 1.0f);
            auto dark = [&](int cx, int cy) {
                int n = 0;
                for (int y = cy - 20; y < cy + 20; ++y)
                    for (int x = cx - 20; x < cx + 20; ++x) n += qGray(img.pixel(x, y)) < 100 ? 1 : 0;
                return n / 1600.0;
            };
            EXPECT_LT(dark(100, 100), 0.05) << "the marked black quadrant still renders";
            EXPECT_GT(dark(200, 200), 0.95) << "the unmarked black quadrant was lost";
        }
    }
    setJbig2DecodingEnabled(was);
}

// Every per-mark image method, end to end: marks restored with None, Remove,
// Pixels and RemoveUnlessInvisible, and one with no method of its own, which
// takes the apply options' method (Remove here). The getters read the
// methods back before the apply.
TEST_F(RedactionImageModel, EveryPerMarkImageMethodIsHonoured) {
    const std::vector<std::string> names = {"ImA", "ImB", "ImC", "ImD", "ImE"};
    const ResFill fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) {
        for (const std::string& n : names) {
            pdf_obj* img = addSolidImage(ctx, doc, kImgW, kImgH, 255, 0, 0);
            putXObject(ctx, res, n.c_str(), img);
            pdf_drop_obj(ctx, img);
        }
    };
    std::string body;
    for (size_t i = 0; i < names.size(); ++i)
        body += "q " + cm(100, 0, 0, 80, 20 + 115.0 * i, 400) + " /" + names[i] + " Do Q ";
    in_ = path("methods.pdf");
    out_ = path("methods_out.pdf");
    buildPdf(*ctx_, in_, {body}, fill);
    auto doc = Document::open(ctx_, in_);
    const std::vector<std::optional<RedactionImageMethod>> methods = {
        RedactionImageMethod::None, RedactionImageMethod::Remove, RedactionImageMethod::Pixels,
        RedactionImageMethod::RemoveUnlessInvisible, std::nullopt};
    {
        auto page = doc->page(0);
        for (size_t i = 0; i < names.size(); ++i) {
            RedactionData m;
            m.pageIndex = 0;
            m.rect = fitzRect(20 + 115.0 * i, 440, 20 + 115.0 * i + 30, 480);
            m.imageMethod = methods[i];
            redact_->restoreRedactionMark(page.get(), m);
        }
        const auto marks = redact_->getRedactionMarks(page.get());
        ASSERT_EQ(marks.size(), names.size());
        for (size_t i = 0; i < names.size(); ++i)
            EXPECT_EQ(marks[i].imageMethod, methods[i]) << "mark " << i << ": the method was not read back";
    }
    RedactionApplyOptions o;
    o.rewriteDocument = false;
    o.imageMethod = RedactionImageMethod::Remove;
    const auto r = redact_->applyAllRedactions(doc.get(), o);
    ASSERT_TRUE(r.success) << r.errorMessage;
    doc->saveAs(out_);
    doc.reset();
    saved_ = Document::open(ctx_, out_);
    const auto infos = imagesOn(0);
    auto placedAt = [&](size_t i) -> const PageImageInfo* {
        for (const auto& info : infos)
            if (std::fabs(info.pageCtm[4] - (20 + 115.0 * i)) < 0.5) return &info;
        return nullptr;
    };
    const QImage img = render(0);
    const auto centre = [](size_t i) { return QPointF(20 + 115.0 * i + 50, kPageH - 440); };
    // A: None -> untouched data, still drawn.
    ASSERT_NE(placedAt(0), nullptr) << "None: the image was removed";
    {
        const Pixels p = readImage(*ctx_, out_, 0, placedAt(0)->xobjectName);
        ASSERT_TRUE(p.ok);
        EXPECT_TRUE(isRedPx(p.at(1, 1))) << "None: pixels under the mark were changed";
    }
    // B: Remove -> gone although the mark covers only a corner.
    EXPECT_EQ(placedAt(1), nullptr) << "Remove: the image is still drawn";
    EXPECT_FALSE(isRedPx(px(img, centre(1))));
    // C: Pixels -> corner blanked, rest kept.
    ASSERT_NE(placedAt(2), nullptr) << "Pixels: the image was removed";
    {
        const Pixels p = readImage(*ctx_, out_, 0, placedAt(2)->xobjectName);
        ASSERT_TRUE(p.ok);
        EXPECT_TRUE(isWhitePx(p.at(1, 1))) << "Pixels: the pixels under the mark survive";
        EXPECT_TRUE(isRedPx(p.at(p.w - 2, p.h - 2)));
        EXPECT_TRUE(isRedPx(px(img, centre(2))));
    }
    // D: RemoveUnlessInvisible on a visible image -> gone.
    EXPECT_EQ(placedAt(3), nullptr) << "RemoveUnlessInvisible: the visible image is still drawn";
    // E: no method of its own -> the options' Remove.
    EXPECT_EQ(placedAt(4), nullptr) << "a mark with no method of its own did not take the options' method";
}

// A method stored on a mark in a FILE (not by this session) is ignored: a
// document cannot ship marks that leave their image in place. The mark is
// applied with the options' method (Pixels by default).
TEST_F(RedactionImageModel, MarkMethodFoundInAFileIsIgnored) {
    in_ = path("foreign.pdf");
    out_ = path("foreign_out.pdf");
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    buildPdf(*ctx_, in_, {body}, oneRedImage());
    const QRectF mark = fitzRect(100, 400, 160, 450);
    // A Redact annotation written by "another session": its stored method
    // says None, under a token this process did not make.
    const std::string marked = in_ + ".marked.pdf";
    ctx_->execute([&](fz_context* ctx) {
        pdf_document* d = pdf_open_document(ctx, in_.c_str());
        pdf_page* pp = pdf_load_page(ctx, d, 0);
        pdf_annot* a = pdf_create_annot(ctx, pp, PDF_ANNOT_REDACT);
        const fz_rect r = fz_make_rect(static_cast<float>(mark.left()), static_cast<float>(mark.top()),
                                       static_cast<float>(mark.right()), static_cast<float>(mark.bottom()));
        pdf_set_annot_rect(ctx, a, r);
        pdf_obj* o = pdf_dict_puts_dict(ctx, pdf_annot_obj(ctx, a), "PDFEMarkOptions", 3);
        pdf_dict_puts_drop(ctx, o, "Session", pdf_new_string(ctx, "0123456789abcdef0123456789abcdef", 32));
        pdf_dict_puts_drop(ctx, o, "Image", pdf_new_name(ctx, "None"));
        pdf_update_annot(ctx, a);
        pdf_drop_annot(ctx, a);
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, d, marked.c_str(), &wo);
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, d);
    });
    auto doc = Document::open(ctx_, marked);
    {
        auto page = doc->page(0);
        const auto marks = redact_->getRedactionMarks(page.get());
        ASSERT_EQ(marks.size(), 1u);
        EXPECT_FALSE(marks[0].imageMethod.has_value()) << "a method found in the file was read as the mark's own";
    }
    RedactionApplyOptions o;
    o.rewriteDocument = false;
    ASSERT_TRUE(redact_->applyAllRedactions(doc.get(), o).success);
    doc->saveAs(out_);
    doc.reset();
    saved_ = Document::open(ctx_, out_);
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    expectDataMatches(infos[0], mark);
    EXPECT_EQ(untouchedRedImages(*ctx_, out_), 0) << "the file's /Image /None was honoured";
}

// A per-mark method survives deleting and restoring the mark (the undo path
// of DeleteRedactionCommand), and two marks with different methods on one
// page are each applied with their own.
TEST_F(RedactionImageModel, PerMarkMethodSurvivesDeleteAndRestore) {
    in_ = path("undo.pdf");
    out_ = path("undo_out.pdf");
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    buildPdf(*ctx_, in_, {body}, oneRedImage());
    auto doc = Document::open(ctx_, in_);
    {
        auto page = doc->page(0);
        RedactionData m;
        m.pageIndex = 0;
        m.rect = fitzRect(100, 400, 160, 450);
        m.imageMethod = RedactionImageMethod::None;
        const int idx = redact_->restoreRedactionMark(page.get(), m);
        const RedactionData saved = redact_->deleteRedactionMark(page.get(), idx);
        ASSERT_EQ(saved.imageMethod, std::optional<RedactionImageMethod>(RedactionImageMethod::None));
        redact_->restoreRedactionMark(page.get(), saved);
        const auto marks = redact_->getRedactionMarks(page.get());
        ASSERT_EQ(marks.size(), 1u);
        EXPECT_EQ(marks[0].imageMethod, std::optional<RedactionImageMethod>(RedactionImageMethod::None));
    }
    RedactionApplyOptions o;
    o.rewriteDocument = false;
    ASSERT_TRUE(redact_->applyAllRedactions(doc.get(), o).success);
    doc->saveAs(out_);
    doc.reset();
    saved_ = Document::open(ctx_, out_);
    const auto infos = imagesOn(0);
    ASSERT_EQ(infos.size(), 1u);
    const Pixels p = readImage(*ctx_, out_, 0, infos[0].xobjectName);
    ASSERT_TRUE(p.ok);
    EXPECT_TRUE(isRedPx(p.at(1, 1))) << "the restored mark lost its None method";
}

// ---------------------------------------------------------------------------
// Flatten
// ---------------------------------------------------------------------------
class FlattenImageModel : public RedactionImageModelBase {
protected:
    // test_form.pdf with a second page carrying copies of page 0's widgets
    // (as the RD3 probe builds it). `sharedResources` makes both pages use
    // one indirect /Resources dictionary.
    std::string twoPageForm(bool sharedResources) {
        const std::string src = std::string(TEST_FIXTURES_DIR) + "/test_form.pdf";
        EXPECT_TRUE(std::filesystem::exists(src));
        const std::string two = path(sharedResources ? "two_shared.pdf" : "two.pdf");
        ctx_->execute([&](fz_context* ctx) {
            pdf_document* pdoc = pdf_open_document(ctx, src.c_str());
            pdf_graft_page(ctx, pdoc, -1, pdoc, 0);
            pdf_obj* p0 = pdf_lookup_page_obj(ctx, pdoc, 0);
            pdf_obj* p1 = pdf_lookup_page_obj(ctx, pdoc, 1);
            pdf_obj* annots = pdf_dict_get(ctx, p0, PDF_NAME(Annots));
            pdf_obj* dst = pdf_new_array(ctx, pdoc, pdf_array_len(ctx, annots));
            pdf_dict_put_drop(ctx, p1, PDF_NAME(Annots), dst);
            pdf_obj* fields = pdf_dict_getp(ctx, pdf_trailer(ctx, pdoc), "Root/AcroForm/Fields");
            for (int i = 0; i < pdf_array_len(ctx, annots); ++i) {
                pdf_obj* copy = pdf_deep_copy_obj(ctx, pdf_array_get(ctx, annots, i));
                pdf_dict_put(ctx, copy, PDF_NAME(P), p1);
                pdf_obj* t = pdf_dict_get(ctx, copy, PDF_NAME(T));
                if (pdf_is_string(ctx, t)) {
                    char name[256];
                    fz_snprintf(name, sizeof name, "%s_p1", pdf_to_text_string(ctx, t));
                    pdf_dict_put_text_string(ctx, copy, PDF_NAME(T), name);
                }
                pdf_obj* ref = pdf_add_object_drop(ctx, pdoc, copy);
                pdf_array_push(ctx, dst, ref);
                if (pdf_is_array(ctx, fields) && !pdf_dict_get(ctx, ref, PDF_NAME(Parent)))
                    pdf_array_push(ctx, fields, ref);
                pdf_drop_obj(ctx, ref);
            }
            if (sharedResources) {
                pdf_obj* res = pdf_add_object(ctx, pdoc, pdf_dict_get_inheritable(ctx, p0, PDF_NAME(Resources)));
                pdf_dict_put(ctx, p0, PDF_NAME(Resources), res);
                pdf_dict_put(ctx, p1, PDF_NAME(Resources), res);
                pdf_drop_obj(ctx, res);
            }
            pdf_write_options wo = pdf_default_write_options;
            pdf_save_document(ctx, pdoc, two.c_str(), &wo);
            pdf_drop_document(ctx, pdoc);
        });
        return two;
    }

    std::string pageBytes(const std::string& file, int i) { return ::pageBytes(*ctx_, file, i); }

    bool hasAcroForm(const std::string& file) {
        bool has = false;
        ctx_->execute([&](fz_context* ctx) {
            pdf_document* d = pdf_open_document(ctx, file.c_str());
            has = pdf_dict_getp(ctx, pdf_trailer(ctx, d), "Root/AcroForm") != nullptr;
            pdf_drop_document(ctx, d);
        });
        return has;
    }
};

// Page 1's annotations, resources and content are byte for byte what they
// were; page 0 shows its widgets' appearance as page content.
TEST_F(FlattenImageModel, FlattenFormLeavesTheOtherPageUntouched) {
    for (bool shared : {false, true}) {
        SCOPED_TRACE(shared ? "shared /Resources" : "own /Resources");
        const std::string two = twoPageForm(shared);
        auto doc = Document::open(ctx_, two);
        ASSERT_NE(doc, nullptr);
        // The baseline goes through the same save as the result.
        const std::string base = path(shared ? "base_shared.pdf" : "base.pdf");
        {
            auto copy = Document::open(ctx_, two);
            copy->saveAs(base);
        }
        const std::string before1 = pageBytes(base, 1);
        pdfe::forms::FormManager forms(ctx_);
        QImage shownBefore;
        {
            auto p0 = doc->page(0);
            ASSERT_TRUE(forms.hasFormFields(p0.get()));
            shownBefore = p0->render(QRectF(), 1.0f);
            forms.flattenForm(p0.get());
        }
        const std::string out = path(shared ? "flat_shared.pdf" : "flat.pdf");
        doc->saveAs(out);
        doc.reset();
        EXPECT_EQ(pageBytes(out, 1), before1) << "flattening page 0 changed page 1";
        EXPECT_TRUE(hasAcroForm(out)) << "the AcroForm was removed while page 1 still has fields";
        auto again = Document::open(ctx_, out);
        auto q0 = again->page(0);
        auto q1 = again->page(1);
        EXPECT_FALSE(forms.hasFormFields(q0.get()));
        EXPECT_TRUE(forms.hasFormFields(q1.get()));
        EXPECT_FALSE(forms.getFormFields(q1.get()).empty()) << "page 1's fields are no longer listed";
        // The baked page looks as it did with its widgets.
        const QImage shownAfter = q0->render(QRectF(), 1.0f);
        ASSERT_EQ(shownAfter.size(), shownBefore.size());
        int differ = 0;
        for (int y = 0; y < shownAfter.height(); y += 2)
            for (int x = 0; x < shownAfter.width(); x += 2) {
                const QRgb a = shownBefore.pixel(x, y), b = shownAfter.pixel(x, y);
                if (std::abs(qGray(a) - qGray(b)) > 40) ++differ;
            }
        EXPECT_LT(differ, 50) << "the flattened page does not show the widgets' appearance";
    }
}

// The AcroForm goes with its last field: flattening page 0 then page 1
// leaves no form.
TEST_F(FlattenImageModel, AcroFormIsRemovedWithTheLastField) {
    const std::string two = twoPageForm(false);
    auto doc = Document::open(ctx_, two);
    pdfe::forms::FormManager forms(ctx_);
    for (int i : {0, 1}) {
        auto p = doc->page(i);
        forms.flattenForm(p.get());
    }
    const std::string out = path("flat_both.pdf");
    doc->saveAs(out);
    doc.reset();
    EXPECT_FALSE(hasAcroForm(out));
    auto again = Document::open(ctx_, out);
    for (int i : {0, 1}) {
        auto p = again->page(i);
        EXPECT_FALSE(forms.hasFormFields(p.get())) << "page " << i;
    }
}

// FL1 (GitHub #437), the decided limit: a flattened image stamp is drawn
// inside a Form XObject. It is enumerated and selectable (occurrence -1,
// inFormXObject), and NOT editable: the editor and removePlacement refuse it
// and the page content is unchanged (docs/image-placement-model.md,
// "Placements inside Form XObjects").
TEST_F(FlattenImageModel, FlattenedImageStampIsSelectableButNotEditable) {
    const std::string src = std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);
    pdfe::annotations::AnnotationManager annots(ctx_);
    QImage stamp(40, 20, QImage::Format_RGB32);
    stamp.fill(qRgb(255, 0, 0));
    const QRectF rect(100.0, 100.0, 200.0, 100.0);
    {
        auto page = doc->page(0);
        auto data = annots.createAnnotation(page.get(), pdfe::annotations::AnnotationType::Stamp, rect);
        ASSERT_GE(data.annotIndex, 0);
        annots.setStampImageAppearance(page.get(), data.annotIndex, stamp);
    }
    ASSERT_TRUE(redact_->flattenAnnotations(doc.get()));
    auto page = doc->page(0);
    const auto infos = images_->enumerateImages(page.get(), doc.get());
    ASSERT_EQ(infos.size(), 1u) << "the flattened stamp is not listed";
    EXPECT_TRUE(infos[0].inFormXObject);
    EXPECT_EQ(infos[0].occurrence, -1);
    EXPECT_NEAR(infos[0].boundingBox.center().x(), rect.center().x(), 1.0);
    EXPECT_NEAR(infos[0].boundingBox.center().y(), rect.center().y(), 1.0);
    const std::string before = path("stamp_before.pdf");
    doc->saveAs(before);
    const std::string contentBefore = pageBytes(before, 0);
    ImageContentEditor editor(ctx_);
    EXPECT_FALSE(editor.resizeImage(page.get(), doc.get(), PlacementId{infos[0].xobjectName, -1}, 50, 50, 10, 10))
        << "an image inside a Form XObject must not be edited through the page";
    EXPECT_FALSE(images_->removePlacement(page.get(), doc.get(), infos[0].xobjectName, -1).success);
    const std::string after = path("stamp_after.pdf");
    doc->saveAs(after);
    EXPECT_EQ(pageBytes(after, 0), contentBefore) << "a refused edit changed the page";
}

// ---------------------------------------------------------------------------
// Claim-check remediation (phase 5): per-mark text and line-art methods,
// stream and colour-key masks, inline images, emptied parent fields.
// ---------------------------------------------------------------------------
namespace {

// The text MuPDF extracts from page `i` of `file`.
std::string pageText(MuPdfContext& mc, const std::string& file, int i) {
    std::string out;
    mc.execute([&](fz_context* ctx) {
        fz_document* d = fz_open_document(ctx, file.c_str());
        fz_page* p = nullptr;
        fz_stext_page* st = nullptr;
        fz_buffer* b = nullptr;
        fz_var(p);
        fz_var(st);
        fz_var(b);
        fz_try(ctx) {
            p = fz_load_page(ctx, d, i);
            st = fz_new_stext_page_from_page(ctx, p, nullptr);
            b = fz_new_buffer_from_stext_page(ctx, st);
            out.assign(fz_string_from_buffer(ctx, b));
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, b);
            fz_drop_stext_page(ctx, st);
            fz_drop_page(ctx, p);
            fz_drop_document(ctx, d);
        }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return out;
}

// The decoded content stream(s) of page `i` of `file`, concatenated.
Bytes pageContent(MuPdfContext& mc, const std::string& file, int i) {
    Bytes out;
    mc.execute([&](fz_context* ctx) {
        pdf_document* d = pdf_open_document(ctx, file.c_str());
        fz_buffer* b = nullptr;
        fz_var(b);
        fz_try(ctx) {
            pdf_obj* contents = pdf_dict_get(ctx, pdf_lookup_page_obj(ctx, d, i), PDF_NAME(Contents));
            const int n = pdf_is_array(ctx, contents) ? pdf_array_len(ctx, contents) : 1;
            for (int k = 0; k < n; ++k) {
                pdf_obj* s = pdf_is_array(ctx, contents) ? pdf_array_get(ctx, contents, k) : contents;
                if (!pdf_is_stream(ctx, s)) continue;
                b = pdf_load_stream(ctx, s);
                unsigned char* data = nullptr;
                const size_t len = fz_buffer_storage(ctx, b, &data);
                out.append(reinterpret_cast<const char*>(data), len);
                fz_drop_buffer(ctx, b);
                b = nullptr;
            }
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, b);
            pdf_drop_document(ctx, d);
        }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return out;
}

// One decoded image as RGB (an alpha-only stencil is read as gray).
struct Decoded {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
    QRgb at(int x, int y) const {
        const size_t i = (static_cast<size_t>(y) * w + x) * 3;
        return qRgb(rgb[i], rgb[i + 1], rgb[i + 2]);
    }
};

// Decodes `im` into the caller-owned `out` (inside a guarded region).
void decodeImage(fz_context* ctx, fz_image* im, Decoded& out) {
    fz_pixmap* pm = nullptr;
    fz_pixmap* conv = nullptr;
    fz_var(pm);
    fz_var(conv);
    fz_try(ctx) {
        pm = fz_get_pixmap_from_image(ctx, im, nullptr, nullptr, nullptr, nullptr);
        out.w = pm->w;
        out.h = pm->h;
        out.rgb.assign(static_cast<size_t>(pm->w) * pm->h * 3, 0);
        if (!pm->colorspace) {
            for (int y = 0; y < pm->h; ++y)
                for (int x = 0; x < pm->w; ++x) {
                    const unsigned char v = pm->samples[static_cast<size_t>(y) * pm->stride + x * pm->n];
                    unsigned char* o = &out.rgb[(static_cast<size_t>(y) * pm->w + x) * 3];
                    o[0] = o[1] = o[2] = v;
                }
        } else {
            fz_pixmap* use = pm;
            if (pm->colorspace != fz_device_rgb(ctx) || pm->alpha) {
                conv = fz_convert_pixmap(ctx, pm, fz_device_rgb(ctx), nullptr, nullptr, fz_default_color_params, 0);
                use = conv;
            }
            for (int y = 0; y < use->h; ++y)
                std::memcpy(&out.rgb[static_cast<size_t>(y) * use->w * 3],
                            use->samples + static_cast<size_t>(y) * use->stride, static_cast<size_t>(use->w) * 3);
        }
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, conv);
        fz_drop_pixmap(ctx, pm);
    }
    fz_catch(ctx) { fz_rethrow(ctx); }
}

// Every Image XObject in `file` (masks included: they are Image XObjects
// too), decoded.
std::vector<Decoded> allImageObjects(MuPdfContext& mc, const std::string& file) {
    std::vector<Decoded> out;
    Decoded one;
    mc.execute([&](fz_context* ctx) {
        pdf_document* d = pdf_open_document(ctx, file.c_str());
        fz_try(ctx) {
            const int n = pdf_xref_len(ctx, d);
            for (int i = 1; i < n; ++i) {
                pdf_obj* o = pdf_new_indirect(ctx, d, i, 0);
                fz_image* im = nullptr;
                fz_var(im);
                fz_try(ctx) {
                    if (pdf_is_stream(ctx, o) &&
                        pdf_name_eq(ctx, pdf_dict_get(ctx, o, PDF_NAME(Subtype)), PDF_NAME(Image))) {
                        im = pdf_load_image(ctx, d, o);
                        decodeImage(ctx, im, one);
                        out.push_back(one);
                    }
                }
                fz_always(ctx) {
                    fz_drop_image(ctx, im);
                    pdf_drop_obj(ctx, o);
                }
                fz_catch(ctx) { fz_report_error(ctx); }
            }
        }
        fz_always(ctx) { pdf_drop_document(ctx, d); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return out;
}

// The images page `i` of `file` draws (XObjects and inline images alike),
// decoded, as MuPDF's device sees them.
std::vector<Decoded> drawnImages(MuPdfContext& mc, const std::string& file, int i) {
    std::vector<Decoded> out;
    Decoded one;
    mc.execute([&](fz_context* ctx) {
        fz_document* d = fz_open_document(ctx, file.c_str());
        fz_page* p = nullptr;
        fz_stext_page* st = nullptr;
        fz_var(p);
        fz_var(st);
        fz_try(ctx) {
            p = fz_load_page(ctx, d, i);
            fz_stext_options so;
            std::memset(&so, 0, sizeof so);
            so.flags = FZ_STEXT_PRESERVE_IMAGES;
            st = fz_new_stext_page_from_page(ctx, p, &so);
            for (fz_stext_block* b = st->first_block; b; b = b->next) {
                if (b->type != FZ_STEXT_BLOCK_IMAGE) continue;
                decodeImage(ctx, b->u.i.image, one);
                out.push_back(one);
            }
        }
        fz_always(ctx) {
            fz_drop_stext_page(ctx, st);
            fz_drop_page(ctx, p);
            fz_drop_document(ctx, d);
        }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return out;
}

// An image XObject over raw (unfiltered) samples; NEW ref.
pdf_obj* addRawImage(fz_context* ctx, pdf_document* doc, int w, int h, int bpc, pdf_obj* cs, bool imageMask,
                     const Bytes& samples) {
    fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(samples.data()),
                                                   samples.size());
    pdf_obj* d = pdf_new_dict(ctx, doc, 8);
    pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject));
    pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Image));
    pdf_dict_put_int(ctx, d, PDF_NAME(Width), w);
    pdf_dict_put_int(ctx, d, PDF_NAME(Height), h);
    pdf_dict_put_int(ctx, d, PDF_NAME(BitsPerComponent), bpc);
    if (imageMask) pdf_dict_put_bool(ctx, d, PDF_NAME(ImageMask), 1);
    else pdf_dict_put(ctx, d, PDF_NAME(ColorSpace), cs);
    pdf_obj* img = pdf_add_stream(ctx, doc, b, d, 0);
    fz_drop_buffer(ctx, b);
    pdf_drop_obj(ctx, d);
    return img;
}

// 4-pixel vertical stripes: true on the odd stripes.
bool stripe(int x) { return (x / 4) % 2 == 1; }

// A Helvetica font resource named F1.
void putHelvetica(fz_context* ctx, pdf_document* doc, pdf_obj* res) {
    pdf_obj* f = pdf_new_dict(ctx, doc, 4);
    pdf_dict_put(ctx, f, PDF_NAME(Type), PDF_NAME(Font));
    pdf_dict_put(ctx, f, PDF_NAME(Subtype), PDF_NAME(Type1));
    pdf_dict_put_name(ctx, f, PDF_NAME(BaseFont), "Helvetica");
    pdf_obj* ref = pdf_add_object_drop(ctx, doc, f);
    pdf_obj* fonts = pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 1);
    pdf_dict_puts_drop(ctx, fonts, "F1", ref);
}

std::string textAt(double x, double y, const char* s) {
    return "BT /F1 20 Tf " + num(x) + " " + num(y) + " Td (" + s + ") Tj ET ";
}

RedactionApplyOptions noBlackBoxes() {
    RedactionApplyOptions o;
    o.blackBoxes = false;
    return o;
}

// A 64 x 48 RGB inline image whose pixel (x, y) is (255, x, y): red to the
// eye and unique in the data. ASCIIHex-encoded.
std::string inlineImage() {
    std::string hex;
    char b[8];
    for (int y = 0; y < kImgH; ++y)
        for (int x = 0; x < kImgW; ++x) {
            std::snprintf(b, sizeof b, "FF%02X%02X", x, y);
            hex += b;
        }
    return "BI /W 64 /H 48 /BPC 8 /CS /RGB /F /AHx ID " + hex + "> EI";
}

// Row 2, columns 2..9 of that image (under the partial mark used below), as
// it is stored in hex, and as binary samples.
Bytes inlineMarkedRunHex() {
    std::string s;
    char b[8];
    for (int x = 2; x < 10; ++x) {
        std::snprintf(b, sizeof b, "FF%02X02", x);
        s += b;
    }
    return s;
}
Bytes inlineMarkedRunBinary() {
    Bytes s;
    for (int x = 2; x < 10; ++x) {
        s.push_back(static_cast<char>(0xFF));
        s.push_back(static_cast<char>(x));
        s.push_back(2);
    }
    return s;
}

// Render comparison of a masked image before and after: counts red samples
// under the mark (shrunk 3) and whether each sample outside it (grown 3) is
// as it was.
struct MaskedVerdict {
    int underRedBefore = 0, underRedAfter = 0, outside = 0, outsideSame = 0, outsideRed = 0, outsideNot = 0;
};
MaskedVerdict compareMasked(const QImage& before, const QImage& after, const QRectF& image, const QRectF& mark,
                            const std::function<QRgb(const QImage&, QPointF)>& px) {
    MaskedVerdict v;
    const QRectF in = mark.adjusted(3, 3, -3, -3), out = mark.adjusted(-3, -3, 3, 3);
    for (double y = image.top() + 2; y < image.bottom() - 2; y += 1)
        for (double x = image.left() + 2; x < image.right() - 2; x += 1) {
            const QPointF p(x, y);
            const bool rb = isRedPx(px(before, p)), ra = isRedPx(px(after, p));
            if (in.contains(p)) {
                v.underRedBefore += rb;
                v.underRedAfter += ra;
            } else if (!out.contains(p)) {
                ++v.outside;
                v.outsideSame += rb == ra;
                v.outsideRed += rb;
                v.outsideNot += !rb;
            }
        }
    return v;
}

} // namespace

// F1: the per-mark TEXT method, end to end. Two runs. With the options' text
// method Remove, a mark restored with None keeps its word and an unset mark
// loses its word. With the options' method None, a mark restored with Remove
// loses its word and an unset mark keeps it. The getters read each method
// back, also after a delete and restore (the undo path).
TEST_F(RedactionImageModel, EveryPerMarkTextMethodIsHonoured) {
    using pdfe::redaction::RedactionTextMethod;
    struct Run {
        RedactionTextMethod options;
        std::optional<RedactionTextMethod> first, second;
        bool firstKept, secondKept;
    };
    const Run runs[] = {
        {RedactionTextMethod::Remove, RedactionTextMethod::None, std::nullopt, true, false},
        {RedactionTextMethod::None, RedactionTextMethod::Remove, std::nullopt, false, true},
    };
    const ResFill fonts = [](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) { putHelvetica(ctx, doc, res); };
    const std::string body = textAt(40, 600, "ALPHASECRET") + textAt(320, 600, "BRAVOSECRET") +
                             textAt(40, 300, "KEEPOUTSIDE");
    int k = 0;
    for (const Run& run : runs) {
        SCOPED_TRACE(k == 0 ? "options Remove" : "options None");
        in_ = path(k == 0 ? "text0.pdf" : "text1.pdf");
        out_ = path(k == 0 ? "text0_out.pdf" : "text1_out.pdf");
        ++k;
        buildPdf(*ctx_, in_, {body}, fonts);
        ASSERT_NE(pageText(*ctx_, in_, 0).find("ALPHASECRET"), std::string::npos) << "oracle: text extractable";
        auto doc = Document::open(ctx_, in_);
        {
            auto page = doc->page(0);
            RedactionData a;
            a.pageIndex = 0;
            a.rect = fitzRect(30, 590, 280, 630);
            a.textMethod = run.first;
            const int idx = redact_->restoreRedactionMark(page.get(), a);
            const RedactionData kept = redact_->deleteRedactionMark(page.get(), idx);
            ASSERT_EQ(kept.textMethod, run.first) << "the text method was lost by delete";
            redact_->restoreRedactionMark(page.get(), kept);
            RedactionData b;
            b.pageIndex = 0;
            b.rect = fitzRect(310, 590, 570, 630);
            b.textMethod = run.second;
            redact_->restoreRedactionMark(page.get(), b);
            const auto marks = redact_->getRedactionMarks(page.get());
            ASSERT_EQ(marks.size(), 2u);
            EXPECT_EQ(marks[0].textMethod, run.first) << "the text method was not read back";
            EXPECT_EQ(marks[1].textMethod, run.second);
        }
        RedactionApplyOptions o;
        o.rewriteDocument = false;
        o.textMethod = run.options;
        ASSERT_TRUE(redact_->applyAllRedactions(doc.get(), o).success);
        doc->saveAs(out_);
        doc.reset();
        const std::string text = pageText(*ctx_, out_, 0);
        EXPECT_EQ(text.find("ALPHASECRET") != std::string::npos, run.firstKept) << text;
        EXPECT_EQ(text.find("BRAVOSECRET") != std::string::npos, run.secondKept) << text;
        EXPECT_NE(text.find("KEEPOUTSIDE"), std::string::npos) << "text outside every mark was removed";
    }
}

// F1: the per-mark LINE-ART method, end to end. Four red squares, black
// boxes off so what is left shows. A: fully covered, mark None -> kept.
// B: partly covered, mark RemoveIfTouched -> removed. C: fully covered, mark
// unset -> the options' RemoveIfCovered -> removed. D: partly covered, mark
// unset -> RemoveIfCovered -> kept.
TEST_F(RedactionImageModel, EveryPerMarkLineArtMethodIsHonoured) {
    using pdfe::redaction::RedactionLineArtMethod;
    const double xs[] = {40, 170, 300, 430};
    std::string body = "1 0 0 rg ";
    for (double x : xs) body += num(x) + " 400 80 80 re f ";
    in_ = path("lineart.pdf");
    out_ = path("lineart_out.pdf");
    buildPdf(*ctx_, in_, {body}, [](fz_context*, pdf_document*, pdf_obj*, int) {});
    const std::optional<RedactionLineArtMethod> methods[] = {
        RedactionLineArtMethod::None, RedactionLineArtMethod::RemoveIfTouched, std::nullopt, std::nullopt};
    const QRectF marks[] = {fitzRect(30, 390, 130, 490), fitzRect(160, 390, 200, 490),
                            fitzRect(290, 390, 390, 490), fitzRect(420, 390, 460, 490)};
    auto doc = Document::open(ctx_, in_);
    {
        auto page = doc->page(0);
        for (int i = 0; i < 4; ++i) {
            RedactionData m;
            m.pageIndex = 0;
            m.rect = marks[i];
            m.lineArtMethod = methods[i];
            redact_->restoreRedactionMark(page.get(), m);
        }
        const auto read = redact_->getRedactionMarks(page.get());
        ASSERT_EQ(read.size(), 4u);
        for (int i = 0; i < 4; ++i) EXPECT_EQ(read[i].lineArtMethod, methods[i]) << "mark " << i;
    }
    RedactionApplyOptions o = noBlackBoxes();
    o.rewriteDocument = false;
    ASSERT_EQ(o.lineArtMethod, RedactionLineArtMethod::RemoveIfCovered);
    ASSERT_TRUE(redact_->applyAllRedactions(doc.get(), o).success);
    doc->saveAs(out_);
    doc.reset();
    saved_ = Document::open(ctx_, out_);
    const QImage img = render(0);
    EXPECT_TRUE(isRedPx(px(img, QPointF(80, kPageH - 440)))) << "A (None): the covered square was removed";
    EXPECT_FALSE(isRedPx(px(img, QPointF(230, kPageH - 440)))) << "B (RemoveIfTouched): the touched square remains";
    EXPECT_FALSE(isRedPx(px(img, QPointF(340, kPageH - 440))))
        << "C (unset, options RemoveIfCovered): the covered square remains";
    EXPECT_TRUE(isRedPx(px(img, QPointF(490, kPageH - 440))))
        << "D (unset, options RemoveIfCovered): a touched square was removed";
}

// F1: text and line-art methods stored on a mark in a FILE (another
// session's token) are ignored: the mark removes its text and its covered
// line art with the options' methods.
TEST_F(RedactionImageModel, TextAndLineArtMethodsFoundInAFileAreIgnored) {
    in_ = path("foreign_text.pdf");
    out_ = path("foreign_text_out.pdf");
    const std::string body = textAt(40, 600, "DELTASECRET") + "1 0 0 rg 300 590 60 40 re f";
    buildPdf(*ctx_, in_, {body}, [](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) {
        putHelvetica(ctx, doc, res);
    });
    const QRectF mark = fitzRect(30, 580, 380, 640);
    const std::string marked = in_ + ".marked.pdf";
    ctx_->execute([&](fz_context* ctx) {
        pdf_document* d = pdf_open_document(ctx, in_.c_str());
        pdf_page* pp = pdf_load_page(ctx, d, 0);
        pdf_annot* a = pdf_create_annot(ctx, pp, PDF_ANNOT_REDACT);
        pdf_set_annot_rect(ctx, a, fz_make_rect(static_cast<float>(mark.left()), static_cast<float>(mark.top()),
                                                static_cast<float>(mark.right()), static_cast<float>(mark.bottom())));
        pdf_obj* opt = pdf_dict_puts_dict(ctx, pdf_annot_obj(ctx, a), "PDFEMarkOptions", 3);
        pdf_dict_puts_drop(ctx, opt, "Session", pdf_new_string(ctx, "fedcba9876543210fedcba9876543210", 32));
        pdf_dict_puts_drop(ctx, opt, "Text", pdf_new_name(ctx, "None"));
        pdf_dict_puts_drop(ctx, opt, "LineArt", pdf_new_name(ctx, "None"));
        pdf_update_annot(ctx, a);
        pdf_drop_annot(ctx, a);
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, d, marked.c_str(), &wo);
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, d);
    });
    auto doc = Document::open(ctx_, marked);
    {
        auto page = doc->page(0);
        const auto marks = redact_->getRedactionMarks(page.get());
        ASSERT_EQ(marks.size(), 1u);
        EXPECT_FALSE(marks[0].textMethod.has_value()) << "a text method found in the file was read as the mark's own";
        EXPECT_FALSE(marks[0].lineArtMethod.has_value());
    }
    RedactionApplyOptions o = noBlackBoxes();
    o.rewriteDocument = false;
    ASSERT_TRUE(redact_->applyAllRedactions(doc.get(), o).success);
    doc->saveAs(out_);
    doc.reset();
    EXPECT_EQ(pageText(*ctx_, out_, 0).find("DELTASECRET"), std::string::npos) << "the file's Text None was honoured";
    saved_ = Document::open(ctx_, out_);
    EXPECT_FALSE(isRedPx(px(render(0), QPointF(330, kPageH - 610)))) << "the file's LineArt None was honoured";
}

// F4: an image with a stream /Mask (a 1-bit stencil of 4-pixel stripes)
// under a partial mark. Under the mark nothing of the image shows; outside,
// the render is what it was (the stripes); no image object in the saved
// file still holds the original pixels or the original stencil.
TEST_F(RedactionImageModel, StencilMaskIsRedactedUnderTheMarkToo) {
    Bytes maskBits(static_cast<size_t>(kImgW / 8) * kImgH, '\0');
    for (int y = 0; y < kImgH; ++y)
        for (int x = 0; x < kImgW; ++x)
            if (stripe(x)) maskBits[static_cast<size_t>(y) * (kImgW / 8) + x / 8] |= static_cast<char>(0x80 >> (x % 8));
    const ResFill fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) {
        pdf_obj* img = addSolidImage(ctx, doc, kImgW, kImgH, 255, 0, 0);
        pdf_obj* mask = addRawImage(ctx, doc, kImgW, kImgH, 1, nullptr, true, maskBits);
        pdf_dict_put(ctx, img, PDF_NAME(Mask), mask);
        putXObject(ctx, res, "Im0", img);
        pdf_drop_obj(ctx, mask);
        pdf_drop_obj(ctx, img);
    };
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    const QRectF mark = fitzRect(100, 400, 160, 450);
    redact({body}, fill, {mark}, noBlackBoxes());
    auto beforeDoc = Document::open(ctx_, in_);
    const QImage before = beforeDoc->page(0)->render(QRectF(), 1.0f);
    const MaskedVerdict v = compareMasked(before, render(0), fitzRect(100, 300, 300, 450), mark, px);
    ASSERT_GT(v.underRedBefore, 0) << "oracle: the masked image shows under the mark before";
    ASSERT_GT(v.outsideRed, 0) << "oracle: the stripes show outside the mark";
    ASSERT_GT(v.outsideNot, 0) << "oracle: the stencil hides part of the image outside the mark";
    EXPECT_EQ(v.underRedAfter, 0) << "the image still shows under the mark";
    EXPECT_GE(v.outsideSame, v.outside - v.outside / 100) << "the masked image changed outside the mark";
    EXPECT_EQ(untouchedRedImages(*ctx_, out_), 0) << "the original image is still in the saved file";
    for (const Decoded& d : allImageObjects(*ctx_, out_)) {
        if (d.w != kImgW || d.h != kImgH) continue;
        bool same = true, inverse = true;
        for (int y = 0; y < d.h && (same || inverse); ++y)
            for (int x = 0; x < d.w; ++x) {
                const bool on = qGray(d.at(x, y)) > 127;
                same = same && on == stripe(x);
                inverse = inverse && on != stripe(x);
            }
        EXPECT_FALSE(same || inverse) << "the original stencil mask is still in the saved file";
    }
}

// F4: an image with a colour-key /Mask array (green keyed out, red shown, in
// 4-pixel stripes) under a partial mark: nothing red under the mark, the
// keyed stripes as they were outside it, and no image object in the saved
// file still holds the original samples.
TEST_F(RedactionImageModel, ColourKeyMaskedImageIsRedactedUnderTheMark) {
    Bytes rgb(static_cast<size_t>(kImgW) * kImgH * 3, '\0');
    for (int y = 0; y < kImgH; ++y)
        for (int x = 0; x < kImgW; ++x) {
            char* p = &rgb[(static_cast<size_t>(y) * kImgW + x) * 3];
            p[0] = stripe(x) ? 0 : static_cast<char>(255);
            p[1] = stripe(x) ? static_cast<char>(255) : 0;
        }
    const ResFill fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res, int) {
        pdf_obj* img = addRawImage(ctx, doc, kImgW, kImgH, 8, PDF_NAME(DeviceRGB), false, rgb);
        pdf_obj* key = pdf_new_array(ctx, doc, 6);
        for (int val : {0, 0, 250, 255, 0, 0}) pdf_array_push_int(ctx, key, val);
        pdf_dict_put_drop(ctx, img, PDF_NAME(Mask), key);
        putXObject(ctx, res, "Im0", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " /Im0 Do Q";
    const QRectF mark = fitzRect(100, 400, 160, 450);
    redact({body}, fill, {mark}, noBlackBoxes());
    auto beforeDoc = Document::open(ctx_, in_);
    const QImage before = beforeDoc->page(0)->render(QRectF(), 1.0f);
    const QImage after = render(0);
    const MaskedVerdict v = compareMasked(before, after, fitzRect(100, 300, 300, 450), mark, px);
    ASSERT_GT(v.underRedBefore, 0) << "oracle: the image shows under the mark before";
    ASSERT_GT(v.outsideNot, 0) << "oracle: the colour key hides the green stripes outside the mark";
    EXPECT_EQ(v.underRedAfter, 0) << "the image still shows under the mark";
    EXPECT_GE(v.outsideSame, v.outside - v.outside / 100) << "the keyed image changed outside the mark";
    int green = 0;
    for (int y = 0; y < after.height(); y += 2)
        for (int x = 0; x < after.width(); x += 2)
            green += qGreen(after.pixel(x, y)) > 200 && qRed(after.pixel(x, y)) < 90 ? 1 : 0;
    EXPECT_EQ(green, 0) << "the keyed-out colour shows after the redaction";
    for (const Decoded& d : allImageObjects(*ctx_, out_)) {
        if (d.w != kImgW || d.h != kImgH) continue;
        EXPECT_NE(Bytes(d.rgb.begin(), d.rgb.end()), rgb) << "the original colour-keyed samples are in the saved file";
    }
}

// F4: an INLINE image (BI ... ID ... EI) under a partial mark: the pixels
// under the mark are blanked in the render and in the image the page now
// draws, the rest survives, and the original inline data (hex or binary) is
// not in the saved file.
TEST_F(RedactionImageModel, InlineImageUnderAPartialMarkIsPixelRedacted) {
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " " + inlineImage() + " Q";
    const QRectF mark = fitzRect(100, 400, 160, 450);
    redact({body}, [](fz_context*, pdf_document*, pdf_obj*, int) {}, {mark});
    ASSERT_NE(pageContent(*ctx_, in_, 0).find(inlineMarkedRunHex()), Bytes::npos) << "oracle: the run is in the input";
    const QImage img = render(0);
    EXPECT_FALSE(isRedPx(px(img, QPointF(110, kPageH - 440)))) << "the inline image still shows under the mark";
    EXPECT_TRUE(isRedPx(px(img, QPointF(250, kPageH - 330)))) << "the inline image was lost outside the mark";
    const auto drawn = drawnImages(*ctx_, out_, 0);
    ASSERT_EQ(drawn.size(), 1u) << "a partly marked inline image must remain";
    ASSERT_EQ(drawn[0].w, kImgW);
    EXPECT_TRUE(isWhitePx(drawn[0].at(5, 5))) << "the drawn image keeps its pixels under the mark";
    EXPECT_EQ(drawn[0].at(50, 40), qRgb(255, 50, 40)) << "the drawn image lost its pixels outside the mark";
    const Bytes content = pageContent(*ctx_, out_, 0);
    EXPECT_EQ(content.find(inlineMarkedRunHex()), Bytes::npos) << "the original inline data (hex) is still there";
    EXPECT_EQ(content.find(inlineMarkedRunBinary()), Bytes::npos) << "the original inline data (binary) is still there";
    EXPECT_EQ(readFile(out_).find(inlineMarkedRunHex()), Bytes::npos);
    for (const Decoded& d : allImageObjects(*ctx_, out_))
        if (d.w == kImgW && d.h == kImgH) EXPECT_NE(d.at(5, 5), qRgb(255, 5, 5)) << "an image object holds the original";
}

// F4: an inline image a mark covers completely is removed: nothing drawn,
// nothing red, and no trace of its data in the saved file.
TEST_F(RedactionImageModel, InlineImageUnderAFullMarkIsRemoved) {
    const std::string body = "q " + cm(200, 0, 0, 150, 100, 300) + " " + inlineImage() + " Q";
    redact({body}, [](fz_context*, pdf_document*, pdf_obj*, int) {}, {fitzRect(90, 290, 310, 460)});
    EXPECT_TRUE(drawnImages(*ctx_, out_, 0).empty()) << "a fully covered inline image is still drawn";
    EXPECT_FALSE(isRedPx(px(render(0), QPointF(200, kPageH - 375))));
    const Bytes content = pageContent(*ctx_, out_, 0);
    EXPECT_EQ(content.find(inlineMarkedRunHex()), Bytes::npos);
    EXPECT_EQ(content.find(inlineMarkedRunBinary()), Bytes::npos);
    EXPECT_EQ(content.find(" ID "), Bytes::npos) << "an inline image is still in the content";
}

// F5: a parent field whose widgets are all on the flattened page is removed
// from the field tree; a parent with a widget left on another page is kept,
// with only that widget as its kid.
TEST_F(FlattenImageModel, EmptiedParentFieldIsRemovedAndAParentWithKidsElsewhereIsKept) {
    const std::string in = path("parents.pdf");
    ctx_->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_create_document(ctx);
        for (int i = 0; i < 2; ++i) {
            pdf_obj* res = pdf_new_dict(ctx, doc, 1);
            fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(" "), 1);
            pdf_obj* page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 612, 792), 0, res, b);
            pdf_insert_page(ctx, doc, -1, page);
            pdf_drop_obj(ctx, page);
            fz_drop_buffer(ctx, b);
            pdf_drop_obj(ctx, res);
        }
        pdf_obj* fields = pdf_new_array(ctx, doc, 2);
        auto parent = [&](const char* name) {
            pdf_obj* p = pdf_new_dict(ctx, doc, 4);
            pdf_dict_put_text_string(ctx, p, PDF_NAME(T), name);
            pdf_dict_put(ctx, p, PDF_NAME(FT), PDF_NAME(Tx));
            pdf_dict_put_text_string(ctx, p, PDF_NAME(V), name);
            pdf_dict_put_array(ctx, p, PDF_NAME(Kids), 2);
            pdf_obj* ref = pdf_add_object_drop(ctx, doc, p);
            pdf_array_push(ctx, fields, ref);
            return ref;
        };
        pdf_obj* grp = parent("grp");
        pdf_obj* shared = parent("shared");
        auto widget = [&](pdf_obj* par, int pageIndex, float y) {
            pdf_obj* pageObj = pdf_lookup_page_obj(ctx, doc, pageIndex);
            pdf_obj* w = pdf_new_dict(ctx, doc, 8);
            pdf_dict_put(ctx, w, PDF_NAME(Type), PDF_NAME(Annot));
            pdf_dict_put(ctx, w, PDF_NAME(Subtype), PDF_NAME(Widget));
            pdf_dict_put_rect(ctx, w, PDF_NAME(Rect), fz_make_rect(100, y, 300, y + 30));
            pdf_dict_put_int(ctx, w, PDF_NAME(F), 4);
            pdf_dict_put(ctx, w, PDF_NAME(P), pageObj);
            pdf_dict_put(ctx, w, PDF_NAME(Parent), par);
            pdf_obj* ref = pdf_add_object_drop(ctx, doc, w);
            pdf_array_push(ctx, pdf_dict_get(ctx, par, PDF_NAME(Kids)), ref);
            pdf_obj* annots = pdf_dict_get(ctx, pageObj, PDF_NAME(Annots));
            if (!annots) annots = pdf_dict_put_array(ctx, pageObj, PDF_NAME(Annots), 4);
            pdf_array_push(ctx, annots, ref);
            pdf_drop_obj(ctx, ref);
        };
        widget(grp, 0, 600);
        widget(grp, 0, 500);
        widget(shared, 0, 400);
        widget(shared, 1, 400);
        pdf_obj* acro = pdf_dict_put_dict(ctx, pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root)),
                                          PDF_NAME(AcroForm), 2);
        pdf_dict_put_drop(ctx, acro, PDF_NAME(Fields), fields);
        pdf_dict_put_string(ctx, acro, PDF_NAME(DA), "/Helv 12 Tf 0 g", 15);
        pdf_drop_obj(ctx, grp);
        pdf_drop_obj(ctx, shared);
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, doc, in.c_str(), &wo);
        pdf_drop_document(ctx, doc);
    });
    auto doc = Document::open(ctx_, in);
    ASSERT_NE(doc, nullptr);
    pdfe::forms::FormManager forms(ctx_);
    {
        auto p0 = doc->page(0);
        ASSERT_TRUE(forms.hasFormFields(p0.get()));
        forms.flattenForm(p0.get());
    }
    const std::string out = path("parents_out.pdf");
    doc->saveAs(out);
    doc.reset();
    int fieldCount = -1, sharedKids = -1, sharedKidOnPage1 = 0;
    bool grpFound = false, sharedFound = false;
    ctx_->execute([&](fz_context* ctx) {
        pdf_document* d = pdf_open_document(ctx, out.c_str());
        pdf_obj* fields = pdf_dict_getp(ctx, pdf_trailer(ctx, d), "Root/AcroForm/Fields");
        pdf_obj* page1 = pdf_lookup_page_obj(ctx, d, 1);
        fieldCount = pdf_array_len(ctx, fields);
        for (int i = 0; i < fieldCount; ++i) {
            pdf_obj* f = pdf_array_get(ctx, fields, i);
            const char* t = pdf_dict_get_text_string(ctx, f, PDF_NAME(T));
            if (std::strcmp(t, "grp") == 0) grpFound = true;
            if (std::strcmp(t, "shared") == 0) {
                sharedFound = true;
                pdf_obj* kids = pdf_dict_get(ctx, f, PDF_NAME(Kids));
                sharedKids = pdf_array_len(ctx, kids);
                for (int k = 0; k < sharedKids; ++k)
                    if (pdf_objcmp(ctx, pdf_dict_get(ctx, pdf_array_get(ctx, kids, k), PDF_NAME(P)), page1) == 0)
                        ++sharedKidOnPage1;
            }
        }
        pdf_drop_document(ctx, d);
    });
    EXPECT_FALSE(grpFound) << "a parent field whose every widget was flattened is still in the field tree";
    EXPECT_TRUE(sharedFound) << "a parent field with a widget on page 1 was removed";
    EXPECT_EQ(sharedKids, 1) << "the shared parent should keep exactly its page-1 widget";
    EXPECT_EQ(sharedKidOnPage1, 1);
    EXPECT_EQ(fieldCount, 1);
    auto again = Document::open(ctx_, out);
    auto q1 = again->page(1);
    EXPECT_TRUE(forms.hasFormFields(q1.get()));
}
