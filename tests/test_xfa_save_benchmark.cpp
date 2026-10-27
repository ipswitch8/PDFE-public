// -*- coding: utf-8 -*-
//
// Pipeline b2e47d91 — Tier 0 (Phase 1): GC-level save benchmark.
//
// Purpose: measure pdf_save_document wall-clock cost at do_garbage =
// 0/1/2/3 on the three known-slow dynamic-XFA / large fixtures, with a
// round-trip correctness check per cell, and emit a data-driven go/no-go
// DECISION for the rest of the pipeline:
//
//   (a) a lower GC level is fast AND round-trips clean  -> Tier 1 + Tier 2
//   (b) even gc=1 is still minutes                      -> Tier 1 only, skip Tier 2
//
// This test makes NO production-source change. It reproduces the EXACT
// option set used by Document::save()/saveAs() (Document.cpp:713-719) —
//   do_compress / do_compress_images / do_compress_fonts /
//   do_preserve_metadata all = 1 — and varies ONLY do_garbage, so the
// measured delta is attributable to the GC pass alone. It also calls
// pdf_drop_page_tree before each save to mirror production (Document.cpp:702).
//
// Gated behind PDFE_RUN_SLOW_SAVE_TESTS (same opt-in as the SaveRoundTrip
// slow tests in test_realworld_external_corpus.cpp). The gc=3 saves alone
// take ~35 min on Release; never run in normal CI.
//
// Optional: set XFA_BENCH_OUT to an absolute path to have the test write
// the Markdown results table directly to that file (in addition to the
// sentinel-delimited block printed to stdout).

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"  // brings <mupdf/fitz.h> + <mupdf/pdf.h> + MuPdfContext

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using pdfe::core::MuPdfContext;

namespace {

namespace fs = std::filesystem;

// The three known-slow fixtures, in ascending baseline-cost order, with
// the previously-measured Release gc=3 wall-clock (ms) used as a sanity
// cross-check (PDFE-LIMITATION-SAVEAS-LARGE-XFA, known_issues.md).
struct Fixture {
    const char* stem;
    long long baselineGc3Ms;  // prior Release measurement
};
const std::vector<Fixture> kFixtures = {
    {"ssa_sf86",   314536},   // 8 MB XFA gov form  (~5.24 min)
    {"irs_p17",    733721},   // 3 MB 142-page pub  (~12.2 min)
    {"irs_i1040", 1030384},   // 4 MB 126-page set  (~17.2 min)
};

const int kGcLevels[] = {0, 1, 2, 3};

// "Fast enough to matter" threshold for the go/no-go decision: a save
// under one minute is a usable interactive save. The decision is (a)
// iff EVERY fixture has a gc level in {1,2} that round-trips clean AND
// completes under this bound. (gc=0 is measured for reference only — it
// performs no collection and is not a correctness-safe production option.)
constexpr long long kFastMs = 60000;

fs::path corpusDir() { return fs::path(TEST_FIXTURES_DIR) / "realworld"; }

struct Cell {
    long long ms = -1;
    bool roundTrip = false;
    bool saveThrew = false;
    std::string err;
};

}  // namespace

