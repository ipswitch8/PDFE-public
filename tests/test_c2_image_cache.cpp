// Tests for C2 — ImageReplacer memory-budgeted cache.
//
// Covers the public cache API exposed by C2:
//   - setCacheBudgetBytes / cacheBudgetBytes
//   - cacheStats (currentBytes, maxBytes, entries, hits, misses, evictions)
//   - default budget (kDefaultCacheBudgetBytes = 256 MB)
//   - clearOriginalCache maintains counters
//   - setCacheBudgetBytes(0) drains the cache
//
// These tests construct ImageReplacer with null component pointers
// because the cache API does not touch any of them. The end-to-end
// eviction behavior (populated cache from real replace() calls) is
// harder to exercise without a full test fixture scaffold; these
// unit tests cover correctness of the cache accounting itself.
//
// See docs/image-pdf-risks.md (C2).

#include <gtest/gtest.h>

#include "tre/ImageReplacer.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/ImageMasker.h"
#include "tre/BitmapCompositor.h"
#include "tre/PdfContentWriter.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QDebug>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

using pdfe::tre::ImageReplacer;
using namespace pdfe::tre;
using namespace pdfe::core;

namespace {

ImageReplacer makeBareReplacer() {
    // The cache API never dereferences any of these pointers.
    return ImageReplacer(
        /*ocrPipeline*/   nullptr,
        /*coordMapper*/   nullptr,
        /*bgDetector*/    nullptr,
        /*fontMatcher*/   nullptr,
        /*imageMasker*/   nullptr,
        /*bitmapCompositor*/ nullptr,
        /*contentWriter*/ nullptr);
}

} // namespace

TEST(C2ImageCacheTest, DefaultBudgetIs256MB) {
    auto r = makeBareReplacer();
    EXPECT_EQ(r.cacheBudgetBytes(),
              ImageReplacer::kDefaultCacheBudgetBytes);
    EXPECT_EQ(ImageReplacer::kDefaultCacheBudgetBytes,
              static_cast<qint64>(256) * 1024 * 1024);
}

TEST(C2ImageCacheTest, InitialStatsAreZero) {
    auto r = makeBareReplacer();
    auto s = r.cacheStats();
    EXPECT_EQ(s.currentBytes, 0);
    EXPECT_EQ(s.entries, 0);
    EXPECT_EQ(s.hits, 0);
    EXPECT_EQ(s.misses, 0);
    EXPECT_EQ(s.evictions, 0);
    EXPECT_EQ(s.maxBytes, ImageReplacer::kDefaultCacheBudgetBytes);
}

TEST(C2ImageCacheTest, SetBudgetRoundTrips) {
    auto r = makeBareReplacer();
    r.setCacheBudgetBytes(64 * 1024 * 1024);
    EXPECT_EQ(r.cacheBudgetBytes(), 64 * 1024 * 1024);

    r.setCacheBudgetBytes(1024);
    EXPECT_EQ(r.cacheBudgetBytes(), 1024);

    // Large values (multi-GB) must be accepted without overflow.
    constexpr qint64 twoGB = static_cast<qint64>(2) * 1024 * 1024 * 1024;
    r.setCacheBudgetBytes(twoGB);
    EXPECT_EQ(r.cacheBudgetBytes(), twoGB);
}

TEST(C2ImageCacheTest, NegativeBudgetClampsToZero) {
    auto r = makeBareReplacer();
    r.setCacheBudgetBytes(-500);
    EXPECT_EQ(r.cacheBudgetBytes(), 0);
}

TEST(C2ImageCacheTest, BudgetZeroDisablesCache) {
    auto r = makeBareReplacer();
    r.setCacheBudgetBytes(0);
    EXPECT_EQ(r.cacheBudgetBytes(), 0);
    auto s = r.cacheStats();
    EXPECT_EQ(s.currentBytes, 0);
    EXPECT_EQ(s.entries, 0);
}

