# Power-event (suspend / resume) handling — scope statement

**GitHub #213 · meta-audit register row WIN-71 · written 2026-09-18**

> **Status: PARTIAL, deliberately.** This document is the scope statement
> required by phase-8 of pipeline `p87-open-issues`. It states what full
> suspend/resume support would require and records what was actually built.
> **It does not claim #213 is complete, and #213 is not closed by it.** The
> handler that exists covers two transitions and does two things; everything
> else on this page is unbuilt and enumerated so that nobody has to rediscover
> it.

---

## 1. What existed before

Nothing. Measured 2026-09-16 in #213 and re-measured 2026-09-18:

```
$ grep -rn "PBT_APMSUSPEND\|PBT_APMRESUME" src/
(no output)
$ grep -rniE "WM_POWERBROADCAST|PowerSetting|RegisterSuspendResume" src/
(no output)
```

PDFE was never told that a suspend happened, so it could not flush anything
before the freeze or reconsider anything after it. WIN-71 also had no tracker
until #213 was filed, while both its Group-G siblings (#75, #97) kept theirs.

## 2. What was built in this phase

| Piece | File | What it does |
|---|---|---|
| Classifier | `src/app/PowerEvents.h/.cpp` — `classifyPowerBroadcast()` | Pure function `(message, wParam) -> PowerTransition`. Recognises `PBT_APMSUSPEND` and `PBT_APMRESUMEAUTOMATIC`; everything else maps to `None`. |
| Coordinator | `src/app/PowerEvents.h/.cpp` — `PowerEventCoordinator` | Headless. Counts transitions, timestamps them, computes the apparent freeze duration, runs one suspend hook and one resume hook, swallows hook exceptions. |
| Adapter | `src/app/PowerEvents.h/.cpp` — `PowerEventFilter` | `QAbstractNativeEventFilter`. Decodes the Win32 `MSG`, dispatches, and **always returns `false`** so the message is never consumed. |
| Install | `src/main.cpp` | One filter on the `QApplication`, in the real app only. |
| Reaction | `src/ui/MainWindow.cpp` constructor | Suspend hook → `AutosaveManager::autosaveNow()`, i.e. publish a crash-recovery snapshot before the machine freezes. Cleared in `~MainWindow`. |
| Pin | `tests/test_power_events.cpp` | Asserts the mapping, the dispatch behaviour, the MainWindow reaction — **and the omissions below, as omissions.** |

### The one behavioural guarantee

**If the machine suspends with unsaved changes open, the work becomes
recoverable before the freeze.** That is the same first step
`MainWindow::handleSessionEnd()` takes for a logoff/shutdown (WIN-68), and for
the same reason: a suspend that never resumes — dead battery, discarded VM,
dropped hibernate image — loses unsaved work exactly as a `shutdown /f` does.

### What it explicitly does *not* do

On resume PDFE records a timestamp and a duration, logs them, and **takes no
other action**. Specifically it does not re-validate file handles, invalidate
render tiles or the page cache, re-check the MuPDF context, re-phase timers, or
re-acquire the single-instance mutex. `lastFreezeDurationMs()` is a diagnostic
and nothing branches on it.

A handler that logs and does nothing is honest only if the test says so, so
`test_power_events.cpp` asserts these as omissions rather than leaving a reader
to assume coverage.

## 3. What full support would require

Five areas, in rough order of how likely each is to bite a real user. None is
built.

### 3.1 In-flight save across a suspend

The highest-severity case and the one this phase does **not** address at all.
`PBT_APMSUSPEND` can arrive while a save is midway through the staged
write/rename in `AtomicFileReplace`. The atomic-replace design means the
*original* file survives, which is the important half; what is unknown is
whether the temporary is cleaned up, whether QPDF's handles survive the freeze,
and whether a resumed save completes or reports a misleading error.

Needed: a save-in-progress interlock the power handler can observe, so suspend
either waits for the critical section (within the ~2 s Windows allows) or marks
the save as interrupted for the resume side to reconcile. Requires an audit of
`AtomicFileReplace` + `SaveWithRebind` for the point of no return.

**Why not now:** it is a concurrency contract across the save pipeline, not a
notification handler, and getting it wrong risks the save path — which is the
one thing in a PDF editor that must never regress.

