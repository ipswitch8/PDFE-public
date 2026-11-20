// -*- coding: utf-8 -*-
// Pipeline image-placement-state, phase 5: two bounds of the metadata
// stripper (src/image/ExifStripper.cpp) carried over from the phase-4
// security audit. See docs/image-placement-model.md, "Metadata stripping".
//
//   (A) The fallback decoder's size bound reads a JPEG's frame header (SOFn).
//       ExifStripBounds.ComSegmentHoldingFfDaDoesNotHideAHugeFrame
//         The search used to stop at the first FF DA byte pair; FF DA inside a
//         COM payload ahead of the real SOF hid the SOF from the bound.
//       ExifStripBounds.BuriedDctFilterWithAHugeFrameIsNotDecoded
//         A DCT filter followed by another filter had no SOF check at all.
//       Both images must be reported Failed, quickly, without decoding.
//   (B) Masks and /Alternates are followed with a visited set.
//       ExifStripBounds.MutuallyReferencingAlternatesFinishFast
//         Two images whose /Alternates list each other N times each used to
//         cost on the order of N^3 visits; now each is visited once, quickly,
//         and both lose their metadata.
#include <gtest/gtest.h>
#include "image/ExifStripper.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QTemporaryDir>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

using namespace pdfe::core;
using pdfe::image::ExifStripper;

namespace {

using Bytes = std::string;
const char* kSecret = "PDFE-BOUNDS-EXIF-SECRET";

Bytes readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

Bytes segment(unsigned char marker, const Bytes& payload) {
    Bytes s;
    s.push_back(static_cast<char>(0xFF));
    s.push_back(static_cast<char>(marker));
    const size_t len = payload.size() + 2;
    s.push_back(static_cast<char>((len >> 8) & 0xFF));
    s.push_back(static_cast<char>(len & 0xFF));
    return s + payload;
}

// A 32 x 32 Qt JPEG.
Bytes qtJpeg() {
    QImage im(32, 32, QImage::Format_RGB32);
    im.fill(QColor(0, 0, 255));
    QByteArray ba;
    QBuffer qb(&ba);
    qb.open(QIODevice::WriteOnly);
    EXPECT_TRUE(im.save(&qb, "JPG", 95));
    return Bytes(ba.constData(), static_cast<size_t>(ba.size()));
}

// Inserts `seg` after SOI and the APP0/APP14 header segments.
Bytes insertAfterHeader(const Bytes& jpeg, const Bytes& seg) {
    size_t at = 2;
    while (at + 4 <= jpeg.size() && static_cast<unsigned char>(jpeg[at]) == 0xFF) {
        const unsigned char m = static_cast<unsigned char>(jpeg[at + 1]);
        if (m != 0xE0 && m != 0xEE) break;
        at += 2 + ((static_cast<unsigned char>(jpeg[at + 2]) << 8) | static_cast<unsigned char>(jpeg[at + 3]));
    }
    return jpeg.substr(0, at) + seg + jpeg.substr(at);
}

// An APP1 Exif segment carrying the secret.
Bytes exifSegment() {
    Bytes exif("Exif\0\0II*\0\x08\0\0\0\0\0\0\0\0\0", 20);
    exif += kSecret;
    return segment(0xE1, exif);
}

// An APP1 Exif segment whose declared length is 12 bytes short of its
// payload: the stream cannot be cut losslessly, so the stripper takes its
// decode-and-re-encode fallback (where the size bound applies).
Bytes badLengthExifSegment() {
    Bytes s = exifSegment();
    const size_t declared = s.size() - 2 - 12;
    s[2] = static_cast<char>((declared >> 8) & 0xFF);
    s[3] = static_cast<char>(declared & 0xFF);
    return s;
}

// Sets the frame header (the first FF C0) to 12000 x 12000.
void hugeFrame(Bytes& jpeg) {
    const size_t sof = jpeg.find(Bytes("\xFF\xC0", 2));
    ASSERT_NE(sof, Bytes::npos);
    jpeg[sof + 5] = static_cast<char>(12000 >> 8);
    jpeg[sof + 6] = static_cast<char>(12000 & 0xFF);
    jpeg[sof + 7] = static_cast<char>(12000 >> 8);
    jpeg[sof + 8] = static_cast<char>(12000 & 0xFF);
}

// An image XObject over `stored` with the given filter(s); NEW ref.
pdf_obj* addImage(fz_context* ctx, pdf_document* doc, const Bytes& stored, pdf_obj* filter) {
    fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(stored.data()),
                                                   stored.size());
    pdf_obj* d = pdf_new_dict(ctx, doc, 8);
    pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject));
    pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Image));
    pdf_dict_put_int(ctx, d, PDF_NAME(Width), 32);
    pdf_dict_put_int(ctx, d, PDF_NAME(Height), 32);
    pdf_dict_put_int(ctx, d, PDF_NAME(BitsPerComponent), 8);
    pdf_dict_put(ctx, d, PDF_NAME(ColorSpace), PDF_NAME(DeviceRGB));
    pdf_dict_put(ctx, d, PDF_NAME(Filter), filter);
    pdf_obj* img = pdf_add_stream(ctx, doc, b, d, 1);
    fz_drop_buffer(ctx, b);
    pdf_drop_obj(ctx, d);
    return img;
}

