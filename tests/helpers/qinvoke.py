# -*- coding: utf-8 -*-
"""Pipeline b3f19d72 phase-11: helper for invoking Q_INVOKABLE-like test seams
from Python pywinauto tests.

Qt6 Q_INVOKABLE methods are not directly callable via UIA — there is no UIA
pattern for "call this method by name and get a return value" in a Win32
process.  Instead, PDFE exposes hidden QLineEdit "pipes" whose objectNames
are stable UIA AutomationIds.  Writing a value to the pipe via
``set_edit_text()`` (UIA ValuePattern) triggers a textChanged signal inside
PDFE that dispatches the corresponding action/method.

Public API
----------
activate_tool_by_name(session, action_object_name) -> bool
    Write ``action_object_name`` to the ``testToolActivatePipe`` QLineEdit
    in the running PDFE instance.  PDFE's textChanged handler calls
    ``MainWindow::activateToolByObjectName()``, which finds the matching
    QAction and triggers it.

    Returns True if the pipe was reachable and the value was written
    successfully.  Returns False on any failure (pipe not found, write
    error, etc.).

click_by_pdf_coord(session, page_index, pdf_x, pdf_y, settle=0.35) -> bool
    Write ``"pageIndex,pdfX,pdfY"`` to the ``testClickByPdfCoordPipe``
    QLineEdit in the running PDFE instance.  PDFE's textChanged handler
    parses the tuple and calls ``PdfCanvas::clickByPdfCoord()``, which
    synthesizes a mouse press+release at the canvas position corresponding
    to the given PDF-space coordinate — routing through the normal FormTool
    hit-test pipeline exactly as a real user click would.

    Returns True if the pipe was reachable and the value was dispatched.
    Returns False on any failure.

customize_toolbars_add_action(session, action_object_name) -> bool
    Write ``action_object_name`` to the ``testCustomizeAddActionPipe``
    QLineEdit in the open CustomizeToolbarsDialog.  PDFE's textChanged
    handler calls ``CustomizeToolbarsDialog::addActionByObjectName()``.

    The CustomizeToolbarsDialog must already be open.  Returns True on
    success, False on any failure.

customize_toolbars_remove_action(session, action_object_name) -> bool
    Write ``action_object_name`` to the ``testCustomizeRemoveActionPipe``
    QLineEdit in the open CustomizeToolbarsDialog.  PDFE's textChanged
    handler calls ``CustomizeToolbarsDialog::removeActionByObjectName()``.

    The CustomizeToolbarsDialog must already be open.  Returns True on
    success, False on any failure.  Requires pipeline b3d7f942 phase-5 build.

customize_toolbars_reset_to_defaults(session) -> bool
    Write ``"1"`` to the ``testCustomizeResetPipe`` QLineEdit in the
    open CustomizeToolbarsDialog.  PDFE's textChanged handler calls
    ``CustomizeToolbarsDialog::resetToDefaults()``.

    The CustomizeToolbarsDialog must already be open.  Returns True on
    success, False on any failure.

customize_toolbars_available_actions(session, settle=0.30) -> list[str]
    Query the available-actions list from the open CustomizeToolbarsDialog
    via the ``testCustomizeAvailableActionsQueryPipe`` trigger +
    ``testCustomizeAvailableActionsResultPipe`` result pipe pair.  Returns
    a list of action display-name strings (one per item in the Available
    list), or an empty list on failure.  The dialog must already be open.
    Requires a pipeline c91e7f3a phase-3 PDFE build.

customize_toolbars_toolbar_contents(session, toolbar_name="current",
                                    settle=0.30) -> list[str]
    Query the current-toolbar-contents list from the open
    CustomizeToolbarsDialog.  ``toolbar_name`` may be ``"current"``
    (whichever toolbar is selected in the combo) or the display-name of a
    specific toolbar.  Returns a list of item label strings or an empty
    list on failure.  Requires a pipeline c91e7f3a phase-3 PDFE build.

customize_toolbars_widget_accessible_names(session,
                                           settle=0.30) -> dict[str, str]
    Query the accessible names of the CustomizeToolbarsDialog's
    interactive widgets.  Returns a ``{objectName: accessibleName}`` dict
    for toolbarCombo, availableList, currentList, addActionButton,
    removeActionButton, addSeparatorButton, moveUpButton, moveDownButton,
    resetToDefaultButton, okButton, cancelButton.  Returns an empty dict
    on failure.  Requires a pipeline c91e7f3a phase-3 PDFE build.

get_text_edit_toolbar_font_families(session, settle=0.30) -> list[str]
    Query the font-family entries from the TextEditToolBar's
    QFontComboBox via the ``testFontFamiliesQueryPipe`` trigger +
    ``testFontFamiliesResultPipe`` result pipe pair.  Returns a list
    where index 0 is the CURRENT selection and indices 1..N are all
    combo entries in order.  Returns an empty list on failure.
    Requires a pipeline c91e7f3a phase-3 PDFE build.

set_text_edit_toolbar_font_family(session, family, settle=0.30) -> bool
    Set the TextEditToolBar's QFontComboBox selection to ``family`` via
    the ``testSetFontFamilyPipe`` pipe.  PDFE's handler calls
    ``combo->setCurrentText(family)`` and emits ``fontFamilyChanged``,
    propagating the change exactly as a user interaction would.  Returns
    True on success, False on failure.  Requires a pipeline c91e7f3a
    phase-3 PDFE build.

move_page_programmatic(session, from_index, to_index, settle=0.50) -> bool
    Write ``"from_index,to_index"`` to the ``testMovePagePipe`` QLineEdit.
    PDFE's handler calls ``pagePanel_->movePageProgrammatic(from, to)``
    which emits ``pageMoved(from, to)`` → MovePageCommand pipeline.
    Bypasses the unreliable pyautogui drag-drop path for PagePanel reorder.
    Requires ``PDFE_TEST_PIPES=1``.  Requires pipeline b3d7f942 phase-3
    PDFE build.

add_redaction_mark_at_pdf_rect(session, page_index, x, y, w, h,
                               settle=0.40) -> bool
    Write ``"page_index,x,y,w,h"`` to the ``testAddRedactionMarkPipe``
    QLineEdit.  Coords are in MuPDF/fitz-space (origin top-left, Y-down,
    points).  PDFE's handler calls
    ``canvas_->addRedactionMarkAtPdfRect(pageIndex, QRectF(x,y,w,h))``,
    which emits ``redactionMarkRequested`` → CreateRedactionCommand pipeline.
    Bypasses the unreliable canvas-drag coordinate-translation path.
    Requires ``PDFE_TEST_PIPES=1``.  Requires pipeline b3d7f942 phase-3
    PDFE build.

get_form_field_panel_value(session, settle=0.40, timeout=5.0) -> str | None
    Read the currently-displayed text value from FormFieldPropertiesPanel
    via the testFormFieldValueQueryPipe / testFormFieldValueResultPipe
    round-trip.  Bypasses IUIAutomation::FindAll (which can deadlock when
    PDFE's main thread is busy painting after a click_by_pdf_coord).
    Returns ``""`` for non-text editors, ``None`` if pipes unreachable.
    Requires pipeline c91e7f3a phase-7 PDFE build.

get_current_annotation_alt_text(session, settle=0.30) -> str
    Write ``"1"`` to the ``testAltTextReadbackTriggerPipe`` QLineEdit.
    PDFE's textChanged handler calls ``MainWindow::getCurrentAnnotationAltText()``
    and writes the result to ``testAltTextReadbackResultPipe``.  This function
    then reads that pipe via UIA ValuePattern and returns the alt-text string.
    Returns ``""`` if no annotation is selected, no document is open, or the
    pipes are not reachable.  Requires phase-15 product build.

set_annot_color(session, r, g, b, settle=0.35) -> bool
    Write ``"R,G,B"`` to the ``testAnnotColorPipe`` QLineEdit.  PDFE's
    textChanged handler applies the given RGB color directly to the currently
    selected annotation via ``onAnnotationPropertiesChanged()``, bypassing the
    QColorDialog UIA dance.  Requires ``PDFE_TEST_ANNOT_PIPES=1`` in the
    PDFE process environment.  Returns True on success.

set_annot_author(session, author, settle=0.35) -> bool
    Write *author* to the ``testAnnotAuthorPipe`` QLineEdit.  PDFE's
    textChanged handler applies the author string directly to the selected
    annotation, bypassing UIA ValuePattern on the QLineEdit (which races
    against the Qt event loop).  Requires ``PDFE_TEST_ANNOT_PIPES=1``.

set_annot_contents(session, contents, settle=0.35) -> bool
    Write *contents* to the ``testAnnotContentsPipe`` QLineEdit.  Same
    pattern as set_annot_author but for the annotation contents field.
    Requires ``PDFE_TEST_ANNOT_PIPES=1``.

find_with_strategy_b(session, *, auto_id=None, name=None, timeout=5.0)
    Locate any UIA widget (not a pipe) using a Strategy-A → Strategy-B
    fallback.  Strategy A uses the existing session's UIA cache; Strategy B
    opens a fresh ``pywinauto.Application(backend="uia").connect(process=pid)``
    to bypass stale COM element handles from a prior PDFE session.  Returns
    the widget or None.  Addresses P-G-INFRA-6 (QListView in
    CustomizeToolbarsDialog) and P-G-INFRA-10 (zoomCombo QComboBox).

Usage
-----
    from tests.helpers.qinvoke import activate_tool_by_name

    ok = activate_tool_by_name(session, "freeTextToolAction")
    if not ok:
        # fall back to UIA toolbar search
        ...
"""

from __future__ import annotations

import re
import threading
import time
from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    from tests.helpers.pdfe_session import PDFESession

# Pipe objectName as set in MainWindow constructor.
_PIPE_AUTO_ID = "testToolActivatePipe"

# Phase-12 click-by-pdf-coord pipe.
_CLICK_PIPE_AUTO_ID = "testClickByPdfCoordPipe"

# Phase-7-loop double-click-by-pdf-coord pipe (deterministic word selection).
_DBLCLICK_PIPE_AUTO_ID = "testDoubleClickByPdfCoordPipe"

# Phase-7-loop type-into-text-edit pipe (deterministic keystroke delivery).
_TEXTEDIT_TYPE_PIPE_AUTO_ID = "testTextEditTypePipe"

# Qt::Key values used by the text-edit type seam.
QT_KEY_BACKSPACE = 0x01000003
QT_KEY_DELETE = 0x01000007
QT_KEY_RETURN = 0x01000004

# Phase-7-loop text-edit selection diagnostic readback pipes.
_TEXTEDIT_DIAG_TRIGGER_PIPE_AUTO_ID = "testTextEditDiagTriggerPipe"
_TEXTEDIT_DIAG_RESULT_PIPE_AUTO_ID = "testTextEditDiagResultPipe"

# Pipeline 9a4e2f71 phase-2: write-only seam to drive a TextEditToolBar
# font-size change (fires fontSizeChanged -> TextEditTool::applyFontSize).
# Replaces a UIA spinbox set-value gesture that no-ops in a non-interactive
# CI runner session (CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #2).
_TOOLBAR_FONT_SIZE_PIPE_AUTO_ID = "testToolbarFontSizePipe"

# Pipeline 9a4e2f71 phase-2: query/result readback for TextEditToolBar
# visibility ("1"/"0") — deterministic ground truth replacing flaky UIA
# enumeration of the toolbar's QFontComboBox child
# (CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #1).
_TOOLBAR_VISIBLE_QUERY_PIPE_AUTO_ID = "testToolbarVisibleQueryPipe"
_TOOLBAR_VISIBLE_RESULT_PIPE_AUTO_ID = "testToolbarVisibleResultPipe"

# Phase-14 CustomizeToolbarsDialog pipes.
_CUSTOMIZE_ADD_PIPE_AUTO_ID    = "testCustomizeAddActionPipe"
_CUSTOMIZE_REMOVE_PIPE_AUTO_ID = "testCustomizeRemoveActionPipe"  # b3d7f942 phase-5 GAP-13
_CUSTOMIZE_RESET_PIPE_AUTO_ID  = "testCustomizeResetPipe"

# Phase-15 AltText readback pipes.
_ALT_TEXT_TRIGGER_PIPE_AUTO_ID = "testAltTextReadbackTriggerPipe"
_ALT_TEXT_RESULT_PIPE_AUTO_ID = "testAltTextReadbackResultPipe"

# Phase-14-loop PreferencesDialog DIALOG-CHILD pipes (Server-2019 UIA
# modal-enumeration hang fix). These QLineEdits are children of the OPEN
# PreferencesDialog (not MainWindow) so they receive UIA ValuePattern
# events while the modal is up — a MainWindow-child pipe would not, because
# Qt modality blocks input to other top-levels. Created only when
# PDFE_TEST_PIPES is set in the PDFE process environment.
_PREF_WIDGET_NAMES_QUERY_PIPE_AUTO_ID  = "testPrefWidgetNamesQueryPipe"
_PREF_WIDGET_NAMES_RESULT_PIPE_AUTO_ID = "testPrefWidgetNamesResultPipe"
_PREF_GET_VALUE_QUERY_PIPE_AUTO_ID     = "testPrefGetValueQueryPipe"
_PREF_GET_VALUE_RESULT_PIPE_AUTO_ID    = "testPrefGetValueResultPipe"
_PREF_SET_VALUE_PIPE_AUTO_ID           = "testPrefSetValuePipe"
_PREF_ACCEPT_PIPE_AUTO_ID              = "testPrefAcceptPipe"
_PREF_REJECT_PIPE_AUTO_ID              = "testPrefRejectPipe"

# Phase-7-loop comment-panel row-inventory readback pipes.  Bridge the
# Server-2019 Qt6 UIA gap where QTreeWidget rows do not enumerate via
# descendants(); read the comment-tree row list from the model instead.
_COMMENT_INVENTORY_TRIGGER_PIPE_AUTO_ID = "testCommentInventoryTriggerPipe"
_COMMENT_INVENTORY_RESULT_PIPE_AUTO_ID = "testCommentInventoryResultPipe"

# Pipeline c91e7f3a phase-3: CustomizeToolbarsDialog list-enumeration pipes.
# These bridge the Qt6/Server-2019 UIA gap where QListView rows are not exposed
# as ListItem descendants.  Each pair: Python writes a query token to the trigger
# pipe; PDFE populates the result pipe; Python reads via UIA ValuePattern.
_CUSTOMIZE_AVAIL_QUERY_PIPE_AUTO_ID  = "testCustomizeAvailableActionsQueryPipe"
_CUSTOMIZE_AVAIL_RESULT_PIPE_AUTO_ID = "testCustomizeAvailableActionsResultPipe"
_CUSTOMIZE_CONTENTS_QUERY_PIPE_AUTO_ID  = "testCustomizeToolbarContentsQueryPipe"
_CUSTOMIZE_CONTENTS_RESULT_PIPE_AUTO_ID = "testCustomizeToolbarContentsResultPipe"
_CUSTOMIZE_WIDGET_NAMES_QUERY_PIPE_AUTO_ID  = "testCustomizeWidgetNamesQueryPipe"
_CUSTOMIZE_WIDGET_NAMES_RESULT_PIPE_AUTO_ID = "testCustomizeWidgetNamesResultPipe"

# Pipeline c91e7f3a phase-3: TextEditToolBar fontComboBox pipes.
# Qt6 UIA bridge does not reliably expose QFontComboBox contents when the
# toolbar is mounted via addToolBar() on Windows Server 2019.
_FONT_FAMILIES_QUERY_PIPE_AUTO_ID  = "testFontFamiliesQueryPipe"
_FONT_FAMILIES_RESULT_PIPE_AUTO_ID = "testFontFamiliesResultPipe"
_SET_FONT_FAMILY_PIPE_AUTO_ID      = "testSetFontFamilyPipe"

# Pipeline c91e7f3a phase-5: form field value set pipe.
# Bypasses the UIA ValuePattern -> editingFinished race in
# FormFieldPropertiesPanel::textEdit_ (QLineEdit only fires editingFinished
# on Enter/blur; UIA set_edit_text does not trigger it).
_FORM_FIELD_VALUE_SET_PIPE_AUTO_ID = "testFormFieldValueSetPipe"

