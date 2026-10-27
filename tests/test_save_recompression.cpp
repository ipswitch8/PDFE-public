// tests/test_save_recompression.cpp
//
// Pipeline 3f8a1c72 Phase 2 — save-path recompression fidelity.
//
// Phase 1 (docs/pipeline-3f8a1c72-findings.md) established, by reading the
// MuPDF 1.26.10 write path, that Document::save()/saveAs() set
// do_compress_images = do_compress_fonts = 1 unconditionally, yet this is
// SAFE per-object because MuPDF's copystream() (pdf-write.c:~809) only
// Flate-compresses a stream that has NO existing /Filter:
//
//   * An image already carrying /DCTDecode (JPEG), /JPXDecode, /FlateDecode,
//     /CCITTFaxDecode, or /JBIG2Decode keeps its filter and is copied
//     byte-for-byte — there is no JPEG/DCT re-encoder anywhere in the write
//     path, so an already-compressed image is never lossily re-encoded.
//   * There is no font-subsetting in the write path (pdf_subset_fonts lives
//     only in pdf-clean-file.c), so an already-subset (XXXXXX+) font program
//     is never re-subset; an already-FlateDecode font program is copied raw.
//   * A *filterless* stream (an uncompressed raw image) IS compressed — which
//     is the intended, lossless behaviour.
//
// These tests convert that source-reading conclusion into regression-guarded
// invariants by exercising the real production Document::saveAs() path:
//   (a) a /DCTDecode image survives save byte-for-byte (no re-encode),
//   (b) an already-FlateDecode + already-subset (XXXXXX+) font program survives
//       save byte-for-byte (no double-subset, no re-compress),
//   (c) a filterless (uncompressed) image is correctly Flate-compressed on save.
//
// The fixtures are built in-memory with the MuPDF C API so the stream filters
// are known exactly. The "already-compressed" baselines are produced by a
// bootstrap save (MuPDF does the initial compression), then the production
// save is asserted not to alter them further.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace fs = std::filesystem;

namespace {

std::string readFileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Raw (still-encoded) bytes of the first stream object matching a predicate,
// plus its /Filter name. Extracted at the MuPDF level so the assertion is on
// the on-disk encoded bytes, not a decoded re-render.
struct StreamInfo {
    bool found = false;
    std::string filter;    // /Filter name, empty if filterless
    std::string rawBytes;  // raw encoded stream bytes
};

}  // namespace

class SaveRecompressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        const auto& info =
            *::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag = std::string(info.name());
        bootstrap_ = fs::temp_directory_path() /
            ("pdfe_recompress_boot_" + tag + ".pdf");
        produced_ = fs::temp_directory_path() /
            ("pdfe_recompress_prod_" + tag + ".pdf");
        std::error_code ec;
        fs::remove(bootstrap_, ec);
        fs::remove(produced_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove(bootstrap_, ec);
        fs::remove(produced_, ec);
    }

    // Build a one-page PDF whose only image XObject carries `imgBytes`.
    //   withDctFilter=true  → stored as /DCTDecode (compressed=1, raw kept).
    //   withDctFilter=false → stored filterless (compressed=0).
    // bootstrapCompress controls the bootstrap save flags so the baseline file
    // is byte-clean for the assertion that follows.
    void buildImagePdf(const std::string& imgBytes, bool withDctFilter,
                       bool bootstrapCompress) {
        const std::string outStr = bootstrap_.string();
        const int compress = bootstrapCompress ? 1 : 0;
        ctx_->execute([&](fz_context* c) {
            pdf_document* doc = pdf_create_document(c);

            pdf_obj* dict = pdf_new_dict(c, doc, 8);
            pdf_dict_put(c, dict, PDF_NAME(Type), PDF_NAME(XObject));
            pdf_dict_put(c, dict, PDF_NAME(Subtype), PDF_NAME(Image));
            pdf_dict_put_int(c, dict, PDF_NAME(Width), 16);
            pdf_dict_put_int(c, dict, PDF_NAME(Height), 16);
            pdf_dict_put_int(c, dict, PDF_NAME(BitsPerComponent), 8);
            pdf_dict_put(c, dict, PDF_NAME(ColorSpace), PDF_NAME(DeviceRGB));
            if (withDctFilter)
                pdf_dict_put(c, dict, PDF_NAME(Filter), PDF_NAME(DCTDecode));
            pdf_obj* imgRef = pdf_add_object(c, doc, dict);
            pdf_drop_obj(c, dict);

            fz_buffer* ib = fz_new_buffer_from_copied_data(
                c, reinterpret_cast<const unsigned char*>(imgBytes.data()),
                imgBytes.size());
            pdf_update_stream(c, doc, imgRef, ib, withDctFilter ? 1 : 0);
            fz_drop_buffer(c, ib);

            pdf_obj* res = pdf_new_dict(c, doc, 1);
            pdf_obj* xo = pdf_dict_put_dict(c, res, PDF_NAME(XObject), 1);
            pdf_dict_puts(c, xo, "Im0", imgRef);

            const char* cs = "q 100 0 0 100 50 50 cm /Im0 Do Q";
            fz_buffer* cb = fz_new_buffer_from_copied_data(
                c, reinterpret_cast<const unsigned char*>(cs),
                std::strlen(cs));
            fz_rect mb = fz_make_rect(0, 0, 200, 200);
            pdf_obj* page = pdf_add_page(c, doc, mb, 0, res, cb);
            pdf_insert_page(c, doc, -1, page);
            fz_drop_buffer(c, cb);
            pdf_drop_obj(c, page);
            pdf_drop_obj(c, res);
            pdf_drop_obj(c, imgRef);

            pdf_write_options wo{};
            wo.do_garbage = 0;
            wo.do_compress = compress;
            wo.do_compress_images = compress;
            wo.do_compress_fonts = compress;
            pdf_save_document(c, doc, outStr.c_str(), &wo);
            pdf_drop_document(c, doc);
        });
    }

    // Build a one-page PDF with a TrueType font whose FontFile2 program is
    // `fontRaw` and whose BaseFont is "XXXXXX+TestFont" (subset prefix). The
    // bootstrap save runs with do_compress_fonts=1 so MuPDF compresses the
    // (filterless) program to FlateDecode — giving an "already-compressed,
    // already-subset" baseline.
    void buildFontPdf(const std::string& fontRaw) {
        const std::string outStr = bootstrap_.string();
        const int len1 = static_cast<int>(fontRaw.size());
        ctx_->execute([&](fz_context* c) {
            pdf_document* doc = pdf_create_document(c);

            // FontFile2 stream (filterless at build time; bootstrap save flates it).
            pdf_obj* ffDict = pdf_new_dict(c, doc, 1);
            pdf_dict_put_int(c, ffDict, PDF_NAME(Length1), len1);
            pdf_obj* ffRef = pdf_add_object(c, doc, ffDict);
            pdf_drop_obj(c, ffDict);
            fz_buffer* fb = fz_new_buffer_from_copied_data(
                c, reinterpret_cast<const unsigned char*>(fontRaw.data()),
                fontRaw.size());
            pdf_update_stream(c, doc, ffRef, fb, 0);
            fz_drop_buffer(c, fb);

            pdf_obj* fd = pdf_new_dict(c, doc, 6);
            pdf_dict_put(c, fd, PDF_NAME(Type), PDF_NAME(FontDescriptor));
            pdf_dict_put_name(c, fd, PDF_NAME(FontName), "XXXXXX+TestFont");
            pdf_dict_put_int(c, fd, PDF_NAME(Flags), 32);
            pdf_dict_put_int(c, fd, PDF_NAME(ItalicAngle), 0);
            pdf_dict_put(c, fd, PDF_NAME(FontFile2), ffRef);
            pdf_obj* fdRef = pdf_add_object(c, doc, fd);
            pdf_drop_obj(c, fd);
            pdf_drop_obj(c, ffRef);

            pdf_obj* font = pdf_new_dict(c, doc, 6);
            pdf_dict_put(c, font, PDF_NAME(Type), PDF_NAME(Font));
            pdf_dict_put(c, font, PDF_NAME(Subtype), PDF_NAME(TrueType));
            pdf_dict_put_name(c, font, PDF_NAME(BaseFont), "XXXXXX+TestFont");
            pdf_dict_put_int(c, font, PDF_NAME(FirstChar), 88);
            pdf_dict_put_int(c, font, PDF_NAME(LastChar), 88);
            pdf_dict_put(c, font, PDF_NAME(FontDescriptor), fdRef);
            pdf_obj* widths = pdf_new_array(c, doc, 1);
            pdf_array_push_int(c, widths, 500);
            pdf_dict_put_drop(c, font, PDF_NAME(Widths), widths);
            pdf_obj* fontRef = pdf_add_object(c, doc, font);
            pdf_drop_obj(c, font);
            pdf_drop_obj(c, fdRef);

            pdf_obj* res = pdf_new_dict(c, doc, 1);
            pdf_obj* fonts = pdf_dict_put_dict(c, res, PDF_NAME(Font), 1);
            pdf_dict_puts(c, fonts, "F0", fontRef);

            const char* cs = "BT /F0 12 Tf 50 100 Td (X) Tj ET";
            fz_buffer* cb = fz_new_buffer_from_copied_data(
                c, reinterpret_cast<const unsigned char*>(cs),
                std::strlen(cs));
            fz_rect mb = fz_make_rect(0, 0, 200, 200);
            pdf_obj* page = pdf_add_page(c, doc, mb, 0, res, cb);
            pdf_insert_page(c, doc, -1, page);
            fz_drop_buffer(c, cb);
            pdf_drop_obj(c, page);
            pdf_drop_obj(c, res);
            pdf_drop_obj(c, fontRef);

            pdf_write_options wo{};
            wo.do_garbage = 1;
            wo.do_compress = 1;
            wo.do_compress_images = 1;
            wo.do_compress_fonts = 1;  // bootstrap compresses the program to Flate
            pdf_save_document(c, doc, outStr.c_str(), &wo);
            pdf_drop_document(c, doc);
        });
    }

    // Pull the raw encoded bytes + /Filter of the first image XObject stream.
    StreamInfo extractImage(const fs::path& path) {
        return extract(path, /*wantImage=*/true);
    }
    // Pull the raw encoded bytes + /Filter of the first FontFile2 (/Length1) stream.
    StreamInfo extractFontProgram(const fs::path& path) {
        return extract(path, /*wantImage=*/false);
    }

    StreamInfo extract(const fs::path& path, bool wantImage) {
        StreamInfo out;
        const std::string p = path.string();
        ctx_->execute([&](fz_context* c) {
            pdf_document* doc = pdf_open_document(c, p.c_str());
            int n = pdf_count_objects(c, doc);
            for (int i = 1; i < n && !out.found; ++i) {
                if (!pdf_obj_num_is_stream(c, doc, i)) continue;
                pdf_obj* o = pdf_load_object(c, doc, i);
                bool match = false;
                if (pdf_is_dict(c, o)) {
                    if (wantImage) {
                        pdf_obj* st = pdf_dict_get(c, o, PDF_NAME(Subtype));
                        match = pdf_name_eq(c, st, PDF_NAME(Image));
                    } else {
                        match = pdf_dict_get(c, o, PDF_NAME(Length1)) != nullptr;
                    }
                    if (match) {
                        pdf_obj* filt = pdf_dict_get(c, o, PDF_NAME(Filter));
                        if (filt && pdf_is_name(c, filt))
                            out.filter = pdf_to_name(c, filt);
                        fz_buffer* b = pdf_load_raw_stream_number(c, doc, i);
                        unsigned char* d = nullptr;
                        size_t len = fz_buffer_storage(c, b, &d);
                        out.rawBytes.assign(reinterpret_cast<char*>(d), len);
                        fz_drop_buffer(c, b);
                        out.found = true;
                    }
                }
                pdf_drop_obj(c, o);
            }
            pdf_drop_document(c, doc);
        });
        return out;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    fs::path bootstrap_;
    fs::path produced_;
};