TEST(C2ImageCacheTest, ClearResetsSize) {
    auto r = makeBareReplacer();
    r.clearOriginalCache();
    auto s = r.cacheStats();
    EXPECT_EQ(s.currentBytes, 0);
    EXPECT_EQ(s.entries, 0);
    // hits/misses are session counters and do NOT reset on clear — only
    // evictions and current usage. Verify they stay at zero before any
    // operation but aren't explicitly reset here.
    EXPECT_EQ(s.hits, 0);
    EXPECT_EQ(s.misses, 0);
}

TEST(C2ImageCacheTest, BudgetChangeDoesNotCorruptCounters) {
    auto r = makeBareReplacer();
    // A sequence of budget changes with an empty cache should be a noop
    // for all counters except maxBytes.
    r.setCacheBudgetBytes(100);
    r.setCacheBudgetBytes(200);
    r.setCacheBudgetBytes(50);
    r.setCacheBudgetBytes(0);
    r.setCacheBudgetBytes(10 * 1024 * 1024);
    auto s = r.cacheStats();
    EXPECT_EQ(s.currentBytes, 0);
    EXPECT_EQ(s.entries, 0);
    EXPECT_EQ(s.hits, 0);
    EXPECT_EQ(s.misses, 0);
    EXPECT_EQ(s.evictions, 0);
    EXPECT_EQ(s.maxBytes, 10 * 1024 * 1024);
}

TEST(C2ImageCacheTest, InvalidateOnEmptyCacheIsNoop) {
    auto r = makeBareReplacer();
    r.invalidateOriginalCache(0);
    r.invalidateOriginalCache(99);
    auto s = r.cacheStats();
    EXPECT_EQ(s.currentBytes, 0);
    EXPECT_EQ(s.entries, 0);
    EXPECT_EQ(s.evictions, 0);  // nothing to evict, no increment
}

// -----------------------------------------------------------------------
// End-to-end eviction tests over a real multi-page image PDF.
//
// They exercise the non-sequential editing pattern that defeated the old
// 3-page cache: touch pages scattered across the document, then come back to
// an early one.
//
// Every budget and page index below is derived from the document actually
// opened -- its page count, and one page's measured cache cost. Nothing is
// expressed in the units of the document these cases were first written
// against (GitHub #261), because a budget chosen for 14 MB pages holds every
// page of a smaller document and the eviction assertions would then pass
// without an eviction ever happening.
// -----------------------------------------------------------------------

namespace {

// GitHub #261. This was an absolute path to a document on another developer's
// Desktop. It exists on no machine in the project, so all three cases below
// SKIP'd on every run, everywhere -- three of the suite's 22 live skips, and
// the C2 cache behaviour they exist to prove was measured nowhere.
//
// What the cases actually need is stated in the fixture's own SetUp: a document
// of at least 20 pages whose pages carry image XObjects, so that non-sequential
// editing across it defeats a small page cache. `phase3_large_100page_image.pdf`
// is a committed fixture with 100 such pages. The original document's identity
// was never load-bearing; its SHAPE was.
//
// The override hook is kept for a developer who wants to run these against a
// specific real-world document, but there is no fall-through to a skip: that is
// what made them invisible.
inline std::filesystem::path specimenBLikeDocument()
{
    if (const char* env = std::getenv("PDFE_LARGE_IMAGE_PDF")) {
        if (env[0] != '\0') {
            std::filesystem::path p(env);
            if (std::filesystem::exists(p)) return p;
        }
    }
    return std::filesystem::path(TEST_FIXTURES_DIR) / "phase3_large_100page_image.pdf";
}

const std::filesystem::path kSpecimenBPath = specimenBLikeDocument();

} // namespace