# Pipeline f2a8c3e1 phase-2: annotation property pipes.
# These pipes are only created when PDFE_TEST_ANNOT_PIPES=1 is set in the
# PDFE process environment.  Gated at the product side; inert in production.
_ANNOT_SELECT_PIPE_AUTO_ID   = "testAnnotSelectPipe"
_ANNOT_COLOR_PIPE_AUTO_ID    = "testAnnotColorPipe"
_ANNOT_AUTHOR_PIPE_AUTO_ID   = "testAnnotAuthorPipe"
_ANNOT_CONTENTS_PIPE_AUTO_ID = "testAnnotContentsPipe"
_ANNOT_CREATE_PIPE_AUTO_ID   = "testAnnotCreatePipe"
_ANNOT_DELETE_PIPE_AUTO_ID   = "testAnnotDeletePipe"
_CANVAS_DRAG_PIPE_AUTO_ID    = "testCanvasDragPipe"

# How long to poll for the pipe to appear after PDFE starts.
_PIPE_TIMEOUT_S = 5.0

# Longer timeout for the annotation property pipes, which are searched after
# lengthy test operations (annotation draw + UIA tree refresh) that can
# temporarily increase UIA enumeration latency in a multi-test batch session.
_ANNOT_PIPE_TIMEOUT_S = 25.0

# Resolved pipe-control cache, keyed by (pid, auto_id).
#
# gap-A14 hardening (pipeline f4a82c1e): the pipe QLineEdits are created once at
# MainWindow construction and live for the whole process, so their UIA elements
# are stable. Re-resolving them on every _write_to_pipe re-runs Strategy A's
# child_window(...).wait("exists"), whose UIA enumeration issues a COM call that
# can BLOCK indefinitely when PDFE's Win32 message pump is busy repainting tiles
# — e.g. inside test_ui_journey_soak's scroll_whole_document / armed_race_burst
# tight scroll loops (settle≈0). The 0.5 s wait cannot interrupt a stuck COM
# call, so pytest-timeout hard-kills the file at 120 s on the Server-2019 CI.
# Caching the resolved control means the loop reuses it and writes go straight
# through the ValuePattern with NO enumeration → no hang. A stale entry (write
# fails) is dropped and re-resolved once (see _write_to_pipe).
_PIPE_CTRL_CACHE: dict = {}

# gap-A14 wedge-verdict cache: {pid: monotonic_deadline}. When _uia_responsive
# reports the provider is wedged, we remember it for a short TTL so subsequent
# _find_pipe calls fast-fail INSTANTLY (no re-probe) until the TTL lapses. The
# wedge is persistent (observed for tens of minutes), and an op issues several
# pipe lookups, so without this a sustained wedge would pay one full probe
# timeout PER lookup (~21 s/op) and a default-budget journey could still exceed
# the pytest-timeout. Re-probing once per TTL detects recovery; a stale window
# only skips a second or two of ops right at recovery, which is harmless for a
# soak whose oracles run at checkpoints.
_UIA_WEDGE_TTL_S = 8.0
_UIA_WEDGE_UNTIL: dict = {}


def is_uia_wedged(session: "PDFESession") -> bool:
    """True if _find_pipe VERY RECENTLY (within the wedge TTL) found this
    session's UIA provider wedged (gap-A14). Cheap and non-blocking — reads the
    verdict marker, runs no probe. Journey oracles use this to distinguish an op
    that could not be DRIVEN (UIA wedge, a harness/platform limitation — PDFE
    stays alive) from a genuine product failure, so a wedge is reported as
    inconclusive rather than a false product defect."""
    pid = getattr(session, "pid", None)
    if pid is None:
        return False
    return time.monotonic() < _UIA_WEDGE_UNTIL.get(pid, 0.0)


def _invalidate_pipe_cache(session: "PDFESession", auto_id: str) -> None:
    """Drop a cached pipe control (call when a write through it fails)."""
    _PIPE_CTRL_CACHE.pop((getattr(session, "pid", None), auto_id), None)


def _process_alive(session: "PDFESession") -> bool:
    """True if PDFE's process is still running (cheap, no UIA). Used to guard
    Strategy C: its descendants()/FindAll on a DEAD window's stale COM handle
    blocks indefinitely (pywinauto's timeout does not bound the COM call), which
    turned a crashed-mid-run PDFE into a full-pytest-timeout hang. A dead process
    cannot own the pipe, so we skip the scan and fast-fail instead. On any probe
    error, default True to preserve the prior lookup behaviour."""
    try:
        from tests.helpers.pdfe_session import list_pdfe_pids  # noqa: PLC0415
        pid = getattr(session, "pid", None)
        return pid is None or pid in list_pdfe_pids()
    except Exception:  # noqa: BLE001
        return True


def _uia_responsive(session: "PDFESession", timeout_s: float = 3.0) -> bool:
    """Bounded probe: True iff PDFE's UIA provider answers a TRIVIAL query in
    ``timeout_s`` seconds. False ONLY on a definite timeout (the gap-A14 wedge).

    Background (gap-A14, diagnosed — do not re-diagnose): after certain ops
    (armed_race_burst / delete_page) PDFE stays ALIVE at ~0% CPU and its Win32
    message pump keeps answering WM_NULL in 0 ms (Windows does NOT flag it hung),
    yet its UIA accessibility provider stops answering tree-enumeration queries.
    Once wedged, EVERY UIA FindAll blocks indefinitely and pywinauto's timeout
    does NOT bound the underlying COM call, so _find_pipe's Strategy A.5 / B / C
    each hang for the whole test (a 40-min / pytest-timeout deadlock). WM_NULL
    responsiveness does NOT predict the wedge, so we cannot use it.

    Design:
      * Get PDFE's top-level HWND cheaply via win32 (NO UIA) so ElementFromHandle
        needs no tree scan.
      * Run the ENTIRE UIA query (fresh IUIAutomation in its own COM apartment,
        ElementFromHandle, then ONE property that round-trips to the provider) on
        a DAEMON thread, bounded by ``threading.Event().wait(timeout_s)``. A fresh
        client (like Strategy A.5) avoids stale COM handles. If the provider is
        wedged, the property access blocks inside the daemon thread; the outer
        wait() times out and we ABANDON the thread (it dies with the process — we
        never join it, so a stuck COM call cannot hang the caller).
      * Return ONLY a bool. A UIA control resolved on a worker thread cannot be
        safely used on the main thread (cross-apartment COM), so nothing but the
        bool ever crosses the thread boundary.

    Fail-open vs fail-closed policy (deliberate, documented):
      * Return False ("wedged") ONLY on a definite timeout — the probe thread did
        not finish within ``timeout_s``. Callers gate ``_process_alive`` first, so
        by here the process is alive; a definite timeout on a live process is the
        wedge signature.
      * Return True ("responsive / fail-open") for everything else: no pid, no
        top-level HWND yet, import/COM-setup error, or the probe FINISHING with an
        error (ElementFromHandle/property raised but did NOT hang). Treating an
        inability-to-probe as a wedge would falsely skip a HEALTHY provider and
        no-op real work, so we fail open there. Only an actual hang fails closed.
    """
    try:
        from tests.helpers.pdfe_session import (  # noqa: PLC0415
            _find_top_window_hwnd,
        )
        pid = getattr(session, "pid", None)
        if not pid:
            return True  # can't probe -> fail-open
        hwnd = _find_top_window_hwnd(pid)
        if not hwnd:
            # No visible top-level window to probe (e.g. still starting, or a
            # transient state). Not a wedge signal -> fail-open.
            return True
    except Exception:  # noqa: BLE001
        return True  # setup/import error -> fail-open

    done = threading.Event()
    ok = {"v": False}

    def _probe() -> None:
        com_inited = False
        try:
            import comtypes  # noqa: PLC0415
            try:
                comtypes.CoInitializeEx(comtypes.COINIT_MULTITHREADED)
                com_inited = True
            except Exception:  # noqa: BLE001
                # Already-initialised or unavailable; proceed regardless.
                pass
            import comtypes.client  # noqa: PLC0415
            import comtypes.gen.UIAutomationClient as _UIA  # noqa: PLC0415

            _uia = comtypes.client.CreateObject(
                "{ff48dba4-60ef-4201-aa87-54103eef594e}",
                interface=_UIA.IUIAutomation,
            )
            # ElementFromHandle is a direct HWND->element map (no tree scan).
            elem = _uia.ElementFromHandle(hwnd)
            # Touch ONE property that round-trips to the provider. If the
            # provider is wedged, THIS is where we block and never set `done`.
            _ = elem.CurrentName
            ok["v"] = True
        except Exception:  # noqa: BLE001
            # Provider errored while alive but did NOT hang -> not the wedge.
            ok["v"] = False
        finally:
            done.set()
            if com_inited:
                try:
                    comtypes.CoUninitialize()
                except Exception:  # noqa: BLE001
                    pass

    t = threading.Thread(target=_probe, name="uia-wedge-probe", daemon=True)
    t.start()
    if not done.wait(timeout_s):
        # Definite timeout on a live process -> WEDGED. Abandon the daemon
        # thread (do NOT join; it dies with the process).
        return False
    # Finished within budget: responsive if the property read succeeded, and
    # fail-open (True) if it merely errored without hanging.
    return True


def _find_pipe(session: "PDFESession",
               auto_id: str,
               lookup_timeout: float = _PIPE_TIMEOUT_S):
    """Locate a hidden pipe QLineEdit using a 3-strategy UIA fallback chain.

    The three strategies are tried in order from fastest to most thorough.
    The first one that returns a live element wins; the others are skipped.

    Strategy A — ``session.window.child_window(auto_id=...)``:
        Direct child_window lookup with a short 3 s wait.  Sufficient under
        normal UIA conditions; fails fast when the UIA tree is overloaded
        (e.g. immediately after an annotation drag) or when stale COM element
        handles from a previously-killed pdfe.exe linger in the cache.

    Strategy B — fresh ``pywinauto.Application(backend="uia").connect(process=pid)``:
        Opens a brand-new IUIAutomation client rooted at the live PID,
        bypassing any stale COM element references that accumulated in the
        existing Application's UIA cache.  Defunct element handles from a
        prior process can match the same AutomationId and never transition to
        "exists"; a fresh connection avoids this.

    Strategy C — ``session.find_optional(auto_id=...)``:
        Full descendants scan via the original session window.  Slowest but
        most thorough — catches widgets whose AutomationId includes a Qt
        full-path prefix not matched by ``child_window()``.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    auto_id:
        UIA AutomationId (== Qt objectName) of the target pipe QLineEdit.
    lookup_timeout:
        Total seconds budget passed to Strategy B's ``wait("exists")`` and
        Strategy C's ``find_optional()`` timeout.

    Returns
    -------
    pywinauto control or None
        The located pipe control, or ``None`` if all three strategies fail.
    """
    # gap-A14 top gate (bounded wedge PROBE). Placed BEFORE the Strategy-0 cache
    # return so cached-control WRITES are protected too: a wedged UIA provider
    # hangs set_edit_text on a cached control just as it hangs enumeration.
    #   * Dead process -> fast None (a dead process cannot own the pipe).
    #   * Alive but UIA-wedged -> _uia_responsive times out (bounded) and we
    #     fast-fail to None; the caller no-ops the op and the journey ends in
    #     bounded time instead of a 40-min FindAll deadlock.
    #   * Alive and responsive -> the probe returns in ms (ElementFromHandle +
    #     one property, no tree scan) and full resolution proceeds as before.
    if not _process_alive(session):
        return None
    _pid = getattr(session, "pid", None)
    # Sustained-wedge fast path: if we very recently found the provider wedged,
    # skip the (bounded but non-zero) re-probe and fast-fail immediately.
    if _pid is not None and time.monotonic() < _UIA_WEDGE_UNTIL.get(_pid, 0.0):
        return None
    if not _uia_responsive(session):
        if _pid is not None:
            _UIA_WEDGE_UNTIL[_pid] = time.monotonic() + _UIA_WEDGE_TTL_S
        return None
    # Responsive now — clear any stale wedge marker so lookups resume at once.
    if _pid is not None:
        _UIA_WEDGE_UNTIL.pop(_pid, None)

    # Strategy 0: cached control (gap-A14 hardening). The pipe QLineEdits are
    # process-lifetime stable, so a previously-resolved control is reused with
    # NO UIA enumeration — this is what keeps the journey soak's tight scroll
    # loops from hanging a lookup while tiles repaint. A stale entry is dropped
    # by _write_to_pipe on write failure, so returning it here is safe.
    _cache_key = (getattr(session, "pid", None), auto_id)
    _cached = _PIPE_CTRL_CACHE.get(_cache_key)
    if _cached is not None:
        return _cached

    pipe = None

    # Strategy A: direct child_window lookup, short quick-pass.
    # Qt6 prefixes the AutomationId with the full widget path
    # ("QApplication.mainWindow.<objectName>"), so this EXACT auto_id match
    # almost always misses and Strategy A.5 (the raw-COM children scan)
    # actually resolves the pipe. The previous 3 s wait("exists") was
    # therefore ~3 s of pure dead time on essentially every pipe call —
    # and these helpers are called several times per edit in the
    # undo/redo-chain tests (5 edits × 3 pipe ops), which is what drove
    # those tests toward the 120 s per-test cap. Drop the wait to 0.5 s:
    # when Strategy A would have matched it still does (fast), and when it
    # would have missed we stop wasting 2.5 s before A.5 succeeds.
    try:
        ctrl = session.window.child_window(auto_id=auto_id, found_index=0)
        ctrl.wait("exists", timeout=0.5)
        pipe = ctrl
    except Exception:  # noqa: BLE001
        pass

    # Strategy A.5: raw COM children scan (pipeline c91e7f3a phase-7).
    # Qt6 prefixes AutomationId with the full widget path, e.g.
    # "QApplication.mainWindow.testToolActivatePipe", so exact
    # auto_id= matches in Strategy A/B fail.  Pipe QLineEdits are
    # DIRECT children of QMainWindow (30 immediate children total);
    # scanning only children via IUIAutomation::FindAll(TreeScope_Children)
    # takes ~20 ms and never needs TreeScope_Descendants.  This avoids
    # the FindAll(Descendants) deadlock that occurs when PDFE's Win32
    # message pump is blocked by in-progress tile painting after document
    # open.
    if pipe is None:
        try:
            import comtypes.client  # noqa: PLC0415
            import comtypes.gen.UIAutomationClient as _UIA  # noqa: PLC0415

            _uia = comtypes.client.CreateObject(
                "{ff48dba4-60ef-4201-aa87-54103eef594e}",
                interface=_UIA.IUIAutomation,
            )
            desktop = _uia.GetRootElement()
            # Find PDFE's top-level window among desktop's immediate children.
            # Using TreeScope_Children (not Descendants) for the desktop scan.
            wtype_cond = _uia.CreatePropertyCondition(
                _UIA.UIA_ControlTypePropertyId, 50032  # Window
            )
            desktop_wins = desktop.FindAll(_UIA.TreeScope_Children, wtype_cond)
            pdfe_elem = None
            for _i in range(desktop_wins.Length):
                _w = desktop_wins.GetElement(_i)
                try:
                    if session.pid and _w.CurrentProcessId == session.pid:
                        pdfe_elem = _w
                        break
                except Exception:  # noqa: BLE001
                    pass
            if pdfe_elem is not None:
                true_cond = _uia.CreateTrueCondition()
                children = pdfe_elem.FindAll(_UIA.TreeScope_Children, true_cond)
                for _j in range(children.Length):
                    _c = children.GetElement(_j)
                    try:
                        _aid = _c.CurrentAutomationId or ""
                    except Exception:  # noqa: BLE001
                        continue
                    _suffix = _aid.rsplit(".", 1)[-1] if "." in _aid else _aid
                    if _suffix == auto_id or _aid == auto_id:
                        # Wrap the raw COM element in a pywinauto wrapper
                        # so callers can use set_edit_text / set_value.
                        try:
                            import pywinauto.uia_element_info as _einfo  # noqa: PLC0415
                            import pywinauto.controls.uiawrapper as _wrapper  # noqa: PLC0415
                            _ei = _einfo.UIAElementInfo(_c)
                            ctrl = _wrapper.UIAWrapper(_ei)
                            pipe = ctrl
                        except Exception:  # noqa: BLE001
                            pass
                        break
        except Exception:  # noqa: BLE001
            pass

    # Strategy B: fresh pywinauto Application connection from the live PID.
    # Qt6's UIA AutomationId format is "QApplication.<parent>.<objectName>",
    # so exact child_window(auto_id=...) fails.  We first try exact match
    # (fast path for simple objectNames), then fall back to a suffix-scan
    # on descendants (matches trailing component after the last dot).
    if pipe is None:
        try:
            import pywinauto  # noqa: F811
            fresh_app = pywinauto.Application(backend="uia")
            fresh_app.connect(process=session.pid, timeout=5)
            fresh_win = fresh_app.window(title_re=r".*PDFE.*")
            fresh_win.wait("visible", timeout=5)
            # Try exact match first (fast).
            ctrl = None
            try:
                c = fresh_win.child_window(auto_id=auto_id, found_index=0)
                c.wait("exists", timeout=int(max(1, lookup_timeout)))
                ctrl = c
            except Exception:  # noqa: BLE001
                pass
            # Suffix-match scan if exact match failed.
            if ctrl is None:
                try:
                    for d in fresh_win.descendants():
                        try:
                            aid = d.element_info.automation_id or ""
                        except Exception:  # noqa: BLE001
                            continue
                        suffix = aid.rsplit(".", 1)[-1] if "." in aid else aid
                        if suffix == auto_id:
                            ctrl = d
                            break
                except Exception:  # noqa: BLE001
                    pass
            if ctrl is not None:
                pipe = ctrl
        except Exception:  # noqa: BLE001
            pass

    # Strategy C: descendants scan via the original session window.
    # gap-A14: this is the only strategy that runs UIA FindAll(Descendants) on
    # the original window, which blocks INDEFINITELY on a DEAD window's stale COM
    # handle. Skip it when the process is gone (it cannot own the pipe) so a
    # crashed-mid-run PDFE fast-fails here instead of hanging the whole run; the
    # probe runs only after A/A.5/B all missed, so it adds no cost to the normal
    # alive-and-found path. (The alive-but-UIA-wedged case is handled in a later
    # change, not here.)
    if pipe is None and _process_alive(session):
        pipe = session.find_optional(auto_id=auto_id, timeout=lookup_timeout)

    # Cache the resolved control so subsequent writes skip the enumeration.
    if pipe is not None:
        _PIPE_CTRL_CACHE[_cache_key] = pipe

    return pipe


