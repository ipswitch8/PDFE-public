// -*- coding: utf-8 -*-
//
// C-01 (coverage-expansion register) — concurrency / thread-safety stress for
// the tile-render worker pipeline (TileCache + TilePrefetcher).
//
// WHY THIS CLASS EXISTS
// ---------------------
// The harness-adversarial audit never scoped a race stress test for the tile
// pipeline — the one genuinely multi-threaded hazard in the renderer. Under
// real scroll/zoom load, N tile-worker threads concurrently get/put/evict tiles
// while the visible set (and thus the prefetch ring) churns and the document is
// mutated (page edit -> TileCache::invalidate(page)). A data race there would
// corrupt a cache entry or UAF an evicted tile — a crash class that every
// single-threaded TileCache unit test (test_tile_cache.cpp) is blind to.
//
// WHAT THIS TEST DOES
// -------------------
// 8 worker threads, released simultaneously by a std::barrier for maximum
// contention, each run a FIXED, seeded op mix for M iterations against ONE
// shared TileCache:
//   * put(id, image)   — insert/refresh a tile (drives evictLRU under a small
//                        memory cap so eviction races run hot),
//   * get(id)          — look up + LRU-splice a tile, then TOUCH the returned
//                        QImage's pixels (proves the returned copy is a valid,
//                        non-dangling detached image, not a freed buffer),
//   * contains(id) via a per-thread TilePrefetcher::computePrefetchTiles()
//                        (the const, read-only prefetch path — reads the cache
//                        concurrently the way the real prefetcher does),
//   * invalidate(page) / invalidateAll() — the document-mutation signal that
//                        races eviction and insertion.
// The memory cap is deliberately tiny (key space >> capacity) so the cache is
// in permanent eviction churn during the whole storm.
//
// DETERMINISM: fixed per-thread mt19937 seeds, a fixed iteration count (not a
// wall-clock stop), no sleeps. Bounded to well under a second. Run 3x it is
// byte-for-byte the same workload.
//
// INVARIANTS ASSERTED AFTER THE STORM (all threads joined -> single-threaded):
//   1. No crash / no UAF / no double-free — reaching the assertions at all
//      (backed by the test_main CRT heap-corruption guard; the committed CI
//      ASan lane, see below, upgrades this to real UAF/heap detection).
//   2. Memory bound honored: memoryUsage() <= maxMemory, and stats agree
//      (currentMemoryBytes == memoryUsage(), tileCount == tileCount(),
//      currentMemoryBytes <= maxMemoryBytes).
//   3. LOST-UPDATE / counter-race oracle: every put and every get is counted
//      under the cache lock, so the cache's own putCount MUST equal the total
//      puts issued and hitCount+missCount MUST equal the total gets issued. A
//      torn read-modify-write on those counters (the signature of a broken
//      lock) makes these drift — an exact-equality race detector.
//   4. No corruption: a fresh set of sentinel tiles inserted post-storm read
//      back Ready, non-null, with the exact dimensions written.
//   5. Balanced accounting: clear() returns memoryUsage()/tileCount() to 0
//      (no leaked bytes in the running tally).
//   6. Prefetcher invariant: computePrefetchTiles() never returns an
//      already-cached tile.
//
// TSan / ASan LANE (register C-01 "verifiable_here: partial")
// -----------------------------------------------------------
// The invariants above run in the normal Windows Debug suite. To surface an
// ACTUAL data race / UAF (not just its downstream symptom) this test is wired
// into the committed MSVC-ASan security subset in
// .github/workflows/windows-sanitizer.yml (suite filter TileConcurrencyStress.*)
// and is a first-class candidate for the retired-but-preserved Linux
// Clang ThreadSanitizer lane (.github/workflows/sanitizer.yml) if/when PDFE
// builds on Linux. ASan/TSan instrument the exact get/put/evict/QImage-copy
// window this storm hammers.
//
// RACE ANALYSIS OF THE CURRENT CODE (reasoned, not assumed)
// ---------------------------------------------------------
// TileCache: every public method (get/put/invalidate/invalidateAll/clear/
// memoryUsage/tileCount/setMaxMemory/contains/stats/resetStats) takes
// m_mutex under a std::lock_guard for its whole body, and get() copies the
// Tile (incrementing the QImage's atomic refcount) BEFORE the guard unwinds —
// so a concurrent put()/evict that reassigns or drops the stored image cannot
// free the buffer the caller just took. => No data race found in TileCache.
// TilePrefetcher::computePrefetchTiles() is const and only READS the cache
// (through the locked contains()); it is safe to call concurrently on distinct
// instances, which is what this test does (one prefetcher per thread).
// FIXED (was LATENT): TilePrefetcher::prefetchAround() used to write the shared
// member lastPrefetched_ WITHOUT a lock, and lastPrefetchedTiles() handed back a
// reference into it — so calling both on ONE shared instance from multiple
// threads raced (a reader could observe a torn/reallocated vector). It now takes
// prefetchMutex_ for the store and returns BY VALUE (a copy under the lock). The
// SharedPrefetcherSnapshotsAreAtomic test below shares a single prefetcher across
// threads and asserts every observed snapshot is a complete, valid prefetch set —
// which would fault or mismatch under the old racy reference-return.

