// -*- coding: utf-8 -*-
// Meta-audit F4 fuzz harness (SEC-06): IMAGE-CODEC decode surface.
//
// WHY THIS HARNESS EXISTS
// -----------------------
// PDFE's one confirmed RCE-class defect — issue #50 / DCA-0011, a heap
// overflow inside the bundled OpenJPEG 2.5.4 (STATUS_HEAP_CORRUPTION
// 0xC0000374) reachable from a malformed JPEG2000 image — was on the
// image-codec surface, and it was found by a HAND-BUILT FIXTURE, not by a
// fuzzer, because no codec fuzz target existed. `fuzz_page_render` renders
// whatever page the raw input happens to parse into; it almost never
// produces a structurally valid image XObject, so the decoders were
// effectively unfuzzed. This harness synthesises the wrapper and puts the
// fuzz bytes exactly where the codec reads them.
//
// SURFACE
// -------
// Three codecs, one per synthesised document, ALL THREE driven on every
// input (rather than one selected by an input byte) so no codec can be
// starved by the mutator's choice of selector byte:
//
//   * JPXDecode   -> MuPDF fz_load_jpx -> OpenJPEG          (the #50 surface)
//   * DCTDecode   -> MuPDF fz_load_jpeg -> libjpeg-turbo
//   * FlateDecode -> MuPDF's zlib inflate + raw-sample unpack
//
// Each document is a minimal 1-page PDF whose single image XObject stream is
// the fuzz input verbatim; Document::open + Page::render then drives the real
// decode path (fz_run_page -> draw device -> fz_get_pixmap_from_image ->
// fz_load_<codec>). Image geometry / colour space / bit depth are derived
// from the input so header-vs-stream mismatches (a classic overflow trigger:
// /Width * /Height * /BitsPerComponent disagreeing with the codestream) are
// reachable.
//
// THE JPX GATE — the single most important correctness point here
// ---------------------------------------------------------------
// PDFE ships with JPEG2000 decoding DISABLED BY DEFAULT (#50). The patched
// MuPDF (vcpkg-overlay-ports/libmupdf/0001-pdfe-jpx-decode-gate.patch) checks
// a process-wide flag at the entry of fz_load_jpx / fz_load_jpx_info and
// returns a 1x1 PLACEHOLDER pixmap WITHOUT ever entering openjpeg while the
// flag is off. A fuzz target that did not flip that flag would fuzz the
// placeholder branch and prove exactly nothing about the surface that
// produced the project's only RCE-class defect.
//
// So this harness explicitly calls pdfe::core::setJpxDecodingEnabled(true)
// once per process, and then VERIFIES it took effect. If the flag does not
// read back as enabled — e.g. the binary was linked against an unpatched
// MuPDF, or the patch was dropped by a port-version bump — the harness
// aborts LOUDLY on stderr rather than quietly running a campaign against a
// placeholder and reporting a clean night. A silently-nothing fuzz target is
// worse than no fuzz target, because it manufactures false assurance.
//
// Opting in here is correct and is NOT a weakening of the product default:
// the fuzzer is the one context where deliberately feeding attacker-shaped
// bytes to OpenJPEG is the entire point, and it runs under ASan/UBSan where
// a corruption is caught rather than exploited.
#include "fuzz_common.h"

#include "core/Document.h"
#include "core/JpxSecurityGate.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>
#include <QRectF>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

// Byte at index i, wrapping; 0 for empty input. (Mirrors the helper in
// fuzz_targets.h — duplicated locally so this self-contained harness, like
// fuzz_page_render.cpp, does not pull in the mutation-surface cores.)
std::uint8_t byte_at(const std::uint8_t* data, std::size_t size, std::size_t i)
{
    if (size == 0) return 0;
    return data[i % size];
}

// ---------------------------------------------------------------
// Enable the JPX decode gate exactly once, and fail loudly if it did not
// take. See the header comment for why this is load-bearing.
// ---------------------------------------------------------------
void ensure_jpx_gate_open()
{
    static const bool kOpened = [] {
        pdfe::core::setJpxDecodingEnabled(true);
        if (!pdfe::core::isJpxDecodingEnabled()) {
            std::fprintf(stderr,
                "\n"
                "!!! fuzz_image_decode: FATAL — the JPX decode gate could not be\n"
                "!!! enabled (setJpxDecodingEnabled(true) did not read back).\n"
                "!!! The JPXDecode arm of this harness would fuzz MuPDF's 1x1\n"
                "!!! placeholder instead of OpenJPEG, i.e. it would prove nothing\n"
                "!!! about the #50 / DCA-0011 surface. Refusing to run rather than\n"
                "!!! reporting a meaningless clean campaign. Check that the vcpkg\n"
                "!!! overlay patch 0001-pdfe-jpx-decode-gate.patch is still applied\n"
                "!!! to libmupdf.\n\n");
            std::fflush(stderr);
            std::abort();
        }
        return true;
    }();
    (void)kOpened;
}

