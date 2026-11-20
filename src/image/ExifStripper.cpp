// -*- coding: utf-8 -*-
#include "image/ExifStripper.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/JpxSecurityGate.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace pdfe::image {

// How the stripper works (GitHub #452, #453).
//
// It used to decode EVERY image XObject to pixels, copy them through a QImage
// and add a fresh image in place of the old one. That lost the image
// dictionary (/SMask, /Mask, /ImageMask, /Decode, the colour space) and read a
// 4-component CMYK pixmap as RGBA, so masks vanished and CMYK colours were
// wrong. Pixels carry no metadata, so that round trip is not needed for a
// well-formed image.
//
// Metadata in a PDF image lives in two places, and the stripper removes it
// from exactly those places:
//
//  1. The image dictionary: /Metadata (an XMP stream) and the other
//     informational entries kInfoKeys lists. They are deleted.
//  2. The encoded data of a JPEG (DCTDecode) or JPEG 2000 (JPXDecode) stream:
//     EXIF/XMP (APP1), comments (COM), IPTC/Photoshop (APP13) and the other
//     application segments, anything after the JPEG's EOI (MPF images,
//     thumbnails, appended EXIF), or every JP2 box a decoder does not need
//     (a whitelist, jp2BoxKept). Those are cut out losslessly: the
//     compressed image data and the tables are copied byte for byte; the JFIF
//     header, a valid ICC profile chunk (APP2 ICC_PROFILE) and the Adobe
//     colour-transform marker (APP14, which says whether CMYK is stored
//     inverted) are kept at their standard size (classifyJpegSegment); and
//     the dictionary - masks, Decode, colour space - is kept.
//
// JPEG or JP2 data that does not parse cleanly (a bad segment length, bytes
// where a marker must be, data cut off before the scan) cannot be stripped
// that way, and leaving it would leave its metadata. It is decoded instead
// and its pixels are stored as a fresh Flate stream in the image's own
// device colour space (gray, RGB or CMYK; anything else becomes RGB), with
// /Decode applied. /SMask and /Mask stay. If it cannot even be decoded, the
// image is reported as NOT stripped (StripReport::failed, stripImage false);
// so is an image over the fallback's size cap (kMaxFallbackPixels), and JPX
// data while JPX decoding is off.
//
// The soft mask and the explicit mask of an image (and their own masks), and
// the images of its /Alternates, are streams of their own, and are treated
// the same way. A page strip also follows the Form XObjects in the page's
// resources and removes the page /Thumb. Inline images are not rewritten:
// those with JPEG data are counted (StripReport::inlineNotProcessed).
//
// An image with nothing to strip is not touched at all. Raw, Flate, LZW,
// RunLength, CCITT and JBIG2 data are bare samples with no room for metadata.

