// -*- coding: utf-8 -*-
// Test-callable cores for the phase-8 write/mutation fuzz harnesses
// (R2-15 / R2-16). Each `run_*` function is the exact body a libFuzzer /
// AFL++ harness executes per input, factored out as an `inline` function so
// it can ALSO be driven directly from the GoogleTest smoke test
// (tests/test_fuzz_harness_smoke.cpp) — no libFuzzer, no sanitizer runtime,
// no LLVMFuzzerTestOneInput.
//
// Why `inline` (not a separate .cpp): the four thin harness .cpp files each
// define their own `pdfe_fuzz_one_input` (a single, colliding symbol), so
// they can never be co-linked into one test binary. Keeping the cores as
// inline functions in this header lets every harness AND the gtest build
// emit its own copy with no multiple-definition conflict, and an unused
// core is simply never ODR-used (so, e.g., fuzz_encryption_dict.cpp does not
// drag in the redaction/forms/textedit object code it never calls).
//
// The surfaces fuzzed here are PDFE's real historical-bug write/mutation
// paths:
//   * content-stream edit  -> ContentStreamEditor (mirror/state-continuity)
//   * save / incremental   -> Document::save + writeRecoveryCopy + saveIncremental
//   * MuJS execution        -> forms::JavaScriptEngine (document + field JS)
//   * redaction apply       -> redaction::RedactionManager
//   * encryption dict       -> MuPDF /Standard crypt parser via Document::open
#pragma once

#include "fuzz_support.h"

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionData.h"
#include "forms/JavaScriptEngine.h"

#include <QRectF>

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::fuzz {

// ---------------------------------------------------------------
// Small deterministic byte-derivation helpers
// ---------------------------------------------------------------

// Byte at index i, wrapping; 0 for empty input.
inline std::uint8_t byte_at(const std::uint8_t* data, std::size_t size, std::size_t i)
{
    if (size == 0) return 0;
    return data[i % size];
}

// A little-endian u32 seeded from the input at offset off (wrapping).
inline std::uint32_t u32_at(const std::uint8_t* data, std::size_t size, std::size_t off)
{
    return  static_cast<std::uint32_t>(byte_at(data, size, off))
        | (static_cast<std::uint32_t>(byte_at(data, size, off + 1)) << 8)
        | (static_cast<std::uint32_t>(byte_at(data, size, off + 2)) << 16)
        | (static_cast<std::uint32_t>(byte_at(data, size, off + 3)) << 24);
}

// N bytes of fuzz input starting at off (wrapping), as a hex string body
// (no angle brackets). Used to fill the fixed-width /O /U /OE /UE crypt
// fields that MuPDF's standard security handler reads.
inline std::string hex_field(const std::uint8_t* data, std::size_t size,
                             std::size_t off, std::size_t n)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t b = byte_at(data, size, off + i);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

// ---------------------------------------------------------------
// R2-16: encrypted-PDF builder that ACTUALLY reaches the crypt parser
// ---------------------------------------------------------------
// The prior fuzz_encryption_dict wrapped raw fuzz bytes as the *entire*
// /Encrypt dict body (`2 0 obj << [random bytes] >>`). MuPDF rejects that at
// the trailer/dict stage and never constructs the standard security handler,
// so pdf_new_crypt / the key-derivation + authentication code was essentially
// never exercised — the whole point of the harness.
//
// This builder instead emits a STRUCTURALLY VALID encrypted PDF: a trailer
// carrying /Encrypt + /ID and a well-formed `/Filter /Standard` dictionary
// whose security-relevant fields (/V /R /Length /P /O /U, and for V>=4 the
// /CF crypt-filter block, plus /OE /UE /Perms for V5) are driven by the fuzz
// input. MuPDF now recognizes the document as encrypted and runs the real
// crypt parser + pdf_compute_encryption_key + authentication against the
// malformed material.
inline std::string build_encrypted_pdf(const std::uint8_t* data, std::size_t size)
{
    // Choose a crypt shape from the input: RC4-40, RC4-128, AESV2, AESV3.
    const int sel = byte_at(data, size, 0) & 0x03;
    int v, r, length;
    const char* cfm;
    switch (sel) {
        case 0:  v = 1; r = 2; length = 40;  cfm = "/V2";     break;
        case 1:  v = 2; r = 3; length = 128; cfm = "/V2";     break;
        case 2:  v = 4; r = 4; length = 128; cfm = "/AESV2";  break;
        default: v = 5; r = 6; length = 256; cfm = "/AESV3";  break;
    }
    const std::size_t ouLen = (v == 5) ? 48 : 32;   // /O /U width
    const std::int32_t P = static_cast<std::int32_t>(u32_at(data, size, 1));

    // Fuzz-filled crypt material as PDF hex strings (no escaping hazards).
    const std::string O  = hex_field(data, size, 5,  ouLen);
    const std::string U  = hex_field(data, size, 5 + ouLen, ouLen);
    const std::string ID = hex_field(data, size, 0, 16);

    std::string enc = "<< /Filter /Standard";
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), " /V %d /R %d /Length %d /P %ld",
                      v, r, length, static_cast<long>(P));
        enc += buf;
    }
    enc += " /O <" + O + "> /U <" + U + ">";
    if (v >= 4) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            " /CF << /StdCF << /Type /CryptFilter /CFM %s /Length %d >> >>"
            " /StmF /StdCF /StrF /StdCF", cfm, length / 8);
        enc += buf;
        if (v == 5) {
            enc += " /OE <"    + hex_field(data, size, 96, 32) + ">";
            enc += " /UE <"    + hex_field(data, size, 128, 32) + ">";
            enc += " /Perms <" + hex_field(data, size, 160, 16) + ">";
        }
    }
    enc += " >>";

    std::string pdf;
    pdf += "%PDF-1.7\n";
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 3 0 R >>\nendobj\n";
    pdf += "2 0 obj\n" + enc + "\nendobj\n";
    pdf += "3 0 obj\n<< /Type /Pages /Kids [] /Count 0 >>\nendobj\n";
    // A deliberately unhelpful xref (offsets need not be exact — MuPDF's
    // repair path rescans objects and still reads the trailer's /Encrypt).
    pdf += "xref\n0 4\n0000000000 65535 f \n";
    pdf += "0000000009 00000 n \n0000000060 00000 n \n0000000120 00000 n \n";
    pdf += "trailer\n<< /Size 4 /Root 1 0 R /Encrypt 2 0 R /ID [<"
         + ID + "><" + ID + ">] >>\n";
    pdf += "startxref\n0\n%%EOF\n";
    return pdf;
}