### 3.2 File-handle and volume validity after resume

`Document` holds MuPDF/QPDF handles onto the source file. Across a long freeze
the file may have been deleted, replaced, or have lived on a network share or
removable volume that is gone. Windows does not invalidate the handle; reads
simply start failing, and PDFE's error paths have never been exercised from
that state.

Needed: a resume-time revalidation (`GetFileInformationByHandle` / re-`stat`,
compare volume serial + file index + mtime) and a defined UX for "the file
under you changed", which PDFE does not currently have for *any* cause.

**Why not now:** the missing piece is the product decision — reload silently,
prompt, or go read-only — not the detection. Building detection with no defined
response would produce a warning nobody can act on.

### 3.3 Render-cache and surface invalidation

WIN-71's original wording asks for "no stale render". Qt normally recreates
backing stores itself, and whether a stale tile is reachable at all is
**unmeasured**. #213 suggests falsifying the "needs a VM" premise first by
forcing a tile-cache/`QBackingStore` invalidation directly, as #97 did — that
investigation has not been done.

Needed: that experiment first. Only if a stale tile is genuinely reachable does
a resume-time cache drop become justified; adding one speculatively would throw
away a warm cache on every lid-open for no measured benefit.

**Why not now:** it would be a fix with no demonstrated defect.

### 3.4 Timer phase and wall-clock jumps

The autosave `QTimer` (30 s), the TSA/OCSP network timeouts and any
`QElapsedTimer`-based measurement all see a large discontinuity. Qt's timers
are monotonic, so the usual symptom is a *late* tick rather than a storm, but
this has not been measured across a real freeze.

Needed: a measurement across a real suspend, then — if warranted — restarting
the autosave timer on resume so the next snapshot is a full interval away
rather than immediate.

**Why not now:** the correct action depends on the measurement, which needs the
VM in §5.

### 3.5 The broader `PBT_*` surface

Not handled: `PBT_APMRESUMESUSPEND` (user-initiated resume only — deliberately
not treated as the resume signal, since it is absent on wake-for-maintenance
and lid wakes), `PBT_APMPOWERSTATUSCHANGE` (AC↔battery, which is where an
autosave-interval or background-OCR policy would belong),
`PBT_POWERSETTINGCHANGE` with `GUID_MONITOR_POWER_ON` / `GUID_LIDSWITCH_STATE_CHANGE`
(requires `RegisterPowerSettingNotification`), and Modern Standby, where the
classic suspend broadcast may not be delivered at all.

**Why not now:** none has an identified PDFE behaviour behind it. Registering
for notifications PDFE does not act on is decoration.

## 4. Why a plain GoogleTest, and not a VM

`phase-62-dispositions.md` §2 deferred WIN-71 "pending VM/second-console
access", correctly — suspending this host kills the RDP session that would
observe the result.

But #97 (WIN-73) was deferred by the same phase for the same reason and was
then closed by a plain GoogleTest, because the row's stated **environment** was
not its **requirement**. The same applies to the part of WIN-71 built here. The
requirement of a power *handler* is "the app is told a suspend happened and
reacts correctly", and every step of that is injectable in-process:

* `classifyPowerBroadcast()` is pure, so the `PBT_*` → transition mapping is
  driven with the real SDK constants and no message loop. `static_assert`s in
  `PowerEvents.cpp` tie the locally-defined constants to the SDK's values, so
  the test cannot pass against a typo'd copy.
* `PowerEventCoordinator` is headless, so the reaction is driven directly.
* `MainWindow`'s hook is observable through the existing
  `hasRecoverySnapshotForTesting()` accessor.

What a VM is still genuinely needed for is §3.1–§3.4: all four are about what a
*real* freeze does to handles, caches, timers and an in-flight write, and none
of those can be simulated by dispatching a notification.

## 5. The remaining manual procedure

Unchanged from `docs/audit/meta-audit-2026-07/followup/phase-62-dispositions.md`
§2, which writes it out step by step including the pre-suspend render capture to
compare against. It must run on a **console** session, not RDP. Its findings
feed §3.1–§3.4.

## 6. Disposition of #213

**Stays open.** What closed in this phase: "nobody has considered it" is no
longer true, and the suspend-side data-loss window is closed. What remains open
is §3.1–§3.4 plus the manual run in §5.