namespace {

// Image-dictionary entries that describe the image rather than draw it.
// /Metadata: XMP. /PieceInfo + /LastModified: private data of the producing
// application. /OPI: the path of the original high-resolution file. /Name and
// /ID: an old name and a Web-capture identifier.
const char* const kInfoKeys[] = {"Metadata", "PieceInfo", "LastModified", "OPI", "Name", "ID"};

enum class Scan { Clean, Stripped, Malformed };

bool startsWith(const unsigned char* p, size_t n, const char* sig, size_t sigLen)
{
    return n >= sigLen && std::memcmp(p, sig, sigLen) == 0;
}

// What to do with one JPEG marker segment (marker m, payload p of n bytes).
enum class Seg { Keep, Drop, Truncate };

// Sizes of the standard payloads of the three application segments a decoder
// reads. Anything a segment carries past them is not needed to decode.
constexpr size_t kJfifPayload = 14;   // "JFIF\0", version, units, densities, thumbnail size
constexpr size_t kAdobePayload = 12;  // "Adobe", version, flags0, flags1, transform
constexpr size_t kIccHeader = 14;     // "ICC_PROFILE\0", sequence number, chunk count

// Kept: every segment that is not an application or comment segment, plus
// the three application segments a decoder reads, cut to their standard
// size (`keep` payload bytes, Truncate):
//  - APP0 JFIF: the 14-byte header. Its thumbnail (Xthumb x Ythumb RGB
//    pixels, possibly of the uncropped original) and anything appended are
//    cut, and the thumbnail size is set to 0x0. A JFXX extension APP0 (a
//    thumbnail of its own) and any other APP0 are dropped.
//  - APP14 Adobe: the 12-byte payload (it says whether CMYK is inverted).
//  - APP2 ICC_PROFILE: kept whole when its chunk header is valid
//    ("ICC_PROFILE\0", then a sequence number 1..count, then a count >= 1),
//    dropped otherwise. The profile itself is kept as it is, including any
//    description or copyright tag in it (a known limit).
// A JFIF or Adobe segment shorter than its standard payload is dropped: a
// decoder ignores it anyway.
Seg classifyJpegSegment(unsigned char m, const unsigned char* p, size_t n, size_t& keep)
{
    keep = n;
    if (m == 0xFE) return Seg::Drop;                             // COM
    if (m < 0xE0 || m > 0xEF) return Seg::Keep;                  // not APPn
    if (m == 0xE0) {
        if (!startsWith(p, n, "JFIF\0", 5) || n < kJfifPayload) return Seg::Drop;
        keep = kJfifPayload;
        return (n == kJfifPayload && p[12] == 0 && p[13] == 0) ? Seg::Keep : Seg::Truncate;
    }
    if (m == 0xE2) {
        const bool valid = startsWith(p, n, "ICC_PROFILE\0", 12) && n >= kIccHeader &&
                           p[12] >= 1 && p[13] >= 1 && p[12] <= p[13];
        return valid ? Seg::Keep : Seg::Drop;
    }
    if (m == 0xEE) {
        if (!startsWith(p, n, "Adobe", 5) || n < kAdobePayload) return Seg::Drop;
        keep = kAdobePayload;
        return n == kAdobePayload ? Seg::Keep : Seg::Truncate;
    }
    return Seg::Drop;                                            // APP1 Exif/XMP, APP13 IPTC, ...
}

// Copies the JPEG in d[0..n) to `out` without its metadata segments and
// without anything after EOI. Clean: nothing to remove (out is unused).
// Stripped: out holds the cleaned JPEG. Malformed: the data does not parse
// cleanly, so segments cannot be found reliably; the caller must not trust
// it to be clean. The entropy-coded data, including any segments between
// progressive scans, is walked marker by marker. Data cut off inside a scan
// is accepted: every segment before the cut has been seen.
Scan stripJpegMetadata(const unsigned char* d, size_t n, std::vector<unsigned char>& out)
{
    out.clear();
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return Scan::Malformed;
    out.reserve(n);
    out.push_back(0xFF);
    out.push_back(0xD8);
    bool removed = false;
    bool inScan = false;
    size_t i = 2;
    while (i < n) {
        if (d[i] != 0xFF) {
            if (!inScan) return Scan::Malformed;            // a marker must be here
            // Entropy-coded data: copy up to the next real marker (0xFF then a
            // byte that is not a stuffed 0x00, a restart marker or fill).
            size_t j = i;
            while (j < n) {
                if (d[j] == 0xFF && j + 1 < n) {
                    const unsigned char m = d[j + 1];
                    if (m != 0x00 && m != 0xFF && !(m >= 0xD0 && m <= 0xD7)) break;
                }
                ++j;
            }
            out.insert(out.end(), d + i, d + j);
            i = j;
            continue;
        }
        size_t j = i;
        while (j < n && d[j] == 0xFF) ++j;                 // fill bytes
        if (j >= n) {                                      // cut off after fill
            if (!inScan) return Scan::Malformed;
            out.insert(out.end(), d + i, d + n);
            break;
        }
        const unsigned char m = d[j];
        if (m == 0xD8) return Scan::Malformed;             // a second SOI
        if (m == 0x00 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            if (m != 0x01 && !inScan) return Scan::Malformed;
            out.insert(out.end(), d + i, d + j + 1);      // no length field
            i = j + 1;
            continue;
        }
        if (m == 0xD9) {                                   // EOI: the image ends here
            out.push_back(0xFF);
            out.push_back(0xD9);
            if (j + 1 < n) removed = true;                 // trailer: MPF, thumbnails, EXIF
            return removed ? Scan::Stripped : Scan::Clean;
        }
        if (j + 2 >= n) return Scan::Malformed;
        const size_t len = (static_cast<size_t>(d[j + 1]) << 8) | d[j + 2];
        if (len < 2 || j + 1 + len > n) return Scan::Malformed;
        size_t keep = 0;
        const Seg action = classifyJpegSegment(m, d + j + 3, len - 2, keep);
        if (action == Seg::Drop) {
            removed = true;
        } else if (action == Seg::Truncate) {
            // The standard payload only, with the segment length rewritten.
            removed = true;
            out.push_back(0xFF);
            out.push_back(m);
            out.push_back(static_cast<unsigned char>(((keep + 2) >> 8) & 0xFF));
            out.push_back(static_cast<unsigned char>((keep + 2) & 0xFF));
            out.insert(out.end(), d + j + 3, d + j + 3 + keep);
            if (m == 0xE0) {                               // JFIF: no thumbnail
                out[out.size() - 2] = 0;
                out[out.size() - 1] = 0;
            }
        } else {
            out.push_back(0xFF);
            out.insert(out.end(), d + j, d + j + 1 + len);
        }
        if (m == 0xDA) inScan = true;                      // SOS: scan data follows
        i = j + 1 + len;
    }
    // No EOI: cut off. Before any scan the image is not decodable as parsed.
    if (!inScan) return Scan::Malformed;
    return removed ? Scan::Stripped : Scan::Clean;
}

uint64_t readBigEndian(const unsigned char* p, int bytes)
{
    uint64_t v = 0;
    for (int k = 0; k < bytes; ++k) v = (v << 8) | p[k];
    return v;
}

// The JP2 boxes a decoder needs, by nesting level. Everything else is
// dropped: xml (XMP), uuid, uinf, jp2i, asoc, lbl, free, the JPX-only jpch,
// jplh and their kin (MuPDF decodes through OpenJPEG, which reads only the
// signature, ftyp, jp2h and jp2c), and any private box.
enum class Jp2Level { Top, Header, Resolution };

bool boxIs(const unsigned char* type, const char* name)
{
    return std::memcmp(type, name, 4) == 0;
}

// Whether a box of `type` is kept at `level`, and the level of its children
// when it is a superbox that is filtered too (`child`, else it is copied
// whole).
bool jp2BoxKept(Jp2Level level, const unsigned char* type, bool& superbox, Jp2Level& child)
{
    superbox = false;
    switch (level) {
    case Jp2Level::Top:
        if (boxIs(type, "jp2h")) { superbox = true; child = Jp2Level::Header; return true; }
        return boxIs(type, "jP  ") || boxIs(type, "ftyp") || boxIs(type, "jp2c") || boxIs(type, "rreq");
    case Jp2Level::Header:
        if (boxIs(type, "res ")) { superbox = true; child = Jp2Level::Resolution; return true; }
        return boxIs(type, "ihdr") || boxIs(type, "bpcc") || boxIs(type, "colr") || boxIs(type, "pclr") ||
               boxIs(type, "cmap") || boxIs(type, "cdef");
    case Jp2Level::Resolution:
        return boxIs(type, "resc") || boxIs(type, "resd");
    }
    return false;
}

// Appends the kept boxes of d[begin..end) at `level` to `out`; a filtered
// superbox gets its length rewritten. Returns false when the box structure
// is broken: a header cut off, a length below the header or past the end of
// the enclosing box. A length of 0 runs to the end of the enclosing box;
// a length of 1 is followed by a 64-bit XLBox.
bool filterJp2Boxes(const unsigned char* d, size_t begin, size_t end, Jp2Level level,
                    std::vector<unsigned char>& out, bool& removed)
{
    size_t i = begin;
    while (i < end) {
        if (end - i < 8) return false;
        uint64_t boxLen = readBigEndian(d + i, 4);
        size_t header = 8;
        if (boxLen == 1) {
            if (end - i < 16) return false;
            boxLen = readBigEndian(d + i + 8, 8);
            header = 16;
        } else if (boxLen == 0) {
            boxLen = end - i;                               // runs to the end
        }
        if (boxLen < header || boxLen > end - i) return false;
        const unsigned char* type = d + i + 4;
        const size_t boxEnd = i + static_cast<size_t>(boxLen);
        bool superbox = false;
        Jp2Level child = level;
        if (!jp2BoxKept(level, type, superbox, child)) {
            removed = true;
        } else if (!superbox) {
            out.insert(out.end(), d + i, d + boxEnd);        // byte for byte
        } else {
            // Header with a placeholder length (the same header size, so a
            // filtered box always fits), the kept children, then the length.
            const size_t at = out.size();
            out.insert(out.end(), header, 0);
            std::memcpy(out.data() + at + 4, type, 4);
            if (!filterJp2Boxes(d, i + header, boxEnd, child, out, removed)) return false;
            const uint64_t newLen = out.size() - at;
            if (header == 16) {
                out[at + 3] = 1;
                for (int k = 0; k < 8; ++k)
                    out[at + 8 + k] = static_cast<unsigned char>(newLen >> (8 * (7 - k)));
            } else {
                if (newLen > 0xFFFFFFFFu) return false;
                for (int k = 0; k < 4; ++k)
                    out[at + k] = static_cast<unsigned char>(newLen >> (8 * (3 - k)));
            }
        }
        i = boxEnd;
    }
    return true;
}

// Copies a JP2/JPX file in d[0..n) to `out` keeping only the boxes a decoder
// needs (a whitelist, jp2BoxKept): the signature, ftyp, rreq, the jp2h header
// superbox with its ihdr/bpcc/colr/pclr/cmap/cdef/res children, and the jp2c
// codestream. Same results as stripJpegMetadata. A bare JPEG 2000 codestream
// (no box structure) has no boxes and is Clean. Known limits: a colr box may
// hold an ICC profile, kept whole like an APP2 ICC_PROFILE; COM marker
// segments inside the codestream are not looked at.
Scan stripJp2Metadata(const unsigned char* d, size_t n, std::vector<unsigned char>& out)
{
    static const unsigned char kSignature[12] = {0, 0, 0, 0x0C, 'j', 'P', ' ', ' ', 0x0D, 0x0A, 0x87, 0x0A};
    out.clear();
    if (n >= 2 && d[0] == 0xFF && d[1] == 0x4F) return Scan::Clean;   // bare codestream (SOC)
    if (n < 12 || std::memcmp(d, kSignature, 12) != 0) return Scan::Malformed;
    out.reserve(n);
    bool removed = false;
    if (!filterJp2Boxes(d, 0, n, Jp2Level::Top, out, removed)) return Scan::Malformed;
    return removed ? Scan::Stripped : Scan::Clean;
}

// The largest image the fallback below decodes: 100 megapixels, and at most
// 4 bytes per pixel of samples (400 MB). The app has no general decode limit
// to share; the only one, the JPX gate's 600-megapixel bound
// (core/JpxSecurityGate.h), is enforced inside MuPDF's JPX decoder only. The
// fallback holds the decoded pixmap, a converted copy and the raw samples at
// once, so it takes a smaller bound. A legitimate photo is far below it (a
// 50-megapixel camera frame is half of it); a page scan at 600 dpi on A3 is
// about 70 megapixels.
constexpr uint64_t kMaxFallbackPixels = 100'000'000;
constexpr uint64_t kMaxFallbackSampleBytes = kMaxFallbackPixels * 4;

// Components per pixel of an image colour space, read from the dictionary
// without loading it. Unknown or missing: 4 (the most a device space has).
uint64_t colourComponents(fz_context* ctx, pdf_obj* xobj)
{
    if (pdf_dict_get_bool(ctx, xobj, PDF_NAME(ImageMask))) return 1;
    pdf_obj* cs = pdf_dict_get(ctx, xobj, PDF_NAME(ColorSpace));
    if (pdf_is_array(ctx, cs)) {
        pdf_obj* family = pdf_array_get(ctx, cs, 0);
        if (pdf_name_eq(ctx, family, PDF_NAME(ICCBased))) {
            const int n = pdf_dict_get_int(ctx, pdf_array_get(ctx, cs, 1), PDF_NAME(N));
            return n > 0 ? static_cast<uint64_t>(n) : 4;
        }
        if (pdf_name_eq(ctx, family, PDF_NAME(DeviceN))) {
            const int n = pdf_array_len(ctx, pdf_array_get(ctx, cs, 1));
            return n > 0 ? static_cast<uint64_t>(n) : 4;
        }
        if (pdf_name_eq(ctx, family, PDF_NAME(Separation)) || pdf_name_eq(ctx, family, PDF_NAME(CalGray)))
            return 1;
        if (pdf_name_eq(ctx, family, PDF_NAME(CalRGB)) || pdf_name_eq(ctx, family, PDF_NAME(Lab))) return 3;
        return 4;                                           // Indexed (expands to its base), Pattern, ...
    }
    if (pdf_name_eq(ctx, cs, PDF_NAME(DeviceGray)) || pdf_name_eq(ctx, cs, PDF_NAME(G))) return 1;
    if (pdf_name_eq(ctx, cs, PDF_NAME(DeviceRGB)) || pdf_name_eq(ctx, cs, PDF_NAME(RGB))) return 3;
    return 4;
}

bool overFallbackLimit(uint64_t w, uint64_t h, uint64_t n)
{
    if (w == 0 || h == 0) return false;                     // the decoder rejects these itself
    if (w > kMaxFallbackPixels || h > kMaxFallbackPixels / w) return true;
    return w * h * std::max<uint64_t>(n, 1) > kMaxFallbackSampleBytes;
}

// The JPEG decoder works from the frame header (SOFn), not from the PDF
// dictionary, so a JPEG's own dimensions are bounded too: every byte pair
// that looks like a SOFn, anywhere in the data. The search does not trust
// segment lengths (the data is malformed) and does not stop at the first
// FF DA either: those two bytes can sit inside a COM or APPn payload, ahead
// of the real frame header, and stopping there would hide it from the bound.
// Entropy-coded data never holds FF C0..CF (an FF there is stuffed as FF 00),
// so a byte pair that only looks like a SOF makes the bound stricter, never
// looser.
bool jpegFrameOverLimit(const unsigned char* d, size_t n)
{
    for (size_t i = 0; i + 9 < n; ++i) {
        if (d[i] != 0xFF) continue;
        const unsigned char m = d[i + 1];
        const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (!sof) continue;
        const uint64_t h = (static_cast<uint64_t>(d[i + 5]) << 8) | d[i + 6];
        const uint64_t w = (static_cast<uint64_t>(d[i + 7]) << 8) | d[i + 8];
        if (overFallbackLimit(w, h, d[i + 9])) return true;
    }
    return false;
}

// The fallback for encoded data that does not parse cleanly: decode the image
// and store its pixels as a fresh Flate stream. The pixels stay in the image's
// device colour space when it is gray, RGB or CMYK (CMYK is NOT turned into
// RGB; Adobe-inverted CMYK comes out of the decoder already un-inverted) and
// become RGB otherwise. /Decode has been applied by the decoder, so it is
// removed; /SMask and /Mask are kept. Throws when the image cannot be decoded.
// Known limits: an ICCBased (or other non-device) colour space becomes the
// matching Device space; a colour-key /Mask array is kept as is and is only
// right for 8-bit samples with the default /Decode; a JPX image whose
// transparency came from /SMaskInData loses it (the Flate stream carries no
// alpha and /SMaskInData is removed). See docs/image-placement-model.md.
//
// Before anything is decoded, the size is bounded (kMaxFallbackPixels): the
// dictionary's /Width x /Height x components, and for JPEG data (`enc`, the
// bytes the DCT decoder reads) every frame header's. JPEG data whose bytes
// could not be read (`enc` null) is never decoded: its frame cannot be
// bounded. A larger image throws, so it is reported as not processed rather
// than decoded (a decompression bomb would otherwise allocate gigabytes). JPX
// data also throws while JPX decoding is off (core/JpxSecurityGate.h): MuPDF
// then returns a 1x1 placeholder, which must never replace the image.
void reencodeImagePixels(fz_context* ctx, pdf_document* doc, pdf_obj* xobj, bool dct, bool jpx,
                         const unsigned char* enc, size_t encLen)
{
    const int dictW = pdf_dict_get_int(ctx, xobj, PDF_NAME(Width));
    const int dictH = pdf_dict_get_int(ctx, xobj, PDF_NAME(Height));
    if (overFallbackLimit(static_cast<uint64_t>(std::max(dictW, 0)), static_cast<uint64_t>(std::max(dictH, 0)),
                          colourComponents(ctx, xobj)))
        fz_throw(ctx, FZ_ERROR_LIMIT, "image too large to re-encode (%d x %d)", dictW, dictH);
    if (dct && !enc)
        fz_throw(ctx, FZ_ERROR_LIMIT, "JPEG data could not be read; its frame size cannot be bounded");
    if (dct && jpegFrameOverLimit(enc, encLen))
        fz_throw(ctx, FZ_ERROR_LIMIT, "JPEG frame too large to re-encode");
    if (jpx && !pdfe::core::isJpxDecodingEnabled())
        fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "JPX decoding is off; the image cannot be re-encoded");