using Fill = std::function<void(fz_context*, pdf_document*, pdf_obj* res)>;

void buildPdf(MuPdfContext& mc, const std::string& path, const char* content, const Fill& fill) {
    mc.execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_create_document(ctx);
        pdf_obj* res = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 2);
        fill(ctx, doc, res);
        fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(content),
                                                       std::strlen(content));
        pdf_obj* page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 200, 200), 0, res, b);
        pdf_insert_page(ctx, doc, -1, page);
        pdf_drop_obj(ctx, page);
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, res);
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, doc, path.c_str(), &wo);
        pdf_drop_document(ctx, doc);
    });
}

class ExifStripBounds : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tmp_.isValid());
        mc_ = std::make_shared<MuPdfContext>();
    }
    std::string path(const char* n) const { return tmp_.filePath(n).toStdString(); }

    // stripImageResult on /Im1 of `file`, with its duration.
    ExifStripper::ImageResult stripOne(const std::string& file, long long& ms) {
        auto doc = Document::open(mc_, file);
        EXPECT_NE(doc, nullptr);
        auto page = doc->page(0);
        ExifStripper st(mc_);
        const auto t0 = std::chrono::steady_clock::now();
        const ExifStripper::ImageResult r = st.stripImageResult(page.get(), doc.get(), "Im1");
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        return r;
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> mc_;
};

} // namespace

// (A) FF DA inside a COM payload ahead of the real frame header must not end
// the search: the 12000 x 12000 frame is seen and the image is not decoded.
TEST_F(ExifStripBounds, ComSegmentHoldingFfDaDoesNotHideAHugeFrame) {
    Bytes jpeg = qtJpeg();
    hugeFrame(jpeg);
    const Bytes com = segment(0xFE, Bytes("PDFE\xFF\xDA\x00\x0C" "comment", 15));
    jpeg = insertAfterHeader(jpeg, com + badLengthExifSegment());
    ASSERT_LT(jpeg.find(Bytes("\xFF\xDA", 2)), jpeg.find(Bytes("\xFF\xC0", 2)))
        << "oracle: an FF DA pair precedes the frame header";
    const std::string in = path("com_ffda.pdf");
    buildPdf(*mc_, in, "q 100 0 0 100 50 50 cm /Im1 Do Q", [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addImage(ctx, doc, jpeg, PDF_NAME(DCTDecode));
        pdf_dict_puts(ctx, pdf_dict_get(ctx, res, PDF_NAME(XObject)), "Im1", img);
        pdf_drop_obj(ctx, img);
    });
    long long ms = 0;
    EXPECT_EQ(stripOne(in, ms), ExifStripper::ImageResult::Failed)
        << "the 12000 x 12000 frame behind FF DA in a comment was decoded";
    EXPECT_LT(ms, 1000) << "took " << ms << " ms";
}