class C2SpecimenBCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::filesystem::exists(kSpecimenBPath)) {
            have_ = false;
            return;
        }
        context_ = std::make_shared<MuPdfContext>();
        try {
            document_ = Document::open(context_, kSpecimenBPath);
        } catch (...) {
            have_ = false;
            return;
        }
        if (!document_ || document_->pageCount() < 20) {
            have_ = false;
            return;
        }

        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        contentWriter_ = std::make_unique<PdfContentWriter>(
            context_.get(), fontEmbedder_.get());
        ocrPipeline_ = std::make_unique<OcrPipeline>(
            context_.get(), document_.get());
        // No tessdata needed — we never call ocrPage(). primeOriginalCache
        // hits getOriginalPageImage which reads the image XObject directly
        // via PdfContentWriter::getOriginalImage.
        replacer_ = std::make_unique<ImageReplacer>(
            ocrPipeline_.get(),
            /*coordMapper*/ nullptr,
            /*bgDetector*/ nullptr,
            /*fontMatcher*/ nullptr,
            /*imageMasker*/ nullptr,
            /*bitmapCompositor*/ nullptr,
            contentWriter_.get());
        have_ = true;
    }

    void TearDown() override {
        replacer_.reset();
        ocrPipeline_.reset();
        contentWriter_.reset();
        fontEmbedder_.reset();
        document_.reset();
        context_.reset();
    }

    std::unique_ptr<Page> pageAt(int idx) { return document_->page(idx); }

    // `count` page indices spread across the document, so the access pattern
    // is non-sequential rather than a walk. Evenly spread when the document
    // has at least 2*count pages; on a shorter one the stride floors at 1 and
    // the indices bunch at the front, which still defeats a page-count cache
    // but is not what "spread" would lead a reader to expect. Said here
    // because the callers assert `pages.size() == count` and would otherwise
    // read as asserting the spacing too.
    std::vector<int> spreadPages(int count) const {
        std::vector<int> out;
        const int n = document_->pageCount();
        const int stride = std::max(1, n / count);
        for (int i = 0; i < count && i * stride < n; ++i) out.push_back(i * stride);
        return out;
    }

    // One page's real cost in the cache, measured on this document. Leaves the
    // cache empty; the hit/miss counters keep their history.
    qint64 measurePageBytes(int pageIndex) {
        replacer_->setCacheBudgetBytes(ImageReplacer::kDefaultCacheBudgetBytes);
        replacer_->clearOriginalCache();
        auto pg = pageAt(pageIndex);
        EXPECT_TRUE(pg) << "page " << pageIndex;
        if (!pg) return 0;
        EXPECT_TRUE(replacer_->primeOriginalCache(pg.get(), document_.get(), pageIndex))
            << "priming page " << pageIndex << " failed, so no page cost can be measured";
        const qint64 bytes = replacer_->cacheStats().currentBytes;
        replacer_->clearOriginalCache();
        return bytes;
    }

    bool have_ = false;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> fontEmbedder_;
    std::unique_ptr<PdfContentWriter> contentWriter_;
    std::unique_ptr<OcrPipeline> ocrPipeline_;
    std::unique_ptr<ImageReplacer> replacer_;
};