def _write_to_pipe(session: "PDFESession",
                   auto_id: str,
                   value: str,
                   settle: float = 0.25,
                   lookup_timeout: float = _PIPE_TIMEOUT_S) -> bool:
    """Shared implementation: find a hidden pipe QLineEdit by AutomationId
    and write *value* to it via the UIA ValuePattern.

    Used by all public helpers in this module.  Each pipe's textChanged
    handler inside PDFE dispatches the corresponding action, then clears
    the pipe (blocking its own signal to avoid a second dispatch).

    Pipe location uses the 3-strategy fallback in ``_find_pipe()``:
    Strategy A (fast child_window) → Strategy B (fresh UIA connection) →
    Strategy C (descendants scan).

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    auto_id:
        UIA AutomationId (== Qt objectName) of the target pipe QLineEdit.
    value:
        Non-empty string to write.  An empty string is a no-op on the
        PDFE side (handlers guard ``if (text.isEmpty()) return``).
    settle:
        Seconds to sleep after writing so PDFE's event loop can process
        the textChanged signal before the caller reads back state.
    lookup_timeout:
        Seconds to wait for the pipe widget to appear in the UIA tree.
        Defaults to ``_PIPE_TIMEOUT_S`` (5 s).  Use ``_ANNOT_PIPE_TIMEOUT_S``
        (15 s) for annotation pipes that are searched after long test
        operations that temporarily increase UIA enumeration latency.

    Returns
    -------
    bool
        True on success, False on any failure.
    """
    if not value:
        return False
    # Up to two attempts: if the write through a CACHED control fails (the
    # cached UIA element went stale), drop the cache entry and re-resolve once.
    for _attempt in (1, 2):
        try:
            pipe = _find_pipe(session, auto_id, lookup_timeout)

            if pipe is None:
                return False

            written = False
            for method_name in ("set_edit_text", "set_value"):
                try:
                    getattr(pipe, method_name)(value)
                    written = True
                    break
                except Exception:  # noqa: BLE001
                    continue

            if written:
                time.sleep(settle)
                return True

            # Write failed — the control may be a stale cache hit. Drop it and
            # loop to force a fresh full resolution on the next attempt.
            _invalidate_pipe_cache(session, auto_id)

        except Exception:  # noqa: BLE001
            _invalidate_pipe_cache(session, auto_id)

    return False


def _write_to_pipe_with_retry(session: "PDFESession",
                              auto_id: str,
                              value: str,
                              settle: float = 0.25,
                              lookup_timeout: float = _PIPE_TIMEOUT_S,
                              attempts: int = 3,
                              backoff: float = 0.6) -> bool:
    """``_write_to_pipe`` with retries for a TRANSIENTLY-slow pipe lookup.

    ``_write_to_pipe`` returns False the moment ``_find_pipe`` yields None — a
    lookup timeout. Right after an annotation is created by drag-and-drop, the
    UIA accessibility-event storm can transiently stall ``window.descendants()``
    / FindAll enumeration past ``lookup_timeout`` on the slow CI runner, so a
    single attempt spuriously "can't reach" a pipe that genuinely exists (the
    seam IS enabled — PDFE_TEST_ANNOT_PIPES=1). The pipe is process-lifetime
    stable, so re-looking-up after the storm settles succeeds. Retry with a
    short backoff before giving up, and drop any cache entry between tries.
    """
    for i in range(max(1, attempts)):
        if _write_to_pipe(session, auto_id, value,
                           settle=settle, lookup_timeout=lookup_timeout):
            return True
        _invalidate_pipe_cache(session, auto_id)
        if i < attempts - 1:
            time.sleep(backoff)
    return False


def activate_tool_by_name(session: "PDFESession",
                          action_object_name: str,
                          settle: float = 0.25) -> bool:
    """Write *action_object_name* to the PDFE test-tool-activate pipe.

    PDFE's ``MainWindow::activateToolByObjectName()`` will trigger the
    matching QAction.  The pipe is a zero-sized hidden QLineEdit with
    ``objectName == "testToolActivatePipe"``; it shows up in the UIA tree
    with AutomationId ``testToolActivatePipe`` and supports the Value
    pattern (``set_edit_text``).

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    action_object_name:
        The ``objectName()`` of the QAction to trigger (e.g.
        ``"freeTextToolAction"``).

    Returns
    -------
    bool
        True on success, False on any failure.
    """
    return _write_to_pipe(session, _PIPE_AUTO_ID, action_object_name,
                          settle=settle)


_ACTIVATE_RESULT_PIPE_AUTO_ID = "testToolActivateResultPipe"

# Last acknowledgement sequence number seen per pid, so activate_tool_and_wait
# needs two UIA round trips per call instead of three. Keyed by pid and not by
# session object: a relaunch gets a new pid and therefore a cold entry, which is
# correct, since the application's counter restarts too.
_ACK_SEQ_CACHE: dict = {}


def reset_ack_seq_cache() -> None:
    """Forget the cached sequence numbers.

    Only needed when a session is replaced by one with a RECYCLED pid, which the
    OS can do. Cheap insurance: a stale entry costs one wrong comparison, and
    the fallback path re-reads from the application.
    """
    _ACK_SEQ_CACHE.clear()


def read_tool_activate_ack(session: "PDFESession") -> Optional[str]:
    """Current value of ``testToolActivateResultPipe``, or None if unreadable.

    GitHub #271 / #294. Format: ``"<seq>:<actionObjectName>:<armed|triggered>"``.

    Resolved through ``_find_pipe``, which is cached by ``(pid, auto_id)``, NOT
    through ``session.find_optional`` — that is Strategy C, a full descendants
    scan. Measured: the uncached read cost ~270 ms, so the poll loop below took
    **4.2 s per arming** against the ~0.3 s it takes cached, and the whole seam
    was slower than the 55-314 ms routes GitHub #271 exists to replace. Same
    lesson the create loop in test_ui_stress.py has twice paid for: never put an
    unmemoised UIA resolution inside a thousand-iteration loop.
    """
    try:
        wid = _find_pipe(session, _ACTIVATE_RESULT_PIPE_AUTO_ID)
        if wid is None:
            return None
        return wid.get_value()
    except Exception:  # noqa: BLE001
        _invalidate_pipe_cache(session, _ACTIVATE_RESULT_PIPE_AUTO_ID)
        return None


def activate_tool_and_wait(session: "PDFESession",
                           action_object_name: str,
                           timeout: float = 3.0,
                           require_armed: bool = True) -> bool:
    """Activate a tool and WAIT until PDFE acknowledges it, not merely accepts it.

    GitHub #271, sharpened by #294. ``activate_tool_by_name`` returns as soon as
    the pipe write succeeds, but ``MainWindow::activateToolByObjectName`` defers
    the actual trigger with ``QTimer::singleShot(0, ...)`` — deliberately,
    because many actions open a modal ``QDialog::exec()`` that would block the
    pipe handler and time out the UIA RPC. So a successful write means
    **queued**, not **armed**, and a caller that proceeds on it is racing the
    trigger.

    #294 measured what that race costs. The 1000-annotation stress loop re-arms
    a single-shot tool by clicking the toolbar at fixed coordinates; the click
    does not re-arm at all, so every later canvas click went to ``SelectTool``
    and created nothing. The document ended up with ONE annotation out of 1000
    while the harness's own counters reported 1000 — one counting keystroke
    delivery, the other an unconditional loop index.

    This waits for the SEQUENCE NUMBER TO CHANGE, not for any particular value.
    Reading a matching value is not enough: the previous iteration's
    acknowledgement is still sitting in the pipe, and accepting it is a false
    completion of exactly the kind that hid #294.

    Parameters
    ----------
    session:
        Live ``PDFESession``.
    action_object_name:
        ``objectName()`` of the QAction (e.g. ``"stickyNoteToolAction"``).
    timeout:
        Seconds to wait for the acknowledgement.
    require_armed:
        When True (the default) the acknowledgement must report ``armed``, i.e.
        a tool object is installed on the canvas. Pass False for actions that
        are not tools, where ``triggered`` is the correct outcome.

    Returns
    -------
    bool
        True only if a NEW acknowledgement arrived naming this action and
        satisfying *require_armed*. False on write failure, timeout, a
        wrong-action acknowledgement, or ``triggered`` when ``armed`` was
        required.
    """
    # The baseline sequence number comes from the CACHE, not from a read.
    #
    # Measured: at 1000 annotations this call averaged 283.9 ms, against 44.6 ms
    # on an empty document -- because a UIA round trip gets more expensive as the
    # accessible tree grows, and this function was making THREE of them (a
    # pre-read, the write, the confirming read). The pre-read is pure waste
    # after the first call: we published the previous acknowledgement ourselves,
    # so we already know what it was.
    #
    # Falling back to a read when the cache is cold is deliberate, and it must
    # be a read rather than an assumption of zero: the application may have been
    # running before this process attached, and assuming a fresh counter would
    # make the first call accept a stale acknowledgement -- the exact failure
    # this function exists to prevent.
    cache_key = getattr(session, "pid", id(session))
    if cache_key in _ACK_SEQ_CACHE:
        before_seq = _ACK_SEQ_CACHE[cache_key]
    else:
        before = read_tool_activate_ack(session)
        before_seq = before.split(":", 1)[0] if before else None

    # settle=0 deliberately: the blind 250 ms pause exists to give the deferred
    # trigger time to land, and waiting for the acknowledgement below is a
    # strictly better version of the same thing. Keeping both would make the
    # seam cost MORE than the routes #271 rejected while measuring less.
    if not activate_tool_by_name(session, action_object_name, settle=0.0):
        return False

    deadline = time.time() + timeout
    while time.time() < deadline:
        ack = read_tool_activate_ack(session)
        if ack:
            parts = ack.split(":")
            if len(parts) == 3:
                seq, name, state = parts
                if seq != before_seq:
                    # Record it before judging. The NEXT call's baseline is
                    # this sequence number whatever verdict we reach about its
                    # contents -- a refusal is still a published ack, and
                    # forgetting it would make the following call compare
                    # against a value two acknowledgements old.
                    _ACK_SEQ_CACHE[cache_key] = seq
                    # A fresh acknowledgement. Judge it on its contents.
                    if name != action_object_name:
                        return False
                    if state == "disabled":
                        # A REFUSAL, not silence. PDFE found the action and
                        # declined because it is disabled -- an ordinary state
                        # (no document open, a signature field focused). Worth
                        # its own branch because the caller would otherwise
                        # read a plain False and the full timeout as "the seam
                        # is broken", which is the wrong thing to go and fix.
                        return False
                    if require_armed and state != "armed":
                        return False
                    return True
        time.sleep(0.01)
    return False


def active_tool(session: "PDFESession",
                timeout: float = 5.0) -> Optional[str]:
    """Which tool the application reports as active, or None if unreadable.

    Reads `MainWindow::activeToolDiag()` through the pre-existing
    `testActiveToolQueryPipe` / `testActiveToolResultPipe` pair. The returned
    value is the **checked action's objectName** from `annotToolGroup_` (or
    `treToolAction` / `textEditToolAction` for those two special dispatch
    paths, or `"none"`).

    GitHub #271. This is the discriminator the arming acknowledgement needs and
    did not have. `activate_tool_and_wait` returning True proves a NEW
    acknowledgement arrived saying `armed` -- but `MainWindow` publishes
    `armed` whenever `canvas_->currentTool()` is non-null, and SelectTool is
    itself a tool object, so `armed` alone cannot distinguish "the action I
    asked for is now current" from "some tool is current".

    `activeToolDiag()` can, because a QActionGroup checks the action when it is
    TRIGGERED. So an acknowledgement published at the queueing site -- before
    the deferred `action->trigger()` runs -- would arrive while the previously
    checked action was still checked. Asserting this value immediately after
    `activate_tool_and_wait` returns is therefore a check the queueing site
    could not pass, which is what makes it worth asserting.

    Read directly after the write without polling for a change, deliberately:
    the query handler runs INLINE on `textChanged` and `activeToolDiag()` is
    O(1), so the result pipe already holds the fresh value by the time the
    write returns. (Contrast `undo_repeat`, whose handler had to be deferred
    precisely because it was not O(1) -- it blocked the UIA write that invoked
    it for 64.79 s. The distinction is handler cost, not a stylistic choice.)
    """
    if not _write_to_pipe(session, "testActiveToolQueryPipe", "1", settle=0.0):
        return None
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            wid = _find_pipe(session, "testActiveToolResultPipe")
            if wid is not None:
                val = wid.get_value()
                if val:
                    return val
        except Exception:  # noqa: BLE001
            _invalidate_pipe_cache(session, "testActiveToolResultPipe")
        time.sleep(0.02)
    return None


_UNDO_REPEAT_PIPE_AUTO_ID = "testUndoRepeatPipe"
_UNDO_REPEAT_RESULT_PIPE_AUTO_ID = "testUndoRepeatResultPipe"


def set_undo_depth(session: "PDFESession",
                   depth: int,
                   timeout: float = 20.0) -> Optional[int]:
    """Set the undo stack's depth limit; return the depth set, or None.

    GitHub #271. ``UndoManager::maxDepth_`` defaults to 500 and
    ``UndoManager.h`` documents it as a "configurable depth limit (default
    500)". Measured 2026-10-09: with 1000 annotations created, a request for
    1010 undos performed exactly 500 -- correct behaviour, because the oldest
    500 commands had already been dropped off the stack.

    So the 1000-annotation stress test's oracle ("undo everything, expect 0
    annotations") was IMPOSSIBLE BY DESIGN, and had been since it was written.
    It was never reachable: the create loop produced one annotation until the
    acknowledged re-arm fixed it.

    ``setMaxDepth`` is public, documented, supported API. Raising it for a
    1000-annotation run configures the product rather than weakening the test,
    which keeps the oracle exactly as written.
    """
    return _undo_pipe_call(session, "depth:%d" % depth, timeout)


def undo_repeat(session: "PDFESession",
                count: int,
                redo: bool = False,
                timeout: float = 120.0) -> Optional[int]:
    """Perform up to *count* undos (or redos) and return how many HAPPENED.

    GitHub #271. Replaces driving Ctrl+Z in a Python loop, which was measured
    doing nothing at all: the 1000-annotation stress test fired 1010 Ctrl+Z
    hotkeys through real SendInput and removed ZERO of 1000 annotations, while
    triggering ``undoAction`` through the pipe removed one immediately. The
    keystroke is being swallowed somewhere between SendInput and the QAction.

    The loop could not tell, because it counted keystrokes SENT -- the same
    defect as the unconditional ``created += 1`` that hid #294, one loop further
    down the same test.

    One pipe call rather than N, because a UIA round trip costs ~270 ms with
    1000 annotations in the document and 1010 of them would be 273 s.

    Returns
    -------
    int or None
        The number of operations actually performed, which may legitimately be
        LESS than *count* when the stack runs out -- ask for 1010 against a
        1000-deep stack and 1000 is the correct answer. None if the pipes are
        unreachable or the reply never arrives: a caller must be able to tell
        "nothing to undo" (0) from "I could not find out" (None), because the
        first is a fact about the document and the second is a broken harness.
    """
    return _undo_pipe_call(session,
                           ("redo:%d" % count) if redo else str(count),
                           timeout)