// ---------------------------------------------------------------
// Core: content-stream edit (mirror / state-continuity surface)
// ---------------------------------------------------------------
inline void run_content_stream_edit(const std::uint8_t* data, std::size_t size)
{
    if (size == 0 || size > 4 * 1024 * 1024) return;
    TempPdfFile tmp(data, size);
    if (!tmp.valid()) return;

    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path());
        if (!doc || doc->pageCount() <= 0) return;
        auto page = doc->page(0);
        if (!page) return;

        // Synthesize a span whose geometry / font are driven by the input,
        // then drive the content-stream rewrite (pdf_filter_page_contents +
        // canonical Tm / Tz operator emission — the mirror-bug code path).
        QRectF bbox = page->boundingBox();
        pdfe::textedit::TextSpan span;
        span.boundingBox = QRectF(
            bbox.x() + (byte_at(data, size, 0) % 50),
            bbox.y() + (byte_at(data, size, 1) % 50),
            10.0 + (byte_at(data, size, 2) % 200),
            5.0 + (byte_at(data, size, 3) % 40));
        span.fontName = "Helv";
        span.fontSize = 6.0f + static_cast<float>(byte_at(data, size, 4) % 40);
        span.textColor = u32_at(data, size, 5) & 0xFFFFFF;
        span.textColorKnown = (byte_at(data, size, 8) & 1) != 0;

        std::u32string newText;
        const std::size_t n = 1 + (size % 24);
        for (std::size_t i = 0; i < n; ++i)
            newText.push_back(0x20 + (byte_at(data, size, 9 + i) % 0x5F));

        std::vector<pdfe::textedit::TextSpan> spans{span};
        std::vector<std::pair<int, std::u32string>> mods{{0, newText}};
        const int trm = (byte_at(data, size, 8) & 2) ? 3 : -1;

        pdfe::textedit::ContentStreamEditor editor(ctx.get());
        editor.applyEdits(page.get(), doc.get(), spans, mods, trm);

        // Exercise the snapshot/restore undo path too (Pipeline F phase-2).
        void* snap = editor.captureContentsSnapshot(doc.get(), 0);
        editor.restoreContentsSnapshot(doc.get(), 0, snap);
        editor.dropContentsSnapshot(snap);
    } catch (const std::exception& e) {
        triage_or_rethrow(e);
    }
}

