// -*- coding: utf-8 -*-
//
// GitHub #154 — MuPdfContext's per-thread clone cache was keyed on the raw
// parent fz_context* and the entry was never erased when the owning instance
// was destroyed.
//
// A raw pointer is unique only among LIVE objects. Destroy a MuPdfContext and
// construct a new one that the allocator happens to place at the SAME address,
// and the dead instance's cached clone aliases the new instance: execute() /
// executeReturn() run every operation against a clone of a context that has
// already been fz_drop_context()'d, sharing the OLD store rather than the new
// one. Triage also found releaseThreadLocalContext() has zero call sites in
// src/, so on a long-lived worker thread nothing but thread exit ever prunes
// the map — the window in which a stale entry can alias is the whole process
// lifetime, and it widens as the codebase grows background workers.
//
// The fix keys the cache on a monotonic, never-reused instance id and erases
// the entry in ~MuPdfContext.
//
// ORACLE DESIGN NOTE — the interesting part of this test is OBSERVING the
// address reuse. If the allocator hands back a different address the property
// under test is never exercised, and an EXPECT that only runs in that case
// would pass VACUOUSLY. So the reuse is hunted in a loop and its absence is a
// hard FAILURE of the test, never a silent green.

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/MuPdfContext.h"

using namespace pdfe::core;

namespace {

// How many construct/destroy/reconstruct rounds to spend hunting for an
// fz_context address the allocator reuses.
//
// The hunt deliberately remembers EVERY address a destroyed instance held, not
// just the previous round's. Matching only the immediately preceding address
// made the oracle sensitive to the very behaviour under test: when the clone is
// leaked (the unfixed code) the heap free-list is shaped differently and the
// next fz_new_context does not land on the block just freed, so the hunt would
// come up empty and the test would redden on "never observed reuse" instead of
// on the aliasing itself. Accepting a hit on any previously-dead address keeps
// the aliasing assertion the thing that fires.
constexpr int kReuseHuntRounds = 256;

struct DeadInstance {
    fz_context* clone = nullptr;   // the clone it cached on this thread
    std::uint64_t instanceId = 0;
};

}  // namespace

// A stale cache entry left by a destroyed context must not be able to alias a
// NEW context that lands on the same address.
TEST(MuPdfContextInstanceKey, StaleCloneCannotAliasReusedAddress)
{
    // Start from a known-clean per-thread cache so the leak accounting below is
    // this test's own.
    MuPdfContext::releaseThreadLocalContext();
    const size_t baseline = MuPdfContext::liveThreadCloneCount();

    std::unordered_map<fz_context*, DeadInstance> dead;  // ctx address -> what died there
    fz_context* reusedAddress = nullptr;   // the fz_context* both instances got
    fz_context* staleClone = nullptr;      // the dead instance's cached clone
    std::uint64_t deadInstanceId = 0;
    std::unique_ptr<MuPdfContext> live;    // the NEW context at reusedAddress
    int roundsUsed = 0;

    for (int round = 0; round < kReuseHuntRounds; ++round) {
        roundsUsed = round + 1;

        auto candidate = std::make_unique<MuPdfContext>();
        ASSERT_NE(candidate->get(), nullptr);
        ASSERT_NE(candidate->instanceId(), 0u);

        const auto hit = dead.find(candidate->get());
        if (hit != dead.end()) {
            // This new context landed on the address of one we already
            // destroyed — and that dead instance had a clone cached on this
            // thread. That is exactly the aliasing setup.
            reusedAddress = candidate->get();
            staleClone = hit->second.clone;
            deadInstanceId = hit->second.instanceId;
            live = std::move(candidate);
            break;
        }

        // Not (yet) a reused address. Turn this instance into a dead one that
        // HAS cached a clone, and remember the address it occupied.
        fz_context* clone = candidate->threadLocalClone();
        ASSERT_NE(clone, nullptr);
        ASSERT_NE(clone, candidate->get());
        dead[candidate->get()] = DeadInstance{clone, candidate->instanceId()};

        candidate.reset();  // pre-fix: leaves entry[that address] behind
    }

    // NOT a skip: without observed reuse the assertions below are vacuous, and
    // a vacuous green here would read as "the aliasing defect is fixed".
    ASSERT_NE(live, nullptr)
        << "Never observed fz_context address reuse across " << kReuseHuntRounds
        << " construct/destroy/reconstruct rounds, so the aliasing scenario was "
           "never actually set up. This test cannot pass vacuously — treat this "
           "as a harness failure (allocator behaviour changed), not as evidence "
           "that GitHub #154 is fixed.";

    // Precondition of the whole oracle: the two instances really do share an
    // address, which is exactly what made the pointer key unsafe.
    ASSERT_EQ(live->get(), reusedAddress);

    // Print the setup so a green run carries the evidence that the scenario was
    // actually constructed, rather than leaving that to be assumed.
    std::cout << "[ #154 ] address reuse observed on round " << roundsUsed
              << ": fz_context " << reusedAddress << " was instance "
              << deadInstanceId << " (clone " << staleClone
              << "), is now instance " << live->instanceId() << std::endl;

    // ... and they are nonetheless distinguishable, because the key is an id.
    EXPECT_NE(live->instanceId(), deadInstanceId)
        << "instance ids must never be reused; a reused id reintroduces the "
           "aliasing the pointer key had";

    // THE PROPERTY: the new instance's operations must run on ITS context, not
    // on the dead instance's cached clone. execute() routes through
    // currentThreadClone(); capture what it actually handed the operation.
    // (Pointer assignment only — no C++ RAII / no throw inside the fz_try.)
    fz_context* usedByExecute = nullptr;
    live->execute([&usedByExecute](fz_context* c) { usedByExecute = c; });

    EXPECT_EQ(usedByExecute, live->get())
        << "execute() on the NEW context ran against a foreign fz_context. "
           "Pre-fix this is the destroyed instance's clone (" << staleClone
        << "), found because the cache was keyed on the recycled address "
        << reusedAddress << " (reuse observed on round " << roundsUsed << ").";

    // Secondary, only when the freed clone's memory was not itself recycled
    // into the new parent context (in which case the addresses are equal for a
    // reason that has nothing to do with the cache).
    if (staleClone != live->get()) {
        EXPECT_NE(usedByExecute, staleClone)
            << "the destroyed instance's clone is still being handed out";
    }

    // The destructor must also have ERASED each dead instance's entry on this
    // thread — not merely made it unfindable — so a long-lived worker does not
    // accumulate one pinned clone per document it ever serviced. Every instance
    // the hunt created above has been destroyed except `live`, which never
    // cloned, so the count must be back at the baseline.
    EXPECT_EQ(MuPdfContext::liveThreadCloneCount(), baseline)
        << "~MuPdfContext did not drop the clone it had cached on this thread; "
           "with releaseThreadLocalContext() called from nowhere in src/, these "
           "accumulate until the thread exits";

    live.reset();
    MuPdfContext::releaseThreadLocalContext();
    EXPECT_EQ(MuPdfContext::liveThreadCloneCount(), baseline);
}