// (A) A DCT filter that is not the last filter: the bytes it reads (here
// after a leading Flate) are scanned for the frame header too. A decoder let
// loose on this data also ends up reporting Failed (the filter after DCT gets
// garbage), so the time is what tells the two apart: with the bound the image
// is refused in a few milliseconds; without it (phase-5 mutation check) the
// decode took about 1 s, whatever the frame size. Hence a 250 ms bound.
TEST_F(ExifStripBounds, BuriedDctFilterWithAHugeFrameIsNotDecoded) {
    Bytes jpeg = qtJpeg();
    hugeFrame(jpeg);
    jpeg = insertAfterHeader(jpeg, exifSegment());
    Bytes deflated;
    mc_->execute([&](fz_context* ctx) {
        fz_buffer* src = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(jpeg.data()),
                                                         jpeg.size());
        size_t zlen = 0;
        unsigned char* z = fz_new_deflated_data_from_buffer(ctx, &zlen, src, FZ_DEFLATE_DEFAULT);
        deflated.assign(reinterpret_cast<const char*>(z), zlen);
        fz_free(ctx, z);
        fz_drop_buffer(ctx, src);
    });
    const std::string in = path("buried_huge.pdf");
    buildPdf(*mc_, in, "q 100 0 0 100 50 50 cm /Im1 Do Q", [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* filters = pdf_new_array(ctx, doc, 3);
        pdf_array_push(ctx, filters, PDF_NAME(FlateDecode));
        pdf_array_push(ctx, filters, PDF_NAME(DCTDecode));
        pdf_array_push(ctx, filters, PDF_NAME(RunLengthDecode));
        pdf_obj* img = addImage(ctx, doc, deflated, filters);
        pdf_drop_obj(ctx, filters);
        pdf_dict_puts(ctx, pdf_dict_get(ctx, res, PDF_NAME(XObject)), "Im1", img);
        pdf_drop_obj(ctx, img);
    });
    long long ms = 0;
    EXPECT_EQ(stripOne(in, ms), ExifStripper::ImageResult::Failed)
        << "a buried DCT filter's 12000 x 12000 frame was decoded";
    EXPECT_LT(ms, 250) << "took " << ms << " ms: the frame was decoded rather than refused";
}

// (B) Two JPEG images with EXIF whose /Alternates list each other N times
// each: the strip finishes quickly and both lose their EXIF. Without the
// visited set the depth limit alone ends the recursion, after about N^3
// visits: the phase-5 mutation check measured 878 ms for N = 64, so N is 128
// (about 8x that) against a bound of 1 s; with the set it takes milliseconds.
TEST_F(ExifStripBounds, MutuallyReferencingAlternatesFinishFast) {
    constexpr int kN = 128;
    const Bytes jpeg = insertAfterHeader(qtJpeg(), exifSegment());
    const std::string in = path("alts_in.pdf"), out = path("alts_out.pdf");
    buildPdf(*mc_, in, "q 100 0 0 100 50 50 cm /Im1 Do Q", [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* a = addImage(ctx, doc, jpeg, PDF_NAME(DCTDecode));
        pdf_obj* b = addImage(ctx, doc, jpeg, PDF_NAME(DCTDecode));
        for (pdf_obj* self : {a, b}) {
            pdf_obj* other = self == a ? b : a;
            pdf_obj* arr = pdf_new_array(ctx, doc, kN);
            for (int i = 0; i < kN; ++i) {
                pdf_obj* entry = pdf_new_dict(ctx, doc, 1);
                pdf_dict_put(ctx, entry, PDF_NAME(Image), other);
                pdf_array_push_drop(ctx, arr, entry);
            }
            pdf_dict_puts_drop(ctx, self, "Alternates", arr);
        }
        pdf_dict_puts(ctx, pdf_dict_get(ctx, res, PDF_NAME(XObject)), "Im1", a);
        pdf_drop_obj(ctx, a);
        pdf_drop_obj(ctx, b);
    });
    const Bytes fileIn = readFile(in);
    ASSERT_NE(fileIn.find(kSecret), Bytes::npos) << "oracle: the images carry EXIF";
    auto doc = Document::open(mc_, in);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ExifStripper st(mc_);
    const auto t0 = std::chrono::steady_clock::now();
    const ExifStripper::StripReport r = st.stripPageImagesReport(page.get(), doc.get());
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 1000) << "mutually referencing /Alternates took " << ms << " ms";
    EXPECT_EQ(r.stripped, 1);
    EXPECT_EQ(r.failed, 0);
    page.reset();
    doc->saveAs(out);
    doc.reset();
    const Bytes saved = readFile(out);
    EXPECT_EQ(saved.find(kSecret), Bytes::npos) << "an image's EXIF survived (not both were stripped)";
    EXPECT_NE(saved.find("Alternates"), Bytes::npos) << "the alternates themselves are kept";
}

