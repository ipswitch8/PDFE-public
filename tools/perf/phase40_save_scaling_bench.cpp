// -*- coding: utf-8 -*-
//
// PERF-01 / follow-up phase F40 — full-rewrite save scaling reproduction and
// mitigation-option measurement harness.
//
// WHAT THIS IS
// ------------
// A standalone, EXCLUDE_FROM_ALL benchmark executable. It is NOT part of
// pdfe_tests and registers no gtest cases; nothing in the shipped product or
// the test suite depends on it, and building `--target pdfe_tests` never
// builds it. It exists so PERF-01's numbers can be reproduced and so candidate
// mitigations can be MEASURED without changing any production code path.
//
// WHAT IT MEASURES
// ----------------
// For a ladder of page counts, and for each variant:
//   * best-of-N wall-clock seconds for the SAVE call only (document build,
//     open and edit are outside the timer, exactly as PERF-01 measured),
//   * output size in bytes,
//   * output object count (pdf_xref_len of the reopened file) — the dedup
//     quality proxy,
//   * a correctness check on the reopened output, so a variant cannot look
//     fast merely by doing less than the operation requires.
//
// VARIANTS
// --------
//   prod        — the REAL production path: Document::open, reorderPages,
//                 markNeedsFullGc, Document::save(out). This is the baseline
//                 PERF-01 measured and is what the product does today.
//   mupdf_gc3   — proxy control: the same edit driven at the MuPDF level
//                 (pdf_rearrange_pages) then pdf_save_document with the SAME
//                 options Document::save() uses. Its only purpose is to show
//                 the proxy tracks `prod`; if it does not, the option numbers
//                 below cannot be read as statements about the product.
//   mupdf_gc1   — option A: the cheap GC tier the existing XFA mitigation
//                 already uses as the default for non-dedup-worthy sessions,
//                 applied to a plain-page STRUCTURAL edit (which today
//                 escalates to gc=3).
//   mupdf_gc2   — option B: the intermediate tier (gc + renumber, no dedup).
//   mupdf_gc3_objstm — option C: keep full dedup, add object streams.
//   mupdf_gc1_objstm — option C': cheap GC plus object streams.
//   mupdf_incr  — informational floor only: incremental append. NOT a
//                 candidate — it retains orphaned objects, which is the
//                 property the full-rewrite path exists to guarantee.
//
// CORPORA
// -------
//   unique  — PERF-01's corpus: distinct per-page content, inline font dict,
//             NO xref table (MuPDF repairs it at open). DEDUP-POOR by
//             construction — there is nothing for gc=3 to merge — so a size
//             comparison on this corpus ALONE would flatter the cheap tiers.
//   uniquex — the same document WITH a valid xref table. Control for two
//             questions: (a) is the repair pass what makes the save
//             super-linear, and (b) the incremental floor, which MuPDF refuses
//             on a repaired file.
//   dupfont — distinct content, but every page carries its OWN byte-identical
//             indirect FONT dictionary. Non-stream duplicate dicts are what
//             MuPDF's gc=3 dedup sweep actually collapses. This is the corpus
//             on which the time-vs-output-size trade-off is real.
//
// USAGE
//   phase40_save_scaling_bench --reps 5 --pages 50,100,250,500,1000,1500,2000,2500 \
//                              --corpus unique,uniquex,dupfont --variants prod,mupdf_gc3,...
// `--keep` leaves the generated inputs and outputs on disk (and prints the
// directory to stderr) so the written files can be compared byte-for-byte --
// that is how the SHA-256 identity check in phase-40/README.md section 2.1a
// was produced. Without it every artifact is deleted at exit.
//
// Emits CSV on stdout: corpus,variant,pages,rep_count,best_sec,worst_sec,
//                      out_bytes,out_objects,ok,note

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "support/PdfContentOracle.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Corpus generation. `unique` is copied verbatim in shape from
// tests/test_fidelity_r3_24_edit_save_scale.cpp::buildLargePdf so the
// reproduction is against the same document the finding used.
// ---------------------------------------------------------------------------
// `unique` (dupFonts=false, withXref=false) reproduces PERF-01's corpus
// byte-for-byte in shape: inline font dict, per-page distinct "(Page n)"
// content, and NO xref table (`startxref 0`), so MuPDF repairs it at open.
// That repair is outside the save timer, exactly as in the finding, but it
// also means an incremental append is impossible on this corpus — hence the
// `withXref` variant, which exists as the control for both questions.
//
// `dupFonts=true` gives every page its OWN indirect, byte-identical font
// dictionary. Those are non-stream objects, which is what MuPDF's gc=3 dedup
// sweep actually collapses (see tests/fixtures/gen_dedup_duplicate_objects.py:
// the project's own dedup oracle uses duplicate FONT DICTS, not duplicate
// streams). This is the corpus on which "gc=1 is cheaper" has a real cost.
struct CorpusSpec {
    bool dupFonts = false;
    bool withXref = false;
};