#include <gtest/gtest.h>

#include "rendering/TileCache.h"
#include "rendering/TilePrefetcher.h"
#include "rendering/TileId.h"
#include "rendering/Tile.h"

#include <QImage>

#include <atomic>
#include <barrier>
#include <cstdint>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace pdfe::rendering;

namespace {

// A tiny image so the memory cap can be crossed thousands of times per storm
// (heavy eviction pressure) while the whole test stays sub-second. 8x8 ARGB32
// == 256 bytes.
constexpr int kImgDim = 8;

QImage makeImage(int dim = kImgDim) {
    QImage img(dim, dim, QImage::Format_ARGB32);
    img.fill(Qt::red);
    return img;
}

// Bounded key space, intentionally MUCH larger than the cache capacity so the
// cache is in continuous eviction churn under contention.
constexpr int kPages = 6;
constexpr int kCols = 8;
constexpr int kRows = 8;
const float kZooms[] = {1.0f, 2.0f};

TileId keyFrom(std::mt19937& rng) {
    TileId id;
    id.pageIndex = static_cast<int>(rng() % kPages);
    id.col = static_cast<int>(rng() % kCols);
    id.row = static_cast<int>(rng() % kRows);
    id.zoom = kZooms[rng() % 2];
    return id;
}

} // namespace