def _undo_pipe_call(session: "PDFESession",
                    spec: str,
                    timeout: float) -> Optional[int]:
    """Write *spec* to the undo pipe and return the integer the app replies.

    Shared by ``undo_repeat`` and ``set_undo_depth``: both write a spec and
    read back a ``"<int>:<tag>"`` reply, and both must distinguish a negative
    reply (the application rejected the spec) and a missing reply (the harness
    could not find out) from a legitimate zero.
    """

    def _read_result() -> Optional[str]:
        try:
            wid = _find_pipe(session, _UNDO_REPEAT_RESULT_PIPE_AUTO_ID)
            return None if wid is None else wid.get_value()
        except Exception:  # noqa: BLE001
            _invalidate_pipe_cache(session, _UNDO_REPEAT_RESULT_PIPE_AUTO_ID)
            return None

    # Key on the reply's SEQUENCE NUMBER, never on the payload changing.
    #
    # GitHub #271, learned the hard way twice. Two consecutive batches
    # legitimately produce the SAME payload: undoing 1000 operations replies
    # "1000:exhausted" and redoing 1000 replies "1000:exhausted" -- identical.
    # A reader comparing values cannot see the second reply arrive, so it polls
    # until timeout and reports "unknown".
    #
    # Measured 2026-10-09: that is exactly what happened. The redo HAD executed
    # (the window title went from "[doc.pdf]" to "[doc.pdf]*" within a second,
    # and no modal appeared), but the stress test reported "the redo-repeat seam
    # gave no reply" and I spent two full runs looking for a crash and a hang in
    # the application before finding the defect in this reader.
    #
    # The arming acknowledgement carries a sequence number for precisely this
    # reason. I reintroduced the defect one pipe over.
    before = _read_result()
    before_seq = before.split(":", 1)[0] if before else None

    if not _write_to_pipe(session, _UNDO_REPEAT_PIPE_AUTO_ID, spec,
                          settle=0.0):
        return None

    # The batch is deferred and runs to completion before replying, so the
    # timeout is in minutes rather than milliseconds.
    deadline = time.time() + timeout
    while time.time() < deadline:
        val = _read_result()
        if val:
            parts = val.split(":")
            # "<seq>:<int>:<tag>"
            if len(parts) >= 3 and parts[0] != before_seq:
                try:
                    n = int(parts[1])
                except ValueError:
                    return None
                return None if n < 0 else n
        time.sleep(0.05)
    return None


def click_by_pdf_coord(session: "PDFESession",
                       page_index: int,
                       pdf_x: float,
                       pdf_y: float,
                       settle: float = 0.35) -> bool:
    """Dispatch ``PdfCanvas::clickByPdfCoord`` via the hidden pipe.

    Writes ``"page_index,pdf_x,pdf_y"`` to the
    ``testClickByPdfCoordPipe`` QLineEdit.  PDFE's textChanged handler
    parses the tuple and synthesises a mouse press+release at the canvas
    position that corresponds to ``(pdf_x, pdf_y)`` on ``page_index``,
    routing through the FormTool hit-test pipeline exactly as a real
    user click would.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    page_index:
        0-based page index.
    pdf_x, pdf_y:
        fitz-space coordinates (origin at top-left, y increases downward).
        Callers must convert from PDF-space (bottom-left origin) using
        fitz_y = page_height - pdf_y.
    settle:
        Seconds to sleep after dispatching the event so PDFE's event
        loop can process the synthesised click before the caller reads
        back the result.

    Returns
    -------
    bool
        True on success, False on any failure.
    """
    payload = f"{page_index},{pdf_x},{pdf_y}"
    try:
        pipe = session.find_optional(auto_id=_CLICK_PIPE_AUTO_ID,
                                     timeout=_PIPE_TIMEOUT_S)
        if pipe is None:
            return False

        written = False
        for method_name in ("set_edit_text", "set_value"):
            try:
                getattr(pipe, method_name)(payload)
                written = True
                break
            except Exception:  # noqa: BLE001
                continue

        if not written:
            return False

        # Pipeline b3f19d72 phase-12 focus restoration:
        # UIA set_edit_text() moves Win32 OS focus to the pipe QLineEdit's
        # native HWND.  PDFE's clickByPdfCoord handler calls
        # canvas->setFocus() (Qt-internal), which sets QApplication::
        # focusWidget() to PdfCanvas but does NOT update Win32 OS focus
        # (because PdfCanvas has no native child HWND — it shares the
        # top-level window's HWND).  Without restoring foreground below,
        # subsequent pyautogui.typewrite() calls (which use SendInput /
        # WM_CHAR) still go to the pipe QLineEdit's native HWND, not to
        # PdfCanvas's keyPressEvent.
        # Calling ensure_foreground on the main HWND moves foreground back
        # to PDFE; on receiving WM_SETFOCUS the Qt main window restores
        # focus to QApplication::focusWidget() = PdfCanvas (as set by
        # canvas->setFocus() inside clickByPdfCoord).
        main_hwnd = getattr(session, "hwnd", 0)
        if main_hwnd and main_hwnd > 0:
            try:
                from tests.helpers.keys import ensure_foreground  # noqa: PLC0415
                ensure_foreground(main_hwnd, timeout=1.5)
            except Exception:  # noqa: BLE001
                pass

        time.sleep(settle)
        return True

    except Exception:  # noqa: BLE001
        return False


def double_click_by_pdf_coord(session: "PDFESession",
                              page_index: int,
                              pdf_x: float,
                              pdf_y: float,
                              settle: float = 0.5) -> bool:
    """Dispatch ``PdfCanvas::doubleClickByPdfCoord`` via the hidden pipe.

    Writes ``"page_index,pdf_x,pdf_y"`` to ``testDoubleClickByPdfCoordPipe``.
    PDFE synthesises a real double-click (press, release, MouseButtonDblClick)
    at the canvas point corresponding to the PDF coordinate, triggering
    ``TextEditTool::onMouseDoubleClick`` word-selection.  Unlike screen-
    fraction double-clicks, this is immune to canvas letterbox margins (the
    gray border around the rendered page), which was the root cause of the
    phase-7 text-edit no-op-edit failures (fraction clicks landed in the
    margin, never on a text span, so nothing was selected and typing was a
    no-op).

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    page_index:
        0-based page index.
    pdf_x, pdf_y:
        fitz-space coordinates (origin top-left, y increases downward) —
        the same space as TextSpan.boundingBox and click_by_pdf_coord.
    settle:
        Seconds to sleep after dispatch so the double-click is processed
        and (because typing usually follows) Win32 foreground is restored
        to PDFE.

    Returns
    -------
    bool
        True on success, False on any failure.
    """
    payload = f"{page_index},{pdf_x},{pdf_y}"
    try:
        # Use the 3-strategy _find_pipe (Strategy A.5 raw-COM children scan
        # locates this direct QMainWindow-child pipe in ~20 ms) instead of
        # session.find_optional, whose Strategy-C-only descendants() full
        # scan costs several seconds per call and — multiplied across the
        # 5-edit undo/redo-chain loops — pushed test_undo_chain_5_steps /
        # test_redo_chain_after_undo toward the 120s per-test cap, where
        # full-suite UIA contention then tipped them over (GAP-A14 hang).
        pipe = _find_pipe(session, _DBLCLICK_PIPE_AUTO_ID, _PIPE_TIMEOUT_S)
        if pipe is None:
            return False

        written = False
        for method_name in ("set_edit_text", "set_value"):
            try:
                getattr(pipe, method_name)(payload)
                written = True
                break
            except Exception:  # noqa: BLE001
                continue
        if not written:
            return False

        # Restore Win32 foreground to PDFE so a subsequent typewrite() goes
        # to the canvas keyPressEvent, not the pipe QLineEdit (same rationale
        # as click_by_pdf_coord).
        main_hwnd = getattr(session, "hwnd", 0)
        if main_hwnd and main_hwnd > 0:
            try:
                from tests.helpers.keys import ensure_foreground  # noqa: PLC0415
                ensure_foreground(main_hwnd, timeout=1.5)
            except Exception:  # noqa: BLE001
                pass

        time.sleep(settle)
        return True

    except Exception:  # noqa: BLE001
        return False


def type_into_text_edit(session: "PDFESession",
                        text: str,
                        settle: float = 0.4) -> bool:
    """Type *text* into the active text-edit tool via the model seam.

    Writes *text* to ``testTextEditTypePipe``; PDFE delivers it to
    ``PdfCanvas::typeIntoTextEdit`` → ``TextEditTool::onKeyPress`` →
    ``onTextInput`` → ``EditTextCommand`` — the exact production keystroke
    path.  This substitutes OS keyboard delivery to the custom-painted
    PdfCanvas widget, which the Win32/UIA harness cannot reliably target
    after a synthetic click (the canvas shares the top-level HWND and has
    no native focus).  A real word-selection / cursor must already be
    active (see double_click_by_pdf_coord).  Returns True on success.
    """
    return _write_to_pipe(session, _TEXTEDIT_TYPE_PIPE_AUTO_ID, text,
                          settle=settle)


def send_key_to_text_edit(session: "PDFESession",
                          qt_key: int,
                          settle: float = 0.4) -> bool:
    """Send a single Qt::Key (e.g. ``QT_KEY_BACKSPACE``) to the active
    text-edit tool via the type seam (payload ``"@KEY:<int>"``).  Drives
    ``PdfCanvas::sendKeyToTextEdit`` → ``TextEditTool::onKeyPress``.
    Returns True on success.
    """
    return _write_to_pipe(session, _TEXTEDIT_TYPE_PIPE_AUTO_ID,
                          f"@KEY:{qt_key}", settle=settle)


def text_edit_selection_diag(session: "PDFESession",
                             settle: float = 0.30) -> dict:
    """Read ``MainWindow::textEditSelectionDiag()`` via the two-pipe seam.

    Returns a dict with keys ``span_count``, ``active_span``, ``sel_start``,
    ``sel_end`` (ints), and ``bboxes`` (list of (x,y,w,h) tuples in PDF
    coords).  ``active_span >= 0`` means a span is currently selected.
    Empty/zeroed dict when the seam is unreachable or no text-edit tool is
    active.  Lets UI tests confirm a double-click actually placed a cursor
    on a real vector-text span before asserting on a typed edit.
    """
    empty = {"span_count": 0, "active_span": -1, "sel_start": -1,
             "sel_end": -1, "bboxes": [], "active_text": "",
             "toolbar_font_size": None, "overlay_font_size": None,
             "selection_run_font_size": None}
    try:
        trig = _find_pipe(session, _TEXTEDIT_DIAG_TRIGGER_PIPE_AUTO_ID,
                          _PIPE_TIMEOUT_S)
        if trig is None:
            return empty
        for m in ("set_edit_text", "set_value"):
            try:
                getattr(trig, m)("1")
                break
            except Exception:  # noqa: BLE001
                continue
        time.sleep(settle)
        raw = _read_result_pipe(session, _TEXTEDIT_DIAG_RESULT_PIPE_AUTO_ID,
                                settle=0.0)
        if not raw:
            return empty
        # Format: "count,active,selStart,selEnd;0:x,y,w,h;...|<activeText>"
        body, _, active_text = raw.partition("|")
        head, _, tail = body.partition(";")
        nums = head.split(",")
        if len(nums) < 4:
            return empty
        out = {
            "span_count": int(nums[0]),
            "active_span": int(nums[1]),
            "sel_start": int(nums[2]),
            "sel_end": int(nums[3]),
            "bboxes": [],
            "active_text": active_text,
            "toolbar_font_size": None,
            "overlay_font_size": None,
            "selection_run_font_size": None,
        }
        if tail:
            for seg in tail.split(";"):
                # Readback fields use "key=value" (e.g. "tbsz=32.00",
                # "ovsz=16.58", "runsz=32.00"); bbox segments use "i:x,y,w,h".
                if seg.startswith("tbsz="):
                    try:
                        out["toolbar_font_size"] = float(seg[len("tbsz="):])
                    except ValueError:
                        pass
                    continue
                if seg.startswith("ovsz="):
                    try:
                        out["overlay_font_size"] = float(seg[len("ovsz="):])
                    except ValueError:
                        pass
                    continue
                if seg.startswith("runsz="):
                    try:
                        out["selection_run_font_size"] = float(seg[len("runsz="):])
                    except ValueError:
                        pass
                    continue
                if ":" not in seg:
                    continue
                _, _, coords = seg.partition(":")
                parts = coords.split(",")
                if len(parts) == 4:
                    try:
                        out["bboxes"].append(tuple(float(p) for p in parts))
                    except ValueError:
                        pass
        return out
    except Exception:  # noqa: BLE001
        return empty


def set_toolbar_font_size(session: "PDFESession",
                          size_pt: float,
                          settle: float = 0.8) -> bool:
    """Drive a TextEditToolBar font-size change deterministically via the
    ``testToolbarFontSizePipe`` seam.

    Writes *size_pt* to the pipe; PDFE's handler calls
    ``TextEditToolBar::setFontSizeForTesting()``, which fires the real
    ``fontSizeChanged`` → ``TextEditTool::applyFontSize`` path (the same one a
    user spinbox edit drives). A real selection must already be active
    (see :func:`_select_word_via_seam` on the test side).

    This replaces a pywinauto UIA set-value gesture on the spinbox, which
    silently no-ops in a non-interactive CI runner session
    (CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #2). Confirm the value
    was applied by reading ``text_edit_selection_diag()["toolbar_font_size"]``
    before asserting the canvas changed. Returns True if the pipe accepted the
    write. Requires PDFE_TEST_PIPES=1 and a pipeline-9a4e2f71 PDFE build.
    """
    return _write_to_pipe(session, _TOOLBAR_FONT_SIZE_PIPE_AUTO_ID,
                          f"{size_pt}", settle=settle)


def text_edit_toolbar_is_visible(session: "PDFESession",
                                 settle: float = 0.25) -> bool | None:
    """Return the TextEditToolBar visibility via the deterministic
    ``testToolbarVisibleQueryPipe`` → ``testToolbarVisibleResultPipe`` seam.

    Returns True/False from ``textEditToolBar_->isVisible()``, or ``None`` if the
    pipe is unreachable. This is ground truth for "is the toolbar shown",
    replacing flaky UIA enumeration of the toolbar's QFontComboBox child, which
    the Qt6 UIA bridge does not reliably expose for a QToolBar on Windows Server
    2019 (CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #1). Requires
    PDFE_TEST_PIPES=1 and a pipeline-9a4e2f71 PDFE build.
    """
    if not _write_to_pipe(session, _TOOLBAR_VISIBLE_QUERY_PIPE_AUTO_ID, "1",
                          settle=settle):
        return None
    raw = _read_result_pipe(session, _TOOLBAR_VISIBLE_RESULT_PIPE_AUTO_ID,
                            settle=0.0)
    if raw is None:
        return None
    return str(raw).strip() == "1"


def customize_toolbars_add_action(session: "PDFESession",
                                  action_object_name: str) -> bool:
    """Trigger ``CustomizeToolbarsDialog::addActionByObjectName`` via test pipe.

    The CustomizeToolbarsDialog must already be open.  Finds the
    ``testCustomizeAddActionPipe`` QLineEdit in the open dialog's UIA tree
    and writes *action_object_name* to it; the textChanged handler inside
    the dialog calls ``addActionByObjectName(action_object_name)``.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    action_object_name:
        The ``objectName()`` of the QAction to add to the currently-selected
        toolbar (e.g. ``"printPreviewAction"``).

    Returns
    -------
    bool
        True on success, False on any failure (pipe not found, write error,
        action not in registry, etc.).
    """
    return _write_to_pipe(session, _CUSTOMIZE_ADD_PIPE_AUTO_ID,
                          action_object_name)


def customize_toolbars_remove_action(session: "PDFESession",
                                     action_object_name: str) -> bool:
    """Trigger ``CustomizeToolbarsDialog::removeActionByObjectName`` via test pipe.

    The CustomizeToolbarsDialog must already be open.  Finds the
    ``testCustomizeRemoveActionPipe`` QLineEdit in the open dialog's UIA tree
    and writes *action_object_name* to it; the textChanged handler inside
    the dialog calls ``removeActionByObjectName(action_object_name)``.

    Pipeline b3d7f942 phase-5 GAP-13.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    action_object_name:
        The ``objectName()`` of the QAction to remove from the
        currently-selected toolbar.

    Returns
    -------
    bool
        True on success, False on any failure (pipe not found, write error,
        action not in current toolbar, etc.).
    """
    return _write_to_pipe(session, _CUSTOMIZE_REMOVE_PIPE_AUTO_ID,
                          action_object_name)