TEST_F(C2SpecimenBCacheTest, NonSequentialEditingKeepsEarlyPages) {
    ASSERT_TRUE(have_)
        << "the large multi-page image document did not open, or has "
           "fewer than 20 pages: " << kSpecimenBPath.string()
        << ". GitHub #261 -- this used to SKIP here, pointed at a path "
           "on another developer's Desktop, so the C2 cache behaviour "
           "was measured nowhere. A missing fixture is now a failure.";

    // Prime 12 pages spread across the document, then re-access page 0.
    // Under the OLD 3-page cache, page 0 was evicted as soon as page 3 was
    // primed and re-reading it counted as a miss. Under the byte budget it
    // is still resident and must register as a hit.
    //
    // The budget must be large enough to hold all 12 -- assert that rather
    // than assume it, or an under-sized budget would turn this into a test
    // of eviction wearing the name of a test of retention.
    const std::vector<int> pages = spreadPages(12);
    ASSERT_EQ(pages.size(), 12u) << "document too short to spread 12 pages across";
    const qint64 pageBytes = measurePageBytes(pages.front());
    ASSERT_GT(pageBytes, 0) << "a primed page occupied no cache bytes";
    ASSERT_GE(ImageReplacer::kDefaultCacheBudgetBytes, 12 * pageBytes)
        << "the default budget cannot hold 12 pages of this document ("
        << pageBytes << " B each), so retention is not what would be measured";

    // Calibration was itself cache traffic: one miss, and one eviction when it
    // cleared. The counters are session totals, so everything below is a delta
    // from here rather than an absolute.
    const auto base = replacer_->cacheStats();
    ASSERT_EQ(base.entries, 0) << "calibration left the cache populated";
    for (int p : pages) {
        auto pg = pageAt(p);
        ASSERT_TRUE(pg) << "page " << p;
        bool ok = replacer_->primeOriginalCache(pg.get(), document_.get(), p);
        EXPECT_TRUE(ok) << "prime page " << p;
    }
    auto statsAfterPrime = replacer_->cacheStats();
    qDebug() << "After priming 12 pages: currentBytes="
             << statsAfterPrime.currentBytes
             << "entries=" << statsAfterPrime.entries
             << "hits=" << statsAfterPrime.hits
             << "misses=" << statsAfterPrime.misses
             << "evictions=" << statsAfterPrime.evictions;

    // All 12 fit, as asserted above.
    EXPECT_EQ(statsAfterPrime.entries, 12);
    EXPECT_EQ(statsAfterPrime.evictions, base.evictions)
        << "nothing should have been evicted: the budget holds all 12";
    EXPECT_EQ(statsAfterPrime.misses, base.misses + 12)
        << "each of the 12 pages should have been a first-time miss";
    EXPECT_EQ(statsAfterPrime.hits, base.hits)
        << "no page was primed twice, so no hit is possible yet";
    EXPECT_LE(statsAfterPrime.currentBytes, statsAfterPrime.maxBytes);

    // Re-access page 0 — must be a cache hit (the old 3-page cache
    // would have evicted it after page 3 was primed).
    auto pg0 = pageAt(0);
    ASSERT_TRUE(pg0);
    replacer_->primeOriginalCache(pg0.get(), document_.get(), 0);
    auto statsAfterRehit = replacer_->cacheStats();
    EXPECT_EQ(statsAfterRehit.hits, statsAfterPrime.hits + 1)
        << "Page 0 should have been a cache hit — this is the C2 fix";
    EXPECT_EQ(statsAfterRehit.misses, statsAfterPrime.misses)
        << "No new miss — the page should have stayed in cache";
}

TEST_F(C2SpecimenBCacheTest, SmallBudgetCausesByteBudgetedEviction) {
    ASSERT_TRUE(have_)
        << "the large multi-page image document did not open, or has "
           "fewer than 20 pages: " << kSpecimenBPath.string()
        << ". GitHub #261 -- this used to SKIP here, pointed at a path "
           "on another developer's Desktop, so the C2 cache behaviour "
           "was measured nowhere. A missing fixture is now a failure.";
    // A budget sized to hold roughly three pages OF THIS DOCUMENT. Prime 10;
    // the cache must evict on a byte basis, not a page-count basis.
    const std::vector<int> pages = spreadPages(10);
    ASSERT_EQ(pages.size(), 10u) << "document too short to spread 10 pages across";
    const qint64 pageBytes = measurePageBytes(pages.front());
    ASSERT_GT(pageBytes, 0) << "a primed page occupied no cache bytes";
    // Calibration's own prime-and-clear already moved the eviction counter.
    const auto base = replacer_->cacheStats();
    replacer_->setCacheBudgetBytes(3 * pageBytes);
    for (int p : pages) {
        auto pg = pageAt(p);
        ASSERT_TRUE(pg);
        replacer_->primeOriginalCache(pg.get(), document_.get(), p);
    }
    auto stats = replacer_->cacheStats();
    qDebug() << "After 10 primes @ 50 MB budget:"
             << "entries=" << stats.entries
             << "currentBytes=" << stats.currentBytes
             << "evictions=" << stats.evictions;

    // The invariants we actually care about:
    // (1) budget is always respected,
    // (2) eviction actually fired (not all 10 still cached),
    // (3) at least one page is still cached (we didn't overshoot).
    EXPECT_LE(stats.currentBytes, stats.maxBytes);
    EXPECT_LT(stats.entries, 10);
    EXPECT_GE(stats.entries, 1);
    EXPECT_EQ(stats.entries + (stats.evictions - base.evictions), 10)
        << "Every primed page must be accounted for as either cached or evicted";
    EXPECT_GE(stats.evictions - base.evictions, 5)
        << "At least half the pages should be out";
    EXPECT_LE(stats.entries, 3)
        << "a three-page budget retained more than three pages";
}