    fz_image* img = nullptr;
    fz_pixmap* pix = nullptr;
    fz_pixmap* conv = nullptr;
    fz_buffer* raw = nullptr;
    fz_buffer* deflated = nullptr;
    unsigned char* z = nullptr;
    fz_var(img);
    fz_var(pix);
    fz_var(conv);
    fz_var(raw);
    fz_var(deflated);
    fz_var(z);
    fz_try(ctx) {
        img = pdf_load_image(ctx, doc, xobj);
        pix = fz_get_pixmap_from_image(ctx, img, nullptr, nullptr, nullptr, nullptr);

        fz_colorspace* cs = fz_pixmap_colorspace(ctx, pix);
        fz_colorspace* target = fz_device_rgb(ctx);
        const char* csName = "DeviceRGB";
        if (cs && fz_colorspace_is_gray(ctx, cs)) { target = fz_device_gray(ctx); csName = "DeviceGray"; }
        else if (cs && fz_colorspace_is_cmyk(ctx, cs)) { target = fz_device_cmyk(ctx); csName = "DeviceCMYK"; }
        fz_pixmap* use = pix;
        if (cs != target || pix->alpha) {
            conv = fz_convert_pixmap(ctx, pix, target, nullptr, nullptr, fz_default_color_params, 0);
            use = conv;
        }

        const size_t rowBytes = static_cast<size_t>(use->w) * use->n;
        raw = fz_new_buffer(ctx, rowBytes * static_cast<size_t>(use->h) + 1);
        for (int y = 0; y < use->h; ++y)
            fz_append_data(ctx, raw, use->samples + static_cast<size_t>(y) * use->stride, rowBytes);
        size_t zlen = 0;
        z = fz_new_deflated_data_from_buffer(ctx, &zlen, raw, FZ_DEFLATE_DEFAULT);
        deflated = fz_new_buffer_from_data(ctx, z, zlen);
        z = nullptr;                                        // owned by `deflated`

        pdf_update_stream(ctx, doc, xobj, deflated, 1);
        pdf_dict_put(ctx, xobj, PDF_NAME(Filter), PDF_NAME(FlateDecode));
        pdf_dict_del(ctx, xobj, PDF_NAME(DecodeParms));
        pdf_dict_del(ctx, xobj, PDF_NAME(Decode));
        pdf_dict_del(ctx, xobj, PDF_NAME(SMaskInData));
        pdf_dict_put_int(ctx, xobj, PDF_NAME(Width), use->w);
        pdf_dict_put_int(ctx, xobj, PDF_NAME(Height), use->h);
        pdf_dict_put_int(ctx, xobj, PDF_NAME(BitsPerComponent), 8);
        pdf_dict_put_name(ctx, xobj, PDF_NAME(ColorSpace), csName);
    }
    fz_always(ctx) {
        fz_free(ctx, z);
        fz_drop_buffer(ctx, deflated);
        fz_drop_buffer(ctx, raw);
        fz_drop_pixmap(ctx, conv);
        fz_drop_pixmap(ctx, pix);
        fz_drop_image(ctx, img);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

// How deep the masks of masks and the /Alternates of an image are followed.
constexpr int kMaxImageDepth = 3;

// The most bytes read in front of a DCT filter that is not the last filter
// (see readCodecInput). More is reported as not processed.
constexpr size_t kMaxBuriedCodecInput = 64u * 1024u * 1024u;

bool isImageCodecFilter(fz_context* ctx, pdf_obj* f, bool& dct, bool& jpx)
{
    dct = pdf_name_eq(ctx, f, PDF_NAME(DCTDecode)) || pdf_name_eq(ctx, f, PDF_NAME(DCT));
    jpx = pdf_name_eq(ctx, f, PDF_NAME(JPXDecode));
    return dct || jpx;
}

// The bytes the filter at index `codec` of the array `filters` reads: the
// stored (decrypted) data with the filters in front of it undone, and none
// after it. Used for a DCT filter that is not the last one, so that its frame
// header can be bounded before the image is decoded. At most
// kMaxBuriedCodecInput bytes; more throws. Returns a NEW buffer.
fz_buffer* readCodecInput(fz_context* ctx, pdf_document* doc, pdf_obj* xobj, pdf_obj* filters, int codec)
{
    fz_stream* raw = nullptr;
    fz_stream* chain = nullptr;
    pdf_obj* front = nullptr;
    fz_buffer* out = nullptr;
    fz_var(raw);
    fz_var(chain);
    fz_var(front);
    fz_var(out);
    fz_try(ctx) {
        raw = pdf_open_raw_stream(ctx, xobj);
        if (codec > 0) {
            pdf_obj* parms = pdf_dict_get(ctx, xobj, PDF_NAME(DecodeParms));
            front = pdf_new_dict(ctx, doc, 2);
            pdf_obj* fs = pdf_dict_put_array(ctx, front, PDF_NAME(Filter), codec);
            pdf_obj* ps = pdf_dict_put_array(ctx, front, PDF_NAME(DecodeParms), codec);
            for (int k = 0; k < codec; ++k) {
                pdf_array_push(ctx, fs, pdf_array_get(ctx, filters, k));
                pdf_obj* p = pdf_is_array(ctx, parms) ? pdf_array_get(ctx, parms, k) : nullptr;
                pdf_array_push(ctx, ps, p ? p : PDF_NULL);
            }
            chain = pdf_open_inline_stream(ctx, doc, front, 0, raw, nullptr);
        } else {
            chain = fz_keep_stream(ctx, raw);
        }
        out = fz_new_buffer(ctx, 64 * 1024);
        unsigned char chunk[16 * 1024];
        for (;;) {
            const size_t got = fz_read(ctx, chain, chunk, sizeof chunk);
            if (got == 0) break;
            if (fz_buffer_storage(ctx, out, nullptr) + got > kMaxBuriedCodecInput)
                fz_throw(ctx, FZ_ERROR_LIMIT, "JPEG data in front of a later filter is too large to check");
            fz_append_data(ctx, out, chunk, got);
        }
    }
    fz_always(ctx) {
        fz_drop_stream(ctx, chain);
        fz_drop_stream(ctx, raw);
        pdf_drop_obj(ctx, front);
    }
    fz_catch(ctx) {
        fz_drop_buffer(ctx, out);
        fz_rethrow(ctx);
    }
    return out;
}

// Marks `obj` as visited by the mask and /Alternates recursion; false when it
// already was. A direct object has no identity and is always visited (a
// stream is always indirect, so this does not happen for an image).
bool firstNestedVisit(fz_context* ctx, std::unordered_set<int>& seen, pdf_obj* obj)
{
    const int num = pdf_to_num(ctx, obj);
    if (num <= 0) return true;
    return seen.insert(num).second;
}

// Removes the metadata of one image XObject (and of its /SMask and /Mask
// streams, their own masks, and the images of its /Alternates), in place.
// Returns true when anything was removed, false when there
// was nothing to remove. Throws (MuPDF) when the image could not be stripped:
// its data can neither be read nor, if malformed, decoded. `scratch` and
// `nested` are caller-owned: this runs inside guarded MuPDF regions, where a
// local with a destructor would leak on a longjmp
// (tools/gate/fz_try_raii_scan.py). `nested` holds the object numbers already
// visited through masks and /Alternates: each is stripped once per walk, so
// images whose /Alternates list each other many times cost one visit each,
// not one per path (that was on the order of N^3 for two such images).
bool stripImageObject(fz_context* ctx, pdf_document* doc, pdf_obj* xobj,
                      std::vector<unsigned char>& scratch, std::unordered_set<int>& nested, int depth = 0)
{
    bool changed = false;
    fz_var(changed);
    firstNestedVisit(ctx, nested, xobj);                    // a mask or alternate naming it ends there

    for (const char* key : kInfoKeys) {
        if (pdf_dict_gets(ctx, xobj, key)) {
            pdf_dict_dels(ctx, xobj, key);
            changed = true;
        }
    }

    // The JPEG / JPX filter is normally the last one. One that is followed
    // by another filter cannot be cut losslessly here (the bytes it decodes
    // are not the stored JPEG), so that stream is handled as malformed: it is
    // decoded and re-encoded, or reported as not processed. It is never
    // reported clean while untouched.
    pdf_obj* filter = pdf_dict_get(ctx, xobj, PDF_NAME(Filter));
    const int nFilters = pdf_is_array(ctx, filter) ? pdf_array_len(ctx, filter) : 0;
    bool dct = false, jpx = false, buried = false;
    int dctAt = -1;                                         // index of the first DCT filter
    if (nFilters > 0) {
        for (int k = 0; k < nFilters; ++k) {
            bool d = false, j = false;
            if (isImageCodecFilter(ctx, pdf_array_get(ctx, filter, k), d, j)) {
                if (d && dctAt < 0) dctAt = k;
                dct = dct || d;
                jpx = jpx || j;
                if (k != nFilters - 1) buried = true;
            }
        }
    } else {
        isImageCodecFilter(ctx, filter, dct, jpx);
    }

    if ((dct || jpx) && pdf_is_stream(ctx, xobj)) {
        fz_compressed_buffer* cbuf = nullptr;
        fz_buffer* data = nullptr;
        fz_buffer* clean = nullptr;
        fz_buffer* front = nullptr;
        pdf_obj* parms = nullptr;
        Scan scan = Scan::Malformed;
        fz_var(cbuf);
        fz_var(data);
        fz_var(clean);
        fz_var(front);
        fz_var(parms);
        fz_var(scan);
        fz_try(ctx) {
            // The encoded JPEG / JPX bytes, with any filters in front of the
            // image filter (e.g. Flate) already undone.
            if (buried) {
                // Not cut: Malformed, so it goes to the fallback. A buried
                // DCT filter's input is still read, only to bound its frame
                // header before anything is decoded.
                if (dct) front = readCodecInput(ctx, doc, xobj, filter, dctAt);
            } else if (dct) {
                cbuf = pdf_load_compressed_stream(ctx, doc, pdf_to_num(ctx, xobj), 0);
                if (cbuf->params.type == FZ_IMAGE_JPEG) data = fz_keep_buffer(ctx, cbuf->buffer);
            } else {
                data = pdf_load_stream(ctx, xobj);   // JPXDecode passes the bytes through
            }
            if (data) {
                unsigned char* bytes = nullptr;
                const size_t len = fz_buffer_storage(ctx, data, &bytes);
                scan = dct ? stripJpegMetadata(bytes, len, scratch)
                           : stripJp2Metadata(bytes, len, scratch);
            }
            if (scan == Scan::Stripped) {
                // Keep the image filter's own parameters (DCT ColorTransform).
                pdf_obj* dp = pdf_dict_get(ctx, xobj, PDF_NAME(DecodeParms));
                if (pdf_is_array(ctx, dp)) dp = pdf_array_get(ctx, dp, pdf_array_len(ctx, dp) - 1);
                if (pdf_is_dict(ctx, dp)) parms = pdf_keep_obj(ctx, dp);

                clean = fz_new_buffer_from_copied_data(ctx, scratch.data(), scratch.size());
                pdf_update_stream(ctx, doc, xobj, clean, 1);
                pdf_dict_put(ctx, xobj, PDF_NAME(Filter), dct ? PDF_NAME(DCTDecode) : PDF_NAME(JPXDecode));
                if (parms) pdf_dict_put(ctx, xobj, PDF_NAME(DecodeParms), parms);
                else pdf_dict_del(ctx, xobj, PDF_NAME(DecodeParms));
                changed = true;
            } else if (scan == Scan::Malformed) {
                // Cannot be cut losslessly: re-encode the pixels, or throw.
                fz_buffer* enc = data ? data : front;
                unsigned char* bytes = nullptr;
                const size_t len = enc ? fz_buffer_storage(ctx, enc, &bytes) : 0;
                reencodeImagePixels(ctx, doc, xobj, dct, jpx, enc ? bytes : nullptr, len);
                changed = true;
            }
        }
        fz_always(ctx) {
            pdf_drop_obj(ctx, parms);
            fz_drop_buffer(ctx, front);
            fz_drop_buffer(ctx, clean);
            fz_drop_buffer(ctx, data);
            fz_drop_compressed_buffer(ctx, cbuf);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    }

    // The soft mask and an explicit mask are images too (a /Mask array is a
    // colour-key range, not a stream, and has nothing to strip), and so is a
    // mask's own mask, down to kMaxImageDepth. /Alternates holds alternate
    // versions of the image (an array of dictionaries, each with its /Image
    // stream); each is stripped the same way. An image already visited in
    // this walk (`nested`, which also ends a cycle) is not visited again.
    if (depth < kMaxImageDepth) {
        for (pdf_obj* key : {PDF_NAME(SMask), PDF_NAME(Mask)}) {
            pdf_obj* mask = pdf_dict_get(ctx, xobj, key);
            if (pdf_is_stream(ctx, mask) && firstNestedVisit(ctx, nested, mask) &&
                stripImageObject(ctx, doc, mask, scratch, nested, depth + 1))
                changed = true;
        }
        pdf_obj* alternates = pdf_dict_gets(ctx, xobj, "Alternates");
        const int nAlternates = pdf_is_array(ctx, alternates) ? pdf_array_len(ctx, alternates) : 0;
        for (int k = 0; k < nAlternates; ++k) {
            pdf_obj* alt = pdf_dict_get(ctx, pdf_array_get(ctx, alternates, k), PDF_NAME(Image));
            if (pdf_is_stream(ctx, alt) && firstNestedVisit(ctx, nested, alt) &&
                stripImageObject(ctx, doc, alt, scratch, nested, depth + 1))
                changed = true;
        }
    }
    return changed;
}

// ---------------------------------------------------------------- page walk

// How deep Form XObjects are followed (the placement scanner's bound).
constexpr int kMaxFormDepth = 8;

// The state of one page's walk, owned by the CALLER's frame (it has
// destructors; the walk runs inside MuPDF's setjmp regions). Images and forms
// are visited once each by object number: an image drawn by two forms, or a
// form shared by two others, is counted once, and a form that contains itself
// ends the recursion.
struct PageWalk {
    std::vector<unsigned char> scratch;
    std::vector<int> images;
    std::vector<int> forms;
    std::unordered_set<int> nested;     // masks and alternates (stripImageObject)
};

bool firstVisit(fz_context* ctx, std::vector<int>& seen, pdf_obj* obj)
{
    const int num = pdf_to_num(ctx, obj);
    if (num <= 0) return true;                              // a direct object: no identity
    if (std::find(seen.begin(), seen.end(), num) != seen.end()) return false;
    seen.push_back(num);
    return true;
}

// Strips every image of the /XObject dictionary of `res`, and the images of
// the Form XObjects in it, recursively (to kMaxFormDepth).
void stripResourceImages(fz_context* ctx, pdf_document* doc, pdf_obj* res, int depth, PageWalk& walk,
                         ExifStripper::StripReport& report)
{
    pdf_obj* xobjects = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    const int n = pdf_dict_len(ctx, xobjects);
    for (int i = 0; i < n; ++i) {
        pdf_obj* xobj = pdf_dict_get_val(ctx, xobjects, i);
        if (!xobj) continue;
        pdf_obj* subtype = pdf_dict_get(ctx, xobj, PDF_NAME(Subtype));
        if (pdf_name_eq(ctx, subtype, PDF_NAME(Image))) {
            if (!firstVisit(ctx, walk.images, xobj)) continue;
            fz_try(ctx) {
                if (stripImageObject(ctx, doc, xobj, walk.scratch, walk.nested)) ++report.stripped;
                else ++report.alreadyClean;
            }
            fz_catch(ctx) {
                ++report.failed;   // could not be stripped; continue with the next
            }
        } else if (pdf_name_eq(ctx, subtype, PDF_NAME(Form)) && depth < kMaxFormDepth &&
                   firstVisit(ctx, walk.forms, xobj)) {
            stripResourceImages(ctx, doc, pdf_dict_get(ctx, xobj, PDF_NAME(Resources)), depth + 1, walk, report);
        }
    }
}

// Counts the inline images (BI ... ID ... EI) of a page's content and of the
// Form XObjects it draws whose data is JPEG or JPEG 2000: those may carry
// application segments, and the stripper does not rewrite content streams,
// so they are not stripped. Inline images in other filters hold bare samples.
// A MuPDF content processor with only the BI, Do-form and resource callbacks
// set: nothing is drawn and no XObject image is loaded. POD only: it is
// allocated by MuPDF.
constexpr int kMaxCountedForms = 256;
constexpr int kResourceSlots = kMaxFormDepth + 2;

struct InlineImageCounter {
    pdf_processor super;
    pdf_document* doc;
    int encoded;
    int depth;
    int resTop;
    pdf_obj* res[kResourceSlots];
    int formCount;
    int forms[kMaxCountedForms];
};

void counterPushResources(fz_context* ctx, pdf_processor* proc, pdf_obj* res)
{
    auto* c = reinterpret_cast<InlineImageCounter*>(proc);
    if (c->resTop < kResourceSlots) c->res[c->resTop] = pdf_keep_obj(ctx, res);
    ++c->resTop;
}

pdf_obj* counterPopResources(fz_context*, pdf_processor* proc)
{
    auto* c = reinterpret_cast<InlineImageCounter*>(proc);
    if (c->resTop <= 0) return nullptr;
    --c->resTop;
    return c->resTop < kResourceSlots ? c->res[c->resTop] : nullptr;   // the caller drops it
}

void counterBI(fz_context* ctx, pdf_processor* proc, fz_image* image, const char*)
{
    fz_compressed_buffer* cb = fz_compressed_image_buffer(ctx, image);
    if (cb && (cb->params.type == FZ_IMAGE_JPEG || cb->params.type == FZ_IMAGE_JPX))
        ++reinterpret_cast<InlineImageCounter*>(proc)->encoded;
}

void counterDoForm(fz_context* ctx, pdf_processor* proc, const char*, pdf_obj* form)
{
    auto* c = reinterpret_cast<InlineImageCounter*>(proc);
    if (c->depth >= kMaxFormDepth || c->formCount >= kMaxCountedForms) return;
    const int num = pdf_to_num(ctx, form);
    for (int k = 0; k < c->formCount; ++k)
        if (c->forms[k] == num) return;                     // shared, or a cycle
    c->forms[c->formCount++] = num;
    pdf_obj* res = pdf_dict_get(ctx, form, PDF_NAME(Resources));
    if (!res && c->resTop > 0 && c->resTop <= kResourceSlots) res = c->res[c->resTop - 1];
    ++c->depth;
    fz_try(ctx) {
        pdf_process_contents(ctx, proc, c->doc, res, form, nullptr, nullptr);
    }
    fz_always(ctx) {
        --c->depth;
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

int countEncodedInlineImages(fz_context* ctx, pdf_document* doc, pdf_obj* pageObj, pdf_obj* resources)
{
    InlineImageCounter* c = nullptr;
    int count = 0;
    fz_var(c);
    fz_var(count);
    fz_try(ctx) {
        c = static_cast<InlineImageCounter*>(pdf_new_processor(ctx, sizeof(InlineImageCounter)));
        c->doc = doc;
        c->super.push_resources = counterPushResources;
        c->super.pop_resources = counterPopResources;
        c->super.op_BI = counterBI;
        c->super.op_Do_form = counterDoForm;
        pdf_process_contents(ctx, &c->super, doc, resources, pdf_dict_get(ctx, pageObj, PDF_NAME(Contents)),
                             nullptr, nullptr);
        pdf_close_processor(ctx, &c->super);
        count = c->encoded;
    }
    fz_always(ctx) {
        if (c) {
            // Resources still pushed when the content broke off.
            while (c->resTop > 0) pdf_drop_obj(ctx, counterPopResources(ctx, &c->super));
            if (!c->super.closed) pdf_close_processor(ctx, &c->super);
            pdf_drop_processor(ctx, &c->super);
        }
    }
    fz_catch(ctx) {
        // Content that cannot be parsed draws nothing more; keep what was counted.
        fz_rethrow_if(ctx, FZ_ERROR_SYSTEM);
        fz_report_error(ctx);
    }
    return count;
}

} // anonymous namespace

class ExifStripper::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    pdf_page* getPdfPage(const pdfe::core::Page* page) const {
        fz_page* fzPage = page->fzPage();
        return pdf_page_from_fz_page(nullptr, fzPage);
    }

    pdf_document* getPdfDoc(const pdfe::core::Document* doc) const {
        fz_document* fzDoc = doc->fzDocument();
        return pdf_document_from_fz_document(nullptr, fzDoc);
    }
};

ExifStripper::ExifStripper(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context))) {
}

ExifStripper::~ExifStripper() = default;

QImage ExifStripper::stripExif(const QImage& image) {
    if (image.isNull()) return QImage();

    // NOTE: a PNG save->loadFromData round-trip does NOT strip metadata --
    // Qt writes QImage::text() entries as PNG tEXt/zTXt chunks and reads them
    // straight back, so embedded GPS/camera/Description tags survive. To
    // actually scrub privacy metadata we rebuild a fresh QImage from the raw
    // pixels only, which carries no text() metadata by construction.
    QImage clean(image.width(), image.height(), image.format());
    if (clean.isNull()) return image;  // unsupported/degenerate format: fail safe

    const int bytesPerLine = std::min(image.bytesPerLine(), clean.bytesPerLine());
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(clean.scanLine(y), image.constScanLine(y),
                    static_cast<size_t>(bytesPerLine));
    }
    // Preserve the palette for indexed formats (pixels index into it).
    if (!image.colorTable().isEmpty()) {
        clean.setColorTable(image.colorTable());
    }
    // clean has an empty text() map -> all embedded metadata is gone.
    return clean;
}