def get_current_annotation_alt_text(session: "PDFESession",
                                     settle: float = 0.30) -> str:
    """Call ``MainWindow::getCurrentAnnotationAltText()`` via the two-pipe seam.

    Writes ``"1"`` to the trigger pipe (``testAltTextReadbackTriggerPipe``),
    which causes PDFE's textChanged handler to invoke
    ``getCurrentAnnotationAltText()`` and write the result into the result pipe
    (``testAltTextReadbackResultPipe``).  This function then reads the result
    pipe via the UIA ValuePattern.

    Requirements:
    - An annotation must already be selected in PDFE (selectedAnnotationPageIndex_
      and selectedAnnotationIndex_ both >= 0).  Use
      ``setSelectedAnnotationForTesting(pageIndex, annotIndex)`` or drive a
      SelectTool click first.
    - The document must be open.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    settle:
        Seconds to sleep after writing the trigger so PDFE's event loop can
        dispatch the textChanged signal and populate the result pipe before
        the caller reads it.

    Returns
    -------
    str
        The alt-text string, or ``""`` if the annotation has no alt text,
        there is no document, or the pipe infrastructure is not reachable.
    """
    try:
        # Strategy A→B→C lookup for the trigger pipe (same fallback chain as
        # _write_to_pipe).  Previously this used Strategy-C-only find_optional,
        # which is vulnerable to stale UIA cache after a prior pdfe.exe session.
        trigger_pipe = _find_pipe(session, _ALT_TEXT_TRIGGER_PIPE_AUTO_ID,
                                   _PIPE_TIMEOUT_S)
        if trigger_pipe is None:
            return ""

        written = False
        for method_name in ("set_edit_text", "set_value"):
            try:
                getattr(trigger_pipe, method_name)("1")
                written = True
                break
            except Exception:  # noqa: BLE001
                continue
        if not written:
            return ""

        time.sleep(settle)

        # Strategy A→B→C lookup for the result pipe.  Previously Strategy-C
        # only.  Using the full chain ensures we don't read from a stale
        # element that was populated by a prior session's result pipe.
        result_pipe = _find_pipe(session, _ALT_TEXT_RESULT_PIPE_AUTO_ID,
                                  _PIPE_TIMEOUT_S)
        if result_pipe is None:
            return ""

        for method_name in ("iface_value", None):
            try:
                if method_name == "iface_value":
                    val = result_pipe.iface_value.CurrentValue
                    return val if val is not None else ""
                else:
                    return result_pipe.window_text() or ""
            except Exception:  # noqa: BLE001
                continue

        return ""

    except Exception:  # noqa: BLE001
        return ""


def comment_panel_row_count(session: "PDFESession",
                            query: str = "*",
                            settle: float = 0.40) -> int:
    """Count comment-panel annotation rows via the model seam.

    Calls ``MainWindow::commentPanelRowCount(query)`` through the two-pipe
    seam: writes ``query`` to ``testCommentInventoryTriggerPipe`` which
    makes PDFE walk the comment QTreeWidget model and write the matching
    row COUNT (a plain integer) into ``testCommentInventoryResultPipe``.

    A count (not a row list) is exchanged because the carrier is a single
    single-line QLineEdit pipe and packing a list into one UIA ValuePattern
    value is lossy (UIA collapses separators).  This is the model-level
    substitute for enumerating the QTreeWidget via UIA ``descendants()``,
    which on Windows Server 2019 Qt6 builds only surfaces viewport rows
    (often none).  Product-side comment-panel refresh wiring is covered by
    the GoogleTest suite (test_comment_panel*.cpp); this seam only replaces
    the flaky read path.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    query:
        ``"*"`` (default) → total annotation-row count.  Any other value →
        count of rows whose column-0 label CONTAINS that token, e.g.
        ``"[Highlight]"`` counts Highlight rows.
    settle:
        Seconds to wait after the trigger so PDFE's event loop can dispatch
        textChanged and populate the result pipe.

    Returns
    -------
    int
        The matching row count, or 0 when the panel is empty or the pipe
        infrastructure is unreachable.
    """
    if not query:
        query = "*"
    try:
        trigger_pipe = _find_pipe(session,
                                  _COMMENT_INVENTORY_TRIGGER_PIPE_AUTO_ID,
                                  _PIPE_TIMEOUT_S)
        if trigger_pipe is None:
            return 0

        written = False
        for method_name in ("set_edit_text", "set_value"):
            try:
                getattr(trigger_pipe, method_name)(query)
                written = True
                break
            except Exception:  # noqa: BLE001
                continue
        if not written:
            return 0

        time.sleep(settle)

        result_pipe = _find_pipe(session,
                                 _COMMENT_INVENTORY_RESULT_PIPE_AUTO_ID,
                                 _PIPE_TIMEOUT_S)
        if result_pipe is None:
            return 0

        raw = None
        for method_name in ("iface_value", None):
            try:
                if method_name == "iface_value":
                    raw = result_pipe.iface_value.CurrentValue
                else:
                    raw = result_pipe.window_text()
                break
            except Exception:  # noqa: BLE001
                continue
        if raw is None:
            return 0
        raw = str(raw).strip()
        try:
            return int(raw)
        except (TypeError, ValueError):
            return 0

    except Exception:  # noqa: BLE001
        return 0


def customize_toolbars_reset_to_defaults(session: "PDFESession") -> bool:
    """Trigger ``CustomizeToolbarsDialog::resetToDefaults`` via test pipe.

    The CustomizeToolbarsDialog must already be open.  Writes a non-empty
    marker to ``testCustomizeResetPipe``; the textChanged handler inside
    the dialog calls ``resetToDefaults()``.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.

    Returns
    -------
    bool
        True on success, False on any failure.
    """
    return _write_to_pipe(session, _CUSTOMIZE_RESET_PIPE_AUTO_ID, "1")


# ---------------------------------------------------------------------------
# Pipeline c91e7f3a phase-3: CustomizeToolbarsDialog list-enumeration pipes
# ---------------------------------------------------------------------------

def _read_result_pipe(session: "PDFESession",
                      result_auto_id: str,
                      settle: float = 0.30) -> str | None:
    """Read the current text of a result pipe via UIA ValuePattern.

    Returns the string value or None on failure.
    """
    try:
        pipe = _find_pipe(session, result_auto_id, lookup_timeout=_PIPE_TIMEOUT_S)
        if pipe is None:
            return None
        time.sleep(settle)
        for method_name in ("iface_value", None):
            try:
                if method_name == "iface_value":
                    val = pipe.iface_value.CurrentValue
                    return val if val is not None else ""
                else:
                    return pipe.window_text() or ""
            except Exception:  # noqa: BLE001
                continue
        return None
    except Exception:  # noqa: BLE001
        return None


def read_dialog_introspection(session: "PDFESession",
                              dialog_object_name: str) -> str | None:
    """Read a modal dialog's recorded inventory via the generic
    introspection seam (TestDialogSeam.h registerDialogIntrospection).

    GAP-A14 / SKIP=FAIL: the record (child-widget objectNames + each named
    QComboBox's item texts) is captured when the dialog opens and PERSISTS in
    a process-static registry after it closes. Read it while MainWindow is
    NON-modal (i.e. after dismissing the dialog) — the query/result pipes are
    MainWindow children that can't receive UIA events while a modal is up.

    Returns the record string
    ``"<obj>|name1,name2,...||combos=Combo:item1;item2@@..."``, the literal
    ``"__null__"`` if no record exists, or None if the pipe is unreachable.
    """
    if not _write_to_pipe(session, "testDialogIntrospectionQueryPipe",
                          dialog_object_name):
        return None
    return _read_result_pipe(session, "testDialogIntrospectionResultPipe")


def dialog_combo_items(session: "PDFESession",
                       dialog_object_name: str,
                       combo_object_name: str) -> list:
    """Return the recorded item texts of a named QComboBox inside a dialog
    whose introspection was captured (UIA-free).

    Parses the ``||combos=`` section of the introspection record. Returns
    ``[]`` if the dialog/combo was not recorded. The dialog must have been
    opened at least once (so registerDialogIntrospection ran) before calling.
    """
    rec = read_dialog_introspection(session, dialog_object_name)
    if not rec or rec == "__null__" or "||combos=" not in rec:
        return []
    combos_part = rec.split("||combos=", 1)[1]
    for entry in combos_part.split("@@"):
        if ":" not in entry:
            continue
        name, items = entry.split(":", 1)
        if name == combo_object_name:
            return [it for it in items.split(";") if it]
    return []


def zoom_combo_current_text(session: "PDFESession") -> str | None:
    """Return the live zoom toolbar combo's current text via the
    testZoomCombo query/result pipe pair (UIA-free readback).

    Pipeline phase-7 (loop-until-clean): the zoom QComboBox lives in a
    QToolBar; on the Windows-Server-2019 CI session the Qt6 UIA bridge does
    not reliably expose a combo-in-toolbar by its AutomationId, so a direct
    find_optional(auto_id="zoomCombo") returns None even though the widget
    exists. MainWindow registers a hidden query/result pipe pair (gated by
    PDFE_TEST_PIPES) whose handler returns ``zoomCombo_->currentText()`` —
    a genuine readback proving the widget exists and is wired.

    Returns the current text (e.g. "100%"), the literal ``"__null__"`` if
    zoomCombo_ is null in PDFE, or None if the pipe is unreachable.
    """
    if not _write_to_pipe(session, "testZoomComboQueryPipe", "1"):
        return None
    return _read_result_pipe(session, "testZoomComboResultPipe")


def inject_file_dialog_path(session: "PDFESession", path: str) -> bool:
    """Pre-seat the path the NEXT QFileDialog save/open should return.

    GAP-A14 / SKIP=FAIL: the native Win32 file modal can't be driven on the
    CI session, so each getSaveFileName/getOpenFileName call site consults
    ``pdfe::ui::testseam::takeFileDialogPath()`` first. Writing here pushes
    *path* onto that FIFO via the ``testFileDialogPathPipe`` MainWindow pipe;
    the very next file dialog returns it without showing a modal.

    Must be called while MainWindow is NOT modal (i.e. before the dialog that
    will consume the path opens), so the pipe receives the UIA value event.
    """
    return _write_to_pipe(session, "testFileDialogPathPipe", path)


def enqueue_dialog_action(session: "PDFESession", spec: str) -> bool:
    """Queue an action script for a modal dialog, replayed when it opens.

    GAP-A14 / SKIP=FAIL. *spec* is pipe-delimited:
    ``"<dialogObjectName>|<verb1>|<verb2>|..."`` where each verb is one of
    ``combo:Name=Value`` / ``edit:Name=Text`` / ``spin:Name=N`` /
    ``check:Name=1`` / ``click:Name`` / ``accept`` / ``reject``. Example::

        enqueue_dialog_action(
            sess,
            "ComplianceDialog|combo:LevelCombo=PDF/A-2b|click:ConvertButton|accept")

    Call BEFORE opening the dialog (while MainWindow is non-modal). When the
    dialog's constructor runs ``registerDialogIntrospection`` it replays the
    script against the real widgets on the dialog's own event loop.
    """
    return _write_to_pipe(session, "testDialogActionPipe", spec)


def attachment_names(session: "PDFESession") -> list:
    """Return the attachment-panel's attachment names (UIA-free).

    Writes ``names`` to ``testAttachmentQueryPipe`` and parses the
    pipe-delimited result. Returns ``[]`` when the panel has no attachments
    or is unreachable.
    """
    if not _write_to_pipe(session, "testAttachmentQueryPipe", "names"):
        return []
    raw = _read_result_pipe(session, "testAttachmentResultPipe")
    if not raw or raw.startswith("err:"):
        return []
    return [p for p in raw.split("|") if p]


def attachment_select(session: "PDFESession", name: str) -> bool:
    """Select the attachment row named *name* in the panel (UIA-free).

    Returns True when the row was found and selected.
    """
    if not _write_to_pipe(session, "testAttachmentQueryPipe", f"select:{name}"):
        return False
    return _read_result_pipe(session, "testAttachmentResultPipe") == "1"


def attachment_trigger(session: "PDFESession", which: str) -> bool:
    """Run the panel's real Add or Extract handler. *which* is ``"add"`` or
    ``"extract"``. Seat the file/dir path via :func:`inject_file_dialog_path`
    first so the handler consumes it instead of showing a native modal.

    Returns True when the handler ran (result == ``"ok"``).
    """
    if which not in ("add", "extract"):
        return False
    if not _write_to_pipe(session, "testAttachmentQueryPipe", which):
        return False
    return _read_result_pipe(session, "testAttachmentResultPipe") == "ok"


def customize_toolbars_available_actions(session: "PDFESession",
                                         settle: float = 0.30) -> list:
    """Return the available-actions list from the open CustomizeToolbarsDialog.

    Writes a query token to ``testCustomizeAvailableActionsQueryPipe``;
    PDFE's handler populates ``testCustomizeAvailableActionsResultPipe``
    with a newline-separated list of action display names.

    The CustomizeToolbarsDialog must already be open.  Returns a list
    of strings (one per available action), or an empty list on failure.
    Requires a pipeline c91e7f3a phase-3 PDFE build.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    settle:
        Seconds to sleep after writing the query so PDFE's event loop
        can populate the result pipe before it is read back.

    Returns
    -------
    list[str]
        Action display-name strings, or ``[]`` on failure.
    """
    ok = _write_to_pipe(session, _CUSTOMIZE_AVAIL_QUERY_PIPE_AUTO_ID,
                        "query", settle=settle)
    if not ok:
        return []
    raw = _read_result_pipe(session, _CUSTOMIZE_AVAIL_RESULT_PIPE_AUTO_ID,
                            settle=0.0)
    if raw is None:
        return []
    # C++ uses '|' as separator because QLineEdit strips '\n'.
    return [x for x in raw.split('|') if x.strip()]


def customize_toolbars_toolbar_contents(session: "PDFESession",
                                        toolbar_name: str = "current",
                                        settle: float = 0.30) -> list:
    """Return the contents of a toolbar from the open CustomizeToolbarsDialog.

    Writes ``toolbar_name`` (or ``"current"`` for the active toolbar) to
    ``testCustomizeToolbarContentsQueryPipe``; PDFE's handler populates
    ``testCustomizeToolbarContentsResultPipe`` with a newline-separated
    list of item labels.

    The CustomizeToolbarsDialog must already be open.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    toolbar_name:
        Display name of the toolbar to query, or ``"current"`` to query
        whichever toolbar is selected in the combo.
    settle:
        Seconds to sleep after writing the query.

    Returns
    -------
    list[str]
        Item label strings (may include ``"— Separator —"`` entries),
        or ``[]`` on failure.
    """
    query = toolbar_name if toolbar_name else "current"
    ok = _write_to_pipe(session, _CUSTOMIZE_CONTENTS_QUERY_PIPE_AUTO_ID,
                        query, settle=settle)
    if not ok:
        return []
    raw = _read_result_pipe(session, _CUSTOMIZE_CONTENTS_RESULT_PIPE_AUTO_ID,
                            settle=0.0)
    if raw is None:
        return []
    # C++ uses '|' as separator because QLineEdit strips '\n'.
    return [x for x in raw.split('|') if x.strip()]


def customize_toolbars_widget_accessible_names(
        session: "PDFESession",
        settle: float = 0.30) -> dict:
    """Return the accessible names of CustomizeToolbarsDialog's widgets.

    Writes a query token to ``testCustomizeWidgetNamesQueryPipe``; PDFE's
    handler populates ``testCustomizeWidgetNamesResultPipe`` with
    newline-separated ``"objectName:accessibleName"`` pairs.

    The CustomizeToolbarsDialog must already be open.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    settle:
        Seconds to sleep after writing the query.

    Returns
    -------
    dict[str, str]
        ``{objectName: accessibleName}`` for all interactive dialog
        widgets, or ``{}`` on failure.
    """
    ok = _write_to_pipe(session, _CUSTOMIZE_WIDGET_NAMES_QUERY_PIPE_AUTO_ID,
                        "query", settle=settle)
    if not ok:
        return {}
    raw = _read_result_pipe(session, _CUSTOMIZE_WIDGET_NAMES_RESULT_PIPE_AUTO_ID,
                            settle=0.0)
    if not raw:
        return {}
    # C++ uses '|' as separator because QLineEdit strips '\n'.
    result: dict = {}
    for line in raw.split('|'):
        if ':' in line:
            obj_name, _, acc_name = line.partition(':')
            result[obj_name.strip()] = acc_name.strip()
    return result


