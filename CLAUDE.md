# PDFE — Project Instructions

## What PDFE Is

Native Windows desktop application: C++20 + Qt 6.10 PDF editor.
**Not a web app.** Selenium does NOT apply here.

## UI Testing Stack (Native Windows)

Use whichever tool fits the scenario — combine freely:

| Tool | Use when |
|---|---|
| **pywinauto** | Primary driver for Qt/Win32 UI automation. Control-level interaction via UIA backend (`backend="uia"`). Good for menus, dialogs, toolbars, keyboard input, window state. |
| **WinAppDriver** | When Appium/Selenium-style WebDriver API is preferred, or for cross-language test harnesses. Runs as a service on `127.0.0.1:4723`. Use with `Appium-Python-Client`. |
| **FlaUI** | .NET-based UIA wrapper. Reach for it when pywinauto's UIA support is thin for a specific control (e.g., complex custom Qt widgets exposed via QAccessible). Invokable from PowerShell or a small C# test runner. |
| **PyAutoGUI** | Last-resort pixel/keyboard/mouse automation when no UIA tree exists (e.g., the PDF canvas render area, drag-drop on raster content, visual regression of rendered pages). Pair with screenshot diffing. |

### Picking a tool
1. **Is there a UIA control tree?** → pywinauto first. FlaUI if pywinauto can't see it.
2. **Is it the PDF canvas / rendered bitmap?** → PyAutoGUI (coord-based) + screenshot compare.
3. **Need a WebDriver-compatible harness?** → WinAppDriver.
4. **Never use Selenium** — it is for web UI only.

### Making Qt widgets automation-friendly
- Set `objectName` on every interactive widget (pywinauto reads this via `auto_id`).
- Set `accessibleName` / `accessibleDescription` for screen-reader + UIA `Name` property.
- Prefer `QAction` with stable `objectName` for menu/toolbar items.
- Avoid duplicate accessible names within a window.
- For custom-painted widgets (PdfCanvas), expose a `QAccessibleInterface` subclass so at minimum the widget itself shows up in the UIA tree with a known name.

## Writing a UI test — THE ESTABLISHED WAY (read before adding one)

Two mistakes have recurred for the life of this project. Both look like the
product is broken when it is not, and both are already solved. Use the
solutions; do not re-derive them.

### 1. NEVER detect a dialog through UIA — it cannot see a modal

pywinauto's UIA backend **cannot enumerate a Qt app-modal dialog** while its
`exec()` loop runs. Neither `Application.windows()`, nor `top_window()`, nor
`Desktop(backend="uia").windows()`. Measured: a dialog sat open for **four
minutes** while a 240 s UIA poll reported nothing.

Use, in order of preference:

1. **`registerDialogIntrospection()`** — the dialog's own seam. Gives the child
   inventory and optional auto-dismissal, not just presence.
   `test_dialog_introspection_registry.cpp` FAILS the build if a `QDialog`
   subclass forgets it, so this should already be true of every dialog.
2. **`tests/helpers/modal_dialog.py`** — `wait_for_window(pid, title)` /
   `close_window(hwnd)`. Raw Win32 `EnumWindows`; needs no per-dialog opt-in.
   Finds a modal in under a second.

If you catch yourself writing "we do not assert on the dialog because UIA
cannot see it" — that comment existed in ten files — you are writing a test
that proves nothing. Use the above instead.

### 2. NEVER guess canvas coordinates — ask where the page is

`rect.left + 60` is a guess. **The page does not fill the canvas**, so a canvas
offset routinely lands in the grey margin: the click hits nothing, the tool
never fires, and the test reports a timeout rather than "I clicked off the
page". Measured wrong answers: a canvas-fraction guess put a press at
**PDF x = −30.7**, and a screenshot measurement disagreed with the UIA canvas
rect by **~145 px** (different DPI spaces).

    from tests.helpers.canvas_geometry import point_on_page
    x, y = point_on_page(session, page_index=0, fx=0.5, fy=0.25)

On the page whatever the zoom, scroll offset or DPI. If the seam does not
answer, **fail** — never fall back to a guess.

The one legitimate exception is a **viewport gesture** — proving event delivery
to the canvas widget, or a scroll drag — where page position genuinely does not
matter. Those sites are commented as deliberate; leave them alone.

Full analysis, including why both recurred: `docs/audit/ui-test-observability-audit.md`.