// ---------------------------------------------------------------
// Minimal 1-page PDF wrapping `stream` as the sole image XObject.
// ---------------------------------------------------------------
// `filter` is the PDF filter name WITHOUT the leading slash — one of
// "JPXDecode", "DCTDecode", "FlateDecode".
//
// No valid xref is emitted (startxref 0): MuPDF's repair path rescans the
// objects and still resolves the page tree. This matches the established
// idiom in pdfe::fuzz::build_encrypted_pdf and keeps the builder free of
// offset bookkeeping that the fuzzer would only have to re-derive.
std::string build_image_pdf(const std::uint8_t* data, std::size_t size,
                            const char* filter, const std::string& stream)
{
    // Geometry driven by the input so header/codestream mismatches — the
    // classic decoder-overflow trigger — are reachable. Bounded so a
    // successful decode cannot allocate an unbounded pixmap.
    const int w = 1 + (byte_at(data, size, 0) % 64);
    const int h = 1 + (byte_at(data, size, 1) % 64);

    // Colour space + bit depth. JPXDecode carries its own colour space in the
    // codestream and both keys are OPTIONAL for it (PDF 1.7 §7.4.9), so the
    // JPX arm sometimes omits them entirely — that "absent, trust the
    // codestream" branch is itself part of the attack surface.
    static const char* kCs[4] =
        { "/DeviceGray", "/DeviceRGB", "/DeviceCMYK", "/DeviceGray" };
    const char* cs  = kCs[byte_at(data, size, 2) & 0x03];
    static const int kBpc[4] = { 1, 2, 4, 8 };
    const int bpc = kBpc[byte_at(data, size, 3) & 0x03];

    const bool isJpx     = (std::string(filter) == "JPXDecode");
    const bool omitCsBpc = isJpx && ((byte_at(data, size, 4) & 1) != 0);

    char imgHdr[512];
    if (omitCsBpc) {
        std::snprintf(imgHdr, sizeof(imgHdr),
            "<< /Type /XObject /Subtype /Image /Width %d /Height %d"
            " /SMaskInData %d /Filter /%s /Length %llu >>",
            w, h, static_cast<int>(byte_at(data, size, 5) % 3), filter,
            static_cast<unsigned long long>(stream.size()));
    } else {
        std::snprintf(imgHdr, sizeof(imgHdr),
            "<< /Type /XObject /Subtype /Image /Width %d /Height %d"
            " /ColorSpace %s /BitsPerComponent %d /Filter /%s /Length %llu >>",
            w, h, cs, bpc, filter,
            static_cast<unsigned long long>(stream.size()));
    }

    char contents[128];
    std::snprintf(contents, sizeof(contents),
                  "q %d 0 0 %d 0 0 cm /Im0 Do Q\n", w, h);
    const std::string content = contents;

    std::string pdf;
    pdf += "%PDF-1.7\n";
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    {
        char pageObj[320];
        std::snprintf(pageObj, sizeof(pageObj),
            "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d]"
            " /Resources << /XObject << /Im0 5 0 R >> >>"
            " /Contents 4 0 R >>\nendobj\n", w, h);
        pdf += pageObj;
    }
    {
        char cHdr[64];
        std::snprintf(cHdr, sizeof(cHdr), "4 0 obj\n<< /Length %llu >>\nstream\n",
                      static_cast<unsigned long long>(content.size()));
        pdf += cHdr;
        pdf += content;
        pdf += "endstream\nendobj\n";
    }
    pdf += "5 0 obj\n";
    pdf += imgHdr;
    pdf += "\nstream\n";
    pdf += stream;
    pdf += "\nendstream\nendobj\n";
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\n";
    pdf += "startxref\n0\n%%EOF\n";
    return pdf;
}

// Open + render one synthesised document, driving the codec.
void decode_one(const std::uint8_t* data, std::size_t size,
                const char* filter, const std::string& stream)
{
    const std::string pdf = build_image_pdf(data, size, filter, stream);
    pdfe::fuzz::TempPdfFile tmp(
        reinterpret_cast<const std::uint8_t*>(pdf.data()), pdf.size());
    if (!tmp.valid()) return;

    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path());
        if (!doc || doc->pageCount() <= 0) return;
        auto page = doc->page(0);
        if (!page) return;
        // Render forces fz_get_pixmap_from_image -> fz_load_<codec>. Small
        // scale keeps the destination pixmap cheap; the codestream is parsed
        // in full regardless of the output scale.
        QImage img = page->render(QRectF(), 0.25f);
        (void)img;
    } catch (const std::exception& e) {
        // Triage rather than a bare catch(...): a std::bad_alloc driven by a
        // corrupted codestream dimension is a FINDING, not a benign refusal.
        pdfe::fuzz::triage_or_rethrow(e);
    }
}

} // namespace

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data,
                                    std::size_t size)
{
    // 64 KB cap: three open+render cycles run per input, so keep each cheap.
    // Image codec bugs are header/early-codestream bugs; they do not need
    // megabyte inputs to reach.
    if (size == 0 || size > 64 * 1024) return;

    ensure_jpx_gate_open();

    const std::string stream(reinterpret_cast<const char*>(data), size);

    // All three codecs, every input. JPXDecode first: it is the surface that
    // produced #50 / DCA-0011, so it gets the freshest process state.
    decode_one(data, size, "JPXDecode",   stream);
    decode_one(data, size, "DCTDecode",   stream);
    decode_one(data, size, "FlateDecode", stream);
}