std::string buildLargePdf(int pages, const CorpusSpec& spec)
{
    const int perPage = spec.dupFonts ? 3 : 2;
    std::string pageRefs;
    for (int i = 0; i < pages; ++i) {
        char refBuf[24];
        std::snprintf(refBuf, sizeof(refBuf), "%d 0 R ", 3 + i * perPage);
        pageRefs += refBuf;
    }

    std::string out = "%PDF-1.7\n";
    std::vector<size_t> offsets;  // offsets[k] = byte offset of object k+1

    auto addObj = [&](const std::string& body) {
        offsets.push_back(out.size());
        out += body;
    };

    addObj("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    addObj("2 0 obj\n<< /Type /Pages /Kids [" + pageRefs + "] /Count " +
           std::to_string(pages) + " >>\nendobj\n");

    int objNum = 3;
    for (int i = 0; i < pages; ++i) {
        char buf[512];
        if (spec.dupFonts) {
            std::snprintf(buf, sizeof(buf),
                "%d 0 obj\n<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 612 792] /Resources << /Font << /F1 %d 0 R "
                ">> >> /Contents %d 0 R >>\nendobj\n",
                objNum, objNum + 2, objNum + 1);
        } else {
            std::snprintf(buf, sizeof(buf),
                "%d 0 obj\n<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 612 792] /Resources << /Font << /F1 << "
                "/Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> "
                "/Contents %d 0 R >>\nendobj\n",
                objNum, objNum + 1);
        }
        addObj(buf);

        char streamBuf[256];
        std::snprintf(streamBuf, sizeof(streamBuf),
            "%d 0 obj\n<< /Length 40 >>\nstream\n"
            "BT /F1 12 Tf 100 700 Td (Page %d) Tj ET\n"
            "endstream\nendobj\n",
            objNum + 1, i + 1);
        addObj(streamBuf);

        if (spec.dupFonts) {
            char fontBuf[192];
            std::snprintf(fontBuf, sizeof(fontBuf),
                "%d 0 obj\n<< /Type /Font /Subtype /Type1 "
                "/BaseFont /Helvetica >>\nendobj\n",
                objNum + 2);
            addObj(fontBuf);
        }
        objNum += perPage;
    }

    const int size = objNum;  // 1..objNum-1 used, plus the free entry 0
    if (!spec.withXref) {
        out += "trailer\n<< /Size " + std::to_string(size) +
               " /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
        return out;
    }

    const size_t xrefPos = out.size();
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "xref\n0 %d\n", size);
    out += hdr;
    out += "0000000000 65535 f \n";
    for (size_t k = 0; k < offsets.size(); ++k) {
        char e[32];
        std::snprintf(e, sizeof(e), "%010llu 00000 n \n",
                      static_cast<unsigned long long>(offsets[k]));
        out += e;
    }
    out += "trailer\n<< /Size " + std::to_string(size) + " /Root 1 0 R >>\n" +
           "startxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";
    return out;
}