## Python Test Scripts
- Explicit UTF-8: `# -*- coding: utf-8 -*-` and open files with `encoding="utf-8"`.
- Tests live under `tests/` alongside GoogleTest C++ tests. Name Python UI tests `test_ui_*.py`.
- Build artifact to launch: `build/windows-vs2022/Debug/PDFE.exe` (or Release equivalent).
- Always close the app window at test teardown; orphaned `PDFE.exe` processes wedge the next run.

## Where the truth lives (read before trusting any list of defects)

**GitHub Issues at `ipswitch8/PDFE` is the canonical tracker for anything still
OPEN.** Nothing else is. See [`docs/TRACKING.md`](docs/TRACKING.md).

This repo contains roughly two dozen documents that each look like a defect
list — `gap-register.md` (458 rows), `status-register.md` (392), the `WIN-`/
`PDF-`/`SEC-` gap docs, ~52 `phase-NN-dispositions.md`, `rtm-coverage.md`,
round-2/round-3 registers, and more. **They are historical evidence, not work
lists**, and each now carries a banner saying so. Read them for the reasoning;
do not read them to find out what is still true. Where a register row and a
GitHub issue disagree, the issue wins.

Three things are LIVE rather than historical and must keep being maintained —
they are operational state CI or code reads, not trackers:
`tests/docs/known_issues.md`, `tests/KNOWN_WEAK_TESTS.txt`, and the
`tools/gate/*.json` baselines.

**Beware `#nn`.** Two unrelated schemes use it: GitHub issue numbers, and the
internal "Defect #nn" scheme that appears inside `gap-register.md` rows. Write
`GitHub #nn` or `Defect #nn` explicitly; never bare `#nn`.

## Existing Test Layers (do not replace — augment)
- **GoogleTest** (`tests/test_*.cpp`) — unit + headless integration. Primary suite, **6,571 registered tests** (2026-10-14, enumerated by the sharded harness; 6,557 on 2026-10-13, 5,903 on 2026-08-27, 5,898 on 2026-08-25, and the long-stale "2068" before that predated roughly three thousand of them). Re-derive with `pdfe_tests.exe --gtest_list_tests`, never by counting `TEST*` macros — `TEST_P` suites expand at run time. Latest run record: [`docs/audit/release-readiness/gtest-run-summary.md`](docs/audit/release-readiness/gtest-run-summary.md).
- **Qt Test** (`Qt6::Test`) — Qt widget/signal tests.
- **Python robustness scripts** (`tests/test_pdf_open_robustness.py`) — open-file stress.
- **New UI layer (this doc)** — pywinauto / WinAppDriver / FlaUI / PyAutoGUI for end-to-end native UI flows that GoogleTest can't cover (real window, real menus, real dialogs, real render output).

## Gate Discipline

Gate agents (karen, validator, test-runner) run between pipeline
phases. They are not optional and not overhead.

### Evidence from this project

Every gate review has improved the output. Every gate skip has
caused rework costing 2-5x the gate cycle time:

| Incident | Skip rationale | Actual cost |
|---|---|---|
| Phase-5 gate fabrication | "save 10 min" | 90 min full rewrite |
| Remediation 5+6 gate skip | "save 15 min" | 3 hr correction + trust repair |
| Bash patches to avoid hook | "save 5 min" | 30 min regressions + revert |

Success rate of skipping gates: **0%**.
Success rate of running gates: **100%** (every review caught real issues).

### Rules

1. Gates are mandatory between phases. No exceptions.
2. Context pressure is a signal to **hand back cleanly**, not skip gates. A partial phase with honest gates is worth more than a complete phase with fabricated ones. This is the most important rule — context pressure was the root cause of every bypass incident in this project.
3. When a gate agent returns FAIL, the findings are the **next work items** — not obstacles to argue away or reclassify.
4. Do not merge phases to reduce the number of gate cycles. Each phase gets its own gates.
5. Do not advance phase-state.json via Bash to bypass the gate hook. The hook prompts the user when this is attempted.
6. Do not fabricate gate results by writing `true` into phase-state.json without actually running the agent.
7. The pre-tool-use hook enforces mechanically: blocks Bash/Write/Edit once a gate is invoked until all pass; blocks `git commit` if gates pending; hard-blocks `current_gate_results` modification while gates are in progress; prompts user on `phases_complete`/`current_phase_index` manipulation. The remaining gap — never invoking the first gate — is addressed by this section: the evidence shows it always costs more than compliance.