namespace {

// A small JPEG with EXIF, followed by `padding` zero bytes, Flate-compressed
// and stored behind [FlateDecode DCTDecode Crypt]. The Crypt filter of an
// unencrypted document passes its data through (MuPDF warns and keeps the
// stream), so the chain decodes to the JPEG's pixels: DCT is "buried" (not
// last) and the image can be decoded and re-encoded. The JPEG decoder stops
// at EOI, so the padding only makes the bytes in front of DCT large.
void buildBuriedJpeg(MuPdfContext& mc, const std::string& file, size_t padding) {
    Bytes data = insertAfterHeader(qtJpeg(), exifSegment());
    data.append(padding, '\0');
    Bytes deflated;
    mc.execute([&](fz_context* ctx) {
        fz_buffer* src = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(data.data()),
                                                         data.size());
        size_t zlen = 0;
        unsigned char* z = fz_new_deflated_data_from_buffer(ctx, &zlen, src, FZ_DEFLATE_DEFAULT);
        deflated.assign(reinterpret_cast<const char*>(z), zlen);
        fz_free(ctx, z);
        fz_drop_buffer(ctx, src);
    });
    buildPdf(mc, file, "q 100 0 0 100 50 50 cm /Im1 Do Q", [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* filters = pdf_new_array(ctx, doc, 3);
        pdf_array_push(ctx, filters, PDF_NAME(FlateDecode));
        pdf_array_push(ctx, filters, PDF_NAME(DCTDecode));
        pdf_array_push(ctx, filters, PDF_NAME(Crypt));
        pdf_obj* img = addImage(ctx, doc, deflated, filters);
        pdf_drop_obj(ctx, filters);
        pdf_dict_puts(ctx, pdf_dict_get(ctx, res, PDF_NAME(XObject)), "Im1", img);
        pdf_drop_obj(ctx, img);
    });
}

} // namespace

// (A) The bytes read in front of a buried DCT filter are capped at 64 MB
// (kMaxBuriedCodecInput). Control: with 1 MB of padding the same image is
// decoded and re-encoded (Stripped), so the chain is decodable and nothing
// else refuses it. Over the cap (70 MB of padding, a ~70 KB file) it is
// reported Failed and left as it was.
TEST_F(ExifStripBounds, BuriedDctInputOverTheCapIsNotProcessed) {
    const std::string small = path("buried_small.pdf");
    buildBuriedJpeg(*mc_, small, 1u << 20);
    long long ms = 0;
    ASSERT_EQ(stripOne(small, ms), ExifStripper::ImageResult::Stripped)
        << "oracle: under the cap the buried-DCT image is decoded and re-encoded";

    const std::string big = path("buried_big.pdf");
    buildBuriedJpeg(*mc_, big, 70u << 20);
    ASSERT_LT(readFile(big).size(), 1u << 20) << "oracle: the file itself is small";
    auto doc = Document::open(mc_, big);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ExifStripper st(mc_);
    EXPECT_EQ(st.stripImageResult(page.get(), doc.get(), "Im1"), ExifStripper::ImageResult::Failed)
        << "more than 64 MB in front of a buried DCT filter was processed";
    const std::string out = path("buried_big_out.pdf");
    page.reset();
    doc->saveAs(out);
    doc.reset();
    bool flate = false;
    mc_->execute([&](fz_context* ctx) {
        pdf_document* d = pdf_open_document(ctx, out.c_str());
        pdf_obj* xo = pdf_dict_getp(ctx, pdf_lookup_page_obj(ctx, d, 0), "Resources/XObject/Im1");
        pdf_obj* f = pdf_dict_get(ctx, xo, PDF_NAME(Filter));
        flate = pdf_is_array(ctx, f) && pdf_array_len(ctx, f) == 3 &&
                pdf_name_eq(ctx, pdf_array_get(ctx, f, 1), PDF_NAME(DCTDecode));
        pdf_drop_document(ctx, d);
    });
    EXPECT_TRUE(flate) << "the refused image was changed (its filter chain is not the original)";
}