ExifStripper::ImageResult ExifStripper::stripImageResult(const pdfe::core::Page* page,
                                                         const pdfe::core::Document* doc,
                                                         const std::string& xobjectName) {
    if (!page || !doc || xobjectName.empty()) return ImageResult::NotFound;
    // Stripping EXIF removes metadata from the image stream; force a
    // full-rewrite save so the original (privacy-sensitive) bytes are
    // scrubbed rather than left orphaned (pipeline b2e47d91, Tier 1).
    doc->markRequiresFullRewrite();

    ImageResult result = ImageResult::NotFound;

    // Owned by THIS frame -- the CALLER's frame, not the lambda's. The lambda
    // body below is the body of MuPdfContext::execute's fz_try, which is
    // setjmp/longjmp: an error from any pdf_*/fz_* call abandons the lambda's
    // entire frame without running destructors (tools/gate/fz_try_raii_scan.py).
    std::vector<unsigned char> scratch;
    std::unordered_set<int> nested;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        if (!pdfPage || !pdfDoc) return;

        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        if (!resources) return;

        pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (!xobjectDict) return;

        pdf_obj* xobj = pdf_dict_gets(ctx, xobjectDict, xobjectName.c_str());
        if (!xobj) return;

        pdf_obj* subtype = pdf_dict_get(ctx, xobj, PDF_NAME(Subtype));
        if (!pdf_name_eq(ctx, subtype, PDF_NAME(Image))) return;

        fz_try(ctx) {
            result = stripImageObject(ctx, pdfDoc, xobj, scratch, nested) ? ImageResult::Stripped
                                                                  : ImageResult::AlreadyClean;
        }
        fz_catch(ctx) {
            result = ImageResult::Failed;   // could not be stripped: its metadata may remain
        }
    });

    return result;
}