// One test that walks all (fixture x gc-level) cells, then writes the
// table + DECISION. Self-skips unless PDFE_RUN_SLOW_SAVE_TESTS is set.
TEST(XfaSaveBenchmark, MeasureGcLevels) {
    const char* optIn = std::getenv("PDFE_RUN_SLOW_SAVE_TESTS");
    if (!optIn || optIn[0] == '\0' || optIn[0] == '0') {
        GTEST_SKIP() << "Tier-0 benchmark is slow (~35+ min on Release); "
                        "set PDFE_RUN_SLOW_SAVE_TESTS=1 to run.";
    }

    auto ctx = std::make_shared<MuPdfContext>();

    auto tmpDir = fs::temp_directory_path() / "pdfe_xfa_bench";
    fs::create_directories(tmpDir);

    // results[fixtureIdx][gcIdx]
    std::vector<std::array<Cell, 4>> results(kFixtures.size());

    for (size_t fi = 0; fi < kFixtures.size(); ++fi) {
        const auto& fx = kFixtures[fi];
        fs::path src = corpusDir() / (std::string(fx.stem) + ".pdf");
        if (!fs::exists(src)) {
            for (int gi = 0; gi < 4; ++gi) {
                results[fi][gi].err = "fixture missing";
            }
            ADD_FAILURE() << "Fixture missing: " << src;
            continue;
        }
        const std::string srcStr = src.string();

        for (int gi = 0; gi < 4; ++gi) {
            const int gc = kGcLevels[gi];
            Cell& cell = results[fi][gi];

            fs::path out = tmpDir / (std::string(fx.stem) + "_gc" +
                                     std::to_string(gc) + ".pdf");
            std::error_code rmEc;
            fs::remove(out, rmEc);
            const std::string outStr = out.string();

            // Open fresh for each level so the GC pass runs on a pristine
            // in-memory load — matching the real "open file, then save" flow.
            pdf_document* doc = nullptr;
            try {
                ctx->execute([&](fz_context* c) {
                    doc = pdf_open_document(c, srcStr.c_str());
                });
            } catch (const std::exception& e) {
                cell.saveThrew = true;
                cell.err = std::string("open: ") + e.what();
                continue;
            }
            if (!doc) {
                cell.saveThrew = true;
                cell.err = "open returned null";
                continue;
            }

            // Timed region: drop page tree + save, exactly as production
            // does (Document.cpp:702 then :713-719), varying only do_garbage.
            auto t0 = std::chrono::steady_clock::now();
            try {
                ctx->execute([&](fz_context* c) {
                    pdf_drop_page_tree(c, doc);
                    pdf_write_options opts{};
                    opts.do_garbage = gc;
                    opts.do_compress = 1;
                    opts.do_compress_images = 1;
                    opts.do_compress_fonts = 1;
                    opts.do_preserve_metadata = 1;
                    pdf_save_document(c, doc, outStr.c_str(), &opts);
                });
            } catch (const std::exception& e) {
                cell.saveThrew = true;
                cell.err = std::string("save: ") + e.what();
            }
            auto t1 = std::chrono::steady_clock::now();
            cell.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          t1 - t0).count();

            // Release the source handle.
            try {
                ctx->execute([&](fz_context* c) { pdf_drop_document(c, doc); });
            } catch (...) { /* best-effort */ }
            doc = nullptr;

            // Round-trip: re-open the saved file and confirm pages > 0.
            if (!cell.saveThrew && fs::exists(out) && fs::file_size(out) > 100) {
                try {
                    ctx->execute([&](fz_context* c) {
                        pdf_document* rd = pdf_open_document(c, outStr.c_str());
                        int n = rd ? pdf_count_pages(c, rd) : 0;
                        if (rd) pdf_drop_document(c, rd);
                        cell.roundTrip = (n > 0);
                    });
                } catch (const std::exception& e) {
                    cell.roundTrip = false;
                    cell.err += std::string(" reopen: ") + e.what();
                }
            }

            fs::remove(out, rmEc);

            std::cout << "[bench] " << fx.stem << " gc=" << gc
                      << " ms=" << cell.ms
                      << " rt=" << (cell.roundTrip ? "PASS" : "FAIL")
                      << (cell.err.empty() ? "" : (" (" + cell.err + ")"))
                      << std::endl;
        }
    }

    // ---- Decision: (a) iff every fixture has a gc in {1,2} that
    //      round-trips clean AND is under kFastMs. ----
    bool decisionA = true;
    for (size_t fi = 0; fi < kFixtures.size(); ++fi) {
        bool fixtureHasFastSafe = false;
        for (int gi = 1; gi <= 2; ++gi) {  // gc=1 and gc=2 only
            const Cell& c = results[fi][gi];
            if (c.roundTrip && c.ms >= 0 && c.ms < kFastMs) {
                fixtureHasFastSafe = true;
                break;
            }
        }
        if (!fixtureHasFastSafe) { decisionA = false; break; }
    }
    const std::string decisionLine =
        decisionA
            ? "DECISION: (a) lower GC is viable — proceed to Tier 1 + Tier 2"
            : "DECISION: (b) lower GC is still slow — proceed to Tier 1 only, skip Tier 2";

    // ---- Build the Markdown table. ----
    auto fmtMs = [](long long ms) -> std::string {
        if (ms < 0) return "—";
        std::ostringstream o;
        o << ms;
        return o.str();
    };
    auto rt = [](const Cell& c) -> std::string {
        if (c.saveThrew) return "THROW";
        return c.roundTrip ? "PASS" : "FAIL";
    };

    std::ostringstream md;
    md << "# XFA / large-form save benchmark — GC level sweep\n\n";
    md << "Pipeline `b2e47d91` Tier 0 (Phase 1). Generated by "
          "`tests/test_xfa_save_benchmark.cpp`\n";
    md << "(`PDFE_RUN_SLOW_SAVE_TESTS=1`, Release build). Option set held "
          "identical to\n";
    md << "`Document::save()` (compress / compress_images / compress_fonts / "
          "preserve_metadata = 1);\n";
    md << "only `do_garbage` varies. Times are milliseconds of the "
          "`pdf_drop_page_tree` + `pdf_save_document` region.\n\n";

    md << "| fixture | gc=0 ms | gc=1 ms | gc=2 ms | gc=3 ms | gc=0 rt | "
          "gc=1 rt | gc=2 rt | gc=3 rt |\n";
    md << "|---|---:|---:|---:|---:|---|---|---|---|\n";
    for (size_t fi = 0; fi < kFixtures.size(); ++fi) {
        const auto& r = results[fi];
        md << "| " << kFixtures[fi].stem << " | "
           << fmtMs(r[0].ms) << " | " << fmtMs(r[1].ms) << " | "
           << fmtMs(r[2].ms) << " | " << fmtMs(r[3].ms) << " | "
           << rt(r[0]) << " | " << rt(r[1]) << " | "
           << rt(r[2]) << " | " << rt(r[3]) << " |\n";
    }
    md << "\n";

    // Baseline cross-check footnote (gc=3 vs prior Release measurement).
    md << "**gc=3 baseline cross-check** (prior Release figures from "
          "`PDFE-LIMITATION-SAVEAS-LARGE-XFA`):\n\n";
    for (size_t fi = 0; fi < kFixtures.size(); ++fi) {
        long long got = results[fi][3].ms;
        long long base = kFixtures[fi].baselineGc3Ms;
        double pct = (base > 0 && got >= 0)
                         ? 100.0 * (double)(got - base) / (double)base
                         : 0.0;
        md << "- `" << kFixtures[fi].stem << "`: measured gc=3 = " << got
           << " ms vs baseline " << base << " ms ("
           << std::showpos << std::fixed << std::setprecision(1) << pct
           << std::noshowpos << "%)";
        if (got >= 0 && base > 0 && (pct > 20.0 || pct < -20.0)) {
            md << " — **>20% deviation**: likely machine-load / build-config "
                  "difference; numbers above are this run's ground truth.";
        }
        md << "\n";
    }
    md << "\n";
    md << "Decision threshold: a fixture is \"fast-safe\" if some gc level in "
          "{1,2} round-trips PASS **and** completes under "
       << kFastMs << " ms.\n";
    md << "Decision is (a) iff **all three** fixtures are fast-safe.\n\n";
    md << decisionLine << "\n";

    // ---- Emit: stdout sentinel block + optional file. ----
    const std::string body = md.str();
    std::cout << "\n===XFA_BENCH_MD_BEGIN===\n"
              << body
              << "===XFA_BENCH_MD_END===\n";

    if (const char* outFile = std::getenv("XFA_BENCH_OUT")) {
        if (outFile[0] != '\0') {
            std::ofstream ofs(outFile, std::ios::binary | std::ios::trunc);
            if (ofs) {
                ofs << body;
                ofs.close();
                std::cout << "[bench] wrote " << outFile << std::endl;
            } else {
                ADD_FAILURE() << "Could not open XFA_BENCH_OUT: " << outFile;
            }
        }
    }

    // The benchmark itself is informational — a cell that throws on save
    // is a real finding (recorded as THROW), not a test failure. Only a
    // total inability to produce any data fails the test.
    bool anyData = false;
    for (auto& row : results)
        for (auto& c : row)
            if (c.ms >= 0) anyData = true;
    EXPECT_TRUE(anyData) << "No benchmark cell produced a measurement.";
}
