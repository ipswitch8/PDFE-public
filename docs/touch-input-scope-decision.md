# Touch and gesture input on PdfCanvas — scope decision

<!-- tracking-banner: exempt -- a decision record; GitHub Issues remains canonical for anything still open -->

**Status:** DEFERRED. Touch/gesture input is a **non-goal** for the current
scope. Nothing is implemented and nothing is planned in this cycle.
**Finding:** GitHub #87, recorded in pipeline `p84-remaining-open`, phase 4.
**Owner:** PDFE maintainers.
**Decided:** 2026-09-12.
**Re-evaluate by:** 2027-03-12, or sooner if any of the triggers in section 4
fires.
**Machine guard:** `tests/test_touch_input_scope_guard.cpp`
(`TouchInputScopeGuard.*`).

---

## 1. What #87 asked

Whether PdfCanvas should handle touch and gesture input — pinch-to-zoom,
two-finger pan, tap-to-select — on touch-capable Windows devices, or whether
that is out of scope.

The issue had been open long enough that it was no longer clear whether "not
implemented" meant "not done yet" or "decided against". This record answers
that, so the next reader does not have to re-derive it, and section 5 explains
why a bare answer was not enough.

## 2. The state of the tree at the time of deciding

Touch is **not implemented, in any form**. Re-derived on 2026-09-12 against
`src/ui/PdfCanvas.cpp` (2426 lines) and `src/ui/PdfCanvas.h` (529 lines):

- No `QTouchEvent`, no `touchEvent()` override.
- No `QEvent::TouchBegin` / `TouchUpdate` / `TouchEnd` / `TouchCancel` handling.
- No `Qt::WA_AcceptTouchEvents`, no `setAttribute` enabling touch.
- No `QGestureEvent`, `grabGesture`, `QPinchGesture`, `QPanGesture`,
  `QSwipeGesture`, `QTapGesture`.

Every occurrence of the words "touch" and "gesture" in those two files is prose
in a comment — "this destructor having to touch a Document", "the only pan
gesture this canvas had was the LEFT-button drag". That is exactly why the
guard test strips comments before matching: a lexical scan that did not would
report this file as already implementing touch.

What the canvas does have is a full mouse and keyboard interaction model:
`mousePressEvent`, `mouseMoveEvent`, `mouseReleaseEvent`, `wheelEvent`,
`keyPressEvent`, middle-button pan, Ctrl+wheel zoom. On a Windows touch device
these still work, because Windows synthesises mouse events from single-finger
touch. Single-finger tap, drag and scroll therefore already function; what is
absent is the *multi-touch* vocabulary — pinch-zoom and two-finger pan — and any
touch-specific hit-target sizing.

## 3. The decision, and the reasoning in the order it matters

**DEFER. Touch is a non-goal for this scope.**

1. **The platform already covers the single-finger case.** Windows touch
   promotion means a touch device is not unusable today; it is merely not
   *optimised*. The gap is a quality-of-experience gap on a minority
   configuration, not a functional hole. That is a materially different
   priority from the open functional defects in the tracker.

2. **A partial touch implementation is worse than none.** The moment
   `WA_AcceptTouchEvents` is set, Windows STOPS synthesising mouse events for
   that widget for the gestures the widget claims. A half-done touch handler
   therefore breaks the single-finger interaction that works today. Touch is an
   all-or-nothing change to the canvas input model, not something to add
   incrementally between other work.

3. **It cannot be tested with the harness this project has.** The UI test layer
   is pywinauto / WinAppDriver / FlaUI / PyAutoGUI, all of which drive *mouse
   and keyboard*. Injecting a synthetic multi-touch contact on Windows needs
   `InjectSyntheticPointerInput` against a `CreateSyntheticPointerDevice`
   handle, which nothing in `tests/` currently touches and which the CI runner
   has never been shown to support. Shipping touch handling with no way to
   redden a test for it is precisely the false-green this project has spent
   several audits removing.

4. **Nobody has asked for it.** No issue other than #87 references touch, and
   #87 is the scoping question itself rather than a user report.

## 4. What would change this

Any one of these reopens the decision:

- A user report of an actual touch workflow that is blocked, not merely clumsy.
- A decision to ship PDFE on a tablet / 2-in-1 form factor as a supported
  target.
- Synthetic-touch injection becoming available in the test harness, which
  removes reason 3.
- Qt gaining, or the project adopting, a gesture layer that does not disable
  mouse synthesis — which removes reason 2.

## 5. Why this record has a machine guard

A scope decision recorded only in prose decays silently. The specific failure
mode here is not that someone disagrees with the decision — it is that someone
adds a `touchEvent()` override to `PdfCanvas` for one narrow purpose, ships it
with no test, and this document keeps asserting that touch is not implemented.
The tree and the record would then disagree with nobody noticing.

`tests/test_touch_input_scope_guard.cpp` closes that. It re-derives the touch
API surface of `src/ui/PdfCanvas.{h,cpp}` from the source on every run, with
comments and string literals stripped, and fails if any touch or gesture symbol
appears in CODE without a corresponding reference from `tests/`. Concretely:

| tree state | guard |
|---|---|
| no touch in PdfCanvas (today) | passes |
| touch added, no test references it | **FAILS**, naming the symbols |
| touch added *with* test coverage | passes — the decision has been reversed deliberately, and this document must be updated |
| this document deleted or renamed | **FAILS** |
| `PdfCanvas.cpp` deleted or renamed | **FAILS** (fail-closed, not silently green) |

The guard is a **floor, not a proof**, in the same sense as
`tests/test_page_load_guard_sites.cpp`: it is a lexical scan, so touch handling
reached through a macro or a `QObject::event()` dispatch that never names a
touch type would be invisible to it. It is there to catch the ordinary way this
decision would rot, not every conceivable one.

## 6. What this record does not decide

- It does not say touch input is unwanted or wrong. It says it is not in this
  scope and is not worth a half-measure.
- It does not cover the **pen / stylus** case (`QTabletEvent`), which is a
  separate input model and is likewise unimplemented and unexamined.
- It does not cover touch-friendly *sizing* of toolbar and menu hit targets,
  which is a styling question independent of event handling.