bool ExifStripper::stripImage(const pdfe::core::Page* page, const pdfe::core::Document* doc,
                               const std::string& xobjectName) {
    const ImageResult r = stripImageResult(page, doc, xobjectName);
    return r == ImageResult::Stripped || r == ImageResult::AlreadyClean;
}

ExifStripper::StripReport ExifStripper::stripPageImagesReport(const pdfe::core::Page* page,
                                                              const pdfe::core::Document* doc) {
    StripReport report;
    if (!page || !doc) return report;
    doc->markRequiresFullRewrite();  // EXIF strip orphans original image bytes → full rewrite

    // In the CALLER's frame for the same reason as stripImageResult() above.
    PageWalk walk;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        if (!pdfPage || !pdfDoc) return;

        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        if (resources) {
            // The page's images, and those inside the Form XObjects it uses.
            stripResourceImages(ctx, pdfDoc, resources, 0, walk, report);
        }

        // A page thumbnail (/Thumb) is a picture of the page as it was: it
        // can show content since removed or cropped away. Viewers regenerate
        // thumbnails, and /Thumb was not used anywhere in src/ when checked
        // (2026-11-18), so it is removed.
        if (pdf_dict_get(ctx, pdfPage->obj, PDF_NAME(Thumb))) {
            pdf_dict_del(ctx, pdfPage->obj, PDF_NAME(Thumb));
            ++report.thumbnailsRemoved;
        }

        report.inlineNotProcessed = countEncodedInlineImages(ctx, pdfDoc, pdfPage->obj, resources);
    });

    return report;
}