# ---------------------------------------------------------------------------
# Pipeline c91e7f3a phase-3: TextEditToolBar fontComboBox pipes
# ---------------------------------------------------------------------------

def get_text_edit_toolbar_font_families(session: "PDFESession",
                                        settle: float = 0.30) -> list:
    """Return font-family entries from the TextEditToolBar's QFontComboBox.

    Writes a query token to ``testFontFamiliesQueryPipe``; PDFE's handler
    populates ``testFontFamiliesResultPipe`` with a newline-separated list
    where the FIRST LINE is the current selection and subsequent lines are
    all combo entries in order.

    This bypasses the Qt6/Server-2019 UIA gap where QFontComboBox contents
    are not reachable via ``child_window(auto_id="fontComboBox")`` when
    the toolbar is mounted via ``addToolBar()``.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    settle:
        Seconds to sleep after writing the query.

    Returns
    -------
    list[str]
        ``[current_selection, entry0, entry1, ...]`` or ``[]`` on failure.
        Index 0 is the current font family; indices 1..N are all entries
        in the combo's population order.
    """
    ok = _write_to_pipe(session, _FONT_FAMILIES_QUERY_PIPE_AUTO_ID,
                        "query", settle=settle)
    if not ok:
        return []
    raw = _read_result_pipe(session, _FONT_FAMILIES_RESULT_PIPE_AUTO_ID,
                            settle=0.0)
    if raw is None:
        return []
    # C++ uses '|' as separator because QLineEdit strips '\n'.
    return [x for x in raw.split('|') if x.strip()]


def set_text_edit_toolbar_font_family(session: "PDFESession",
                                      family: str,
                                      settle: float = 0.30) -> bool:
    """Set the TextEditToolBar's QFontComboBox to ``family`` via test pipe.

    Writes ``family`` to ``testSetFontFamilyPipe``; PDFE's handler calls
    ``combo->setCurrentText(family)`` and emits ``fontFamilyChanged``,
    propagating the selection exactly as a user interaction would.

    This bypasses the Qt6/Server-2019 UIA gap where QFontComboBox
    contents are not reachable via UIA Value pattern on the combo box
    when the toolbar is mounted via ``addToolBar()``.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    family:
        Font family name to select (e.g. ``"Arial"``).  If the family
        is not in the combo, ``setCurrentText`` will set the text to
        the typed string but the combo's current index may be -1.
    settle:
        Seconds to sleep after writing so PDFE's event loop can process
        the selection and update any dependent state before the caller
        reads back.

    Returns
    -------
    bool
        True on success, False on any failure.
    """
    if not family:
        return False
    return _write_to_pipe(session, _SET_FONT_FAMILY_PIPE_AUTO_ID,
                          family, settle=settle)


# ---------------------------------------------------------------------------
# Phase-14-loop: PreferencesDialog dialog-child pipe helpers
#
# On Windows-Server-2019 pywinauto's UIA descendants()/FindAll blocks
# indefinitely when enumerating the widgets of an OPEN modal QDialog, which
# wedges the whole pytest file (the thread-method timeout cannot interrupt a
# native COM call).  These helpers drive the PreferencesDialog entirely
# through dialog-CHILD QLineEdit pipes, never enumerating the modal via UIA.
#
# The pipes are children of the dialog top-level window (whose title is
# "Preferences", NOT "PDFE"), so we cannot rely on Strategy-B's
# title_re=".*PDFE.*" match.  _find_dialog_child_pipe() below scans EVERY
# top-level window owned by the PDFE process using TreeScope_Children only
# (never TreeScope_Descendants), so it is immune to the modal-enumeration
# hang while still reaching dialog children.
# ---------------------------------------------------------------------------

def _find_dialog_child_pipe(session: "PDFESession",
                            auto_id: str,
                            lookup_timeout: float = _PIPE_TIMEOUT_S):
    """Locate a hidden pipe QLineEdit that is a CHILD of any top-level
    window owned by the PDFE process, using child-scope COM scans only.

    Unlike ``_find_pipe()`` (which scans only the FIRST PID window's
    children, then falls back to descendants() walks that can hang on an
    open modal), this iterates over ALL of the process's top-level windows
    and scans each window's *immediate children* with
    ``TreeScope_Children``.  Qt parents a QDialog's hidden 0x0 QLineEdit
    pipes directly under the dialog window, so a child-scope scan reaches
    them without ever invoking the descendants/FindAll path that deadlocks
    on Server-2019.

    Returns a pywinauto UIAWrapper or None.
    """
    deadline = time.monotonic() + lookup_timeout
    while time.monotonic() < deadline:
        try:
            import comtypes.client  # noqa: PLC0415
            import comtypes.gen.UIAutomationClient as _UIA  # noqa: PLC0415
            import pywinauto.uia_element_info as _einfo  # noqa: PLC0415
            import pywinauto.controls.uiawrapper as _wrapper  # noqa: PLC0415

            _uia = comtypes.client.CreateObject(
                "{ff48dba4-60ef-4201-aa87-54103eef594e}",
                interface=_UIA.IUIAutomation,
            )
            desktop = _uia.GetRootElement()
            wtype_cond = _uia.CreatePropertyCondition(
                _UIA.UIA_ControlTypePropertyId, 50032  # Window
            )
            # All top-level windows (child scope of the desktop).
            wins = desktop.FindAll(_UIA.TreeScope_Children, wtype_cond)
            true_cond = _uia.CreateTrueCondition()
            for _i in range(wins.Length):
                _w = wins.GetElement(_i)
                try:
                    if not session.pid or _w.CurrentProcessId != session.pid:
                        continue
                except Exception:  # noqa: BLE001
                    continue
                # Scan THIS window's immediate children only.
                try:
                    children = _w.FindAll(_UIA.TreeScope_Children, true_cond)
                except Exception:  # noqa: BLE001
                    continue
                for _j in range(children.Length):
                    _c = children.GetElement(_j)
                    try:
                        _aid = _c.CurrentAutomationId or ""
                    except Exception:  # noqa: BLE001
                        continue
                    _suffix = _aid.rsplit(".", 1)[-1] if "." in _aid else _aid
                    if _suffix == auto_id or _aid == auto_id:
                        try:
                            _ei = _einfo.UIAElementInfo(_c)
                            return _wrapper.UIAWrapper(_ei)
                        except Exception:  # noqa: BLE001
                            return None
                # Fallback: the pipe may be nested below the dialog window
                # (e.g. under a layout container) rather than a direct child.
                # A TreeScope_Subtree scan rooted at THIS PID window is safe:
                # the modal-enumeration deadlock is specific to enumerating
                # from the BLOCKED main window into the modal; enumerating a
                # window's OWN subtree (including the dialog itself) returns.
                try:
                    sub = _w.FindAll(_UIA.TreeScope_Subtree, true_cond)
                except Exception:  # noqa: BLE001
                    sub = None
                if sub is not None:
                    for _k in range(sub.Length):
                        _c = sub.GetElement(_k)
                        try:
                            _aid = _c.CurrentAutomationId or ""
                        except Exception:  # noqa: BLE001
                            continue
                        _suffix = (_aid.rsplit(".", 1)[-1]
                                   if "." in _aid else _aid)
                        if _suffix == auto_id or _aid == auto_id:
                            try:
                                _ei = _einfo.UIAElementInfo(_c)
                                return _wrapper.UIAWrapper(_ei)
                            except Exception:  # noqa: BLE001
                                return None
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.25)
    return None


def _write_dialog_child_pipe(session: "PDFESession",
                             auto_id: str,
                             value: str,
                             settle: float = 0.30) -> bool:
    """Write *value* to a dialog-child pipe located via child-scope scan."""
    if not value:
        return False
    pipe = _find_dialog_child_pipe(session, auto_id)
    if pipe is None:
        return False
    for method_name in ("set_edit_text", "set_value"):
        try:
            getattr(pipe, method_name)(value)
            time.sleep(settle)
            return True
        except Exception:  # noqa: BLE001
            continue
    return False


def _read_dialog_child_pipe(session: "PDFESession",
                            auto_id: str,
                            settle: float = 0.0):
    """Read a dialog-child result pipe's current value (or None)."""
    pipe = _find_dialog_child_pipe(session, auto_id)
    if pipe is None:
        return None
    if settle:
        time.sleep(settle)
    for method_name in ("iface_value", None):
        try:
            if method_name == "iface_value":
                val = pipe.iface_value.CurrentValue
                return val if val is not None else ""
            return pipe.window_text() or ""
        except Exception:  # noqa: BLE001
            continue
    return None


def preferences_dialog_is_open(session: "PDFESession",
                               lookup_timeout: float = _PIPE_TIMEOUT_S) -> bool:
    """Return True if the PreferencesDialog is open and its test pipes are
    reachable (the accept pipe exists as a dialog child)."""
    return _find_dialog_child_pipe(
        session, _PREF_ACCEPT_PIPE_AUTO_ID, lookup_timeout) is not None


def preferences_widget_inventory(session: "PDFESession",
                                 settle: float = 0.30) -> dict:
    """Return ``{objectName: {"acc": accessibleName, "kind": kind}}`` for
    every interactive PreferencesDialog widget.

    Drives the ``testPrefWidgetNamesQueryPipe`` → ``...ResultPipe`` seam
    (C++ ``PreferencesDialog::widgetInventory``).  The dialog must already
    be open.  Returns ``{}`` on failure.
    """
    ok = _write_dialog_child_pipe(
        session, _PREF_WIDGET_NAMES_QUERY_PIPE_AUTO_ID, "query", settle=settle)
    if not ok:
        return {}
    raw = _read_dialog_child_pipe(
        session, _PREF_WIDGET_NAMES_RESULT_PIPE_AUTO_ID)
    if not raw:
        return {}
    # The record is "<widget recs>||tabs=Tab1;Tab2;...". Strip the tab
    # suffix before splitting widget records on '|'.
    widget_part = raw.split("||tabs=", 1)[0]
    result: dict = {}
    for rec in widget_part.split('|'):
        parts = rec.split(':')
        if len(parts) >= 3:
            obj_name = parts[0].strip()
            acc_name = parts[1].strip()
            kind = parts[2].strip()
            if obj_name:
                result[obj_name] = {"acc": acc_name, "kind": kind}
    return result


def preferences_tab_labels(session: "PDFESession",
                           settle: float = 0.30) -> list:
    """Return the PreferencesDialog tab labels (e.g. ``["General", "View",
    "Security", "OCR", "Advanced"]``) via the widget-inventory seam's
    ``||tabs=`` suffix.  The dialog must already be open.  Returns ``[]``
    on failure."""
    ok = _write_dialog_child_pipe(
        session, _PREF_WIDGET_NAMES_QUERY_PIPE_AUTO_ID, "query", settle=settle)
    if not ok:
        return []
    raw = _read_dialog_child_pipe(
        session, _PREF_WIDGET_NAMES_RESULT_PIPE_AUTO_ID)
    if not raw or "||tabs=" not in raw:
        return []
    tabs_part = raw.split("||tabs=", 1)[1]
    return [t for t in tabs_part.split(';') if t.strip()]


def preferences_get_widget_value(session: "PDFESession",
                                 object_name: str,
                                 settle: float = 0.30):
    """Return the current value of the named PreferencesDialog widget.

    Drives the ``testPrefGetValueQueryPipe`` → ``...ResultPipe`` seam.
    Spin/dspin → numeric string; checkbox → "true"/"false"; line edit →
    text; combo → "index@@text" (caller splits on "@@").  Returns the raw
    string, ``"__notfound__"`` if the widget is absent, or ``None`` if the
    pipe is unreachable.
    """
    ok = _write_dialog_child_pipe(
        session, _PREF_GET_VALUE_QUERY_PIPE_AUTO_ID, object_name,
        settle=settle)
    if not ok:
        return None
    return _read_dialog_child_pipe(
        session, _PREF_GET_VALUE_RESULT_PIPE_AUTO_ID)


def preferences_set_widget_value(session: "PDFESession",
                                 object_name: str,
                                 value: str,
                                 settle: float = 0.30) -> bool:
    """Set the named PreferencesDialog widget to ``value`` via the
    ``testPrefSetValuePipe`` seam (payload ``"objectName=value"``).

    The C++ handler writes "ok"/"fail" into the get-value result pipe;
    this function reads that back and returns the corresponding bool.
    """
    payload = f"{object_name}={value}"
    ok = _write_dialog_child_pipe(
        session, _PREF_SET_VALUE_PIPE_AUTO_ID, payload, settle=settle)
    if not ok:
        return False
    reply = _read_dialog_child_pipe(
        session, _PREF_GET_VALUE_RESULT_PIPE_AUTO_ID)
    return (reply or "").strip().lower() == "ok"


def preferences_accept(session: "PDFESession", settle: float = 0.50) -> bool:
    """Accept (OK) the open PreferencesDialog via the ``testPrefAcceptPipe``
    seam.  Routes through the same ``accept()`` slot the OK button fires,
    so the production QSettings-persistence path is exercised."""
    return _write_dialog_child_pipe(
        session, _PREF_ACCEPT_PIPE_AUTO_ID, "1", settle=settle)


def preferences_reject(session: "PDFESession", settle: float = 0.50) -> bool:
    """Reject (Cancel) the open PreferencesDialog via the
    ``testPrefRejectPipe`` seam."""
    return _write_dialog_child_pipe(
        session, _PREF_REJECT_PIPE_AUTO_ID, "1", settle=settle)


# ---------------------------------------------------------------------------
# GAP-A14 keyboard-state seam: generic dialog-child keyboard query helpers.
#
# Every dialog instrumented with
# ``pdfe::ui::testseam::registerDialogIntrospection(this)`` carries a
# dialog-CHILD QLineEdit query/result pipe pair
# (``testDialogKeyboardQueryPipe`` / ``testDialogKeyboardResultPipe``) created
# when PDFE_TEST_PIPES is set.  These pipes receive UIA ValuePattern events
# WHILE the modal dialog is up (a MainWindow-child pipe would not), so the
# keyboard tests can verify modal open-state, focus traversal, and
# default-button activation WITHOUT enumerating the modal via UIA
# descendants()/FindAll (which hangs on the Server-2019 CI session).
# ---------------------------------------------------------------------------

_DIALOG_KEYBOARD_QUERY_PIPE_AUTO_ID = "testDialogKeyboardQueryPipe"
_DIALOG_KEYBOARD_RESULT_PIPE_AUTO_ID = "testDialogKeyboardResultPipe"


def dialog_keyboard_query(session: "PDFESession",
                          command: str,
                          settle: float = 0.30) -> str:
    """Send a keyboard-seam *command* to the open modal dialog and return
    the result string.

    *command* is one of:
      ``"focus"`` -> objectName of the currently-focused widget (walked up
                     to the first ancestor with a non-empty objectName, else
                     className, else "none").
      ``"modal"`` -> "1" if this dialog is the active modal widget, else "0".
      ``"tab"``   -> advance focus via QDialog::focusNextChild() and return
                     the new focused objectName (same resolution as focus).
      ``"enter"`` -> click the dialog's default QPushButton; returns
                     "clicked:<objectName>" or "nodefault".

    Returns the result string, or ``""`` if the pipe is unreachable (dialog
    not open, or PDFE not built with the keyboard seam).
    """
    if not command:
        return ""
    ok = _write_dialog_child_pipe(
        session, _DIALOG_KEYBOARD_QUERY_PIPE_AUTO_ID, command, settle=settle)
    if not ok:
        return ""
    raw = _read_dialog_child_pipe(
        session, _DIALOG_KEYBOARD_RESULT_PIPE_AUTO_ID)
    return (raw or "") if raw is not None else ""


def dialog_focused_widget(session: "PDFESession") -> str:
    """Return the objectName of the currently-focused widget in the open
    modal dialog (or className / "none")."""
    return dialog_keyboard_query(session, "focus")


def dialog_is_modal(session: "PDFESession") -> str:
    """Return "1" if the instrumented dialog is the active modal widget,
    else "0" (or "" if the seam is unreachable)."""
    return dialog_keyboard_query(session, "modal")


def dialog_tab_advance(session: "PDFESession") -> str:
    """Advance focus one step in the dialog's real focus/tab chain and return
    the new focused widget's objectName."""
    return dialog_keyboard_query(session, "tab")


