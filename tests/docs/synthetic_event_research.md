# Synthetic Qt Event Spontaneity Research (P-GAP-98)

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Pipeline:** 3e9b1d7c phase-4
**Date:** 2026-05-22
**Outcome:** **DEFERRED** — no approach feasible without major architecture changes.

## Problem

The B-pipes added in f7c2a4e8 phase-3 (testCanvasDragPipe, testTripleClickPipe) synthesize `QMouseEvent` instances and dispatch them via `QApplication::sendEvent`. The events arrive at `PdfCanvas::mousePressEvent` and are dispatched to the active tool's `onMousePress/Move/Release`. But the receiving tools (`TextSelector`, `StickyNoteTool`, `SelectTool`) don't establish their expected state:

- `TextSelector` doesn't build a text-selection region from a synthetic drag
- `StickyNoteTool` doesn't create an annotation from a synthetic click+release pair
- `SelectTool` rubber-band logic doesn't multi-select annotations from a synthetic drag
- Qt6 triple-click detection (timing-based) doesn't recognize three back-to-back synthetic clicks

The 7 P-GAP-92 canvas tests (handle drag corner/edge/rotate, text-selection drag/triple-click, rubber-band, image-properties post-click) fail at the tool-side reaction stage, not at the event-arrival stage.

## Hypothesis

`QMouseEvent::spontaneous()` returns `false` for events created in code and sent via `QApplication::sendEvent`. Several Qt classes (including some text-selection / drag-establish paths) filter on `spontaneous()` to distinguish real OS-driven input from internal redispatch.

## Four candidate approaches evaluated

### (a) `QTest::mouseClick` / `QTest::mousePress`

**What it does:** the Qt Test module provides utilities that set up event spontaneity properly internally via `QWindowSystemInterface::handleMouseEvent`.

**Why it doesn't work for our architecture:** QtTest is linked into `pdfe_tests.exe` (the GoogleTest binary) but NOT into `pdfe.exe` (the production binary that Python UI tests connect to). Linking QtTest into `pdfe.exe` would either:
- Bloat the production binary with test-only code, OR
- Require a `#ifdef PDFE_TESTING` build flag and a separate test build

Both are significant architectural changes outside this pipeline's scope.

### (b) `Qt::MouseEventSynthesizedByQt` source flag

**What it does:** the longer `QMouseEvent` constructor accepts a `Qt::MouseEventSource` flag (`MouseEventNotSynthesized`, `MouseEventSynthesizedBySystem`, `MouseEventSynthesizedByQt`, `MouseEventSynthesizedByApplication`).

**Why it doesn't help:** the source flag is informational — it tells receivers WHO synthesized the event. It does NOT change `event->spontaneous()`. Qt's tools that filter on spontaneity will still drop these events. Verified by reading the Qt6 source: `QInputEvent::source()` and `QInputEvent::spontaneous()` are independent properties.

### (c) `QWindowSystemInterface::handleMouseEvent`

**What it does:** the platform-integration layer accepts mouse events that get dispatched as if from the OS, with proper spontaneity.

**Why it doesn't work for production code:** requires including private Qt headers (`<QtGui/private/qwindowsysteminterface_p.h>`) which are not part of the public Qt API. Cross-Qt-version compatibility is not guaranteed; the API is documented as "private and subject to change." Building production code against private headers is a hard rule violation.

### (d) Win32 `SendInput`

**What it does:** injects mouse events at the OS level via the Win32 `SendInput` API. Events appear as fully real OS-originated input.

**Why it's the only viable option in theory but hard in practice:**
- Coords must be in screen pixels, not client area — requires accurate window-to-screen mapping
- Modifier key state (Ctrl/Shift) requires separate `KEYEVENTF_KEYDOWN`/`KEYEVENTF_KEYUP` sequencing around the click
- Cross-process interaction is asynchronous — the test must poll for the side-effect rather than expect synchronous completion
- Reliability still requires foreground-window focus which can be stolen by other processes
- Estimated effort: 2-4 hours of careful Win32 input-injection coding + per-test integration

## Decision: **DEFERRED**

None of (a) (b) (c) are feasible without architectural changes outside this pipeline's scope. Option (d) is technically feasible but the effort is comparable to a phase of its own.

The 7 P-GAP-92 D2 tests continue as xfail with updated reasons citing this research note. P-GAP-98 marker stays DEFERRED with this document as the audit trail.

## P-GAP-74 (F1 atlas timing) side-effect assessment

The atlas-dialog timing issue in test_precondition_panels_phase4.py is NOT a synthetic-event spontaneity issue — it's an async UI-population timing problem already mitigated by the 2.0→4.0s wait increase in f7c2a4e8 phase-6. Spontaneity research does not unblock P-GAP-74. Confirmed DEFERRED.

## Recommendation for future pipeline

If the canvas test cluster (P-GAP-92) becomes a higher priority, allocate a focused pipeline phase to implement Option (d) — Win32 SendInput injection — with proper coord mapping, modifier sequencing, and async polling. Scope estimate: 1 dedicated phase, 2-4 hours of dev + 1 hour per converted test for tuning.

## References

- `f7c2a4e8` phase-3 commit `ba080e8` documents the original P-GAP-98 finding
- `tests/docs/post_b5e3a9f2_followups.md` Theme D2 lists the 7 affected tests
- `tests/KNOWN_WEAK_TESTS.txt` P-GAP-92 / P-GAP-98 entries