int ExifStripper::stripPageImages(const pdfe::core::Page* page, const pdfe::core::Document* doc) {
    return stripPageImagesReport(page, doc).metadataFree();
}

ExifStripper::StripReport ExifStripper::stripAllImagesReport(pdfe::core::Document* doc) {
    StripReport total;
    if (!doc) return total;
    doc->markRequiresFullRewrite();  // EXIF strip orphans original image bytes → full rewrite

    const int numPages = doc->pageCount();
    for (int i = 0; i < numPages; ++i) {
        auto page = doc->page(i);
        if (!page) continue;
        const StripReport r = stripPageImagesReport(page.get(), doc);
        total.stripped += r.stripped;
        total.alreadyClean += r.alreadyClean;
        total.failed += r.failed;
        total.thumbnailsRemoved += r.thumbnailsRemoved;
        total.inlineNotProcessed += r.inlineNotProcessed;
    }
    return total;
}

int ExifStripper::stripAllImages(pdfe::core::Document* doc) {
    return stripAllImagesReport(doc).metadataFree();
}

QString ExifStripper::describe(const StripReport& report) {
    auto tr = [](const char* s) { return QCoreApplication::translate("ExifStripper", s); };
    if (report.total() == 0 && report.thumbnailsRemoved == 0 && report.inlineNotProcessed == 0)
        return tr("No images found to strip metadata from.");
    QStringList parts;
    parts << tr("Metadata removed from %1 image(s).").arg(report.stripped);
    if (report.alreadyClean > 0)
        parts << tr("%1 image(s) had no metadata to remove.").arg(report.alreadyClean);
    if (report.failed > 0)
        parts << tr("%1 image(s) could not be processed; their metadata may remain.").arg(report.failed);
    if (report.inlineNotProcessed > 0)
        parts << tr("%1 inline JPEG image(s) were not processed; their metadata may remain.")
                     .arg(report.inlineNotProcessed);
    if (report.thumbnailsRemoved > 0)
        parts << tr("%1 page thumbnail(s) removed.").arg(report.thumbnailsRemoved);
    return parts.join(QLatin1Char(' '));
}

} // namespace pdfe::image