// The id supply itself: monotonic and never reused, including across
// destruction (which is the only way a pointer could ever repeat).
TEST(MuPdfContextInstanceKey, InstanceIdsAreUniqueAcrossDestruction)
{
    std::vector<std::uint64_t> ids;
    std::uint64_t previous = 0;

    for (int i = 0; i < 32; ++i) {
        MuPdfContext ctx;
        const std::uint64_t id = ctx.instanceId();
        EXPECT_NE(id, 0u) << "0 is reserved for the moved-from state";
        EXPECT_GT(id, previous) << "instance ids must increase monotonically";
        previous = id;
        ids.push_back(id);
    }

    // Belt and braces: no duplicates at all.
    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t j = i + 1; j < ids.size(); ++j) {
            EXPECT_NE(ids[i], ids[j]) << "duplicate instance id at " << i << "/" << j;
        }
    }
}

// A move must carry the id, so a clone cached before the move is still found
// after it (and the moved-from husk claims nothing).
TEST(MuPdfContextInstanceKey, MoveCarriesInstanceIdAndItsCachedClone)
{
    MuPdfContext::releaseThreadLocalContext();
    const size_t baseline = MuPdfContext::liveThreadCloneCount();

    std::uint64_t id = 0;
    fz_context* clone = nullptr;
    {
        MuPdfContext source;
        id = source.instanceId();
        clone = source.threadLocalClone();
        ASSERT_NE(clone, nullptr);
        EXPECT_EQ(MuPdfContext::liveThreadCloneCount(), baseline + 1);

        MuPdfContext moved(std::move(source));
        EXPECT_EQ(moved.instanceId(), id);
        EXPECT_EQ(source.instanceId(), 0u);  // NOLINT(bugprone-use-after-move)

        // The cache entry survives the move and still belongs to the owner.
        EXPECT_EQ(moved.threadLocalClone(), clone);
        EXPECT_EQ(MuPdfContext::liveThreadCloneCount(), baseline + 1)
            << "the move must not have created a second clone";
    }

    // Destroying the (moved-to) owner released it; the husk released nothing.
    EXPECT_EQ(MuPdfContext::liveThreadCloneCount(), baseline);
    MuPdfContext::releaseThreadLocalContext();
}
