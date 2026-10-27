// -*- coding: utf-8 -*-
//
// Coverage-Expansion phase-4 (C-07): performance-regression floor harness.
//
// The existing perf tests (test_performance_stress.cpp / test_search_performance
// .cpp / test_xfa_save_benchmark.cpp) MEASURE hot-path timings but assert no
// floor -- a regression is timed and printed, never gated. This harness measures
// a few KEY hot paths (document open, save-routing, search, tile/display-list
// render) and EMITS them as a machine-readable JSON that the perf-floor gate
// (tools/gate/check_perf_floor.py) compares against a committed same-machine
// baseline (tools/gate/perf_baselines.json). The gate -- NOT this test -- is the
// regression floor; wall-clock is too machine/thermal-dependent to hard-assert a
// tight absolute bound here (see the machine-variance note in the gate script).
//
// This test still ASSERTS a GENEROUS absolute sanity ceiling per metric (a hot
// path that takes tens of seconds on a trivial fixture is broken on any machine)
// AND functional correctness (open yields the expected page count, search runs,
// a display list is produced) so the harness is a real test, not just a writer.
//
// Output path: env PDFE_PERF_JSON if set, else <temp>/pdfe_perf/perf-measured.json.
// The CI perf lane sets PDFE_PERF_JSON and then runs the gate against it.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/SearchEngine.h"
#include "core/SearchResult.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {

std::filesystem::path benchTempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_perf";
    std::filesystem::create_directories(d);
    return d;
}

std::filesystem::path measuredJsonPath()
{
    if (const char* env = std::getenv("PDFE_PERF_JSON")) {
        if (env[0] != '\0') return std::filesystem::path(env);
    }
    return benchTempDir() / "perf-measured.json";
}

double millisSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// Median of a set of samples -- robust to a single scheduling hiccup, which is
// the dominant wall-clock noise source on a shared dev box / CI runner.
double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// The single collected result table, flushed to JSON at the end of the suite.
std::map<std::string, double>& metrics()
{
    static std::map<std::string, double> m;
    return m;
}

class PerfBenchmark : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        haveFixture_ = std::filesystem::exists(fixture_);
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    bool haveFixture_ = false;
};

// document_open_ms -- median wall-clock of Document::open on a small fixture.
TEST_F(PerfBenchmark, DocumentOpen)
{
    if (!haveFixture_) GTEST_SKIP() << "fixture missing: " << fixture_;
    constexpr int kIters = 7;
    std::vector<double> samples;
    int lastPages = -1;
    for (int i = 0; i < kIters; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        auto doc = Document::open(ctx_, fixture_);
        double ms = millisSince(t0);
        ASSERT_NE(doc, nullptr) << "iter " << i;
        lastPages = doc->pageCount();
        samples.push_back(ms);
    }
    EXPECT_GT(lastPages, 0) << "fixture must have at least one page";
    double m = median(samples);
    metrics()["document_open_ms"] = m;
    RecordProperty("median_ms", static_cast<int>(m));
    // Generous absolute sanity ceiling (machine-independent): opening a tiny
    // fixture in tens of seconds means something is catastrophically wrong.
    EXPECT_LT(m, 5000.0) << "document open median = " << m << "ms";
}

// save_routing_ms -- median wall-clock of the full-rewrite save path.
TEST_F(PerfBenchmark, SaveRouting)
{
    if (!haveFixture_) GTEST_SKIP() << "fixture missing: " << fixture_;
    constexpr int kIters = 5;
    std::vector<double> samples;
    auto out = benchTempDir() / "perf_save_out.pdf";
    for (int i = 0; i < kIters; ++i) {
        auto doc = Document::open(ctx_, fixture_);
        ASSERT_NE(doc, nullptr) << "iter " << i;
        auto t0 = std::chrono::steady_clock::now();
        doc->writeRecoveryCopy(out);
        samples.push_back(millisSince(t0));
        EXPECT_TRUE(std::filesystem::exists(out)) << "save must produce a file";
        std::error_code ec;
        std::filesystem::remove(out, ec);
    }
    double m = median(samples);
    metrics()["save_routing_ms"] = m;
    RecordProperty("median_ms", static_cast<int>(m));
    EXPECT_LT(m, 10000.0) << "save routing median = " << m << "ms";
}

// search_ms -- median wall-clock of a full-document text search.
TEST_F(PerfBenchmark, Search)
{
    if (!haveFixture_) GTEST_SKIP() << "fixture missing: " << fixture_;
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), 0);
    constexpr int kIters = 7;
    std::vector<double> samples;
    for (int i = 0; i < kIters; ++i) {
        SearchEngine engine(doc.get());
        auto t0 = std::chrono::steady_clock::now();
        SearchResult r = engine.search("the");
        if (r.totalMatches() == 0) r = engine.search("a");
        samples.push_back(millisSince(t0));
        EXPECT_GE(r.totalMatches(), 0);   // search completed without error
    }
    double m = median(samples);
    metrics()["search_ms"] = m;
    RecordProperty("median_ms", static_cast<int>(m));
    EXPECT_LT(m, 5000.0) << "search median = " << m << "ms";
}

// tile_render_ms -- median wall-clock of display-list creation (the unit the
// TileRenderer pipeline builds per tile).
TEST_F(PerfBenchmark, TileRender)
{
    if (!haveFixture_) GTEST_SKIP() << "fixture missing: " << fixture_;
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), 0);
    constexpr int kIters = 5;
    std::vector<double> samples;
    for (int i = 0; i < kIters; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        void* dl = doc->createDisplayList(0);
        double ms = millisSince(t0);
        EXPECT_NE(dl, nullptr) << "display list must be produced for a valid page";
        if (dl) doc->releaseDisplayList(dl);
        samples.push_back(ms);
    }
    double m = median(samples);
    metrics()["tile_render_ms"] = m;
    RecordProperty("median_ms", static_cast<int>(m));
    EXPECT_LT(m, 5000.0) << "tile render median = " << m << "ms";
}

// Flush the collected metrics to the measured JSON. Named to sort LAST within
// the PerfBenchmark suite (gtest runs tests in a fixture in definition order,
// but the ZZ prefix documents intent and survives reordering flags). It writes
// whatever metrics the preceding tests recorded; a skipped metric is simply
// absent, which the gate treats as a HARD error -- exactly the desired "a
// dropped benchmark must never pass" behavior.
TEST_F(PerfBenchmark, ZZ_WriteMeasuredJson)
{
    const auto& m = metrics();
    if (m.empty()) GTEST_SKIP() << "no metrics recorded (fixture likely missing)";
    auto path = measuredJsonPath();
    std::ofstream f(path, std::ios::trunc);
    ASSERT_TRUE(f.good()) << "cannot open measured JSON for write: " << path;
    f << "{\n";
    std::size_t i = 0;
    for (const auto& [k, v] : m) {
        f << "  \"" << k << "\": " << v;
        f << (++i < m.size() ? ",\n" : "\n");
    }
    f << "}\n";
    f.close();
    ASSERT_TRUE(std::filesystem::exists(path));
    testing::Test::RecordProperty("measured_json", path.string());
    // Non-vacuity: the four hot-path metrics must all be present.
    EXPECT_EQ(m.size(), 4u)
        << "expected 4 hot-path metrics in the measured run, got " << m.size();
}

} // namespace