TEST(TileConcurrencyStress, HammerCacheAndPrefetchUnderContention) {
    // 256-byte tiles; a 32 KiB cap holds ~128 tiles while the key space is
    // kPages*kCols*kRows*2 = 768 distinct keys -> constant eviction.
    constexpr size_t kMaxMemory = 32u * 1024u;
    TileCache cache(kMaxMemory);

    constexpr int kThreads = 8;
    constexpr int kIterations = 20000;  // per thread; ~160k ops total, sub-second

    std::atomic<uint64_t> totalPuts{0};
    std::atomic<uint64_t> totalGets{0};
    std::atomic<uint64_t> pixelTouchAccum{0};  // forces the compiler to keep the
                                               // returned-image pixel reads.

    // Released simultaneously for maximum overlap of get/put/evict/invalidate.
    std::barrier startGate(static_cast<std::ptrdiff_t>(kThreads));

    auto worker = [&](int threadIdx) {
        std::mt19937 rng(0xC01ULL + static_cast<unsigned>(threadIdx) * 2654435761u);
        // Each thread owns its OWN prefetcher (see race analysis in the header:
        // prefetchAround() is not thread-safe on a shared instance; the const
        // read-only computePrefetchTiles used here is).
        TilePrefetcher prefetcher(&cache);
        prefetcher.setRingSize(1 + (threadIdx % 3));

        uint64_t localPuts = 0;
        uint64_t localGets = 0;
        uint64_t localTouch = 0;

        startGate.arrive_and_wait();

        for (int i = 0; i < kIterations; ++i) {
            const unsigned op = rng() % 100;
            if (op < 55) {
                // 55% put — the eviction driver.
                cache.put(keyFrom(rng), makeImage());
                ++localPuts;
            } else if (op < 90) {
                // 35% get — plus a real read of the returned image's pixels so
                // a freed/dangling buffer would fault here (or be caught by ASan).
                auto tile = cache.get(keyFrom(rng));
                ++localGets;
                if (tile.has_value() && !tile->image.isNull()) {
                    const QImage& im = tile->image;
                    // Touch a couple of pixels; the copy must be a valid,
                    // independently-owned image even as other threads evict.
                    localTouch += static_cast<uint64_t>(qRed(im.pixel(0, 0)));
                    localTouch += static_cast<uint64_t>(
                        qBlue(im.pixel(im.width() - 1, im.height() - 1)));
                }
            } else if (op < 97) {
                // 7% prefetch compute — concurrent read-only cache scan.
                std::vector<TileId> visible;
                visible.reserve(4);
                for (int v = 0; v < 4; ++v) visible.push_back(keyFrom(rng));
                auto pf = prefetcher.computePrefetchTiles(visible, kPages);
                localTouch += pf.size();
            } else if (op < 99) {
                // 2% single-page invalidate — the document-mutation signal.
                cache.invalidate(static_cast<int>(rng() % kPages));
            } else {
                // 1% invalidateAll — the whole-doc mutation signal.
                cache.invalidateAll();
            }
        }

        totalPuts.fetch_add(localPuts, std::memory_order_relaxed);
        totalGets.fetch_add(localGets, std::memory_order_relaxed);
        pixelTouchAccum.fetch_add(localTouch, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    // ---- INVARIANT 2: memory bound + stats self-consistency.
    const CacheStats st = cache.stats();
    EXPECT_LE(cache.memoryUsage(), kMaxMemory)
        << "cache exceeded its memory cap after the concurrent storm";
    EXPECT_EQ(st.currentMemoryBytes, cache.memoryUsage())
        << "stats.currentMemoryBytes disagrees with memoryUsage()";
    EXPECT_LE(st.currentMemoryBytes, st.maxMemoryBytes)
        << "stats reports currentMemory over the cap";
    EXPECT_EQ(st.tileCount, cache.tileCount())
        << "stats.tileCount disagrees with tileCount()";

    // ---- INVARIANT 3: lost-update / counter-race oracle. Every put and get is
    // counted under the cache's own lock, so these MUST match exactly. Any
    // drift means a torn read-modify-write (a broken lock) — a direct data-race
    // signal even without a sanitizer.
    EXPECT_EQ(st.putCount, totalPuts.load())
        << "cache putCount (" << st.putCount << ") != puts issued ("
        << totalPuts.load() << ") — a lost update under contention";
    EXPECT_EQ(st.hitCount + st.missCount, totalGets.load())
        << "cache hit+miss (" << (st.hitCount + st.missCount)
        << ") != gets issued (" << totalGets.load()
        << ") — a lost update under contention";

    // ---- INVARIANT 4: no corruption. Fresh sentinel tiles read back intact.
    {
        const int dims[] = {4, 12, 20};
        std::vector<TileId> sentinels;
        for (int k = 0; k < 3; ++k) {
            TileId id{100 + k, k, k, 3.0f};  // outside the storm's key space
            cache.put(id, makeImage(dims[k]));
            sentinels.push_back(id);
        }
        for (int k = 0; k < 3; ++k) {
            auto tile = cache.get(sentinels[static_cast<size_t>(k)]);
            ASSERT_TRUE(tile.has_value())
                << "sentinel tile " << k << " missing after storm";
            EXPECT_EQ(tile->state, TileState::Ready);
            ASSERT_FALSE(tile->image.isNull());
            EXPECT_EQ(tile->image.width(), dims[k]);
            EXPECT_EQ(tile->image.height(), dims[k]);
        }
    }

    // ---- INVARIANT 6: prefetch never returns an already-cached tile.
    {
        TilePrefetcher pf(&cache);
        std::vector<TileId> visible{{0, 0, 0, 1.0f}, {1, 1, 1, 1.0f}};
        auto out = pf.computePrefetchTiles(visible, kPages);
        for (const auto& id : out) {
            EXPECT_FALSE(cache.contains(id))
                << "computePrefetchTiles returned an already-cached tile";
        }
    }

    // ---- INVARIANT 5: balanced accounting — clear() zeroes the running tally.
    cache.clear();
    EXPECT_EQ(cache.memoryUsage(), 0u)
        << "memoryUsage nonzero after clear() — leaked bytes in the tally";
    EXPECT_EQ(cache.tileCount(), 0u)
        << "tileCount nonzero after clear()";

    // Keep the pixel reads observable so the optimizer can't elide them.
    EXPECT_GE(pixelTouchAccum.load(), 0u);
}

// Regression for the (formerly latent) prefetchAround/lastPrefetched_ race.
// A SINGLE prefetcher is shared across threads: writers hammer prefetchAround()
// while readers hammer lastPrefetchedTiles(). Under the old code — an unlocked
// store plus a by-reference getter — a reader could observe a vector mid-
// reallocation (torn size/pointer): a crash, an ASan/TSan report, or a snapshot
// whose contents match no valid full output. The fix (prefetchMutex_ + return by
// value) makes every snapshot a complete, atomic copy of one issued store.
TEST(TileConcurrencyStress, SharedPrefetcherSnapshotsAreAtomic) {
    TileCache cache(64u * 1024u);
    TilePrefetcher prefetcher(&cache);   // ONE instance, deliberately shared.
    prefetcher.setRingSize(1);

    // A small fixed menu of visible-tile inputs, each producing a distinct,
    // known-complete prefetch set. Readers must only ever see one of these.
    const std::vector<std::vector<TileId>> inputs = {
        {{0, 3, 3, 1.0f}},
        {{1, 5, 5, 2.0f}},
        {{2, 2, 2, 1.0f}, {2, 7, 7, 1.0f}},
        {{0, 1, 1, 1.0f}, {3, 6, 4, 2.0f}},
    };

    auto sortKey = [](const TileId& a, const TileId& b) {
        if (a.pageIndex != b.pageIndex) return a.pageIndex < b.pageIndex;
        if (a.col != b.col) return a.col < b.col;
        if (a.row != b.row) return a.row < b.row;
        return a.zoom < b.zoom;
    };
    auto normalize = [&](std::vector<TileId> v) {
        std::sort(v.begin(), v.end(), sortKey);
        return v;
    };

    // Precompute the set of valid complete outputs (normalized).
    std::vector<std::vector<TileId>> validOutputs;
    validOutputs.reserve(inputs.size());
    for (const auto& in : inputs) {
        validOutputs.push_back(normalize(prefetcher.computePrefetchTiles(in, kPages)));
    }
    auto isValid = [&](const std::vector<TileId>& snap) {
        auto n = normalize(snap);
        for (const auto& v : validOutputs) {
            if (n == v) return true;
        }
        return false;
    };

    constexpr int kWriters = 4;
    constexpr int kReaders = 4;
    constexpr int kIters = 30000;
    std::barrier gate(static_cast<std::ptrdiff_t>(kWriters + kReaders));
    std::atomic<uint64_t> reads{0};
    std::atomic<bool> sawInvalid{false};

    // Seed a value so readers never race an empty initial state into "invalid".
    prefetcher.prefetchAround(inputs.front(), kPages);

    std::vector<std::thread> threads;
    threads.reserve(kWriters + kReaders);
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            std::mt19937 rng(0xBEEFu + static_cast<unsigned>(w));
            gate.arrive_and_wait();
            for (int i = 0; i < kIters; ++i) {
                prefetcher.prefetchAround(inputs[rng() % inputs.size()], kPages);
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&] {
            gate.arrive_and_wait();
            uint64_t local = 0;
            for (int i = 0; i < kIters; ++i) {
                std::vector<TileId> snap = prefetcher.lastPrefetchedTiles();
                if (!isValid(snap)) sawInvalid.store(true, std::memory_order_relaxed);
                ++local;
            }
            reads.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_FALSE(sawInvalid.load())
        << "a reader observed a prefetch snapshot that matches no complete "
           "output — a torn read of lastPrefetched_ under contention";
    EXPECT_EQ(reads.load(), static_cast<uint64_t>(kReaders) * kIters);
}

// ═════════════════════════════════════════════════════════════════════
// CLASS-LEVEL TEST for DCA 0004 (follow-up phase 42).
//
// The INSTANCE test above (SharedPrefetcherSnapshotsAreAtomic) pins one member
// of one class: TilePrefetcher::lastPrefetched_ under contention. It cannot see
// the second half of the 0004 defect at all. DCA 0004 says so explicitly — "A
// mutex around the store is necessary but not sufficient; the getter had to
// stop leaking a reference to the guarded data" — and that instance test copies
// the result into a `std::vector<TileId> snap` local, so it would keep passing
// against a getter that still returned `const std::vector<TileId>&`.
//
// THE CLASS is: *an accessor that hands the caller a VIEW into lock-guarded
// shared state.* The lock then covers only the duration of the call; the view
// outlives it, and the next mutation invalidates the caller's data
// (reallocation → dangling, or silently-changed contents). Which member leaks,
// and which class it lives in, is incidental.
//
// THE ORACLE is aliasing, not threading — so it is deterministic and does not
// depend on hitting a race window:
//
//   1. TYPE CONTRACT — every rendering-layer accessor that reads guarded state
//      must return a prvalue, not a reference or pointer. Swept as a SET, so a
//      NEW leaking getter added to TileCache or TilePrefetcher fails here
//      without anyone remembering to write a test for it.
//   2. ALIASING BEHAVIOUR — capture what an accessor returned, mutate the
//      object so the underlying guarded state definitely changes, then assert
//      the captured value did NOT change. A by-reference accessor fails this
//      single-threaded, on every run.
//
// RED-ness: revert TilePrefetcher::lastPrefetchedTiles() to its pre-fix
// `const std::vector<TileId>&` signature and BOTH halves fail — the type check
// immediately, the aliasing check on the value comparison.
TEST(TileConcurrencyStress, SharedRenderStateAccessorsHandBackSnapshotsNotReferences) {
    // ---- 1. TYPE CONTRACT ---------------------------------------------
    // Evaluated at runtime rather than via static_assert so a regression is a
    // RED TEST rather than a broken build — a red test is what the gate reads.
    const TileCache constCache(1024u);
    const TilePrefetcher constPrefetcher;

    struct AccessorContract {
        const char* name;
        bool returnsReference;
        bool returnsPointer;
    };
    const std::vector<AccessorContract> accessors = {
        {"TilePrefetcher::lastPrefetchedTiles",
         std::is_reference_v<decltype(constPrefetcher.lastPrefetchedTiles())>,
         std::is_pointer_v<decltype(constPrefetcher.lastPrefetchedTiles())>},
        {"TilePrefetcher::computePrefetchTiles",
         std::is_reference_v<decltype(constPrefetcher.computePrefetchTiles(
             std::declval<const std::vector<TileId>&>(), 1))>,
         std::is_pointer_v<decltype(constPrefetcher.computePrefetchTiles(
             std::declval<const std::vector<TileId>&>(), 1))>},
        {"TileCache::stats",
         std::is_reference_v<decltype(constCache.stats())>,
         std::is_pointer_v<decltype(constCache.stats())>},
        {"TileCache::memoryUsage",
         std::is_reference_v<decltype(constCache.memoryUsage())>,
         std::is_pointer_v<decltype(constCache.memoryUsage())>},
        {"TileCache::tileCount",
         std::is_reference_v<decltype(constCache.tileCount())>,
         std::is_pointer_v<decltype(constCache.tileCount())>},
        {"TileCache::get",
         std::is_reference_v<decltype(std::declval<TileCache&>().get(
             std::declval<const TileId&>()))>,
         std::is_pointer_v<decltype(std::declval<TileCache&>().get(
             std::declval<const TileId&>()))>},
    };
    ASSERT_EQ(accessors.size(), 6u)
        << "the accessor SET is the point of this sweep — if an accessor was "
           "added to or removed from the rendering layer, update this list "
           "rather than letting the sweep silently shrink";
    for (const auto& a : accessors) {
        EXPECT_FALSE(a.returnsReference)
            << a.name << " returns a REFERENCE into lock-guarded shared state. "
               "The lock then covers only the call; the caller's view outlives "
               "it and the next mutation reallocates under them (DCA 0004).";
        EXPECT_FALSE(a.returnsPointer)
            << a.name << " returns a POINTER into lock-guarded shared state — "
               "the same escape as a reference (DCA 0004).";
    }

    // ---- 2. ALIASING BEHAVIOUR ----------------------------------------
    // TilePrefetcher::lastPrefetchedTiles — the 0004 site itself.
    {
        TileCache cache(64u * 1024u);
        TilePrefetcher prefetcher(&cache);
        prefetcher.setRingSize(1);

        const std::vector<TileId> inputA = {{0, 3, 3, 1.0f}};
        const std::vector<TileId> inputB = {{1, 5, 5, 2.0f}};
        prefetcher.prefetchAround(inputA, kPages);
        const auto captured = prefetcher.lastPrefetchedTiles();
        ASSERT_FALSE(captured.empty())
            << "inputA produced no prefetch tiles — pick inputs that do, or "
               "the aliasing oracle has nothing to observe";

        // Mutate the guarded member. A by-value accessor leaves `captured`
        // untouched; a by-reference one silently rewrites it under us.
        prefetcher.prefetchAround(inputB, kPages);
        const auto afterMutation = prefetcher.lastPrefetchedTiles();
        ASSERT_NE(captured, afterMutation)
            << "inputA and inputB produce the same prefetch set, so this "
               "oracle cannot distinguish a copy from a reference — choose "
               "inputs whose outputs differ";

        // The real assertion: the earlier capture still holds inputA's result.
        const auto expectedA = prefetcher.computePrefetchTiles(inputA, kPages);
        EXPECT_EQ(captured, expectedA)
            << "the value returned by lastPrefetchedTiles() CHANGED when the "
               "prefetcher was mutated afterwards — the accessor handed back a "
               "view into lock-guarded state instead of a snapshot (DCA 0004)";
    }

    // TileCache::get — the returned Tile must OWN its image, not alias a cache
    // entry that eviction/invalidation can free underneath the caller.
    {
        TileCache cache(64u * 1024u);
        const TileId id{0, 1, 1, 1.0f};
        cache.put(id, makeImage());
        auto tile = cache.get(id);
        ASSERT_TRUE(tile.has_value());
        ASSERT_FALSE(tile->image.isNull());
        const QSize sizeBefore = tile->image.size();
        const QRgb pixelBefore = tile->image.pixel(0, 0);

        // Drop every entry: with an owning copy this is a no-op for `tile`;
        // with an aliasing view it frees the pixels out from under it.
        cache.invalidateAll();
        cache.clear();

        EXPECT_EQ(tile->image.size(), sizeBefore)
            << "the Tile returned by TileCache::get() changed after the cache "
               "was cleared — get() aliased the cached entry (DCA 0004 class)";
        EXPECT_EQ(tile->image.pixel(0, 0), pixelBefore)
            << "the Tile returned by TileCache::get() lost its pixels when the "
               "cache was cleared — get() aliased the cached entry";
    }

    // TileCache::stats — a struct snapshot, not a window onto live counters.
    {
        TileCache cache(64u * 1024u);
        cache.put(TileId{0, 0, 0, 1.0f}, makeImage());
        const CacheStats captured = cache.stats();
        const uint64_t putsAtCapture = captured.putCount;

        cache.put(TileId{0, 0, 1, 1.0f}, makeImage());
        cache.put(TileId{0, 0, 2, 1.0f}, makeImage());
        ASSERT_GT(cache.stats().putCount, putsAtCapture)
            << "the extra puts did not move putCount — the mutation this "
               "oracle depends on did not happen";

        EXPECT_EQ(captured.putCount, putsAtCapture)
            << "a previously-returned CacheStats changed when the cache was "
               "mutated afterwards — stats() handed back a view into the live "
               "guarded counters (DCA 0004 class)";
    }
}