// ─────────────────────────────────────────────────────────────────────
// (a) An already-DCTDecode (JPEG) image survives Document::saveAs() with its
//     encoded stream bytes preserved byte-for-byte — no lossy re-encode.
// ─────────────────────────────────────────────────────────────────────
TEST_F(SaveRecompressionTest, DctImageBytesPreservedThroughProductionSave) {
    // Minimal JPEG-shaped payload (SOI ... EOI). The save path never decodes
    // image streams, so arbitrary DCT-tagged bytes exercise the copy path.
    std::string jpeg;
    jpeg.push_back('\xFF'); jpeg.push_back('\xD8'); jpeg.push_back('\xFF');
    for (int i = 0; i < 240; ++i)
        jpeg.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
    jpeg.push_back('\xFF'); jpeg.push_back('\xD9');

    // Byte-clean baseline: DCT stored raw, bootstrap save does not compress.
    buildImagePdf(jpeg, /*withDctFilter=*/true, /*bootstrapCompress=*/false);
    StreamInfo before = extractImage(bootstrap_);
    ASSERT_TRUE(before.found) << "bootstrap image not found";
    EXPECT_EQ(before.filter, "DCTDecode");
    ASSERT_EQ(before.rawBytes, jpeg) << "bootstrap already altered the bytes";

    // Production path: real Document::saveAs() with do_compress_images=1.
    auto doc = Document::open(ctx_, bootstrap_);
    ASSERT_NE(doc, nullptr);
    doc->saveAs(produced_);
    doc.reset();

    StreamInfo after = extractImage(produced_);
    ASSERT_TRUE(after.found) << "image dropped by production save";
    EXPECT_EQ(after.filter, "DCTDecode") << "filter changed — re-encoded";
    EXPECT_EQ(after.rawBytes, jpeg)
        << "JPEG stream bytes were altered by do_compress_images=1";
}

