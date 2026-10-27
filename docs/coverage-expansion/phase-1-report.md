# Coverage-Expansion Phase 1 — C-01 concurrency + C-02 endurance/leak

Two real, gated test classes the harness-adversarial audit never scoped were
added. Both use real oracles, run deterministically in bounded time in the
Windows headless suite, and were proven able to FAIL. No product source was
changed — no genuine race or leak was found in the exercised paths.

Files:
- `tests/test_tile_concurrency_stress.cpp` (C-01)
- `tests/test_endurance_leak.cpp` (C-02)
- registered in `tests/CMakeLists.txt`
- C-01 wired into the ASan CI lane `.github/workflows/windows-sanitizer.yml`

---

## C-01 — concurrency / thread-safety (TileCache + TilePrefetcher)

### Design
`TileConcurrencyStress.HammerCacheAndPrefetchUnderContention`: 8 worker threads,
released simultaneously by a `std::barrier` for maximum overlap, each run a
fixed, seeded (`std::mt19937`, deterministic per-thread seed) op mix for 20,000
iterations against ONE shared `TileCache`:
- 55% `put` (insert/refresh — the eviction driver),
- 35% `get` + a real read of the returned `QImage`'s pixels (proves the returned
  copy is a valid, independently-owned image, not a freed/dangling buffer),
- 7% `TilePrefetcher::computePrefetchTiles` (the const, read-only concurrent
  cache scan — one prefetcher **per thread**, never shared),
- 2% `invalidate(page)` and 1% `invalidateAll` (the document-mutation signal
  that races insertion/eviction).

The memory cap is deliberately tiny (32 KiB) versus a 768-key space, so the
cache is in continuous eviction churn throughout the storm. ~160k ops total;
runs in ~5.4 s. Deterministic: fixed seeds, fixed iteration count (not a
wall-clock stop), no sleeps.

### Invariants asserted (post-join, single-threaded)
1. **No crash / UAF / double-free** — reaching the assertions at all (backed by
   the test_main CRT heap guard; the ASan lane upgrades this to real detection).
2. **Memory bound** — `memoryUsage() <= maxMemory`; stats self-consistent
   (`currentMemoryBytes == memoryUsage()`, `tileCount == tileCount()`,
   `currentMemoryBytes <= maxMemoryBytes`).
3. **Lost-update / counter-race oracle** — every put and get is counted under the
   cache lock, so `putCount == puts issued` and `hitCount+missCount == gets
   issued` must hold EXACTLY. A torn read-modify-write (broken lock) makes these
   drift — a data-race detector that needs no sanitizer.
4. **No corruption** — fresh sentinel tiles read back `Ready`, non-null, exact
   dimensions.
5. **Balanced accounting** — `clear()` returns `memoryUsage()`/`tileCount()` to 0.
6. **Prefetcher invariant** — `computePrefetchTiles` never returns a cached tile.

### TSan / ASan lane
C-01 is wired into the committed MSVC-ASan security subset
(`.github/workflows/windows-sanitizer.yml`, filter `TileConcurrencyStress.*`),
so ASan instruments the exact get/put/evict/QImage-copy window under the
concurrent storm on every dispatched run. It is also a first-class candidate for
the retired-but-preserved Linux Clang **ThreadSanitizer** lane
(`.github/workflows/sanitizer.yml`) if/when PDFE builds on Linux (that file is
retired only because PDFE is currently Windows-only, per its own header).

### Was a race found? NO (with one documented latent hazard)
- **TileCache**: every public method holds `m_mutex` for its whole body, and
  `get()` copies the `Tile` (atomic `QImage` refcount increment) BEFORE the lock
  unwinds, so a concurrent `put`/evict cannot free the buffer the caller just
  took. Correctly serialized — **no data race**.
- **TilePrefetcher::computePrefetchTiles** is const and only reads the cache
  through the locked `contains()`; safe to call concurrently on distinct
  instances (which is what the test does).
- **LATENT, reported (NOT a shipped bug, NOT fixed):**
  `TilePrefetcher::prefetchAround()` writes the shared mutable member
  `lastPrefetched_` with no lock, so calling it on ONE shared instance from
  multiple threads would race. Production owns a single prefetcher on the UI
  thread, so this cannot occur in the shipped app — the test therefore never
  shares a prefetcher across threads, and the hazard is documented here rather
  than silently worked around. If a future design ever shares a prefetcher across
  threads, `prefetchAround` must be guarded.