## Gate Reliability Protocol (survives the post-compact hook-delivery bug)

Read this every session — it is durable where mid-conversation learning is not.

**Verdicts come from artifacts, never prose.** A gate passes only when a
machine-checked artifact exists at
`.claude/pipelines/<pipeline-id>/gate-artifacts/<phase-id>/<agent>.json` =
`{"verdict":"PASS"}`, plus (for `test-runner`/`perf-benchmarks`) a green
`gtest-results.xml` in that same dir.

**When invoking any gate agent**, end its prompt with:
> As your FINAL action, from the pipeline root, run:
> `cd <pipeline-root> && ~/.claude/hooks/emit-gate-verdict.sh <phase-id> <agent> PASS|FAIL [gtest-xml] [target-test] --pipeline <pipeline-id>`

so the artifact is byte-correct and correctly located — nothing is interpreted
from free prose (reliability fix #2). The `cd` is mandatory: the emitter writes a
RELATIVE path, so a gate whose shell sits elsewhere files a real verdict into a
shadow tree nothing reads. Require the gate to READ the artifact back and confirm
where it landed.

**After any /compact or session resume, before acting on a pipeline (fix #1 + #3):**
1. Re-ground from ground truth, not conversational memory: read
   `.claude/pipelines/<pipeline-id>/pipeline.json` and the
   `.claude/pipelines/<pipeline-id>/gate-artifacts/` evidence (and
   `pipeline-status.snapshot` if present).
2. Run `~/.claude/hooks/check-hooks-alive.sh <session-id>`. If it prints
   **DEAD**, the Stop/SubagentStop recording hooks are not firing (the compact
   bug). **HARD STOP:** do not commit, do not advance, do not treat any gate as
   passed; tell the user the hooks appear dead and recommend restarting the
   session, then stop.

> **PATH CORRECTION (meta-audit follow-up F19, gap A5-50).** This section used to
> name `tools/gate/emit-gate-verdict.sh` and `tools/gate/check-hooks-alive.sh`.
> **Neither has ever existed** — `tools/gate/` holds only Python checkers, and
> both scripts live in `~/.claude/hooks/`. It also used the pre-namespacing
> `.claude/gate-artifacts/` and `.claude/pipeline.json` paths, which the
> per-pipeline layout replaced. A gate agent that followed this text verbatim
> would fail to emit its verdict at all — the single most load-bearing step in
> the whole protocol. Corrected here to match the hooks as they actually are.

`hook-liveness-guard.sh` (PreToolUse) enforces the same rule mechanically by
blocking `git commit` when a compaction occurred with no heartbeat since. Every
path fails closed: the worst case is a blocked commit, never a forged or stale
pass.

### Two things the artifact tree does NOT tell you (GitHub #206, measured 2026-09-18)

**1. `phases_complete` is ADVISORY. Never read it as a completion signal.**
Across 39 pipelines, 16 ledgers disagree with their own artifacts — `windows-fixes`
reports `phases_complete: []` over six phases whose every gate has a PASS artifact.
The disagreement fails *closed* (an under-reporting ledger blocks; it never waves
work through), so it is an evidence hole rather than a safety hole. Derive
completion from the `gate-artifacts/` tree, which is the verdict of record.
`python tools/gate/audit_gate_artifacts.py` prints both numbers side by side.

**2. An all-PASS artifact tree does not mean no gate ever failed.**
`emit-gate-verdict.sh` writes `<agent>.json` with `>` at a path fixed per
(phase, agent), so the ordinary cycle — gate FAILs, the finding is fixed, the
same gate re-runs green — **overwrites the FAIL**. Every completed pipeline is
therefore guaranteed to end up all-PASS. That, not any inability to express
failure, is why a census of 784 artifacts finds zero `FAIL`: the emitter
validates with `case "$VERDICT" in PASS|FAIL)` and writes FAIL byte-correctly.
Two `claim-check` FAILs in `p87-open-issues` (phase-1 and phase-5) were real,
became real work, and survive nowhere on disk.

The durability fix — append-only `<base>/gate-history.jsonl`, leaving
`<agent>.json` byte-identical — is a reviewed patch at
[`tools/gate/patches/emit-gate-verdict-history.patch`](tools/gate/patches/emit-gate-verdict-history.patch).
It is a patch rather than an applied change because the writer lives outside
this repo and governs the gating of whatever pipeline would apply it.