// ─────────────────────────────────────────────────────────────────────
// (b) An already-FlateDecode, already-subset (XXXXXX+) font program survives
//     Document::saveAs() byte-for-byte — not double-subset, not re-compressed.
// ─────────────────────────────────────────────────────────────────────
TEST_F(SaveRecompressionTest, SubsetFontProgramNotDoubleProcessedOnSave) {
    std::string fontRaw;
    for (int i = 0; i < 600; ++i)
        fontRaw.push_back(static_cast<char>((i * 13 + 7) & 0xFF));

    buildFontPdf(fontRaw);  // bootstrap save flates the program to FlateDecode

    StreamInfo before = extractFontProgram(bootstrap_);
    ASSERT_TRUE(before.found) << "bootstrap FontFile2 not found";
    EXPECT_EQ(before.filter, "FlateDecode")
        << "bootstrap should have produced a compressed font program";
    std::string bootBytes = readFileBytes(bootstrap_);
    EXPECT_NE(bootBytes.find("XXXXXX+TestFont"), std::string::npos)
        << "subset prefix missing from bootstrap";

    auto doc = Document::open(ctx_, bootstrap_);
    ASSERT_NE(doc, nullptr);
    doc->saveAs(produced_);
    doc.reset();

    StreamInfo after = extractFontProgram(produced_);
    ASSERT_TRUE(after.found) << "FontFile2 dropped by production save";
    EXPECT_EQ(after.filter, "FlateDecode");
    EXPECT_EQ(after.rawBytes, before.rawBytes)
        << "font program bytes changed — re-subset or re-compressed on save";

    std::string prodBytes = readFileBytes(produced_);
    EXPECT_NE(prodBytes.find("XXXXXX+TestFont"), std::string::npos)
        << "subset prefix changed by production save";
}

// ─────────────────────────────────────────────────────────────────────
// (c) A filterless (uncompressed) image IS Flate-compressed on save — the
//     intended, lossless behaviour of do_compress_images for raw streams.
// ─────────────────────────────────────────────────────────────────────
TEST_F(SaveRecompressionTest, UncompressedImageIsCompressedOnSave) {
    // 16x16 RGB, highly compressible (all one colour) raw pixel data.
    std::string raw(16 * 16 * 3, '\x80');

    // Byte-clean baseline: filterless, bootstrap save leaves it uncompressed.
    buildImagePdf(raw, /*withDctFilter=*/false, /*bootstrapCompress=*/false);
    StreamInfo before = extractImage(bootstrap_);
    ASSERT_TRUE(before.found);
    EXPECT_TRUE(before.filter.empty()) << "baseline image should be filterless";
    ASSERT_EQ(before.rawBytes.size(), raw.size());

    auto doc = Document::open(ctx_, bootstrap_);
    ASSERT_NE(doc, nullptr);
    doc->saveAs(produced_);
    doc.reset();

    StreamInfo after = extractImage(produced_);
    ASSERT_TRUE(after.found);
    EXPECT_EQ(after.filter, "FlateDecode")
        << "filterless image should be compressed by do_compress_images=1";
    EXPECT_LT(after.rawBytes.size(), raw.size())
        << "compressed stream should be smaller than the raw pixels";
}