---

## C-02 — endurance / soak as a GATED leak oracle

### Design
`EnduranceLeakTest.SteadyStateResourceDeltaBoundedOverLongSession`: a fixed op
sequence per iteration, all through real production paths:

```
open(test_3page.pdf)
  -> annotate (CreateAnnotationCommand + setContents)
  -> redact   (CreateRedactionCommand)
  -> rotate   (RotatePageCommand)          [the "edit"]
  -> undo x3 -> redo x3                     [do/undo churn]
  -> saveAs(temp)                           [full-rewrite path]
  -> reopen(temp) + verify pageCount        [save->reopen leg + save-with-verify]
  -> destroy doc / undo stack / page wrappers
```

Every iteration opens the SAME pristine staged fixture (edits are never fed
back), so a healthy iteration is steady-state. Output temp path alternates
between two files so a just-saved file is never reopened while locked. Page
wrappers are held alive for the whole iteration (commands reference them by raw
pointer) and destroyed only at scope exit — deliberately avoiding the op99
insert-blank/`commandPages.clear()` dangling-Page path, since this test measures
leaks, not that crash.

Warm-up 12 iterations, measured 100 iterations; ~0.75 s total.

### Oracle — warm-up + bounded-growth (same methodology as R3-18
`test_open_close_leak_cycle.cpp` / `test_ui_journey_soak.py`)
Snapshot resources AFTER warm-up (so one-time MuPDF/Qt cache fills are not
misread as a leak), run K measured iterations, then assert each resource grew by
less than a bounded budget:
- **Process commit** (`PROCESS_MEMORY_COUNTERS_EX.PrivateUsage`) — primary; more
  stable than raw working set. Budget 40 MiB. (Working set / RSS is also sampled
  and logged.)
- **Kernel handles** (`GetProcessHandleCount`), **GDI objects** and **USER
  objects** (`GetGuiResources`) — budget K/3 == 33 each.

Samples are taken via a steady-sample helper that pumps deferred deletes and
keeps the minimum over several reads, shedding transient spikes. Windows-only
(the counters are Win32); trivially-skips elsewhere, matching R3-18.

### Leak-injection PROOF (the oracle is not vacuous)
A deliberate per-iteration leak — a 2 MiB heap block (faulted in) plus one
kernel handle + one GDI + one USER object, none freed — is compiled in but INERT
by default, armed only by `PDFE_ENDURANCE_INJECT_LEAK=1`.

**GREEN (committed default, 3/3 runs):**
```
commit growth +282 KiB .. +418 KiB   (budget 40 MiB)   handles/gdi/user growth +0/+0/+0
```
**RED (injection armed):**
```
commit  base=65,159,168  final=276,205,568  growth +211,046,400 B  (~2 MiB/iter, budget 40 MiB)  -> FAIL
handles +100  gdi +100  user +100          (budget 33 each)                                       -> FAIL
[  FAILED  ] EnduranceLeakTest.SteadyStateResourceDeltaBoundedOverLongSession
```
Every one of the four product assertions fails under injection with exactly the
expected linear signature (~2 MiB/iter commit, ~1/iter each handle class), and
all pass with injection off. The env-gate is the "remove the injection"
mechanism; the committed default is GREEN. This is the established PDFE
non-vacuity idiom (same as R3-18) — no assertion was weakened.

### Was a product leak found? NO
Over 100 real edit/save/reopen iterations, commit growth was ~280–420 KiB
(< 0.5 MiB total, budget 40 MiB) and handle/GDI/USER growth was exactly 0. The
exercised open → annotate/redact/rotate → undo/redo → saveAs → reopen session is
leak-free.

---

## Verification summary
- `taskkill` + clean synchronous build succeeds (no concurrent cmake).
- Both suites GREEN; JUnit XML parsed; deterministic across 3 consecutive runs.
- Leak oracle proven to catch a real per-op leak (RED under injection).
- `git status --porcelain -- src/`: **CLEAN** — no product source changed (no real
  race/leak discovered; the one latent prefetcher hazard is documented, not a
  shipped bug).
- New tests registered in `tests/CMakeLists.txt`; C-01 wired into the ASan CI
  lane. No new fixtures (both reuse floor fixtures already pinned:
  `test_3page.pdf`); no fixture-floor or README change needed.
- No test simplified, skipped, or weakened.