def dialog_focusable_controls(session: "PDFESession") -> list:
    """Return the objectNames of every control in the open modal dialog that
    the user can put keyboard focus on -- the set a full Tab cycle OUGHT to
    visit exactly.

    GitHub #80. Membership is decided in C++ (TestDialogSeam.h, ``focusable``
    command) by ``focusPolicy() != Qt::NoFocus`` plus visible-and-enabled --
    deliberately NOT by ``Qt::TabFocus``. A control focusable by click but
    skipped by Tab is a keyboard-accessibility defect; defining the expected
    set by TabFocus would drop such a control from the expectation at the very
    moment it vanished from the observation, so the comparison would agree with
    itself and see nothing.

    Names are resolved the same way ``dialog_focused_widget`` /
    ``dialog_tab_advance`` resolve them (nearest ancestor with an
    application-assigned objectName, skipping Qt's internal ``qt_*`` names), so
    the two lists are directly comparable as sets.

    Raises AssertionError if the seam is unreachable -- an empty list must not
    be mistaken for "this dialog has no focusable controls", which would make a
    set-equality assertion trivially true.
    """
    reply = dialog_keyboard_query(session, "focusable")
    if not reply.startswith("focusable:"):
        raise AssertionError(
            "dialog 'focusable' seam unreachable (reply=%r). The dialog may "
            "not be open, or pdfe.exe predates the focusable command -- "
            "rebuild it. An empty expected set would make set-equality "
            "vacuous, so this is a hard error." % (reply,))
    return [n for n in reply[len("focusable:"):].split(",") if n]


def dialog_widget_text(session: "PDFESession", object_name: str) -> str:
    """Return the TEXT of a named widget inside the open modal dialog.

    The introspection registry answers "which widgets exist"; this answers
    "what do they say". That is the difference between an oracle proving a
    dialog OPENED and one proving it opened with the RIGHT CONTENT — the
    distinction that made the region-OCR test unable to discriminate
    ``page_rect_on_canvas`` from a canvas-fraction guess, since both produce
    a dialog containing *some* text.

    Handles QTextEdit / QPlainTextEdit / QLineEdit / QLabel / QAbstractButton.
    Newlines arrive escaped as a literal ``\\n`` because the transport is a
    single-line QLineEdit read through UIA ValuePattern; they are unescaped
    here so callers see the real string.

    Raises AssertionError if the widget is absent or carries no text accessor.
    Those are DISTINCT failures from "the widget is empty", and conflating
    them would let a typo in *object_name* read as an empty OCR result — a
    silent false PASS in exactly the assertion this exists to support.
    """
    reply = dialog_keyboard_query(session, "text:%s" % object_name)
    if reply.startswith("text:"):
        return reply[5:].replace("\\n", "\n")
    if reply.startswith("nowidget:"):
        raise AssertionError(
            "no widget named %r in the open dialog -- check the objectName; "
            "an absent widget must not be mistaken for empty text"
            % object_name)
    if reply.startswith("notext:"):
        raise AssertionError(
            "widget %r exists but has no text accessor (not a QTextEdit / "
            "QPlainTextEdit / QLineEdit / QLabel / QAbstractButton)"
            % object_name)
    raise AssertionError(
        "dialog text seam unreachable for %r (reply=%r). The dialog may not "
        "be open, or pdfe.exe predates the text: command -- rebuild it."
        % (object_name, reply))


def dialog_press_default(session: "PDFESession") -> str:
    """Click the dialog's default QPushButton; returns
    "clicked:<objectName>" or "nodefault"."""
    return dialog_keyboard_query(session, "enter")


# ---------------------------------------------------------------------------
# Pipeline f2a8c3e1 phase-2: annotation property pipe helpers
# ---------------------------------------------------------------------------
# ROOT CAUSE (P11-UIA-FLAKE-1): the four flaky annotation-property tests
# depended on two UIA paths that race against the Qt event loop:
#
# (A) QColorDialog UIA race: ``wait_for_top_level_window("Select Color")``
#     polls the Windows accessibility tree, but QColorDialog's HWND is not
#     registered in the UIA tree until one full Qt event-loop tick after
#     ``QColorDialog::getColor()`` opens it.  The ``descendants(control_type=
#     "Edit")`` walk then traverses a COM UIA cache that can contain stale
#     elements from a prior session's color dialog.  Specific racing call:
#     ``session.wait_for_top_level_window("Select Color", timeout=3)`` in
#     ``_drive_color_dialog_pick_different()`` (test_ui_annotations.py:1082).
#
# (B) QLineEdit/QTextEdit UIA ValuePattern race: ``edit.set_edit_text(text)``
#     triggers the Qt UIA provider to call ``QLineEdit::setText()`` /
#     ``QTextEdit::setPlainText()``, emitting ``textChanged``.  But the signal
#     delivery to ``onAnnotationPropertiesChanged()`` is not synchronous with
#     the UIA call — the Qt event loop processes it on the next iteration.
#     When the test immediately reads back the field value, the signal may not
#     yet have fired, so the author/contents command is never pushed to undo.
#     Specific racing call: ``edit.set_edit_text(target_author)`` in
#     ``test_properties_panel_author_field_persists`` (line ~2717) and
#     ``test_properties_panel_contents_field_persists`` (line ~2816).
#
# FIX: three hidden QLineEdit pipes in MainWindow (gated by
# PDFE_TEST_ANNOT_PIPES=1) drive the annotation properties directly via
# ``onAnnotationPropertiesChanged()`` — the same synchronous code path that
# the UI uses after a user interaction.  No dialog, no UIA ValuePattern on
# QLineEdit/QTextEdit, no event-loop timing dependency.
# ---------------------------------------------------------------------------

def select_annot(session: "PDFESession",
                 page_index: int,
                 annot_index: int,
                 settle: float = 0.35) -> bool:
    """Select an annotation by index via the testAnnotSelectPipe seam.

    Writes ``"pageIndex,annotIndex"`` to ``testAnnotSelectPipe``.  PDFE's
    textChanged handler sets ``selectedAnnotationPageIndex_`` and
    ``selectedAnnotationIndex_``, reads fresh annotation data from
    ``annotationManager_``, populates ``annotationProperties_``, and raises
    the Annotation Properties dock — exactly what a SelectTool canvas click
    would do, but without the non-deterministic hit-test.

    Root cause addressed: SelectTool hit-test is unreliable for text-markup
    annotations (highlights/strikethrough/underline) because the annotation's
    bounding rect in page-space maps to a very thin canvas strip (the text line
    height, typically 10–15 canvas pixels at fit-page zoom).  A pixel-fraction
    canvas click at y=0.20 sometimes lands outside this strip depending on
    render state, leaving ``selectedAnnotationPageIndex_`` = -1 and causing
    the subsequent ``set_annot_color()`` call to silently no-op.  Using the
    select pipe provides exact index-based selection regardless of canvas geometry.

    Requirements:
    - PDFE launched with ``PDFE_TEST_ANNOT_PIPES=1`` in its environment.
    - A document with at least ``page_index + 1`` pages must be open.
    - The annotation at ``annot_index`` on that page must exist.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    page_index:
        0-based page index.
    annot_index:
        0-based annotation index within the page.
    settle:
        Seconds to sleep after writing so PDFE's event loop can process
        the selection and update the UIA tree before the caller proceeds.

    Returns
    -------
    bool
        True on success, False if the pipe is unreachable.
    """
    payload = f"{int(page_index)},{int(annot_index)}"
    # Retry: this is fired right after drag-creating an annotation, whose UIA
    # event storm can transiently stall the pipe lookup on slow CI (the seam IS
    # enabled). See _write_to_pipe_with_retry.
    return _write_to_pipe_with_retry(session, _ANNOT_SELECT_PIPE_AUTO_ID,
                                     payload, settle=settle,
                                     lookup_timeout=_ANNOT_PIPE_TIMEOUT_S)


def create_annot(session: "PDFESession",
                 annot_type: str,
                 page_index: int,
                 x: float, y: float, w: float, h: float,
                 settle: float = 0.6) -> bool:
    """Create an annotation deterministically via the testAnnotCreatePipe seam.

    Writes ``"type,page,x,y,w,h"`` (fitz points) to ``testAnnotCreatePipe``.
    PDFE's textChanged handler runs the same ``CreateAnnotationCommand`` path a
    tool create takes (``MainWindow::createAnnotationForTest``): commits the
    annotation, refreshes the comment panel + canvas, and updates the title.

    Root cause addressed: a synthetic canvas click does NOT reliably COMMIT an
    annotation create (only a real OS-mouse drag commits, and that is
    timing-fragile) — the flake in ``test_sticky_note_appears_in_comment_panel``
    (``before==after==0``). This seam removes the create race entirely; the
    caller keeps its own model-seam oracle (e.g. the comment-tree row count).

    ``annot_type`` ∈ Text (sticky note) / FreeText / Highlight / Underline /
    StrikeOut / Square / Circle / Line / Ink / Stamp.

    Requirements: PDFE launched with ``PDFE_TEST_ANNOT_PIPES=1``; a document with
    at least ``page_index + 1`` pages open.

    Returns True on success, False if the pipe is unreachable (PDFE not built
    with the seam, or ``PDFE_TEST_ANNOT_PIPES`` not set).
    """
    payload = f"{annot_type},{int(page_index)},{x},{y},{w},{h}"
    return _write_to_pipe(session, _ANNOT_CREATE_PIPE_AUTO_ID,
                          payload, settle=settle,
                          lookup_timeout=_ANNOT_PIPE_TIMEOUT_S)


def delete_selected_annot(session: "PDFESession",
                          settle: float = 0.5) -> bool:
    """Delete the currently-selected annotation via the testAnnotDeletePipe seam.

    Writes a non-empty token to ``testAnnotDeletePipe``; PDFE's handler calls
    ``deleteSelectedAnnotation()`` (the same command path the UI uses). Pair with
    ``select_annot(session, page, index)`` for a fully position-independent,
    race-free delete — no synthetic canvas click / Delete-key delivery.

    Returns True on success, False if the pipe is unreachable.
    """
    return _write_to_pipe(session, _ANNOT_DELETE_PIPE_AUTO_ID,
                          "1", settle=settle,
                          lookup_timeout=_ANNOT_PIPE_TIMEOUT_S)


def canvas_drag(session: "PDFESession",
                page_index: int,
                x1: float, y1: float, x2: float, y2: float,
                settle: float = 0.8) -> bool:
    """Drag on the canvas via the testCanvasDragPipe seam (fitz points).

    Writes ``"page,x1,y1,x2,y2"`` to ``testCanvasDragPipe``; PDFE synthesizes a
    press→moves→release QMouseEvent sequence through ``PdfCanvas::dragByPdfCoord``
    (the same event path a real drag takes, but with deterministic timing). Used
    to MOVE a selected annotation by dragging its body from (x1,y1) to (x2,y2)
    without a real OS-mouse drag.

    Returns True on success, False if the pipe is unreachable.
    """
    payload = f"{int(page_index)},{x1},{y1},{x2},{y2}"
    return _write_to_pipe(session, _CANVAS_DRAG_PIPE_AUTO_ID,
                          payload, settle=settle,
                          lookup_timeout=_ANNOT_PIPE_TIMEOUT_S)


def set_annot_color(session: "PDFESession",
                    r: int, g: int, b: int,
                    settle: float = 0.35) -> bool:
    """Apply RGB color directly to the selected annotation via the color pipe.

    Writes ``"R,G,B"`` (e.g. ``"0,0,255"``) to ``testAnnotColorPipe``.
    PDFE's textChanged handler parses the triple, patches the selected
    annotation's color, and calls ``onAnnotationPropertiesChanged()`` — the
    same code path as clicking the annotColorButton and accepting the
    QColorDialog, but without opening the dialog.

    Root cause addressed: QColorDialog UIA non-determinism (P11-UIA-FLAKE-1
    case A).  ``wait_for_top_level_window`` races the Qt event loop;
    ``descendants(control_type="Edit")`` returns stale COM UIA cache objects
    from prior test sessions.

    Requirements:
    - PDFE launched with ``PDFE_TEST_ANNOT_PIPES=1`` in its environment.
    - An annotation must already be model-selected (SelectTool click fired).

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    r, g, b:
        Integer RGB components in [0, 255].
    settle:
        Seconds to sleep after writing so PDFE's event loop can process
        the textChanged signal and refresh the canvas tile.

    Returns
    -------
    bool
        True on success, False if the pipe is unreachable (PDFE not built
        with the seam, or ``PDFE_TEST_ANNOT_PIPES`` not set).
    """
    payload = f"{int(r)},{int(g)},{int(b)}"
    # Retry for the same post-annotation-creation UIA-storm lookup stall.
    return _write_to_pipe_with_retry(session, _ANNOT_COLOR_PIPE_AUTO_ID,
                                     payload, settle=settle,
                                     lookup_timeout=_ANNOT_PIPE_TIMEOUT_S)


def set_annot_author(session: "PDFESession",
                     author: str,
                     settle: float = 0.35) -> bool:
    """Apply author string directly to the selected annotation via the author pipe.

    Writes *author* to ``testAnnotAuthorPipe``.  PDFE's textChanged handler
    patches the selected annotation's author field and calls
    ``onAnnotationPropertiesChanged()`` — same code path as editing the
    ``annotAuthorEdit`` QLineEdit, but without depending on UIA ValuePattern
    delivery timing.

    Root cause addressed: QLineEdit textChanged signal delivery race
    (P11-UIA-FLAKE-1 case B).  ``edit.set_edit_text()`` invokes the UIA
    ValuePattern, which calls ``QLineEdit::setText()`` asynchronously via the
    Qt UIA bridge; ``onAnnotationPropertiesChanged()`` may not have run by the
    time the test proceeds to save the document.

    Requirements:
    - PDFE launched with ``PDFE_TEST_ANNOT_PIPES=1`` in its environment.
    - An annotation must already be model-selected.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    author:
        Non-empty author string to set.
    settle:
        Seconds to sleep after writing.

    Returns
    -------
    bool
        True on success, False on any failure.
    """
    if not author:
        return False
    return _write_to_pipe(session, _ANNOT_AUTHOR_PIPE_AUTO_ID,
                          author, settle=settle,
                          lookup_timeout=_ANNOT_PIPE_TIMEOUT_S)


def set_annot_contents(session: "PDFESession",
                       contents: str,
                       settle: float = 0.35) -> bool:
    """Apply contents string directly to the selected annotation via the contents pipe.

    Writes *contents* to ``testAnnotContentsPipe``.  PDFE's textChanged
    handler patches the selected annotation's contents field and calls
    ``onAnnotationPropertiesChanged()`` — same code path as editing
    ``annotContentsEdit`` QTextEdit, but without depending on UIA ValuePattern.

    Root cause addressed: same as ``set_annot_author`` (P11-UIA-FLAKE-1 case B).

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    contents:
        Non-empty contents string to set.
    settle:
        Seconds to sleep after writing.

    Returns
    -------
    bool
        True on success, False on any failure.
    """
    if not contents:
        return False
    return _write_to_pipe(session, _ANNOT_CONTENTS_PIPE_AUTO_ID,
                          contents, settle=settle,
                          lookup_timeout=_ANNOT_PIPE_TIMEOUT_S)


# ---------------------------------------------------------------------------
# Pipeline 3a9f1c72 phase-3: generic Strategy-B fallback for non-pipe lookups
# ---------------------------------------------------------------------------