void writeFile(const fs::path& p, const std::string& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

struct Result {
    double bestSec = -1.0;
    double worstSec = -1.0;
    long long outBytes = -1;
    int outObjects = -1;
    bool ok = false;
    std::string note;
};

// ---------------------------------------------------------------------------
// Output inspection: object count + a content check on the reopened file.
// The content check is what stops a "fast" variant from being fast because it
// silently produced something other than the edited document.
// ---------------------------------------------------------------------------
struct Inspection {
    int objects = -1;
    int pages = -1;
    bool firstPageIsOriginalLast = false;
    bool firstPageHasText = false;
};

Inspection inspectOutput(const fs::path& out, int expectedPages, bool uniqueCorpus)
{
    Inspection r;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return r;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        pdf_document* doc = pdf_open_document(ctx, out.string().c_str());
        fz_try(ctx) {
            r.objects = pdf_xref_len(ctx, doc);
            r.pages = pdf_count_pages(ctx, doc);
        }
        fz_always(ctx) { pdf_drop_document(ctx, doc); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    }
    fz_catch(ctx) {
        r.objects = -1;
    }
    fz_drop_context(ctx);

    // Content check via the shared decompressed-content oracle (the SAME
    // oracle the R3-24 guard test uses), on a fresh context.
    if (r.pages > 0) {
        const std::string s = pdfe::testsupport::decompressedPageContent(out, 0);
        r.firstPageHasText = s.find("Tj") != std::string::npos;
        if (uniqueCorpus) {
            const std::string want = "(Page " + std::to_string(expectedPages) + ")";
            r.firstPageIsOriginalLast = s.find(want) != std::string::npos;
        } else {
            // The `dup` corpus cannot distinguish pages by content, so the
            // reversal is not content-verifiable there; page count + non-empty
            // content is the honest limit of what this corpus can assert.
            r.firstPageIsOriginalLast = r.firstPageHasText;
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// The MuPDF-level variants.
// ---------------------------------------------------------------------------
struct SaveOpts {
    int garbage = 3;
    int objstms = 0;
    int incremental = 0;
};

// Returns save seconds, or -1 on failure (note filled).
double runMupdfVariant(const fs::path& src, const fs::path& out,
                       int pages, const SaveOpts& so, std::string& note)
{
    double sec = -1.0;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) { note = "no fz_context"; return -1.0; }

    // For an incremental append the write target must be the file the document
    // was loaded from, so the source is copied to `out` first and edited there.
    const fs::path openPath = so.incremental ? out : src;
    if (so.incremental) {
        std::error_code ec;
        fs::remove(out, ec);
        fs::copy_file(src, out, fs::copy_options::overwrite_existing, ec);
        if (ec) { fz_drop_context(ctx); note = "copy failed"; return -1.0; }
    }

    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        pdf_document* doc = pdf_open_document(ctx, openPath.string().c_str());
        fz_try(ctx) {
            std::vector<int> order(static_cast<size_t>(pages));
            std::iota(order.rbegin(), order.rend(), 0);
            pdf_rearrange_pages(ctx, doc, pages, order.data(),
                                PDF_CLEAN_STRUCTURE_KEEP);
            pdf_drop_page_tree(ctx, doc);

            pdf_write_options opts{};
            opts.do_garbage = so.garbage;
            opts.do_compress = 1;
            opts.do_compress_images = 1;
            opts.do_compress_fonts = 1;
            opts.do_preserve_metadata = 1;
            opts.do_use_objstms = so.objstms;
            opts.do_incremental = so.incremental;

            const std::string target =
                (so.incremental ? out : out).string();
            if (!so.incremental) {
                std::error_code ec;
                fs::remove(out, ec);
            }
            auto t0 = std::chrono::steady_clock::now();
            pdf_save_document(ctx, doc, target.c_str(), &opts);
            auto el = std::chrono::steady_clock::now() - t0;
            sec = std::chrono::duration<double>(el).count();
        }
        fz_always(ctx) { pdf_drop_document(ctx, doc); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    }
    fz_catch(ctx) {
        note = fz_caught_message(ctx) ? fz_caught_message(ctx) : "fz error";
        sec = -1.0;
    }
    fz_drop_context(ctx);
    return sec;
}

// The production path, unmodified.
double runProdVariant(const fs::path& src, const fs::path& out,
                      int pages, std::string& note)
{
    double sec = -1.0;
    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, src);
        if (!doc) { note = "open failed"; return -1.0; }
        if (doc->pageCount() != pages) { note = "page count mismatch"; return -1.0; }
        std::vector<int> order(static_cast<size_t>(pages));
        std::iota(order.rbegin(), order.rend(), 0);
        doc->reorderPages(order);
        doc->markNeedsFullGc();
        if (doc->effectiveGarbageLevel() != 3) {
            note = "effectiveGarbageLevel != 3";
            return -1.0;
        }
        std::error_code ec;
        fs::remove(out, ec);
        auto t0 = std::chrono::steady_clock::now();
        doc->save(out);
        auto el = std::chrono::steady_clock::now() - t0;
        sec = std::chrono::duration<double>(el).count();
    } catch (const std::exception& e) {
        note = e.what();
        return -1.0;
    }
    return sec;
}

std::vector<std::string> splitCsv(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    int reps = 5;
    std::string pagesArg = "50,100,250,500,1000,1500,2000,2500";
    std::string corpusArg = "unique,uniquex,dupfont";
    bool keep = false;
    std::string variantsArg =
        "prod,mupdf_gc3,mupdf_gc2,mupdf_gc1,mupdf_gc3_objstm,"
        "mupdf_gc1_objstm,mupdf_incr";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (a == "--reps") reps = std::stoi(next());
        else if (a == "--pages") pagesArg = next();
        else if (a == "--corpus") corpusArg = next();
        else if (a == "--variants") variantsArg = next();
        else if (a == "--keep") keep = true;  // leave outputs on disk for byte-level comparison
        else { std::cerr << "unknown arg: " << a << "\n"; return 2; }
    }

    std::vector<int> ladder;
    for (const auto& p : splitCsv(pagesArg)) ladder.push_back(std::stoi(p));
    const auto corpora = splitCsv(corpusArg);
    const auto variants = splitCsv(variantsArg);

    const fs::path td = fs::temp_directory_path() / "pdfe_phase40_bench";
    std::error_code ec;
    fs::remove_all(td, ec);
    fs::create_directories(td);

    std::cout << "corpus,variant,pages,rep_count,best_sec,worst_sec,"
                 "out_bytes,out_objects,ok,note\n";
    std::cout.flush();

    for (const auto& corpus : corpora) {
        CorpusSpec spec;
        if (corpus == "unique")       spec = {false, false};
        else if (corpus == "uniquex") spec = {false, true};
        else if (corpus == "dupfont") spec = {true,  false};
        else { std::cerr << "unknown corpus: " << corpus << "\n"; return 2; }
        // Every corpus carries per-page distinct "(Page n)" content, so the
        // reversal is content-verifiable on all of them.
        const bool uniqueCorpus = true;
        for (int pages : ladder) {
            const fs::path src = td / (corpus + "_in_" + std::to_string(pages) + ".pdf");
            writeFile(src, buildLargePdf(pages, spec));
            const auto inBytes = static_cast<long long>(fs::file_size(src));
            (void)inBytes;

            for (const auto& variant : variants) {
                Result res;
                std::string note;
                for (int r = 0; r < reps; ++r) {
                    const fs::path out =
                        td / (corpus + "_" + variant + "_out_" +
                              std::to_string(pages) + "_" + std::to_string(r) + ".pdf");
                    double sec = -1.0;
                    if (variant == "prod") {
                        sec = runProdVariant(src, out, pages, note);
                    } else {
                        SaveOpts so;
                        if (variant == "mupdf_gc3")            so = {3, 0, 0};
                        else if (variant == "mupdf_gc2")       so = {2, 0, 0};
                        else if (variant == "mupdf_gc1")       so = {1, 0, 0};
                        else if (variant == "mupdf_gc0")       so = {0, 0, 0};
                        else if (variant == "mupdf_gc3_objstm") so = {3, 1, 0};
                        else if (variant == "mupdf_gc1_objstm") so = {1, 1, 0};
                        else if (variant == "mupdf_incr")      so = {0, 0, 1};
                        else { note = "unknown variant"; break; }
                        sec = runMupdfVariant(src, out, pages, so, note);
                    }
                    if (sec < 0.0) break;
                    if (res.bestSec < 0.0 || sec < res.bestSec) res.bestSec = sec;
                    if (sec > res.worstSec) res.worstSec = sec;

                    // Inspect only on the LAST rep, so inspection cost never
                    // sits between two timed saves.
                    if (r == reps - 1) {
                        res.outBytes = static_cast<long long>(fs::file_size(out, ec));
                        const auto insp = inspectOutput(out, pages, uniqueCorpus);
                        res.outObjects = insp.objects;
                        res.ok = (insp.pages == pages) &&
                                 insp.firstPageHasText &&
                                 insp.firstPageIsOriginalLast;
                        if (!res.ok) {
                            note += "[pages=" + std::to_string(insp.pages) +
                                    " text=" + std::to_string(insp.firstPageHasText) +
                                    " reordered=" +
                                    std::to_string(insp.firstPageIsOriginalLast) + "]";
                        }
                    }
                    std::error_code rmec;
                    if (r != reps - 1 && !keep) fs::remove(out, rmec);
                }
                std::printf("%s,%s,%d,%d,%.4f,%.4f,%lld,%d,%d,%s\n",
                            corpus.c_str(), variant.c_str(), pages, reps,
                            res.bestSec, res.worstSec, res.outBytes,
                            res.outObjects, res.ok ? 1 : 0, note.c_str());
                std::fflush(stdout);
            }
            std::error_code rmec;
            if (!keep) fs::remove(src, rmec);
        }
    }

    if (keep) {
        std::cerr << "outputs kept in: " << td.string() << std::endl;
    } else {
        fs::remove_all(td, ec);
    }
    return 0;
}