TEST_F(C2SpecimenBCacheTest, BudgetShrinkEvictsImmediately) {
    ASSERT_TRUE(have_)
        << "the large multi-page image document did not open, or has "
           "fewer than 20 pages: " << kSpecimenBPath.string()
        << ". GitHub #261 -- this used to SKIP here, pointed at a path "
           "on another developer's Desktop, so the C2 cache behaviour "
           "was measured nowhere. A missing fixture is now a failure.";
    // Prime 10 pages with the default budget, then shrink the budget to two
    // pages' worth and verify eviction ran immediately rather than waiting
    // for the next insertion.
    const std::vector<int> pages = spreadPages(10);
    ASSERT_EQ(pages.size(), 10u) << "document too short to spread 10 pages across";
    const qint64 pageBytes = measurePageBytes(pages.front());
    ASSERT_GT(pageBytes, 0) << "a primed page occupied no cache bytes";
    ASSERT_GE(ImageReplacer::kDefaultCacheBudgetBytes, 10 * pageBytes)
        << "the default budget cannot hold the 10 pages this case shrinks away from";
    for (int p : pages) {
        auto pg = pageAt(p);
        ASSERT_TRUE(pg);
        replacer_->primeOriginalCache(pg.get(), document_.get(), p);
    }
    auto before = replacer_->cacheStats();
    EXPECT_EQ(before.entries, 10);

    const qint64 shrunkBudget = 2 * pageBytes;
    replacer_->setCacheBudgetBytes(shrunkBudget);
    auto after = replacer_->cacheStats();
    qDebug() << "After shrink to 30 MB:"
             << "entries=" << after.entries
             << "bytes=" << after.currentBytes
             << "evictions=" << after.evictions;
    EXPECT_LE(after.currentBytes, shrunkBudget);
    EXPECT_LE(after.entries, 2);
    EXPECT_GE(after.evictions, before.evictions + 8);
}

// -----------------------------------------------------------------------
// Documentation of the OLD behavior that C2 replaces:
// The previous implementation hardcoded kMaxCachedPages = 3 and had no
// byte accounting. Verify the new API does not carry that constant.
// -----------------------------------------------------------------------
TEST(C2ImageCacheTest, DefaultBudgetExceedsOldThreePageLimit) {
    // Before C2, a typical 300 DPI Letter page was ~31 MB when decoded
    // to ARGB32 (2550 * 3300 * 4). The old 3-page cap = ~93 MB.
    // The new default budget of 256 MB must fit at least 6 such pages,
    // so non-sequential editing across 4-6 pages no longer re-triggers
    // JPEG recompression.
    const qint64 typicalPageBytes = 2550LL * 3300LL * 4LL;  // ~31 MB
    const qint64 sixPages = 6 * typicalPageBytes;
    EXPECT_GE(ImageReplacer::kDefaultCacheBudgetBytes, sixPages)
        << "Default budget should hold at least 6 typical 300 DPI pages";
}