def find_with_strategy_b(session: "PDFESession",
                         *,
                         auto_id: str | None = None,
                         name: str | None = None,
                         timeout: float = 5.0):
    """Locate a UIA widget using a Strategy-A → Strategy-B fallback.

    For non-pipe widgets (e.g. a QListView, QComboBox, custom control) that
    live permanently in the PDFE window — not hidden pipes written to via
    textChanged — the stale-UIA-cache root cause (P-G-INFRA-6, -10) can still
    cause ``find_optional()`` to return ``None`` even when the widget is alive
    in the real process.  This helper wraps the same Strategy-A/B pattern used
    by ``_find_pipe()`` without Strategy-C's full descendants scan (which is
    only needed for pipe QLineEdits that may have a Qt full-path prefix).

    Strategy A — ``session.find_optional(auto_id=..., name=...)``:
        Uses the existing session's UIA cache.  Fast and sufficient in the
        common case.  May return ``None`` when the cache retains handles from a
        previously-killed pdfe.exe and the new process's widget hasn't flushed
        the old entries.

    Strategy B — ``pywinauto.Application(backend="uia").connect(process=pid)``:
        Opens a fresh IUIAutomation client rooted at the live PID.  Bypasses
        the stale cache entirely.  ``child_window()`` + ``wait("exists")`` is
        used rather than a full descendants scan, which is fast enough for
        top-level children of the main PDFE window.

    Callers in test files should switch from bare ``session.find_optional()``
    to this helper when the widget is known to be flaky under the stale-cache
    failure mode (e.g. after an in-process restart or a prior test that killed
    and relaunched PDFE).

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    auto_id:
        UIA AutomationId (== Qt objectName) to match, or ``None``.
    name:
        UIA Name property to match, or ``None``.  At least one of *auto_id*
        or *name* must be provided.
    timeout:
        Total seconds budget.  Strategy A gets ``timeout / 3`` seconds;
        Strategy B gets the remaining ``timeout * 2 / 3`` seconds.

    Returns
    -------
    pywinauto control or None
        The located widget, or ``None`` if both strategies fail.

    Examples
    --------
    # P-G-INFRA-6: QListView in CustomizeToolbarsDialog
    available_list = find_with_strategy_b(session, auto_id="availableActionsList")

    # P-G-INFRA-10: zoom combo in main toolbar
    zoom_combo = find_with_strategy_b(session, auto_id="zoomCombo", timeout=8.0)
    """
    if auto_id is None and name is None:
        raise ValueError("find_with_strategy_b: at least one of auto_id or name must be provided")

    a_timeout = max(1.0, timeout / 3)
    b_timeout = max(1.0, timeout * 2 / 3)

    # Strategy A: use the existing session's find_optional (descendants scan
    # with cached UIA client).  Quick pass with a reduced timeout so Strategy B
    # gets a meaningful slice of the total budget.
    widget = session.find_optional(auto_id=auto_id, name=name, timeout=a_timeout)
    if widget is not None:
        return widget

    # Strategy B: fresh pywinauto Application connection from the live PID.
    # Opens a new IUIAutomation client, discarding stale COM element handles
    # from any previously-killed pdfe.exe that share the same AutomationId.
    try:
        import pywinauto  # noqa: F811
        fresh_app = pywinauto.Application(backend="uia")
        fresh_app.connect(process=session.pid, timeout=5)
        fresh_win = fresh_app.window(title_re=r".*PDFE.*")
        fresh_win.wait("visible", timeout=5)
        kwargs: dict = {}
        if auto_id is not None:
            kwargs["auto_id"] = auto_id
        if name is not None:
            kwargs["title"] = name
        ctrl = fresh_win.child_window(**kwargs)
        ctrl.wait("exists", timeout=int(max(1, b_timeout)))
        return ctrl
    except Exception:  # noqa: BLE001
        return None


# ---------------------------------------------------------------------------
# Pipeline c91e7f3a phase-5: form field value set pipe
# Pipeline c91e7f3a phase-7: form field value read-back pipe pair
# ---------------------------------------------------------------------------

# Pipeline c91e7f3a phase-7: form field value read-back pipe pair.
# testFormFieldValueQueryPipe_  — Python writes a trigger token here.
# testFormFieldValueResultPipe_ — PDFE writes the current text value here.
# PDFE uses "__empty__" as a sentinel when the field value is empty.
_FORM_FIELD_VALUE_QUERY_PIPE_AUTO_ID  = "testFormFieldValueQueryPipe"
_FORM_FIELD_VALUE_RESULT_PIPE_AUTO_ID = "testFormFieldValueResultPipe"


def move_page_programmatic(session: "PDFESession",
                           from_index: int,
                           to_index: int,
                           settle: float = 0.50) -> bool:
    """Dispatch ``PagePanel::movePageProgrammatic`` via the hidden pipe.

    Writes ``"from_index,to_index"`` to the ``testMovePagePipe`` QLineEdit.
    PDFE's textChanged handler parses the pair and calls
    ``pagePanel_->movePageProgrammatic(from, to)``, which emits
    ``pageMoved(from, to)`` — routed through the existing MovePageCommand
    pipeline exactly as a drag-drop reorder would.

    Requirements:
    - PDFE launched with ``PDFE_TEST_PIPES=1`` in its environment.
    - A document must be open with at least ``max(from_index, to_index) + 1``
      pages.
    - ``from_index != to_index`` and both must be non-negative and in range.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    from_index:
        0-based source page index.
    to_index:
        0-based destination page index.
    settle:
        Seconds to sleep after dispatching so PDFE's event loop can
        process the MovePageCommand and refresh the PagePanel before
        the caller reads back state.

    Returns
    -------
    bool
        True on success, False on any failure (pipe not found, write
        error, invalid indices, no document open).
    """
    payload = f"{int(from_index)},{int(to_index)}"
    return _write_to_pipe(session, "testMovePagePipe", payload, settle=settle)


def add_redaction_mark_at_pdf_rect(session: "PDFESession",
                                    page_index: int,
                                    x: float,
                                    y: float,
                                    w: float,
                                    h: float,
                                    settle: float = 0.40) -> bool:
    """Dispatch ``PdfCanvas::addRedactionMarkAtPdfRect`` via the hidden pipe.

    Writes ``"page_index,x,y,w,h"`` to the ``testAddRedactionMarkPipe``
    QLineEdit.  PDFE's textChanged handler calls
    ``canvas_->addRedactionMarkAtPdfRect(pageIndex, QRectF(x,y,w,h))``,
    which emits ``redactionMarkRequested`` — connected to the
    CreateRedactionCommand pipeline — giving per-mark undo behaviour
    identical to a user drag.

    Coordinate space: **MuPDF/fitz-space** — origin at top-left of the
    page, Y increases downward, units are PDF points.  To convert from
    standard PDF coordinates (origin bottom-left, Y upward) use::

        fitz_y = page_height_pt - pdf_y_top

    For a standard letter page (612 × 792 pt):
    - PDF y_bottom=66.2, y_top=99.2 → fitz y=692.8, h=33.0
    - Full-width band: x=0, w=612

    Requirements:
    - PDFE launched with ``PDFE_TEST_PIPES=1`` in its environment.
    - A document must be open and the Redaction Tool must be active
      (so the ``redactionMarkRequested`` signal is routed).
    - ``w > 0`` and ``h > 0``.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    page_index:
        0-based page index.
    x, y:
        Top-left corner of the redaction rectangle in fitz-space points.
    w, h:
        Width and height in fitz-space points (must be positive).
    settle:
        Seconds to sleep after dispatching so PDFE's event loop can
        create the CreateRedactionCommand and refresh state.

    Returns
    -------
    bool
        True on success, False on any failure (pipe not found, invalid
        rect, page out of range).
    """
    payload = f"{int(page_index)},{x},{y},{w},{h}"
    return _write_to_pipe(session, "testAddRedactionMarkPipe",
                          payload, settle=settle)


def get_redaction_mark_count(session: "PDFESession",
                             settle: float = 0.30,
                             timeout: float = 5.0) -> int:
    """Read the pending redaction mark count from RedactionPanel.

    Writes a trigger token to ``testRedactionMarkCountQueryPipe``.
    PDFE's textChanged handler calls
    ``redactionPanel_->topLevelItemCount()`` and writes the integer
    (as a decimal string) to ``testRedactionMarkCountResultPipe``.
    This function reads the result pipe and returns the count.

    Bypasses the UIA QTreeWidget child-enumeration approach used by
    ``_count_redaction_marks_in_panel`` in the redaction tests: Qt6
    exposes each cell of a QTreeWidget row as a separate DataItem
    child in the UIA tree (4 cells × N rows = 4N items), inflating
    the count 4×.  This pipe reads
    ``treeWidget_->topLevelItemCount()`` directly.

    Requirements:
    - PDFE launched with ``PDFE_TEST_PIPES=1``.
    - Redaction panel must be visible (activated by redact tool selection).
    - Requires pipeline b3d7f942 phase-3 PDFE build.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    settle:
        Seconds to sleep after writing the trigger token.
    timeout:
        Seconds to wait for each pipe to appear in the UIA tree.

    Returns
    -------
    int
        The current pending mark count, or -1 if the pipes are not
        reachable or the result is unparseable.
    """
    try:
        query_pipe = _find_pipe(session, "testRedactionMarkCountQueryPipe",
                                 lookup_timeout=timeout)
        if query_pipe is None:
            return -1

        written = False
        for method_name in ("set_edit_text", "set_value"):
            try:
                getattr(query_pipe, method_name)("1")
                written = True
                break
            except Exception:  # noqa: BLE001
                continue
        if not written:
            return -1

        time.sleep(settle)

        result_pipe = _find_pipe(session, "testRedactionMarkCountResultPipe",
                                  lookup_timeout=timeout)
        if result_pipe is None:
            return -1

        val: str | None = None
        for method_name in ("iface_value", None):
            try:
                if method_name == "iface_value":
                    v = result_pipe.iface_value.CurrentValue
                    val = v if v is not None else ""
                else:
                    val = result_pipe.window_text() or ""
                break
            except Exception:  # noqa: BLE001
                continue

        if not val:
            return -1

        try:
            return int(val.strip())
        except ValueError:
            return -1

    except Exception:  # noqa: BLE001
        return -1


def get_form_field_panel_value(session: "PDFESession",
                                settle: float = 0.40,
                                timeout: float = 5.0) -> str | None:
    """Read the currently-displayed text value from FormFieldPropertiesPanel.

    Writes a trigger token to ``testFormFieldValueQueryPipe``.  PDFE's
    textChanged handler calls ``FormFieldPropertiesPanel::currentTextValue()``
    and writes the result to ``testFormFieldValueResultPipe`` (using the
    sentinel ``"__empty__"`` when the value is the empty string).  This
    function reads the result pipe and returns the value.

    This bypasses the ``IUIAutomation::FindAll`` traversal used by
    ``_form_field_text_value``, which can deadlock when PDFE's main Win32
    message pump is busy painting the FormFieldPropertiesPanel immediately
    after a ``click_by_pdf_coord``-dispatched ``QApplication::sendEvent``
    completes.  Because the query/result round-trip runs entirely within
    PDFE's event loop (Qt signal delivery), no cross-process COM call is
    needed.

    Requirements:
    - PDFE must be built with pipeline c91e7f3a phase-7 C++ changes.
    - A form field must already be selected (``_click_field_by_name`` or
      ``click_by_pdf_coord`` succeeded and the FormFieldPropertiesPanel
      is showing a text editor at stack index 0 or 1).

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    settle:
        Seconds to sleep after writing the trigger token, to allow
        PDFE's event loop to process the textChanged signal and populate
        the result pipe before it is read.
    timeout:
        Seconds to wait for each pipe to appear in the UIA tree.

    Returns
    -------
    str or None
        The field's current text value, ``""`` if the stack shows a
        non-text editor (checkbox/combo/list/no-editor), or ``None``
        if the pipes are not reachable (PDFE not built with the seam,
        or no field selected).
    """
    try:
        # Step 1: locate and trigger the query pipe.
        query_pipe = _find_pipe(session, _FORM_FIELD_VALUE_QUERY_PIPE_AUTO_ID,
                                 lookup_timeout=timeout)
        if query_pipe is None:
            return None

        written = False
        for method_name in ("set_edit_text", "set_value"):
            try:
                getattr(query_pipe, method_name)("1")
                written = True
                break
            except Exception:  # noqa: BLE001
                continue
        if not written:
            return None

        time.sleep(settle)

        # Step 2: read the result pipe.
        result_pipe = _find_pipe(session, _FORM_FIELD_VALUE_RESULT_PIPE_AUTO_ID,
                                  lookup_timeout=timeout)
        if result_pipe is None:
            return None

        val: str | None = None
        for method_name in ("iface_value", None):
            try:
                if method_name == "iface_value":
                    v = result_pipe.iface_value.CurrentValue
                    val = v if v is not None else ""
                else:
                    val = result_pipe.window_text() or ""
                break
            except Exception:  # noqa: BLE001
                continue

        if val is None:
            return None

        # Translate the "__empty__" sentinel back to an empty string.
        return "" if val == "__empty__" else val

    except Exception:  # noqa: BLE001
        return None


def set_form_field_panel_value(session: "PDFESession",
                                value: str,
                                settle: float = 0.35) -> bool:
    """Set the currently-selected form field's text value via the pipe seam.

    Writes *value* to ``testFormFieldValueSetPipe``.  PDFE's textChanged
    handler calls ``FormFieldPropertiesPanel::setTextValueForTesting(value)``,
    which sets the text on the QLineEdit and directly fires ``fieldValueChanged``
    → ``MainWindow::onFormFieldValueChanged`` creates a ``SetFieldValueCommand``.

    This bypasses the UIA ValuePattern → ``editingFinished`` race: UIA
    ``set_edit_text`` writes the text into the Qt widget but does not trigger
    ``editingFinished`` (which QLineEdit only emits on Enter/Tab/focus-loss),
    so the value would be visible in the panel but never committed to the PDF.

    Requirements:
    - A form field must already be selected in the FormFieldPropertiesPanel
      (i.e. ``_click_field_by_name`` or ``click_by_pdf_coord`` has succeeded).
    - The selected field must be a single-line text field (index 0 of the
      stacked editor).  Multiline text fields commit on every keystroke via
      ``textChanged``; all other field types (checkbox, combo, list) are not
      affected by this pipe.

    Parameters
    ----------
    session:
        Live ``PDFESession`` with a running PDFE.exe.
    value:
        Non-empty text string to commit.
    settle:
        Seconds to sleep after writing so PDFE's event loop can process
        the ``SetFieldValueCommand`` before the caller reads back state.

    Returns
    -------
    bool
        True on success, False on any failure (pipe not found, write error,
        no form field selected, or selected field is not a single-line text
        field).
    """
    if not value:
        return False
    return _write_to_pipe(session, _FORM_FIELD_VALUE_SET_PIPE_AUTO_ID,
                          value, settle=settle)


# --------------------------------------------------------------------------- #
# GitHub #303 -- the key trace, used to prove a pipe write no longer steals
# keyboard focus.
# --------------------------------------------------------------------------- #

_KEY_TRACE_QUERY_PIPE_AUTO_ID = "testKeyTraceQueryPipe"
_KEY_TRACE_RESULT_PIPE_AUTO_ID = "testKeyTraceResultPipe"


def key_trace_set_mode(session: "PDFESession", mode: str,
                       settle: float = 0.4) -> bool:
    """Arm the application's key trace. ``mode`` is ``all`` or ``ctrlz``.

    ``all`` is what #303 needs: a Ctrl+Z-only trace cannot tell "no Ctrl+Z
    arrived" from "no key of any kind arrived", and those are different
    findings with different owners. Both modes clear the trace, so this also
    serves as the reset between measurements.
    """
    if mode not in ("all", "ctrlz", "clear"):
        raise ValueError("key trace mode must be all|ctrlz|clear, got %r"
                         % mode)
    return _write_to_pipe(session, _KEY_TRACE_QUERY_PIPE_AUTO_ID, mode,
                          settle=settle)


def key_trace_dump(session: "PDFESession",
                   settle: float = 0.4) -> Optional[str]:
    """Return the recorded trace, or None if the harness could not find out.

    The application answers ``"empty"`` when it recorded nothing, which is
    deliberately distinct from this function's ``None`` ("the pipe did not
    answer"). Collapsing those two is what made #295 hard to pin down, so they
    stay distinct here too.
    """
    if not _write_to_pipe(session, _KEY_TRACE_QUERY_PIPE_AUTO_ID, "dump",
                          settle=settle):
        return None
    try:
        wid = _find_pipe(session, _KEY_TRACE_RESULT_PIPE_AUTO_ID)
        return None if wid is None else wid.get_value()
    except Exception:  # noqa: BLE001
        _invalidate_pipe_cache(session, _KEY_TRACE_RESULT_PIPE_AUTO_ID)
        return None


def key_trace_focus_targets(trace: str) -> "list[str]":
    """The ``focus=`` value of every record in a dumped trace, in order.

    The application formats each record as

        event=KeyPress key=0x5a mods=0x4000000 target=X[Class] focus=Y[Class] ...

    so the focused widget is a field, not something to infer. Returns [] for
    the application's ``"empty"`` answer, which a caller must distinguish from
    a trace it never obtained -- see ``key_trace_dump``.
    """
    if not trace or trace == "empty":
        return []
    out = []
    for record in trace.split(";"):
        m = re.search(r"focus=(\S+)", record)
        if m:
            out.append(m.group(1))
    return out


def focus_is_a_test_pipe(focus_field: str) -> bool:
    """Does a trace's ``focus=`` field name one of the hidden test pipes?

    Mirrors `pdfe::ui::TestPipeFocusGuard::isTestPipe`: a QLineEdit whose
    objectName starts with ``test`` and ends with ``Pipe``. The trace renders
    the field as ``objectName[ClassName]``, so the name is split out before
    the rule is applied rather than pattern-matching the whole string -- a
    widget named ``testFooPipe`` of some other class would otherwise be
    misjudged, and the guard deliberately only covers QLineEdit.
    """
    if not focus_field:
        return False
    name, _, rest = focus_field.partition("[")
    cls = rest.rstrip("]")
    return (name.startswith("test") and name.endswith("Pipe")
            and cls == "QLineEdit")