// ---------------------------------------------------------------
// Core: save / incremental round-trip
// ---------------------------------------------------------------
inline void run_save_roundtrip(const std::uint8_t* data, std::size_t size)
{
    if (size == 0 || size > 4 * 1024 * 1024) return;
    TempPdfFile tmp(data, size);
    if (!tmp.valid()) return;

    // Distinct output path for the full-rewrite / recovery-copy branch.
    std::filesystem::path outPath =
        tmp.path().parent_path() /
        (tmp.path().stem().string() + "_out.pdf");

    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path());
        if (!doc) return;

        const std::uint8_t branch = byte_at(data, size, 0);

        // Recovery-copy write (self-contained full rewrite to a NEW file).
        try { doc->writeRecoveryCopy(outPath); }
        catch (const std::exception& e) { triage_or_rethrow(e); }

        if (branch & 1) {
            // Full-rewrite saveAs to the distinct output path.
            try { doc->saveAs(outPath); }
            catch (const std::exception& e) { triage_or_rethrow(e); }
        } else {
            // Incremental append to the opened file.
            try { doc->saveIncremental(); }
            catch (const std::exception& e) { triage_or_rethrow(e); }
        }

        // Round-trip: reopen whatever we wrote and walk it.
        try {
            auto ctx2 = std::make_shared<pdfe::core::MuPdfContext>();
            std::filesystem::path reopen =
                std::filesystem::exists(outPath) ? outPath : tmp.path();
            auto doc2 = pdfe::core::Document::open(ctx2, reopen);
            if (doc2) (void)doc2->pageCount();
        } catch (const std::exception& e) {
            triage_or_rethrow(e);
        }
    } catch (const std::exception& e) {
        triage_or_rethrow(e);
    }

    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

// ---------------------------------------------------------------
// Core: MuJS execution (document + form-field JavaScript)
// ---------------------------------------------------------------
inline void run_mujs_exec(const std::uint8_t* data, std::size_t size)
{
    if (size == 0 || size > 4 * 1024 * 1024) return;
    TempPdfFile tmp(data, size);
    if (!tmp.valid()) return;

    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path());
        if (!doc || doc->pageCount() <= 0) return;
        auto page = doc->page(0);
        if (!page) return;

        pdfe::forms::JavaScriptEngine engine(ctx);
        engine.enable(page.get());   // runs document-level /OpenAction + /Names JS

        std::string value(reinterpret_cast<const char*>(data),
                          size > 64 ? 64 : size);
        std::string change = value.substr(0, value.size() / 2);

        const int widget = static_cast<int>(byte_at(data, size, 0));
        engine.fireKeystrokeEvent(page.get(), widget, value, change, 0, 0, true);
        engine.fireValidateEvent(page.get(), widget, value);
        engine.fireCalculateEvent(page.get(), widget);
        (void)engine.fireFormatEvent(page.get(), widget);
        engine.fireFocusEvent(page.get(), widget);
        engine.fireBlurEvent(page.get(), widget);
        engine.calculateAll(page.get());

        static const char* kEvents[] =
            {"willSave", "didSave", "willPrint", "didPrint", "willClose"};
        engine.fireDocumentEvent(kEvents[byte_at(data, size, 1) % 5], page.get());
    } catch (const std::exception& e) {
        triage_or_rethrow(e);
    }
}

// ---------------------------------------------------------------
// Core: redaction apply
// ---------------------------------------------------------------
inline void run_redaction_apply(const std::uint8_t* data, std::size_t size)
{
    if (size == 0 || size > 4 * 1024 * 1024) return;
    TempPdfFile tmp(data, size);
    if (!tmp.valid()) return;

    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path());
        if (!doc || doc->pageCount() <= 0) return;
        auto page = doc->page(0);
        if (!page) return;

        pdfe::redaction::RedactionManager mgr(ctx);
        QRectF bbox = page->boundingBox();
        QRectF rect(
            bbox.x() + (byte_at(data, size, 0) % 40),
            bbox.y() + (byte_at(data, size, 1) % 40),
            20.0 + (byte_at(data, size, 2) % 200),
            10.0 + (byte_at(data, size, 3) % 100));
        mgr.createRedactionMark(page.get(), rect);

        pdfe::redaction::RedactionApplyOptions opts;
        opts.rewriteDocument = (byte_at(data, size, 4) & 1) != 0;
        mgr.applyPageRedactions(page.get(), opts);
        mgr.applyAllRedactions(doc.get(), opts);
    } catch (const std::exception& e) {
        triage_or_rethrow(e);
    }
}

// ---------------------------------------------------------------
// Core: encryption dict (feeds the crypt-shaped input to Document::open)
// ---------------------------------------------------------------
inline void run_encryption_dict(const std::uint8_t* data, std::size_t size)
{
    if (size == 0 || size > 64 * 1024) return;

    const std::string pdf = build_encrypted_pdf(data, size);
    TempPdfFile tmp(reinterpret_cast<const std::uint8_t*>(pdf.data()), pdf.size());
    if (!tmp.valid()) return;

    // Empty password: exercises key-derivation + user-auth against the
    // malformed /O /U material.
    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path(), "");
        (void)doc;
    } catch (const std::exception& e) {
        triage_or_rethrow(e);
    }
    // Non-empty password: exercises the password-matching path.
    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path(), "fuzzpass");
        (void)doc;
    } catch (const std::exception& e) {
        triage_or_rethrow(e);
    }
}

} // namespace pdfe::fuzz
